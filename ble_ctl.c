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
#include "ble_hwtest.h"
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
static bool want_diag, diag_sent;
static bool want_clock, clock_sent, want_pins, want_scan;
static uint32_t last_pins_tx_us, last_scan_tx_us;

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void ctl_reset(void)
{
  want_status = want_cfg = want_slots_req = want_result = want_diag = false;
  want_clock = want_pins = want_scan = false;
  hwtest_reset();                                        // the pin monitor stops with the connection
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
    case 0x3A:
      if (panel_busy()) queue_result(0x3A, SLR_PANEL_BUSY);
      else if (panel_diag_start()) { diag_sent = false; queue_result(0x3A, SLR_OK); }
      else queue_result(0x3A, SLR_PANEL_BUSY);
      break;
    case 0x3B: want_diag = true; break;
    case 0x3C: clock_sent = false; queue_result(0x3C, hwtest_clock_start() ? SLR_OK : SLR_PANEL_BUSY); break;
    case 0x3D: want_clock = true; break;
    case 0x3E:
      if ((len == 2 || len == 6) && hwtest_pins_start(v[1], len == 6 ? (uint32_t)(v[2] | (v[3] << 8) | ((uint32_t)v[4] << 16) | ((uint32_t)v[5] << 24)) : 0xFFFFFFFFu)) want_pins = true;
      else queue_result(0x3E, SLR_BAD_SLOT);
      break;
    case 0x3F:
      if (len == 7 && v[1] == 2) queue_result(0x3F, hwtest_scan_start(v[2], (uint32_t)(v[3] | (v[4] << 8) | ((uint32_t)v[5] << 16) | ((uint32_t)v[6] << 24))) ? SLR_OK : SLR_BAD_SLOT);
      else if (len == 11 && v[1] == 5) queue_result(0x3F, hwtest_scan_start_pwr(v[2], (uint32_t)(v[3] | (v[4] << 8) | ((uint32_t)v[5] << 16) | ((uint32_t)v[6] << 24)), (uint32_t)(v[7] | (v[8] << 8) | ((uint32_t)v[9] << 16) | ((uint32_t)v[10] << 24))) ? SLR_OK : SLR_BAD_SLOT);
      else if (len == 2 && v[1] == 3) want_scan = true;
      else if (len == 2 && v[1] == 4) { hwtest_scan_abort(); want_scan = true; }
      else { if (len == 2 && v[1] == 1) hwtest_pins_clear(); want_pins = true; }
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

// panel diagnostics report (see panel_diag_t in ble_panel.h)
static void send_diag(void)
{
  const panel_diag_t *d = panel_diag();
  uint8_t r[20];
  memset(r, 0, sizeof(r));
  r[0] = 0xB8; r[1] = d->state;
  r[2] = d->busy_off; r[3] = d->sda_pd_off; r[4] = d->sda_pu_off; r[5] = d->busy_on; r[6] = d->sda_pd_on; r[7] = d->sda_pu_on;
  r[8] = d->busy_rstlow; r[9] = d->busy_10; r[10] = d->busy_100; r[11] = d->busy_500;
  r[12] = (uint8_t)d->busy_ms; r[13] = (uint8_t)(d->busy_ms >> 8);
  r[14] = (uint8_t)d->id1; r[15] = (uint8_t)(d->id1 >> 8);
  r[16] = (uint8_t)d->id2; r[17] = (uint8_t)(d->id2 >> 8);
  r[18] = (uint8_t)d->id_slow; r[19] = (uint8_t)(d->id_slow >> 8);
  att_notify(r, 20);
}

// clock test report (see ble_hwtest.h): B9 state flags sel ctune_xi ctune_xo fact_xi fact_xo lfxo_ms(2) cnt_lfxo(3) cnt_lfrco(3) cnt_ulfrco(3) captune
static void send_clock(void)
{
  const hwtest_clock_t *c = hwtest_clock();
  uint8_t r[20];
  memset(r, 0, sizeof(r));
  r[0] = 0xB9; r[1] = c->state; r[2] = c->flags; r[3] = c->sel;
  r[4] = c->ctune_xi; r[5] = c->ctune_xo; r[6] = c->fact_xi; r[7] = c->fact_xo;
  r[8] = (uint8_t)c->lfxo_ms; r[9] = (uint8_t)(c->lfxo_ms >> 8);
  r[10] = (uint8_t)c->cnt_lfxo; r[11] = (uint8_t)(c->cnt_lfxo >> 8); r[12] = (uint8_t)(c->cnt_lfxo >> 16);
  r[13] = (uint8_t)c->cnt_lfrco; r[14] = (uint8_t)(c->cnt_lfrco >> 8); r[15] = (uint8_t)(c->cnt_lfrco >> 16);
  r[16] = (uint8_t)c->cnt_ulfrco; r[17] = (uint8_t)(c->cnt_ulfrco >> 8); r[18] = (uint8_t)(c->cnt_ulfrco >> 16);
  r[19] = c->captune;
  att_notify(r, 20);
}

// pin monitor report: BA mode elapsed_s(2) level(4) seen_hi(4) seen_lo(4) edges(2) last_pin 0
static void send_pins(void)
{
  const hwtest_pins_t *p = hwtest_pins();
  uint8_t r[20];
  memset(r, 0, sizeof(r));
  r[0] = 0xBA; r[1] = p->mode; r[2] = (uint8_t)p->elapsed_s; r[3] = (uint8_t)(p->elapsed_s >> 8);
  wr32(r + 4, p->level); wr32(r + 8, p->seen_hi); wr32(r + 12, p->seen_lo);
  r[16] = (uint8_t)p->edges; r[17] = (uint8_t)(p->edges >> 8); r[18] = p->last;
  att_notify(r, 20);
}

// I2C scanner report: BB state opt pairs_total pairs_done stuck hits (sda scl addr)x3 high(4)
static void send_scan(void)
{
  const hwtest_scan_t *c = hwtest_scan();
  uint8_t r[20];
  memset(r, 0, sizeof(r));
  r[0] = 0xBB; r[1] = (uint8_t)(c->state | (c->opt << 4)); r[2] = c->pairs_total; r[3] = c->pairs_done; r[4] = c->stuck; r[5] = c->hits;
  r[6] = c->hit[0].sda; r[7] = c->hit[0].scl; r[8] = c->hit[0].addr; r[9] = c->hit[0].pwr; r[10] = c->pwr_now;
  wr32(r + 12, c->high);
  wr32(r + 16, c->held);
  att_notify(r, 20);
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
  hwtest_poll();
  if (reboot_pending && (int32_t)(now - reboot_at_us) >= 0) sys_reboot();

  uint8_t st, er, sr, sh;
  if (slide_panel_event(&st, &er, &sr, &sh)) {
    uint8_t r[5] = { 0xB7, st, er, sr, sh };
    att_notify(r, 5);
    last_tx_us = now;
  }
  if ((uint32_t)(now - last_tx_us) < PACE_US) return;

  if (panel_diag()->state == 2 && !diag_sent) { diag_sent = true; want_diag = true; }      // the report goes out by itself when the run ends
  if (hwtest_clock()->state == 2 && !clock_sent) { clock_sent = true; want_clock = true; }   // the clock report goes out by itself when the test ends
  if (hwtest_pins_changed()) want_pins = true;
  if (hwtest_pins()->mode != PM_MODE_OFF && (uint32_t)(now - last_pins_tx_us) >= 1000000u) want_pins = true;
  if (want_clock && hwtest_clock()->state != 1) { send_clock(); want_clock = false; last_tx_us = now; return; }
  if (hwtest_scan_changed()) want_scan = true;
  if (hwtest_scan()->state == 1 && (uint32_t)(now - last_scan_tx_us) >= 500000u) want_scan = true;
  if (want_scan) { send_scan(); want_scan = false; last_scan_tx_us = now; last_tx_us = now; return; }
  if (want_pins) { send_pins(); want_pins = false; last_pins_tx_us = now; last_tx_us = now; return; }
  if (want_diag) { send_diag(); want_diag = false; last_tx_us = now; return; }
  if (want_result) { uint8_t r[3] = { 0xB3, res_op, res_status }; att_notify(r, 3); want_result = false; last_tx_us = now; return; }
  if (want_slots_req && !slide_scanning()) { want_slots_req = false; slots_left = 1; }
  if (slots_left <= 7) { send_slot(slots_left++); last_tx_us = now; return; }
  if (want_cfg) { send_cfg(); want_cfg = false; last_tx_us = now; return; }
  if (hw_left < 4) { send_hw(hw_left++); last_tx_us = now; return; }
  if (want_status) { send_status(); want_status = false; last_tx_us = now; return; }
}
