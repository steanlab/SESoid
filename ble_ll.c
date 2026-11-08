// SES V300, BLE stage 2: own BLE link layer (slave role) on RAIL.
//
// How the 150 us rule (T_IFS) is met: the radio does it in hardware. In a connection RAIL is told
//   "after a good received packet go to TX after rxToTx us, after a transmit go back to RX".
// The answer must therefore sit in the TX FIFO BEFORE the master's packet is complete. Its header (SN, NESN) depends on
// the master's packet, so it is predicted, and the prediction is corrected by the next exchange:
//   * SN of our packet:   we number our packets with a counter idx (SN = idx & 1). A = number of our packets the master
//                         has accepted, inferred from the NESN bit of every master packet (A++ when NESN != A & 1).
//                         The next packet carries idx = A + (1 if the packet sent last is expected to be accepted).
//   * NESN of our packet: predicted from what the master will do (it sends a new SN iff our last NESN acknowledged its last packet).
// A wrong prediction (packet loss) costs one wasted exchange and repairs itself; no data is lost because a data PDU
// stays in its slot until the master's NESN shows it was accepted.
// See tools/ll_sim.py for a simulation of exactly this logic with random packet loss.
#include <string.h>
#include "ble_ll.h"
#include "ble_att.h"
#include "ble_diag.h"
#include "rail.h"
#include "rail_ble.h"
#include "em_device.h"

#define T_IFS_US          150u
#define RXTOTX_MIN_US     120u    // RAIL_MINIMUM_TRANSITION_US is 100 on EFR32xG22
#define RXTOTX_MAX_US     190u
#define ARM_LEAD_US       3000u   // the next event is armed this long before its nominal start
#define WIN_MARGIN_PRE_US 200u    // receiver is on this long before the earliest possible master packet
#define WIN_MARGIN_POST   250u
#define IN_N              16
#define OUT_N             8

typedef struct {
  bool     used;
  bool     has_data;
  ll_pdu_t pdu;
} slot_t;

typedef struct {
  // ---- connection parameters
  volatile bool active;
  RAIL_Handle_t h;
  uint32_t aa, crcinit;
  uint32_t interval_us, timeout_us;
  uint16_t latency;
  uint8_t  chm[5], used_ch[37], n_used, hop, last_unmapped;
  // ---- event schedule (main context)
  uint16_t ev_cnt;
  uint32_t est;               // estimated anchor (start of preamble of the master's first packet) of the armed event
  uint32_t last_real_anchor;  // anchor really measured
  uint32_t arm_at;
  uint32_t t_start;
  uint32_t cur_widen, cur_extra;
  bool     rx_pending;      // polled mode: RAIL_StartRx still to be called at rx_start
  uint16_t rx_phys;
  uint32_t rx_start;
  uint32_t rx_search_us;
  volatile bool ev_tx_seen;
  uint32_t search_extra;      // transmit window length while the master has not been heard yet (or after an update instant)
  bool     synced;
  volatile bool     heard_any;
  volatile uint32_t last_rx_time;
  uint32_t rxtotx;
  volatile int32_t  gap_err_acc;
  volatile uint32_t gap_err_n;
  // ---- pending procedures
  bool upd_pending;  uint16_t upd_instant;  uint32_t upd_woff_us, upd_wsize_us, upd_interval_us, upd_timeout_us;  uint16_t upd_latency;
  bool chm_pending;  uint16_t chm_instant;  uint8_t chm_new[5];
  bool terminate;    uint32_t terminate_at;
  // ---- link layer sequence state (shared with the interrupt)
  volatile bool in_arm;
  volatile bool ev_got;
  volatile uint32_t anchor_ev;
  uint8_t  nesn_exp;          // SN we expect next from the master
  uint8_t  last_rx_sn;        // SN of the last master packet we received
  uint8_t  last_tx_nesn;      // NESN we sent in our last packet
  uint32_t A;                 // our packets accepted by the master
  uint32_t assigned;          // number of idx values that have content in slots
  bool     have_last;
  uint32_t last_idx;
  slot_t   slot[2];
  bool     prep_valid;
  uint32_t prep_idx;
  uint8_t  prep_nesn;
  uint8_t  prep_len;          // bytes written to the TX FIFO (header + payload)
  uint8_t  cur_chan;
  uint32_t last_rx_start;
  uint32_t last_rx_total;
} conn_t;

static conn_t c;
static ll_pdu_t inbox[IN_N];
static volatile uint8_t in_head, in_tail;
static ll_pdu_t outbox[OUT_N];
static volatile uint8_t out_head, out_tail;
static RAIL_Handle_t g_h;
static volatile bool s_cal_needed;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void note(RAIL_Status_t st, uint32_t where)
{
  if (st != RAIL_STATUS_NO_ERROR) {
    g_diag.last_status = (uint32_t)st;
    g_diag.status_where = where;
  }
}

bool ll_connected(void) { return c.active; }

void ll_init(RAIL_Handle_t h) { g_h = h; c.active = false; }

// ---------------------------------------------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------------------------------------------
static bool inbox_push(const ll_pdu_t *p)          // interrupt context
{
  uint8_t next = (uint8_t)((in_tail + 1) % IN_N);
  if (next == in_head) { DIAG_INC(inbox_ovf); return false; }
  inbox[in_tail] = *p;
  __DMB();
  in_tail = next;
  return true;
}

static bool inbox_pop(ll_pdu_t *p)                 // main context
{
  if (in_head == in_tail) return false;
  *p = inbox[in_head];
  __DMB();
  in_head = (uint8_t)((in_head + 1) % IN_N);
  return true;
}

bool ll_send(uint8_t llid, const uint8_t *data, uint8_t len)    // main context (producer)
{
  if (len > LL_MAX_PAYLOAD) return false;
  uint8_t next = (uint8_t)((out_tail + 1) % OUT_N);
  if (next == out_head) { DIAG_INC(outbox_full); return false; }
  outbox[out_tail].llid = llid;
  outbox[out_tail].len = len;
  memcpy(outbox[out_tail].data, data, len);
  __DMB();
  out_tail = next;
  return true;
}

static bool outbox_pop(ll_pdu_t *p)                // interrupt/arming context (consumer)
{
  if (out_head == out_tail) return false;
  *p = outbox[out_head];
  __DMB();
  out_head = (uint8_t)((out_head + 1) % OUT_N);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Time stamps
// ---------------------------------------------------------------------------------------------------------------
// Convert a RAIL time stamp to the time of the start of the preamble. total = bytes after the access address incl. CRC.
static uint32_t to_preamble_start(RAIL_PacketTimePosition_t pos, uint32_t t, uint32_t total)
{
  switch (pos) {
    case RAIL_PACKET_TIME_AT_SYNC_END:
    case RAIL_PACKET_TIME_AT_SYNC_END_USED_TOTAL:
      return t - 40u;
    case RAIL_PACKET_TIME_AT_PACKET_END:
    case RAIL_PACKET_TIME_AT_PACKET_END_USED_TOTAL:
      return t - 40u - total * 8u;
    default:
      return t;
  }
}

static void trace_add(uint8_t dir_chan, uint8_t b0, uint8_t b1)
{
  uint32_t p = g_diag.trace_pos % TRACE_N;
  g_diag.trace[p][0] = dir_chan;
  g_diag.trace[p][1] = b0;
  g_diag.trace[p][2] = b1;
  g_diag.trace[p][3] = (uint8_t)c.A;
  g_diag.trace_pos++;
}

// ---------------------------------------------------------------------------------------------------------------
// Preparing the next packet of ours (interrupt context after a transmit, or main context while arming an event)
// ---------------------------------------------------------------------------------------------------------------
static void prepare_next(RAIL_Handle_t h)
{
  uint32_t idx = c.A + ((c.have_last && c.last_idx == c.A) ? 1u : 0u);
  slot_t *s = (slot_t *)&c.slot[idx & 1u];
  if (idx >= c.assigned) {                 // fresh index: take the next queued PDU (if any)
    s->has_data = outbox_pop(&s->pdu);
    s->used = true;
    c.assigned = idx + 1u;
  }
  // predicted SN of the master's next packet: it is new iff our last NESN acknowledged its last packet
  uint8_t pred_sn = (c.last_tx_nesn != c.last_rx_sn) ? (uint8_t)(c.last_rx_sn ^ 1u) : c.last_rx_sn;
  uint8_t nesn = (pred_sn == c.nesn_exp) ? (uint8_t)(c.nesn_exp ^ 1u) : c.nesn_exp;
  uint8_t llid = s->has_data ? s->pdu.llid : 1u;
  uint8_t len = s->has_data ? s->pdu.len : 0u;
  uint8_t pkt[2 + LL_MAX_PAYLOAD];
  pkt[0] = (uint8_t)(llid | (nesn << 2) | ((idx & 1u) << 3));    // MD = 0
  pkt[1] = len;
  if (len) memcpy(&pkt[2], s->pdu.data, len);
  uint16_t wr = RAIL_WriteTxFifo(h, pkt, (uint16_t)(2u + len), true);
  if (wr != (uint16_t)(2u + len)) note(RAIL_STATUS_INVALID_PARAMETER, 11);
  c.prep_idx = idx;
  c.prep_nesn = nesn;
  c.prep_len = (uint8_t)(2u + len);
  c.prep_valid = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Interrupt context
// ---------------------------------------------------------------------------------------------------------------
static void rx_isr(RAIL_Handle_t h)
{
  RAIL_RxPacketInfo_t info;
  RAIL_RxPacketHandle_t ph = RAIL_GetRxPacketInfo(h, RAIL_RX_PACKET_HANDLE_OLDEST_COMPLETE, &info);
  if (ph == RAIL_RX_PACKET_HANDLE_INVALID) return;
  uint8_t buf[40];
  bool ok = (info.packetStatus == RAIL_RX_PACKET_READY_SUCCESS) && info.packetBytes >= 2u && info.packetBytes <= sizeof(buf);
  uint32_t t_start = 0;
  if (ok) {
    RAIL_CopyRxPacket(buf, &info);
    RAIL_RxPacketDetails_t det;
    det.timeReceived.timePosition = RAIL_PACKET_TIME_AT_PACKET_END;
    det.timeReceived.totalPacketBytes = (uint16_t)(info.packetBytes + 3u);
    if (RAIL_GetRxPacketDetails(h, ph, &det) == RAIL_STATUS_NO_ERROR && det.timeReceived.timePosition != RAIL_PACKET_TIME_INVALID) {
      t_start = to_preamble_start(det.timeReceived.timePosition, det.timeReceived.packetTime, info.packetBytes + 3u);
      g_diag.last_rssi = (uint32_t)(int32_t)det.rssi;
    } else {
      t_start = RAIL_GetTime() - 40u - (info.packetBytes + 3u) * 8u - 20u;
    }
  }
  RAIL_ReleaseRxPacket(h, ph);
  if (!ok || buf[1] + 2u != info.packetBytes) { DIAG_INC(rx_bad); return; }
  if (c.in_arm) return;

  uint8_t b0 = buf[0], len = buf[1];
  uint8_t llid = b0 & 3u, nesn = (b0 >> 2) & 1u, sn = (b0 >> 3) & 1u;

  // 1. what did the master accept of ours?
  if (nesn != (uint8_t)(c.A & 1u)) {
    c.A++;
    DIAG_INC(a_count);
    slot_t *done = (slot_t *)&c.slot[(c.A - 1u) & 1u];
    done->used = false;
    done->has_data = false;
  } else if (c.have_last) {
    DIAG_INC(nack_seen);
  }
  // 2. new data or a retransmission?
  if (sn == c.nesn_exp) {
    c.nesn_exp ^= 1u;
    if (len > 0u && llid != 0u && len <= LL_MAX_PAYLOAD) {
      ll_pdu_t p;
      p.llid = llid;
      p.len = len;
      memcpy(p.data, &buf[2], len);
      inbox_push(&p);
    }
  } else {
    DIAG_INC(rx_dup);
  }
  c.last_rx_sn = sn;
  c.last_rx_start = t_start;
  c.last_rx_total = info.packetBytes + 3u;
  if (!c.ev_got) {
    c.ev_got = true;
    c.anchor_ev = t_start;
    DIAG_INC(ev_heard);
  }
  c.heard_any = true;
  c.last_rx_time = RAIL_GetTime();
  DIAG_INC(rx_good);
  trace_add(c.cur_chan, b0, len);
}

static void tx_isr(RAIL_Handle_t h)
{
  if (c.in_arm) return;
  c.ev_tx_seen = true;
  if (c.prep_valid) {
    c.last_idx = c.prep_idx;
    c.last_tx_nesn = c.prep_nesn;
    c.have_last = true;
    c.prep_valid = false;
    DIAG_INC(tx_sent);
    // measure the real turnaround: end of the master's packet -> start of ours
    RAIL_TxPacketDetails_t d;
    d.isAck = false;
    d.timeSent.timePosition = RAIL_PACKET_TIME_AT_PREAMBLE_START;
    d.timeSent.totalPacketBytes = (uint16_t)(c.prep_len + 3u);
    if (RAIL_GetTxPacketDetails(h, &d) == RAIL_STATUS_NO_ERROR) {
      uint32_t tx_start = to_preamble_start(d.timeSent.timePosition, d.timeSent.packetTime, c.prep_len + 3u);
      uint32_t rx_end = c.last_rx_start + 40u + c.last_rx_total * 8u;
      int32_t gap = (int32_t)(tx_start - rx_end);
      g_diag.gap_last = (uint32_t)gap;
      if (gap > -1000 && gap < 1000) {
        if (g_diag.gap_n == 0u || gap < (int32_t)g_diag.gap_min) g_diag.gap_min = (uint32_t)gap;
        if (g_diag.gap_n == 0u || gap > (int32_t)g_diag.gap_max) g_diag.gap_max = (uint32_t)gap;
        g_diag.gap_n++;
        c.gap_err_acc += gap - (int32_t)T_IFS_US;
        c.gap_err_n++;
      }
    }
    trace_add((uint8_t)(0x80u | c.cur_chan), (uint8_t)(1u | 0u), c.prep_len);
  }
  prepare_next(h);
}

void ll_on_event(RAIL_Handle_t h, RAIL_Events_t events)
{
  uint32_t lp = g_diag.evlog_pos % EVLOG_N;
  g_diag.evlog[lp][0] = RAIL_GetTime();
  g_diag.evlog[lp][1] = (uint32_t)events;
  g_diag.evlog[lp][2] = (uint32_t)(events >> 32);
  g_diag.evlog_pos++;
  if (events & RAIL_EVENT_RX_SYNC1_DETECT) DIAG_INC(sync1);
  if (events & RAIL_EVENT_SCHEDULED_RX_STARTED) DIAG_INC(sched_start);
  if (events & RAIL_EVENT_RX_SCHEDULED_RX_END) DIAG_INC(sched_end);
  g_diag.last_ev_lo = (uint32_t)events;
  g_diag.last_ev_hi = (uint32_t)(events >> 32);
  if (events & RAIL_EVENT_RX_PACKET_RECEIVED) rx_isr(h);
  if (events & RAIL_EVENT_TX_PACKET_SENT) tx_isr(h);
  if (events & (RAIL_EVENT_RX_FRAME_ERROR | RAIL_EVENT_RX_PACKET_ABORTED)) DIAG_INC(rx_bad);
  if (events & RAIL_EVENT_RX_TIMEOUT) DIAG_INC(rx_timeout);
  if (events & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW)) DIAG_INC(tx_fail);
  if (events & RAIL_EVENT_RX_FIFO_OVERFLOW) DIAG_INC(rx_overflow);
  if (events & RAIL_EVENT_CAL_NEEDED) s_cal_needed = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Event scheduling (main context)
// ---------------------------------------------------------------------------------------------------------------
static void build_used_table(void)
{
  c.n_used = 0;
  for (uint8_t i = 0; i < 37; i++) {
    if ((c.chm[i >> 3] >> (i & 7)) & 1u) c.used_ch[c.n_used++] = i;
  }
}

static uint8_t next_channel(void)       // channel selection algorithm #1
{
  uint8_t un = (uint8_t)((c.last_unmapped + c.hop) % 37u);
  c.last_unmapped = un;
  if ((c.chm[un >> 3] >> (un & 7)) & 1u) return un;
  return c.used_ch[un % c.n_used];
}

static void set_state_timing(void)
{
  RAIL_StateTiming_t t;
  t.idleToRx = RAIL_TRANSITION_TIME_KEEP;
  t.idleToTx = RAIL_TRANSITION_TIME_KEEP;
  t.txToTx = RAIL_TRANSITION_TIME_KEEP;
  t.txToRx = 100;                                  // be listening again well before the master's next packet (150 us after our end)
  t.rxToTx = c.rxtotx;
  t.rxSearchTimeout = c.rx_search_us;
  t.txToRxSearchTimeout = 700;                     // if the master does not continue the event, stop listening
  note(RAIL_SetStateTiming(c.h, &t), 12);
  g_diag.rxtotx_cfg = c.rxtotx;
}

static void arm_event(bool first)
{
  RAIL_Handle_t h = c.h;
  uint32_t base = 0, extra, widen, start, end;
  uint8_t logical;
  uint16_t phys;
  bool advance = !first;

  if (first) {
    c.ev_cnt = 0;
  } else {
    // close the event that has just ended: where did the master really start?
    if (c.ev_got) {
      int32_t err = (int32_t)(c.anchor_ev - c.est);
      if (err < (int32_t)g_diag.anchor_err_min) g_diag.anchor_err_min = (uint32_t)err;
      if (err > (int32_t)g_diag.anchor_err_max) g_diag.anchor_err_max = (uint32_t)err;
      if (err < -(int32_t)(c.cur_widen + WIN_MARGIN_PRE_US + 1000u) ||
          err > (int32_t)(c.cur_extra + c.cur_widen + WIN_MARGIN_POST + 1000u)) {
        DIAG_INC(anchor_bad);                 // a time stamp that does not fit the window it was received in: not trusted
        base = c.est;
      } else {
        base = c.anchor_ev;
        c.last_real_anchor = base;
        c.synced = true;
      }
      if (!c.ev_tx_seen) DIAG_INC(rx_no_tx);
    } else {
      base = c.est;
      DIAG_INC(ev_missed);
    }
  }

  for (uint32_t guard = 0;; guard++) {
    if (advance) {
      c.ev_cnt++;
      if (c.upd_pending && (int16_t)(c.ev_cnt - c.upd_instant) == 0) {
        c.est = base + c.interval_us + c.upd_woff_us;
        c.interval_us = c.upd_interval_us;
        c.timeout_us = c.upd_timeout_us;
        c.latency = c.upd_latency;
        c.search_extra = c.upd_wsize_us;
        c.synced = false;
        c.upd_pending = false;
        DIAG_INC(upd_applied);
      } else {
        c.est = base + c.interval_us;
      }
      if (c.chm_pending && (int16_t)(c.ev_cnt - c.chm_instant) == 0) {
        memcpy((void *)c.chm, c.chm_new, 5);
        build_used_table();
        c.chm_pending = false;
        DIAG_INC(chm_applied);
      }
    }
    advance = true;
    extra = c.synced ? 0u : c.search_extra;
    logical = next_channel();
    phys = (uint16_t)(logical + (logical < 11u ? 1u : 2u));
    uint32_t age_us = c.est - c.last_real_anchor;
    widen = age_us / 3700u + 40u;                     // 270 ppm of clock error plus a margin
    start = c.est - widen - WIN_MARGIN_PRE_US;
    end = c.est + extra + widen + WIN_MARGIN_POST;
    if (guard >= 64u || (int32_t)(end - RAIL_GetTime()) > 300) break;
    DIAG_INC(catchup);                                // the window is already over (main loop was stalled): count it as a missed event
    base = c.est;
  }
  c.cur_chan = logical;
  c.cur_widen = widen;
  c.cur_extra = extra;
  c.arm_at = c.est + c.interval_us - ARM_LEAD_US;

  // tune the turnaround from the measured gaps (keeps the answer at 150 us +- a few us)
  if (c.gap_err_n >= 16u) {
    int32_t avg = c.gap_err_acc / (int32_t)c.gap_err_n;
    g_diag.gap_sum_err = (uint32_t)avg;
    if (avg > 2 || avg < -2) {
      int32_t nv = (int32_t)c.rxtotx - avg;
      if (nv < (int32_t)RXTOTX_MIN_US) nv = (int32_t)RXTOTX_MIN_US;
      if (nv > (int32_t)RXTOTX_MAX_US) nv = (int32_t)RXTOTX_MAX_US;
      c.rxtotx = (uint32_t)nv;
    }
    c.gap_err_acc = 0;
    c.gap_err_n = 0;
  }

  bool sched = g_diag.knob_sched != 0u;
  if (!sched) start -= 150u;                          // the polled start has a little more jitter than the scheduled one
  c.in_arm = true;
  RAIL_Idle(h, RAIL_IDLE_ABORT, true);
  if (s_cal_needed) {
    s_cal_needed = false;
    DIAG_INC(cal_runs);
    note(RAIL_Calibrate(h, NULL, RAIL_CAL_ALL_PENDING), 5);
  }
  RAIL_ResetFifo(h, true, true);
  c.rx_search_us = sched ? 0u : (end - start);
  set_state_timing();
  note(RAIL_BLE_ConfigChannelRadioParams(h, c.crcinit, c.aa, logical, false), 1);
  c.ev_got = false;
  c.ev_tx_seen = false;
  prepare_next(h);

  c.rx_pending = false;
  if (sched) {
    RAIL_ScheduleRxConfig_t cfg;
    cfg.start = start;
    cfg.startMode = RAIL_TIME_ABSOLUTE;
    cfg.end = end;
    cfg.endMode = RAIL_TIME_ABSOLUTE;
    cfg.rxTransitionEndSchedule = 1;
    cfg.hardWindowEnd = 0;
    RAIL_Status_t st = RAIL_ScheduleRx(h, phys, &cfg, NULL);
    if (st != RAIL_STATUS_NO_ERROR) {
      note(st, 4);
      DIAG_INC(arm_fail);
      st = RAIL_StartRx(h, phys, NULL);                // too late for the schedule: listen right away
      note(st, 4);
    }
  } else {
    c.rx_phys = phys;
    c.rx_start = start;
    c.rx_pending = true;                               // ll_poll() starts the receiver when the time comes
  }
  c.in_arm = false;
  int32_t dt = (int32_t)(start - RAIL_GetTime());      // how long before the window we finished arming (negative = late)
  if (dt < (int32_t)g_diag.arm_dt_min) g_diag.arm_dt_min = (uint32_t)dt;
  if (dt > (int32_t)g_diag.arm_dt_max) g_diag.arm_dt_max = (uint32_t)dt;
  DIAG_INC(ev_armed);
  g_diag.ev_cnt = c.ev_cnt;
}

// ---------------------------------------------------------------------------------------------------------------
// Connection start / end
// ---------------------------------------------------------------------------------------------------------------
static void ll_end(uint32_t reason)
{
  RAIL_Handle_t h = c.h;
  c.active = false;
  g_diag.conn_ms = (RAIL_GetTime() - c.t_start) / 1000u;
  g_diag.end_age_ms = (RAIL_GetTime() - c.last_rx_time) / 1000u;
  RAIL_Idle(h, RAIL_IDLE_ABORT, true);
  note(RAIL_ConfigEvents(h, RAIL_EVENT_RX_SYNC1_DETECT | RAIL_EVENT_SCHEDULED_RX_STARTED | RAIL_EVENT_RX_SCHEDULED_RX_END, 0), 14);
  RAIL_StateTransitions_t tr = { RAIL_RF_STATE_RX, RAIL_RF_STATE_RX };
  note(RAIL_SetRxTransitions(h, &tr), 13);
  note(RAIL_SetTxTransitions(h, &tr), 13);
  RAIL_StateTiming_t t;
  t.idleToRx = RAIL_TRANSITION_TIME_KEEP;
  t.idleToTx = RAIL_TRANSITION_TIME_KEEP;
  t.txToTx = RAIL_TRANSITION_TIME_KEEP;
  t.txToRx = RAIL_TRANSITION_TIME_KEEP;
  t.rxToTx = RAIL_TRANSITION_TIME_KEEP;
  t.rxSearchTimeout = 0;
  t.txToRxSearchTimeout = 0;
  RAIL_SetStateTiming(h, &t);
  RAIL_ResetFifo(h, true, true);
  in_head = in_tail = 0;
  out_head = out_tail = 0;
  att_reset();
  DIAG_INC(disconns);
  g_diag.disc_reason = reason;
  g_diag.stage = 3;
}

bool ll_start(const uint8_t *ci, uint32_t ci_end_us)
{
  if (c.active || ci[1] != 34u) return false;
  const uint8_t *d = ci + 14;
  uint32_t aa = rd32(d);
  uint32_t crcinit = (uint32_t)(d[4] | (d[5] << 8) | (d[6] << 16));
  uint8_t winsize = d[7];
  uint16_t winoff = rd16(d + 8), interval = rd16(d + 10), latency = rd16(d + 12), timeout = rd16(d + 14);
  uint8_t hop = d[21] & 0x1Fu;
  if (interval < 6u || interval > 3200u || hop < 5u || hop > 16u || aa == 0u || timeout < 10u) return false;
  if (winsize == 0u || winsize > 8u) return false;
  uint8_t pop = 0;
  for (int i = 0; i < 5; i++) {
    uint8_t m = d[16 + i];
    if (i == 4) m &= 0x1Fu;
    while (m) { pop += (m & 1u); m >>= 1; }
  }
  if (pop < 2u) return false;

  c.h = g_h;
  RAIL_Handle_t h = c.h;
  c.aa = aa;
  c.crcinit = crcinit;
  c.interval_us = (uint32_t)interval * 1250u;
  c.timeout_us = (uint32_t)timeout * 10000u;
  c.latency = latency;
  memcpy((void *)c.chm, d + 16, 5);
  c.chm[4] &= 0x1Fu;
  c.hop = hop;
  c.last_unmapped = 0;
  build_used_table();
  c.upd_pending = c.chm_pending = c.terminate = false;
  c.ev_cnt = 0;
  c.rx_pending = false;
  c.ev_tx_seen = false;
  c.ev_got = false;
  c.t_start = RAIL_GetTime();
  c.est = ci_end_us + 1250u + (uint32_t)winoff * 1250u;      // earliest moment the master's first packet can start
  c.last_real_anchor = c.est;
  c.search_extra = (uint32_t)winsize * 1250u;
  c.synced = false;
  c.heard_any = false;
  c.last_rx_time = ci_end_us;
  c.rxtotx = T_IFS_US;
  c.gap_err_acc = 0;
  c.gap_err_n = 0;
  c.nesn_exp = 0;
  c.last_rx_sn = 1;           // makes the prediction for the first master packet SN = 0
  c.last_tx_nesn = 0;
  c.A = 0;
  c.assigned = 0;
  c.have_last = false;
  c.last_idx = 0;
  c.slot[0].used = c.slot[1].used = false;
  c.slot[0].has_data = c.slot[1].has_data = false;
  c.prep_valid = false;
  in_head = in_tail = 0;
  out_head = out_tail = 0;
  att_reset();

  RAIL_Idle(h, RAIL_IDLE_ABORT, true);
  RAIL_StateTransitions_t rx_tr = { RAIL_RF_STATE_TX, RAIL_RF_STATE_IDLE };      // good packet -> answer; bad packet -> event over
  RAIL_StateTransitions_t tx_tr = { RAIL_RF_STATE_RX, RAIL_RF_STATE_IDLE };
  note(RAIL_SetRxTransitions(h, &rx_tr), 13);
  note(RAIL_SetTxTransitions(h, &tx_tr), 13);
  g_diag.rxtotx_cfg = 0;
  c.rx_search_us = 0;
  set_state_timing();
  g_diag.arm_dt_min = 0x7FFFFFFFu;  g_diag.arm_dt_max = 0x80000000u;
  g_diag.anchor_err_min = 0x7FFFFFFFu;  g_diag.anchor_err_max = 0x80000000u;
  g_diag.poll_gap_max = 0;  g_diag.conn_ms = 0;  g_diag.end_age_ms = 0;
  g_diag.evlog_pos = 0;
  memset((void *)g_diag.evlog, 0, sizeof(g_diag.evlog));
  // extra RAIL events only while connected (they cost interrupts): sync word detected, scheduled receive start/end, timeout
  note(RAIL_ConfigEvents(h, RAIL_EVENT_RX_SYNC1_DETECT | RAIL_EVENT_SCHEDULED_RX_STARTED | RAIL_EVENT_RX_SCHEDULED_RX_END | RAIL_EVENT_RX_TIMEOUT,
                         RAIL_EVENT_RX_SYNC1_DETECT | RAIL_EVENT_SCHEDULED_RX_STARTED | RAIL_EVENT_RX_SCHEDULED_RX_END | RAIL_EVENT_RX_TIMEOUT), 14);

  g_diag.interval_us = c.interval_us;
  g_diag.timeout_ms = (uint32_t)timeout * 10u;
  g_diag.latency = latency;
  g_diag.hop = hop;
  g_diag.aa = aa;
  g_diag.crcinit = crcinit;
  g_diag.chm_lo = rd32(d + 16);
  g_diag.chm_hi = d[20];
  DIAG_INC(conns);
  g_diag.stage = 4;

  c.active = true;
  arm_event(true);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// LL control procedures (main context)
// ---------------------------------------------------------------------------------------------------------------
static void ctrl_send(const uint8_t *d, uint8_t len)
{
  ll_send(3, d, len);
  DIAG_INC(ll_rsp_sent);
}

static void unknown_rsp(uint8_t op)
{
  uint8_t r[2] = { 0x07, op };
  ctrl_send(r, 2);
}

static void ll_control(const ll_pdu_t *p)
{
  DIAG_INC(ll_ctrl_rx);
  g_diag.ll_ctrl_op = p->data[0];
  for (uint8_t i = 0; i < sizeof(g_diag.last_ll); i++) g_diag.last_ll[i] = (i < p->len) ? p->data[i] : 0;
  const uint8_t *d = p->data;
  switch (d[0]) {
    case 0x00:   // LL_CONNECTION_UPDATE_IND
      if (p->len == 12) {
        c.upd_woff_us = (uint32_t)rd16(d + 2) * 1250u;
        c.upd_wsize_us = (uint32_t)d[1] * 1250u;
        c.upd_interval_us = (uint32_t)rd16(d + 4) * 1250u;
        c.upd_latency = rd16(d + 6);
        c.upd_timeout_us = (uint32_t)rd16(d + 8) * 10000u;
        c.upd_instant = rd16(d + 10);
        c.upd_pending = true;
      }
      break;
    case 0x01:   // LL_CHANNEL_MAP_IND
      if (p->len == 8) {
        memcpy(c.chm_new, d + 1, 5);
        c.chm_new[4] &= 0x1Fu;
        c.chm_instant = rd16(d + 6);
        c.chm_pending = true;
      }
      break;
    case 0x02:   // LL_TERMINATE_IND
      c.terminate = true;
      c.terminate_at = RAIL_GetTime() + c.interval_us + 5000u;
      g_diag.disc_reason = 0x100u | (p->len > 1 ? d[1] : 0u);
      break;
    case 0x08: { // LL_FEATURE_REQ -> LL_FEATURE_RSP, no optional features
      uint8_t r[9] = { 0x09, 0, 0, 0, 0, 0, 0, 0, 0 };
      ctrl_send(r, 9);
      break;
    }
    case 0x0C: { // LL_VERSION_IND -> ours: Bluetooth 5.0, company Silicon Labs, subversion 0
      uint8_t r[6] = { 0x0C, 0x09, 0xFF, 0x02, 0x00, 0x00 };
      ctrl_send(r, 6);
      break;
    }
    case 0x12: { // LL_PING_REQ
      uint8_t r[1] = { 0x13 };
      ctrl_send(r, 1);
      break;
    }
    case 0x14: { // LL_LENGTH_REQ -> LL_LENGTH_RSP: stay at 27 bytes / 328 us
      uint8_t r[9] = { 0x15, 27, 0, 0x48, 0x01, 27, 0, 0x48, 0x01 };
      ctrl_send(r, 9);
      break;
    }
    case 0x07: case 0x09: case 0x0D: case 0x11: case 0x13: case 0x15:
      break;   // responses / rejects from the master: nothing to do
    default:
      unknown_rsp(d[0]);
      break;
  }
}

static void l2cap_other(uint16_t cid, const uint8_t *p, uint8_t len)
{
  if (cid == 0x0005 && len >= 4) {                // LE signalling: reject whatever it is (Command Not Understood)
    DIAG_INC(sig_rx);
    uint8_t out[10] = { 6, 0, 5, 0, 0x01, p[1], 0x02, 0x00, 0x00, 0x00 };   // L2CAP length 6, CID 5; Command Reject, same id, reason 0
    ll_send(2, out, 10);
  } else if (cid == 0x0006) {                     // SMP: pairing is not supported -> Pairing Failed (reason 5)
    DIAG_INC(smp_rx);
    uint8_t out[6] = { 2, 0, 6, 0, 0x05, 0x05 };
    ll_send(2, out, 6);
  }
}

static void data_pdu(const ll_pdu_t *p)
{
  if (p->llid != 2 || p->len < 4) return;           // continuation fragments are not expected (MTU 23 fits one PDU)
  uint16_t l2len = rd16(p->data);
  uint16_t cid = rd16(p->data + 2);
  if (l2len + 4u != p->len) return;
  if (cid == 0x0004) att_rx(p->data + 4, (uint8_t)l2len);
  else l2cap_other(cid, p->data + 4, (uint8_t)l2len);
}

void ll_poll(void)
{
  static uint32_t last_poll;
  if (!c.active) { last_poll = 0; return; }
  uint32_t now = RAIL_GetTime();
  g_diag.poll_calls++;
  if (last_poll != 0u) {
    uint32_t gap = now - last_poll;
    if (gap > g_diag.poll_gap_max) g_diag.poll_gap_max = gap;
  }
  last_poll = now;
  g_diag.radio_state = (uint32_t)RAIL_GetRadioState(c.h);

  // polled mode: open the receive window at the planned moment
  if (c.rx_pending && (int32_t)(now - c.rx_start) >= 0) {
    c.rx_pending = false;
    if ((int32_t)(now - c.rx_start) > 200) DIAG_INC(rxpend_late);
    note(RAIL_StartRx(c.h, c.rx_phys, NULL), 4);
  }

  ll_pdu_t p;
  while (inbox_pop(&p)) {
    if (p.llid == 3) ll_control(&p);
    else data_pdu(&p);
  }

  if (c.terminate && (int32_t)(now - c.terminate_at) >= 0) { ll_end(g_diag.disc_reason); return; }
  uint32_t lim = c.heard_any ? c.timeout_us : 6u * c.interval_us + 10000u;
  if ((uint32_t)(now - c.last_rx_time) > lim) { ll_end(0x200u); return; }
  if ((int32_t)(now - c.arm_at) >= 0) arm_event(false);
}
