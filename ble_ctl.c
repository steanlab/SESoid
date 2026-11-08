// SES V300 (not badge154): control protocol, see ble_ctl.h
#include <string.h>
#include "ble_ctl.h"
#include "ble_att.h"
#include "ble_slide.h"
#include "ble_panel.h"
#include "ble_sys.h"
#include "ble_up.h"
#include "ble_ll.h"
#include "ble_diag.h"
#include "rail.h"

#define PACE_US 12000u            // minimum time between two notifications of a burst

static bool want_status, want_cfg, want_slots_req;
static uint8_t slots_left;        // next slot to send (1..7), 8 = nothing pending
static uint8_t hw_left;           // next hw fragment (0..3), 4 = nothing pending
static uint32_t last_tx_us;
static uint32_t reboot_at_us;
static bool reboot_pending;
static uint8_t res_op, res_status;
static bool want_result;

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void ctl_reset(void)
{
  want_status = want_cfg = want_slots_req = want_result = false;
  slots_left = 8;
  hw_left = 4;
}

static void queue_result(uint8_t op, uint8_t status)
{
  res_op = op; res_status = status; want_result = true;
}

bool ctl_write(const uint8_t *v, uint8_t len)
{
  if (len == 0 || v[0] < 0x30 || v[0] > 0x3F) return false;
  switch (v[0]) {
    case 0x30: want_status = true; break;
    case 0x31:
      slide_rescan();                                    // the table is read again, then it is sent
      want_slots_req = true;
      break;
    case 0x32: queue_result(0x32, len == 2 ? slide_show(v[1]) : SLR_BAD_SLOT); break;
    case 0x33: queue_result(0x33, slide_next()); break;
    case 0x34:
      if (len == 8) {
        slide_cfg_t c;
        c.enabled = v[1]; c.mode = v[2]; c.interval_min = (uint16_t)(v[3] | (v[4] << 8)); c.mask = v[5];
        c.temp_c = (int8_t)v[6]; c.tx_dbm = (int8_t)v[7];
        slide_set_cfg(&c);
      }
      want_cfg = true;
      break;
    case 0x35: want_cfg = true; break;
    case 0x36:
      if (len == 2 && v[1] == 0xA5) { reboot_pending = true; reboot_at_us = RAIL_GetTime() + 400000u; uint8_t r = 0xB5; att_notify(&r, 1); }
      break;
    case 0x37: hw_left = 0; break;
    case 0x38:
      queue_result(0x38, len == 2 ? (panel_busy() ? SLR_PANEL_BUSY : (!up_idle() ? SLR_FLASH_BUSY : (panel_show_fill(v[1]) ? SLR_OK : SLR_PANEL_BUSY))) : SLR_BAD_SLOT);
      break;
    case 0x39:
      queue_result(0x39, panel_busy() ? SLR_PANEL_BUSY : (panel_show_splash() ? SLR_OK : SLR_PANEL_BUSY));
      break;
    default: break;
  }
  return true;
}

static void send_status(void)
{
  uint8_t r[18];
  r[0] = 0xB0; r[1] = (uint8_t)panel_status(); r[2] = panel_error(); r[3] = (uint8_t)panel_source(); r[4] = panel_shown();
  r[5] = (uint8_t)g_diag.pn_state;
  wr32(r + 6, slide_next_in_s()); wr32(r + 10, slide_uptime_s());
  r[14] = (uint8_t)panel_count(); r[15] = (uint8_t)(panel_count() >> 8);
  r[16] = (uint8_t)g_diag.pn_crc_bad;
  r[17] = (uint8_t)((slide_cfg()->enabled ? 1u : 0u) | (up_idle() ? 2u : 0u) | (slide_scanning() ? 4u : 0u));
  att_notify(r, 18);
}

static void send_cfg(void)
{
  const slide_cfg_t *c = slide_cfg();
  uint8_t r[12];
  int16_t a = slide_tx_actual_ddbm();
  r[0] = 0xB4; r[1] = 1; r[2] = c->enabled; r[3] = c->mode; r[4] = (uint8_t)c->interval_min; r[5] = (uint8_t)(c->interval_min >> 8);
  r[6] = c->mask; r[7] = (uint8_t)c->temp_c; r[8] = (uint8_t)c->tx_dbm; r[9] = (uint8_t)a; r[10] = (uint8_t)((uint16_t)a >> 8);
  att_notify(r, 11);
}

static void send_slot(uint8_t s)
{
  const slot_info_t *i = slide_slot(s);
  uint8_t r[11];
  r[0] = 0xB1; r[1] = s;
  r[2] = (uint8_t)((i->valid ? 1u : 0u) | (i->show ? 2u : 0u) | ((slide_cfg()->mask & (1u << s)) ? 4u : 0u) | (slide_current() == s ? 8u : 0u));
  wr32(r + 3, i->total); wr32(r + 7, i->crc);
  att_notify(r, 11);
}

// hardware information, 4 fragments
//   0: fw major, fw minor, reset cause (4), uptime s (4), part word (4)
//   1: EUI64 (8), flash KB (2), RAM KB (2)                     (the unique ID: the page shows it only on screen)
//   2: flash JEDEC (3), panel chip ID (2), panel error, panel refresh ms (4), updates (2), crc mismatches
//   3: connection interval us (4), connections (2), last RSSI (1, signed dBm), tx actual (2, tenth dBm), watchdog on, log entries used
static void send_hw(uint8_t idx)
{
  uint8_t r[20];
  memset(r, 0, sizeof(r));
  r[0] = 0xB6; r[1] = idx;
  switch (idx) {
    case 0:
      r[2] = FW_VERSION_MAJOR; r[3] = FW_VERSION_MINOR; wr32(r + 4, sys_reset_cause()); wr32(r + 8, slide_uptime_s()); wr32(r + 12, sys_part());
      att_notify(r, 16);
      break;
    case 1: {
      sys_eui64(r + 2);
      uint32_t m = sys_mem_kb();
      r[10] = (uint8_t)m; r[11] = (uint8_t)(m >> 8); r[12] = (uint8_t)(m >> 16); r[13] = (uint8_t)(m >> 24);
      att_notify(r, 14);
      break;
    }
    case 2:
      r[2] = (uint8_t)(g_diag.fl_jedec >> 16); r[3] = (uint8_t)(g_diag.fl_jedec >> 8); r[4] = (uint8_t)g_diag.fl_jedec;
      r[5] = (uint8_t)g_diag.pn_chip; r[6] = (uint8_t)(g_diag.pn_chip >> 8);
      r[7] = panel_error(); wr32(r + 8, g_diag.pn_refresh_ms);
      r[12] = (uint8_t)panel_count(); r[13] = (uint8_t)(panel_count() >> 8); r[14] = (uint8_t)g_diag.pn_crc_bad;
      att_notify(r, 15);
      break;
    default:
      wr32(r + 2, g_diag.interval_us); r[6] = (uint8_t)g_diag.conns; r[7] = (uint8_t)(g_diag.conns >> 8);
      r[8] = (uint8_t)g_diag.last_rssi; r[9] = (uint8_t)slide_tx_actual_ddbm(); r[10] = (uint8_t)((uint16_t)slide_tx_actual_ddbm() >> 8);
      r[11] = (uint8_t)g_diag.sys_wdog; r[12] = (uint8_t)g_diag.sl_log_next;
      att_notify(r, 13);
      break;
  }
}

void ctl_poll(void)
{
  uint32_t now = RAIL_GetTime();
  if (reboot_pending && (int32_t)(now - reboot_at_us) >= 0) sys_reboot();

  uint8_t st, er, sr, sh;
  if (slide_panel_event(&st, &er, &sr, &sh)) {
    uint8_t r[5] = { 0xB7, st, er, sr, sh };
    att_notify(r, 5);
    last_tx_us = now;
  }
  if ((uint32_t)(now - last_tx_us) < PACE_US) return;

  if (want_result) { uint8_t r[3] = { 0xB3, res_op, res_status }; att_notify(r, 3); want_result = false; last_tx_us = now; return; }
  if (want_slots_req && !slide_scanning()) { want_slots_req = false; slots_left = 1; }
  if (slots_left <= 7) { send_slot(slots_left++); last_tx_us = now; return; }
  if (want_cfg) { send_cfg(); want_cfg = false; last_tx_us = now; return; }
  if (hw_left < 4) { send_hw(hw_left++); last_tx_us = now; return; }
  if (want_status) { send_status(); want_status = false; last_tx_us = now; return; }
}
