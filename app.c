// SES V300, stage 1: does the FG22 radio do Bluetooth LE through RAIL?
//
// What this firmware does (nothing else, it touches no flash and no pins):
//   * every ~100 ms sends one connectable advertisement (ADV_IND) named "V300-probe" on channels 37, 38 and 39,
//   * after each advertisement listens ~1.5 ms on the same channel (this is where a SCAN_REQ or a CONNECT_IND from a phone arrives),
//   * keeps counters and the last received packets in the RAM structure g_probe (read it with tools/ble_probe.py).
// Nothing is answered: tapping "connect" in a phone app will time out, the CONNECT_IND is just logged.
#include <stdint.h>
#include <string.h>
#include "sl_rail_util_init.h"
#include "rail.h"
#include "rail_ble.h"

#define PROBE_MAGIC 0x31454C42u   // "BLE1" little endian
#define PROBE_END   0x444E4531u   // "1END"

typedef struct {
  uint32_t magic;
  uint32_t struct_size;
  uint32_t stage;            // 1 = main reached, 2 = RAIL ready, 3 = advertising
  uint32_t supports_ble;     // RAIL_SupportsProtocolBLE()
  uint32_t ble_enabled;      // RAIL_BLE_IsEnabled()
  uint32_t rail_version;     // major<<16 | minor<<8 | rev
  uint32_t last_status;      // last non-zero RAIL status returned by a call
  uint32_t status_where;     // which call it was (see W_*)
  uint32_t adv_rounds;       // advertising rounds (3 channels each)
  uint32_t tx_started;
  uint32_t tx_sent;          // RAIL_EVENT_TX_PACKET_SENT
  uint32_t tx_failed;        // aborted / blocked / underflow
  uint32_t rx_packets;       // good packets copied
  uint32_t rx_bad;           // frame error / aborted / bad CRC
  uint32_t rx_overflow;
  uint32_t cal_runs;
  uint32_t ev_lo, ev_hi;     // OR of all RAIL events seen (to see what happened at all)
  uint32_t last_ev_lo, last_ev_hi;
  uint32_t tx_power_ddbm;    // TX power in 1/10 dBm
  uint32_t wr_len_last;      // bytes accepted by RAIL_WriteTxFifo
  uint32_t scan_req;         // received PDUs of type SCAN_REQ (0x03)
  uint32_t connect_ind;      // received PDUs of type CONNECT_IND (0x05)
  uint32_t last_rx_len;
  int32_t  last_rx_rssi;
  uint8_t  last_rx[48];      // raw bytes as RAIL delivers them
  uint32_t connect_len;
  uint8_t  connect_raw[48];  // raw bytes of the last CONNECT_IND
  uint32_t end;
} probe_t;

enum { W_NONE, W_BLE_PARAMS, W_WRITE, W_STARTTX, W_STARTRX, W_CAL };

volatile probe_t g_probe;

static volatile uint64_t s_events;
static volatile uint8_t  s_rx_ready;
static volatile uint32_t s_rx_len;
static volatile int32_t  s_rx_rssi;
static uint8_t s_rx[64];
static volatile uint8_t s_cal_needed;

#define BLE_ADV_AA   0x8E89BED6u
#define BLE_ADV_CRC  0x555555u

static RAIL_Handle_t h;

// RAIL calls this from interrupt context.
void sl_rail_util_on_event(RAIL_Handle_t rail_handle, RAIL_Events_t events)
{
  s_events |= events;
  g_probe.last_ev_lo = (uint32_t)events;
  g_probe.last_ev_hi = (uint32_t)(events >> 32);
  g_probe.ev_lo |= (uint32_t)events;
  g_probe.ev_hi |= (uint32_t)(events >> 32);

  if (events & RAIL_EVENT_TX_PACKET_SENT) {
    g_probe.tx_sent++;
  }
  if (events & (RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW)) {
    g_probe.tx_failed++;
  }
  if (events & RAIL_EVENT_RX_FIFO_OVERFLOW) {
    g_probe.rx_overflow++;
  }
  if (events & (RAIL_EVENT_RX_FRAME_ERROR | RAIL_EVENT_RX_PACKET_ABORTED)) {
    g_probe.rx_bad++;
  }
  if (events & RAIL_EVENT_CAL_NEEDED) {
    s_cal_needed = 1;
  }
  if (events & RAIL_EVENT_RX_PACKET_RECEIVED) {
    RAIL_RxPacketInfo_t info;
    RAIL_RxPacketHandle_t ph = RAIL_GetRxPacketInfo(rail_handle, RAIL_RX_PACKET_HANDLE_OLDEST_COMPLETE, &info);
    if (ph != RAIL_RX_PACKET_HANDLE_INVALID) {
      if (info.packetStatus == RAIL_RX_PACKET_READY_SUCCESS && info.packetBytes <= sizeof(s_rx) && !s_rx_ready) {
        RAIL_RxPacketDetails_t det;
        RAIL_CopyRxPacket(s_rx, &info);
        s_rx_len = info.packetBytes;
        s_rx_rssi = 0;
        if (RAIL_GetRxPacketDetails(rail_handle, ph, &det) == RAIL_STATUS_NO_ERROR) {
          s_rx_rssi = det.rssi;
        }
        s_rx_ready = 1;
      } else {
        g_probe.rx_bad++;
      }
      RAIL_ReleaseRxPacket(rail_handle, ph);
    }
  }
}

static void note(RAIL_Status_t st, uint32_t where)
{
  if (st != RAIL_STATUS_NO_ERROR) {
    g_probe.last_status = (uint32_t)st;
    g_probe.status_where = where;
  }
}

static uint32_t build_adv(uint8_t *p)
{
  static const uint8_t name[] = "V300-probe";
  uint32_t n = sizeof(name) - 1;
  uint8_t *q = p;
  // advertising PDU header: type ADV_IND (0), TxAdd = 1 (random address)
  *q++ = 0x40;
  *q++ = 0;                       // length, filled in below
  // AdvA, static random address C3:DE:00:30:00:01, LSB first
  *q++ = 0x01; *q++ = 0x00; *q++ = 0x30; *q++ = 0x00; *q++ = 0xDE; *q++ = 0xC3;
  // AD: flags = LE general discoverable, BR/EDR not supported
  *q++ = 0x02; *q++ = 0x01; *q++ = 0x06;
  // AD: complete local name
  *q++ = (uint8_t)(n + 1); *q++ = 0x09;
  memcpy(q, name, n); q += n;
  p[1] = (uint8_t)(q - p - 2);
  return (uint32_t)(q - p);
}

static void handle_rx(void)
{
  if (!s_rx_ready) {
    return;
  }
  uint32_t len = s_rx_len;
  g_probe.rx_packets++;
  g_probe.last_rx_len = len;
  g_probe.last_rx_rssi = s_rx_rssi;
  for (uint32_t i = 0; i < sizeof(g_probe.last_rx); i++) {
    g_probe.last_rx[i] = (i < len) ? s_rx[i] : 0;
  }
  // PDU type is the low nibble of the first header byte (if RAIL delivers the header first, as it should in BLE mode)
  uint8_t type = s_rx[0] & 0x0F;
  if (type == 0x03) {
    g_probe.scan_req++;
  } else if (type == 0x05) {
    g_probe.connect_ind++;
    g_probe.connect_len = len;
    for (uint32_t i = 0; i < sizeof(g_probe.connect_raw); i++) {
      g_probe.connect_raw[i] = (i < len) ? s_rx[i] : 0;
    }
  }
  s_rx_ready = 0;
}

static void wait_us(uint32_t us)
{
  uint32_t t0 = RAIL_GetTime();
  while ((uint32_t)(RAIL_GetTime() - t0) < us) {
    handle_rx();
  }
}

void app_init(void)
{
  memset((void *)&g_probe, 0, sizeof(g_probe));
  g_probe.magic = PROBE_MAGIC;
  g_probe.struct_size = sizeof(g_probe);
  g_probe.end = PROBE_END;
  g_probe.stage = 1;

  h = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
  RAIL_Version_t ver;
  RAIL_GetVersion(&ver, false);
  g_probe.rail_version = ((uint32_t)ver.major << 16) | ((uint32_t)ver.minor << 8) | ver.rev;
  g_probe.supports_ble = RAIL_SupportsProtocolBLE(h) ? 1 : 0;
  g_probe.ble_enabled = RAIL_BLE_IsEnabled(h) ? 1 : 0;
  g_probe.tx_power_ddbm = (uint32_t)RAIL_GetTxPowerDbm(h);
  g_probe.stage = 2;
}

void app_process_action(void)
{
  static const uint8_t logical[3]  = { 37, 38, 39 };
  static const uint16_t phys[3]    = { 0, 12, 39 };   // RAIL channel numbers of the three advertising channels
  static uint8_t pdu[40];
  static uint32_t next_us;
  static uint32_t started;
  uint32_t now = RAIL_GetTime();

  if (!started) {
    started = 1;
    next_us = now;
  }
  if ((int32_t)(now - next_us) < 0) {
    handle_rx();
    return;
  }

  g_probe.stage = 3;
  uint32_t len = build_adv(pdu);

  for (int i = 0; i < 3; i++) {
    RAIL_Idle(h, RAIL_IDLE_ABORT, true);
    if (s_cal_needed) {
      s_cal_needed = 0;
      g_probe.cal_runs++;
      note(RAIL_Calibrate(h, NULL, RAIL_CAL_ALL_PENDING), W_CAL);
    }
    note(RAIL_BLE_ConfigChannelRadioParams(h, BLE_ADV_CRC, BLE_ADV_AA, logical[i], false), W_BLE_PARAMS);
    uint16_t wr = RAIL_WriteTxFifo(h, pdu, (uint16_t)len, true);
    g_probe.wr_len_last = wr;
    if (wr != len) {
      note(RAIL_STATUS_INVALID_PARAMETER, W_WRITE);
    }
    RAIL_Status_t st = RAIL_StartTx(h, phys[i], RAIL_TX_OPTIONS_DEFAULT, NULL);
    note(st, W_STARTTX);
    if (st == RAIL_STATUS_NO_ERROR) {
      g_probe.tx_started++;
    }
    // After the transmission the radio switches to receive on the same channel (RAIL transition TX_SUCCESS -> RX).
    wait_us(1800);
  }
  RAIL_Idle(h, RAIL_IDLE_ABORT, true);
  g_probe.adv_rounds++;

  // next advertising event after 100 ms plus a little jitter (0 to ~7 ms) so that we do not stay in step with another device
  next_us += 100000u + ((g_probe.adv_rounds * 2654435761u) >> 25) * 50u;
}
