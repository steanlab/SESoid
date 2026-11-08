// SES V300, BLE stage 2: diagnostic structure g_diag in RAM, read by tools/ble_probe.py over SWD (without halting the CPU).
// The list of fields below is the single source of truth: tools/ble_probe.py takes the names from this file's order,
// so only append new fields at the END of DIAG_FIELDS (and keep the tool in step).
// knob_sched is WRITABLE over SWD (ble_probe.py --set knob_sched=1): 0 = receive windows started by the main loop with
// RAIL_StartRx (default), 1 = RAIL_ScheduleRx.
#ifndef BLE_DIAG_H
#define BLE_DIAG_H

#include <stdint.h>

#define DIAG_MAGIC 0x32454C42u   // "BLE2" little endian
#define DIAG_END   0x444E4532u   // "2END"
#define TRACE_N    32
#define EVLOG_N    24

#define DIAG_FIELDS(X) \
  X(magic) X(struct_size) X(nfields) X(heartbeat) X(stage) X(rail_version) \
  X(last_status) X(status_where) \
  X(adv_rounds) X(adv_tx_sent) X(adv_tx_failed) X(scan_req) X(connect_ind) X(connect_ignored) \
  X(conns) X(disconns) X(disc_reason) \
  X(ev_armed) X(ev_heard) X(ev_missed) \
  X(rx_good) X(rx_bad) X(rx_dup) X(rx_overflow) X(rx_timeout) X(tx_sent) X(tx_fail) \
  X(a_count) X(nack_seen) \
  X(gap_last) X(gap_min) X(gap_max) X(gap_n) X(rxtotx_cfg) X(gap_sum_err) \
  X(interval_us) X(timeout_ms) X(latency) X(hop) X(aa) X(crcinit) X(chm_lo) X(chm_hi) X(ev_cnt) \
  X(ll_ctrl_rx) X(ll_ctrl_op) X(ll_rsp_sent) \
  X(att_rx) X(att_op) X(att_err_sent) X(att_write_n) X(att_write_bytes) X(notify_sent) X(notify_dropped) X(cccd) \
  X(sig_rx) X(smp_rx) \
  X(inbox_ovf) X(outbox_full) X(cal_runs) X(last_rssi) X(arm_fail) X(upd_applied) X(chm_applied) \
  X(last_ev_lo) X(last_ev_hi) \
  X(knob_sched) X(conn_ms) X(end_age_ms) X(arm_dt_min) X(arm_dt_max) X(poll_gap_max) X(anchor_err_min) X(anchor_err_max) \
  X(anchor_bad) X(rx_no_tx) X(sync1) X(sched_start) X(sched_end) X(radio_state) X(evlog_pos) X(poll_calls) X(rxpend_late) X(catchup)

typedef struct {
#define X(n) uint32_t n;
  DIAG_FIELDS(X)
#undef X
  uint8_t  trace[TRACE_N][4];   // per exchange: [0] = channel | 0x80 for our TX, [1] = header byte 0, [2] = length byte, [3] = low byte of A
  uint32_t trace_pos;
  uint8_t  last_ll[16];         // last LL control PDU received (opcode first)
  uint8_t  last_att[24];        // last ATT PDU received
  uint8_t  last_att_tx[24];     // last ATT PDU sent
  uint32_t evlog[EVLOG_N][3];   // RAIL events in the connection: [0] = RAIL time (us), [1] = event bits 0..31, [2] = bits 32..63
  uint32_t end;
} ble_diag_t;

extern volatile ble_diag_t g_diag;

#define DIAG_INC(f) (g_diag.f++)

#endif
