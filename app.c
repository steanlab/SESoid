// SES V300, BLE stage 2: a connectable BLE peripheral on RAIL (own link layer, see ble_ll.c), with a tiny GATT server (ble_att.c).
//
// This file: initialisation, the advertising loop (ADV_IND "V300-probe" on 37/38/39 every ~100 ms, listening 1.8 ms after each),
// hand-over to the link layer when a CONNECT_IND for us arrives, back to advertising after a disconnect.
// Nothing here touches the e-paper panel, the external flash or any pin.
#include <stdint.h>
#include <string.h>
#include "sl_rail_util_init.h"
#include "rail.h"
#include "rail_ble.h"
#include "ble_diag.h"
#include "ble_ll.h"
#include "ble_att.h"

volatile ble_diag_t g_diag;

static volatile uint8_t  s_rx_ready;
static volatile uint32_t s_rx_len;
static volatile uint32_t s_rx_end_us;       // RAIL time of the end of the received packet
static uint8_t s_rx[64];
static volatile uint8_t s_cal_needed;

#define BLE_ADV_AA   0x8E89BED6u
#define BLE_ADV_CRC  0x555555u

// our device address (static random, LSB first): C3:DE:00:30:00:01
static const uint8_t ADV_ADDR[6] = { 0x01, 0x00, 0x30, 0x00, 0xDE, 0xC3 };

static RAIL_Handle_t h;

static void note(RAIL_Status_t st, uint32_t where)
{
  if (st != RAIL_STATUS_NO_ERROR) {
    g_diag.last_status = (uint32_t)st;
    g_diag.status_where = where;
  }
}

// Time of the end of a received packet from a RAIL time stamp. total = bytes after the access address incl. CRC.
static uint32_t to_packet_end(RAIL_PacketTimePosition_t pos, uint32_t t, uint32_t total)
{
  switch (pos) {
    case RAIL_PACKET_TIME_AT_PACKET_END:
    case RAIL_PACKET_TIME_AT_PACKET_END_USED_TOTAL:
      return t;
    case RAIL_PACKET_TIME_AT_SYNC_END:
    case RAIL_PACKET_TIME_AT_SYNC_END_USED_TOTAL:
      return t + total * 8u;
    default:
      return t + 40u + total * 8u;
  }
}

// RAIL calls this from interrupt context.
void sl_rail_util_on_event(RAIL_Handle_t rail_handle, RAIL_Events_t events)
{
  if (events & RAIL_EVENT_CAL_NEEDED) {
    s_cal_needed = 1;
  }
  if (ll_connected()) {
    ll_on_event(rail_handle, events);
    return;
  }

  g_diag.last_ev_lo = (uint32_t)events;
  g_diag.last_ev_hi = (uint32_t)(events >> 32);
  if (events & RAIL_EVENT_TX_PACKET_SENT) {
    g_diag.adv_tx_sent++;
  }
  if (events & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW)) {
    g_diag.adv_tx_failed++;
  }
  if (events & RAIL_EVENT_RX_FIFO_OVERFLOW) {
    g_diag.rx_overflow++;
  }
  if (events & (RAIL_EVENT_RX_FRAME_ERROR | RAIL_EVENT_RX_PACKET_ABORTED)) {
    g_diag.rx_bad++;
  }
  if (events & RAIL_EVENT_RX_PACKET_RECEIVED) {
    RAIL_RxPacketInfo_t info;
    RAIL_RxPacketHandle_t ph = RAIL_GetRxPacketInfo(rail_handle, RAIL_RX_PACKET_HANDLE_OLDEST_COMPLETE, &info);
    if (ph != RAIL_RX_PACKET_HANDLE_INVALID) {
      if (info.packetStatus == RAIL_RX_PACKET_READY_SUCCESS && info.packetBytes <= sizeof(s_rx) && !s_rx_ready) {
        RAIL_RxPacketDetails_t det;
        RAIL_CopyRxPacket(s_rx, &info);
        s_rx_len = info.packetBytes;
        det.timeReceived.timePosition = RAIL_PACKET_TIME_AT_PACKET_END;
        det.timeReceived.totalPacketBytes = (uint16_t)(info.packetBytes + 3u);
        if (RAIL_GetRxPacketDetails(rail_handle, ph, &det) == RAIL_STATUS_NO_ERROR) {
          s_rx_end_us = to_packet_end(det.timeReceived.timePosition, det.timeReceived.packetTime, info.packetBytes + 3u);
          g_diag.last_rssi = (uint32_t)(int32_t)det.rssi;
        } else {
          s_rx_end_us = RAIL_GetTime();
        }
        s_rx_ready = 1;
      } else {
        g_diag.rx_bad++;
      }
      RAIL_ReleaseRxPacket(rail_handle, ph);
    }
  }
}

static uint32_t build_adv(uint8_t *p)
{
  static const uint8_t name[] = "V300-probe";
  uint32_t n = sizeof(name) - 1;
  uint8_t *q = p;
  *q++ = 0x40;                    // PDU header: ADV_IND, TxAdd = 1 (random address), ChSel = 0 (channel selection algorithm #1)
  *q++ = 0;                       // length, filled in below
  memcpy(q, ADV_ADDR, 6); q += 6;
  *q++ = 0x02; *q++ = 0x01; *q++ = 0x06;          // flags: LE general discoverable, BR/EDR not supported
  *q++ = (uint8_t)(n + 1); *q++ = 0x09;           // complete local name
  memcpy(q, name, n); q += n;
  p[1] = (uint8_t)(q - p - 2);
  return (uint32_t)(q - p);
}

// advertising-mode receive handling
static void handle_rx(void)
{
  if (!s_rx_ready) {
    return;
  }
  uint32_t len = s_rx_len;
  g_diag.rx_good++;
  uint8_t type = s_rx[0] & 0x0F;
  if (type == 0x03) {
    g_diag.scan_req++;
  } else if (type == 0x05) {
    g_diag.connect_ind++;
    // CONNECT_IND: header 2 + InitA 6 + AdvA 6 + LLData 22 = 36 bytes; it must be addressed to our address
    if (len == 36u && memcmp(&s_rx[8], ADV_ADDR, 6) == 0 && ll_start(s_rx, s_rx_end_us)) {
      s_rx_ready = 0;
      return;
    }
    g_diag.connect_ignored++;
  }
  s_rx_ready = 0;
}

static void wait_us(uint32_t us)
{
  uint32_t t0 = RAIL_GetTime();
  while ((uint32_t)(RAIL_GetTime() - t0) < us) {
    handle_rx();
    if (ll_connected()) {
      return;
    }
  }
}

void app_init(void)
{
  memset((void *)&g_diag, 0, sizeof(g_diag));
  g_diag.magic = DIAG_MAGIC;
  g_diag.struct_size = sizeof(g_diag);
  g_diag.end = DIAG_END;
  g_diag.nfields = 0
#define X(n) +1
    DIAG_FIELDS(X)
#undef X
  ;
  g_diag.stage = 1;
  g_diag.knob_sched = 0;   // 0 = polled RAIL_StartRx, 1 = RAIL_ScheduleRx (set over SWD with ble_probe.py --set knob_sched=1)

  h = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
  RAIL_Version_t ver;
  RAIL_GetVersion(&ver, false);
  g_diag.rail_version = ((uint32_t)ver.major << 16) | ((uint32_t)ver.minor << 8) | ver.rev;
  ll_init(h);
  g_diag.stage = 2;
}

void app_process_action(void)
{
  static const uint8_t logical[3]  = { 37, 38, 39 };
  static const uint16_t phys[3]    = { 0, 12, 39 };   // RAIL channel numbers of the three advertising channels
  static uint8_t pdu[40];
  static uint32_t next_us;
  static uint32_t started;
  static uint8_t was_connected;

  g_diag.heartbeat++;

  if (ll_connected()) {
    was_connected = 1;
    ll_poll();
    return;
  }
  uint32_t now = RAIL_GetTime();
  if (was_connected) {            // the link layer just gave the radio back: start advertising again right away
    was_connected = 0;
    next_us = now;
  }
  if (!started) {
    started = 1;
    next_us = now;
  }
  if ((int32_t)(now - next_us) < 0) {
    handle_rx();
    return;
  }

  g_diag.stage = 3;
  uint32_t len = build_adv(pdu);

  for (int i = 0; i < 3 && !ll_connected(); i++) {
    RAIL_Idle(h, RAIL_IDLE_ABORT, true);
    if (s_cal_needed) {
      s_cal_needed = 0;
      g_diag.cal_runs++;
      note(RAIL_Calibrate(h, NULL, RAIL_CAL_ALL_PENDING), 5);
    }
    note(RAIL_BLE_ConfigChannelRadioParams(h, BLE_ADV_CRC, BLE_ADV_AA, logical[i], false), 1);
    uint16_t wr = RAIL_WriteTxFifo(h, pdu, (uint16_t)len, true);
    if (wr != len) {
      note(RAIL_STATUS_INVALID_PARAMETER, 2);
    }
    note(RAIL_StartTx(h, phys[i], RAIL_TX_OPTIONS_DEFAULT, NULL), 3);
    // After the transmission the radio switches to receive on the same channel (RAIL transition TX_SUCCESS -> RX).
    wait_us(1800);
  }
  if (ll_connected()) {
    return;
  }
  RAIL_Idle(h, RAIL_IDLE_ABORT, true);
  g_diag.adv_rounds++;

  // next advertising event after 100 ms plus a little jitter (0 to ~7 ms)
  next_us += 100000u + ((g_diag.adv_rounds * 2654435761u) >> 25) * 50u;
}
