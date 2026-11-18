// SES V300 (not badge154): slideshow logic, see ble_slide.h
#include <string.h>
#include "ble_slide.h"
#include "ble_batt.h"
#include "ble_panel.h"
#include "ble_demo.h"
#include "ble_hwtest.h"
#include "ble_sys.h"
#include "ble_flash.h"
#include "ble_up.h"
#include "ble_ll.h"
#include "ble_diag.h"
#include "rail.h"
#include "sl_rail_util_init.h"

#define LOG_ENTRIES 16u
#define ENTRY_SIZE  256u
#define ENTRY_MAGIC 0x31534553u            // "SES1"

static slide_cfg_t cfg;
static slot_info_t slots[FL_SLOTS + 1];     // index 1..10
static uint8_t cur_slot;
static uint8_t clean_seq[5];               // fill colour codes of the running cleaning sequence
static uint8_t clean_len, clean_pos;       // sequence length / next step (clean_len 0 = none running)
static bool demo_tour;                    // a demo tour is running: the screens follow each other, the slideshow waits
static uint8_t demo_steps[DEMO_COUNT + 5]; // the tour: DS_* or a picture number 0..DEMO_COUNT-1
static uint8_t demo_n, demo_pos;           // steps / next step
static uint8_t demo_restore;               // the slot that was shown before the tour (0 = none: the next slide of the playlist)
static uint32_t demo_gap_s;                // the next picture of the tour is shown at this uptime
#define DS_SPLASH 0xF0u
#define DS_SLOT   0xF1u
#define DS_HW     0xF2u
#define DS_BACK   0xF3u
static uint8_t demo_slot;                  // the slide shown in the tour (0 = none)
static uint8_t clean_slot;                 // a white full-screen update is running; this slide follows it (0 = none)
static uint8_t changes;                    // slide changes since the last full-screen update
static uint32_t log_next;                  // index of the first free log entry (LOG_ENTRIES = full)
static bool persist_pending;

typedef enum { P_IDLE, P_ERASE, P_PROG } pstate_t;
static pstate_t ps;
static uint8_t entry[ENTRY_SIZE];
static uint8_t secret[8];                  // per-tag random secret (see ble_acc.h)
static bool have_secret;

static bool scan_wanted;
static uint8_t scan_idx;                   // 1..10 while scanning, 0 = not scanning

static uint32_t last_us, acc_us, uptime_s;
static uint32_t due_s;                     // uptime at which the next automatic change happens (0xFFFFFFFF = none)
static bool was_busy;
static bool event_pending;
static uint8_t ev_status, ev_err, ev_src, ev_shown;
static bool was_up_idle = true;
static uint32_t rng = 0x2545F491u;
static bool tx_apply_due;
static int16_t tx_actual;

static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void set_defaults(slide_cfg_t *c)
{
  c->enabled = 1;
  c->mode = 0;
  c->interval_min = 60;
  c->mask = 0x7FE;                         // slots 1..10
  c->clean_n = 10;
  c->clean_mode = 0;
  c->batt_low = 70;
  c->batt_crit = 50;
  c->flags = 0;
  c->temp_c = 25;
  c->tx_dbm = 6;
  c->nfc = 0;
}

static void sanitize(slide_cfg_t *c)
{
  c->enabled = c->enabled ? 1u : 0u;
  c->mode = c->mode ? 1u : 0u;
  c->nfc = c->nfc ? 1u : 0u;
  if (c->interval_min < 1u) c->interval_min = 1u;
  if (c->interval_min > 1440u) c->interval_min = 1440u;
  c->mask &= 0x7FEu;
  if (c->clean_mode > 2u) c->clean_mode = 0u;
  if (c->batt_low < 10u || c->batt_low > 200u) c->batt_low = 70u;
  if (c->batt_crit < 5u || c->batt_crit > c->batt_low) c->batt_crit = c->batt_low < 50u ? c->batt_low : 50u;
  c->flags &= 15u;
  if (c->clean_n > 250u) c->clean_n = 250u;
  if (c->temp_c < -20) c->temp_c = -20;
  if (c->temp_c > 60) c->temp_c = 60;
  if (c->tx_dbm < -30) c->tx_dbm = -30;
  if (c->tx_dbm > 20) c->tx_dbm = 20;
}

// ---- settings log ----------------------------------------------------------------------------------------------------
static void pack_entry(uint8_t *e, const slide_cfg_t *c)
{
  memset(e, 0xFF, ENTRY_SIZE);
  wr32(e, ENTRY_MAGIC);
  e[4] = 4;                                // layout version
  e[5] = c->enabled; e[6] = c->mode; e[7] = (uint8_t)c->mask;
  e[8] = (uint8_t)c->interval_min; e[9] = (uint8_t)(c->interval_min >> 8);
  e[10] = (uint8_t)c->temp_c; e[11] = (uint8_t)c->tx_dbm;
  e[12] = c->nfc;                          // was reserved (0) in older entries, which therefore read as "off"
  e[13] = (uint8_t)(c->mask >> 8); e[14] = c->clean_n; e[15] = c->clean_mode;
  e[16] = c->batt_low; e[17] = c->batt_crit; e[18] = c->flags;
  memcpy(e + 24, secret, 8);
  wr32(e + 32, up_crc32(0, e, 32));
}

static bool unpack_entry(const uint8_t *e, slide_cfg_t *c)
{
  if (rd32(e) != ENTRY_MAGIC || e[4] != 4 || rd32(e + 32) != up_crc32(0, e, 32)) return false;
  memcpy(secret, e + 24, 8); have_secret = true;
  c->enabled = e[5]; c->mode = e[6]; c->mask = (uint16_t)(e[7] | (e[13] << 8)); c->clean_n = e[14]; c->clean_mode = e[15]; c->batt_low = e[16]; c->batt_crit = e[17]; c->flags = e[18] == 0xFFu ? 0u : e[18];     // 0xFF = an entry written before the flags existed
  c->interval_min = (uint16_t)(e[8] | (e[9] << 8));
  c->temp_c = (int8_t)e[10]; c->tx_dbm = (int8_t)e[11]; c->nfc = e[12];
  sanitize(c);
  return true;
}

static void load_settings(void)
{
  uint8_t e[40];
  set_defaults(&cfg);
  have_secret = false;
  log_next = LOG_ENTRIES;
  bool free_found = false;
  for (uint32_t i = 0; i < LOG_ENTRIES; i++) {
    int guard = 0;
    while (!fl_read_page(FL_CFG_SECTOR + i * ENTRY_SIZE, e, sizeof(e)) && guard++ < 1000) { }
    if (e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF) {
      if (!free_found) { log_next = i; free_found = true; }
      continue;
    }
    slide_cfg_t c;
    if (unpack_entry(e, &c)) cfg = c;
  }
  if (!have_secret) {                      // first start (or the settings flash was erased): make a new secret and save it with the settings
    RAIL_Handle_t rh = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
    uint16_t got = RAIL_GetRadioEntropy(rh, secret, sizeof(secret));
    uint32_t t = RAIL_GetTime();
    for (uint16_t i = got; i < sizeof(secret); i++) { t = t * 1664525u + 1013904223u; secret[i] = (uint8_t)(t >> 24); }
    have_secret = true;
    persist_pending = true;
  }
  g_diag.sl_log_next = log_next;
}

const uint8_t *slide_secret(void) { return secret; }

// persist: find the next free entry (erase the sector first when the log is full), then program one page
static void persist_poll(void)
{
  if (ps == P_IDLE) {
    if (!persist_pending || !up_idle() || fl_busy() || panel_flash_busy()) return;
    fl_set_range(FL_CFG_SECTOR, FL_CFG_SECTOR + FL_SECTOR);
    if (log_next >= LOG_ENTRIES) {
      if (!fl_start_erase(FL_CFG_SECTOR)) { fl_set_range(FL_TEST_LO, FL_TEST_END); return; }
      ps = P_ERASE;
    } else {
      pack_entry(entry, &cfg);
      if (!fl_start_program(FL_CFG_SECTOR + log_next * ENTRY_SIZE, entry)) { fl_set_range(FL_TEST_LO, FL_TEST_END); return; }
      persist_pending = false;             // the values that go into the entry are the ones in `cfg` right now
      ps = P_PROG;
    }
    return;
  }
  fl_poll();
  if (fl_busy() || !fl_done) return;
  fl_done = false;
  if (ps == P_ERASE) {
    if (fl_res.result == FLR_OK) log_next = 0;
    ps = P_IDLE;                           // the program step follows on the next call (persist_pending is still set)
    if (fl_res.result != FLR_OK) persist_pending = false;
  } else {
    if (fl_res.result == FLR_OK) { log_next++; g_diag.sl_saves++; }
    else if (fl_res.result == FLR_VERIFY) { log_next++; persist_pending = false; }       // a half-written entry cannot be written again: skip it (it is ignored on load), the next save uses the next one
    ps = P_IDLE;
  }
  g_diag.sl_log_next = log_next;
  fl_set_range(FL_TEST_LO, FL_TEST_END);
}

bool slide_deep_ok(void) { return (cfg.flags & 1u) && !clean_len && ps == P_IDLE && !scan_wanted && !persist_pending; }
bool slide_flash_busy(void) { return ps != P_IDLE; }

// ---- slot table ------------------------------------------------------------------------------------------------------
static void scan_poll(void)
{
  if (!scan_idx) {
    if (!scan_wanted || !up_idle() || ps != P_IDLE || fl_busy() || panel_flash_busy()) return;
    scan_wanted = false;
    scan_idx = 1;
  }
  uint8_t r[16];
  if (!fl_read_page(FL_SLOT_BASE(scan_idx) + FL_SLOT_REC, r, sizeof(r))) return;
  slot_info_t *s = &slots[scan_idx];
  s->valid = (memcmp(r, "SLD1", 4) == 0) && rd32(r + 4) != 0 && rd32(r + 4) <= FL_SLOT_REC;
  s->total = s->valid ? rd32(r + 4) : 0;
  s->crc = s->valid ? rd32(r + 8) : 0;
  s->show = s->valid && s->total == SLIDE_FRAME_BYTES;
  if (++scan_idx > FL_SLOTS) {
    scan_idx = 0;
    uint32_t vm = 0;
    for (int i = 1; i <= (int)FL_SLOTS; i++) if (slots[i].show) vm |= 1u << i;
    g_diag.sl_valid = vm;
  }
}

void slide_rescan(void) { scan_wanted = true; }
bool slide_scanning(void) { return scan_wanted || scan_idx != 0; }
const slot_info_t *slide_slot(uint8_t slot) { return &slots[slot < 1 || slot > FL_SLOTS ? 0 : slot]; }
uint8_t slide_current(void) { return cur_slot; }
uint32_t slide_uptime_s(void) { return uptime_s; }
const slide_cfg_t *slide_cfg(void) { return &cfg; }

// ---- TX power ---------------------------------------------------------------------------------------------------------
void slide_apply_tx(void)
{
  RAIL_Handle_t h = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
  RAIL_Status_t st = RAIL_SetTxPowerDbm(h, (RAIL_TxPower_t)((int)cfg.tx_dbm * 10));
  tx_actual = (int16_t)RAIL_GetTxPowerDbm(h);
  g_diag.sl_tx_ddbm = (uint32_t)(int32_t)tx_actual;
  g_diag.sl_tx_status = (uint32_t)st;
  tx_apply_due = false;
}
int16_t slide_tx_actual_ddbm(void) { return tx_actual; }

void slide_set_cfg(const slide_cfg_t *c)
{
  slide_cfg_t n = *c;
  sanitize(&n);
  bool tx_changed = n.tx_dbm != cfg.tx_dbm;
  bool changed = memcmp(&n, &cfg, sizeof(n)) != 0;
  cfg = n;
  panel_set_temp(cfg.temp_c);
  panel_set_auto_temp((cfg.flags & 4u) != 0);
  panel_set_rotate((cfg.flags & 8u) != 0);
  batt_set_thresholds((uint16_t)(2000u + 10u * cfg.batt_low), (uint16_t)(2000u + 10u * cfg.batt_crit));
  if (tx_changed) tx_apply_due = true;
  if (changed) persist_pending = true;
  // new interval / on-off: restart the countdown from now
  due_s = cfg.enabled ? uptime_s + (uint32_t)cfg.interval_min * 60u : 0xFFFFFFFFu;
  g_diag.sl_cfgmask = cfg.mask;
}

// ---- playlist ----------------------------------------------------------------------------------------------------------
static uint32_t rnd(void)
{
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

static uint8_t pick_next(void)
{
  uint8_t cand[FL_SLOTS], n = 0;
  for (uint8_t s = 1; s <= FL_SLOTS; s++) if (slots[s].show && (cfg.mask & (1u << s))) cand[n++] = s;
  if (n == 0) return 0;
  if (cfg.mode) {
    if (n == 1) return cand[0];
    uint8_t k;
    do { k = cand[rnd() % n]; } while (k == cur_slot);
    return k;
  }
  for (uint8_t s = (uint8_t)(cur_slot + 1u); s <= FL_SLOTS; s++) if (slots[s].show && (cfg.mask & (1u << s))) return s;
  return cand[0];
}

static uint8_t start_show(uint8_t slot)
{
  if (slot < 1 || slot > FL_SLOTS || !slots[slot].show) return SLR_BAD_SLOT;
  if (panel_busy()) return SLR_PANEL_BUSY;
  if (!up_idle() || ps != P_IDLE || fl_busy() || slide_scanning()) return SLR_FLASH_BUSY;
  if (!panel_show_slot(slot, slots[slot].crc)) return SLR_PANEL_BUSY;
  cur_slot = slot;
  g_diag.sl_cur = slot;
  return SLR_OK;
}

uint8_t slide_show(uint8_t slot) { return start_show(slot); }

// ---- the TAG HARDWARE screen: the facts and the current readings, as text for ble_demo.c
static char *ap(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *apn(char *p, uint32_t v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v); while (n) *p++ = t[--n]; return p; }
static char *ap1(char *p, int32_t tenths) { if (tenths < 0) { *p++ = '-'; tenths = -tenths; } p = apn(p, (uint32_t)tenths / 10u); *p++ = '.'; *p++ = (char)('0' + (uint32_t)tenths % 10u); return p; }
static char *apd(char *p, int32_t v) { if (v < 0) { *p++ = '-'; v = -v; } return apn(p, (uint32_t)v); }
static void demo_hw_fill(void)
{
  char b[48], *p;
  demo_hw_clear();
  demo_hw_set(0, "PROCESSOR", "EFR32FG22C121F256GM40, M33 38.4 MHz");
  demo_hw_set(1, "PANEL", "5.9 in, 720 x 256, BWRY, PDi 0x0D04");
  { int8_t pt; int16_t dc = sys_temp_dc();
    p = ap(b, "panel "); if (panel_temp_sensor(&pt)) { p = apd(p, pt); p = ap(p, " C"); } else p = ap(p, "n/a");
    p = ap(p, ", chip "); if (dc != SYS_TEMP_NA) { p = ap1(p, dc); p = ap(p, " C"); } else p = ap(p, "n/a");
    *p = 0; demo_hw_set(2, "TEMPERATURE", b); }
  p = ap(b, "5 x CR2450, ");
  if (batt_mv()) { uint32_t mv = batt_mv(); p = apn(p, mv / 1000u); *p++ = '.'; *p++ = (char)('0' + mv / 100u % 10u); *p++ = (char)('0' + mv / 10u % 10u); p = ap(p, " V, "); p = ap(p, batt_level() == BATT_OK ? "OK" : batt_level() == BATT_LOW ? "low" : "critical"); }
  else p = ap(p, "not measured");
  *p = 0; demo_hw_set(3, "BATTERY", b);
  { uint32_t n = 0; for (uint8_t i = 1; i <= FL_SLOTS; i++) if (slots[i].show) n++;
    p = ap(b, "Winbond EF 40 13, 512 KiB, slides "); p = apn(p, n); p = ap(p, "/"); p = apn(p, FL_SLOTS); *p = 0; demo_hw_set(4, "FLASH CHIP", b); }
  { const hwtest_clock_t *k = hwtest_clock();
    p = ap(b, "38.4 MHz, 32.768 kHz");
    if (k->state == 2 && (k->flags & 0x10u) && k->cnt_lfxo > 500000u && k->cnt_lfxo < 700000u) { int32_t ppm = (int32_t)(((int64_t)(600000 - (int32_t)k->cnt_lfxo) * 1000000) / 600000); p = ap(p, " ("); if (ppm >= 0) *p++ = '+'; p = apd(p, ppm); p = ap(p, " ppm)"); }
    *p = 0; demo_hw_set(5, "CRYSTALS", b); }
  { int32_t rs = (int32_t)g_diag.last_rssi;
    p = ap(b, "BLE 2.4 GHz, TX "); p = ap1(p, slide_tx_actual_ddbm()); p = ap(p, " dBm");
    if (rs < 0 && rs > -128) { p = ap(p, ", RSSI "); p = apd(p, rs); p = ap(p, " dBm"); }
    *p = 0; demo_hw_set(6, "RADIO", b); }
  demo_hw_set(7, "NFC", "NTAG I2C plus, NDEF 112 bytes");
  { uint32_t up = uptime_s;
    p = ap(b, "SESoid "); p = apn(p, FW_VERSION_MAJOR); *p++ = '.'; p = apn(p, FW_VERSION_MINOR); p = ap(p, ", up "); 
    if (up >= 3600u) { p = apn(p, up / 3600u); p = ap(p, " h "); }
    p = apn(p, up / 60u % 60u); p = ap(p, " min, "); p = apn(p, panel_count()); p = ap(p, " screens"); *p = 0; demo_hw_set(8, "FIRMWARE", b); }
}

bool slide_demo_active(void) { return demo_tour; }

static bool demo_idle_ok(void) { return !panel_busy() && up_idle() && ps == P_IDLE && !fl_busy() && !slide_scanning() && !clean_len && !clean_slot; }

// n: 0 = stop (the tour ends after the current screen with the slide that was shown before), 1..DEMO_COUNT = that picture, 0xFE = the hardware screen,
// 0xFF = tour: splash (mask bit 12), the pictures of mask (bit i = picture i + 1), the slide `slot` (0 = none), the hardware screen (mask bit 13), back to the earlier slide
uint8_t slide_demo(uint8_t n, uint16_t mask, uint8_t slot)
{
  if (n == 0) { if (demo_tour && demo_pos < demo_n) demo_pos = (uint8_t)(demo_n - 1u); return SLR_OK; }     // the last step puts the earlier slide back
  if (n != 0xFFu && n != 0xFEu && n > DEMO_COUNT) return SLR_BAD_SLOT;
  if (panel_busy()) return SLR_PANEL_BUSY;
  if (!demo_idle_ok()) return SLR_FLASH_BUSY;
  if (batt_level() == BATT_CRIT) return SLR_BAD_SLOT;
  if (n == 0xFFu) {
    demo_n = 0;
    if (mask & 0x1000u) demo_steps[demo_n++] = DS_SPLASH;
    for (uint8_t i = 0; i < DEMO_COUNT; i++) if (mask & (1u << i)) demo_steps[demo_n++] = i;
    demo_slot = (slot >= 1 && slot <= FL_SLOTS && slots[slot].show) ? slot : 0;
    if (demo_slot) demo_steps[demo_n++] = DS_SLOT;
    if (mask & 0x2000u) demo_steps[demo_n++] = DS_HW;
    if (demo_n == 0) return SLR_BAD_SLOT;                 // nothing chosen
    demo_steps[demo_n++] = DS_BACK;
    demo_pos = 0; demo_restore = cur_slot; demo_gap_s = uptime_s + 10u; demo_tour = true;      // the first screen comes 10 s after the command: the page disconnects meanwhile
    (void)hwtest_clock_start();                         // the crystal figures for the hardware screen
    return SLR_OK;                                      // the first step starts from slide_poll
  }
  if (n == 0xFEu) { demo_hw_fill(); return panel_show_demo(DEMO_PAT_HW) ? SLR_OK : SLR_PANEL_BUSY; }
  demo_tour = false;
  return panel_show_demo((uint8_t)(n - 1u)) ? SLR_OK : SLR_PANEL_BUSY;
}

uint8_t slide_clean(uint8_t mode)
{
  if (mode > 2u) mode = 0u;
  if (panel_busy()) return SLR_PANEL_BUSY;
  if (!up_idle() || ps != P_IDLE || fl_busy() || slide_scanning() || clean_len) return SLR_FLASH_BUSY;
  static const uint8_t seq[3][5] = { {1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 1, 3, 2, 1} };       // 0 black, 1 white, 2 yellow, 3 red
  static const uint8_t len[3] = { 1, 2, 5 };
  for (uint8_t i = 0; i < 5; i++) clean_seq[i] = seq[mode][i];
  if (!panel_show_fill(clean_seq[0])) return SLR_PANEL_BUSY;
  clean_len = len[mode]; clean_pos = 1;
  changes = 0;
  return SLR_OK;
}

uint8_t slide_next(void)
{
  uint8_t s = pick_next();
  if (!s) {                                // nothing in the playlist: fall back to any showable slot
    for (uint8_t i = 1; i <= FL_SLOTS && !s; i++) if (slots[i].show) s = i;
  }
  if (!s) return SLR_BAD_SLOT;
  if (cfg.clean_n && batt_level() != BATT_CRIT && (uint8_t)(changes + 1u) >= cfg.clean_n) {          // every N-th change goes through a cleaning sequence first
    uint8_t r = slide_clean(cfg.clean_mode);
    if (r != SLR_OK) return r;
    clean_slot = s;
    return SLR_OK;
  }
  uint8_t r = start_show(s);
  if (r == SLR_OK && changes < 255u) changes++;
  return r;
}

uint32_t slide_next_in_s(void)
{
  if (!cfg.enabled || due_s == 0xFFFFFFFFu) return 0xFFFFFFFFu;
  return (int32_t)(due_s - uptime_s) > 0 ? due_s - uptime_s : 0u;
}

bool slide_panel_event(uint8_t *status, uint8_t *err, uint8_t *src, uint8_t *shown)
{
  if (!event_pending) return false;
  event_pending = false;
  *status = ev_status; *err = ev_err; *src = ev_src; *shown = ev_shown;
  return true;
}

// ---- init / poll -------------------------------------------------------------------------------------------------------
void slide_init(void)
{
  fl_init();
  load_settings();
  panel_set_temp(cfg.temp_c);
  panel_set_auto_temp((cfg.flags & 4u) != 0);
  panel_set_rotate((cfg.flags & 8u) != 0);
  batt_set_thresholds((uint16_t)(2000u + 10u * cfg.batt_low), (uint16_t)(2000u + 10u * cfg.batt_crit));
  slide_apply_tx();
  last_us = RAIL_GetTime();
  rng ^= last_us;
  scan_wanted = true;
  for (int guard = 0; guard < 100 && slide_scanning(); guard++) scan_poll();      // not connected: this takes a few milliseconds
  due_s = 0xFFFFFFFFu;                     // the countdown starts when the boot splash is finished
  if (!PANEL_SPLASH_ENABLE && cfg.enabled) due_s = (uint32_t)cfg.interval_min * 60u;
  g_diag.sl_cfgmask = cfg.mask;
}

void slide_poll(void)
{
  uint32_t now = RAIL_GetTime();
  acc_us += (uint32_t)(now - last_us);
  last_us = now;
  while (acc_us >= 1000000u) { acc_us -= 1000000u; uptime_s++; }

  // a panel update has just finished: remember, tell the phone, restart the countdown
  bool busy = panel_busy();
  if (was_busy && !busy) {
    ev_status = (uint8_t)panel_status(); ev_err = panel_error(); ev_src = (uint8_t)panel_source(); ev_shown = panel_shown();
    event_pending = true;
    due_s = cfg.enabled ? uptime_s + (uint32_t)cfg.interval_min * 60u : 0xFFFFFFFFu;
    demo_gap_s = uptime_s + 1u;                        // the next screen of a tour follows at once (the panel was busy until now)
  }
  was_busy = busy;
  if (clean_len && !busy) {                            // the cleaning sequence goes on step by step, then the slide follows
    if (clean_pos < clean_len) {
      if (panel_show_fill(clean_seq[clean_pos])) clean_pos++;
    } else {
      clean_len = 0; clean_pos = 0;
    }
  }
  if (demo_tour && !busy && (int32_t)(uptime_s - demo_gap_s) >= 0 && demo_idle_ok()) {    // the tour goes on with the next screen
    uint8_t st = demo_steps[demo_pos];
    if (batt_level() == BATT_CRIT) { demo_tour = false; }
    else if (st == DS_BACK) {
      demo_tour = false;
      if (demo_restore && start_show(demo_restore) == SLR_OK) { } else (void)slide_next();
    } else {
      bool ok;
      if (st == DS_SPLASH) ok = panel_show_splash();
      else if (st == DS_SLOT) ok = start_show(demo_slot) == SLR_OK;
      else if (st == DS_HW) { demo_hw_fill(); ok = panel_show_demo(DEMO_PAT_HW); }
      else ok = panel_show_demo(st);
      if (ok) demo_pos++;
    }
  }
  if (clean_slot && !busy && !clean_len) {             // the cleaning is done: now the slide itself
    if (start_show(clean_slot) == SLR_OK) clean_slot = 0;
    else if (!panel_busy() && up_idle() && ps == P_IDLE && !fl_busy() && !slide_scanning() && !slots[clean_slot].show) clean_slot = 0;   // the slide vanished
  }

  bool idle = up_idle();
  if (idle && !was_up_idle) scan_wanted = true;        // an upload / verify has just ended: the slot table may have changed
  was_up_idle = idle;

  if (tx_apply_due && (!ll_connected() || ll_free_us() > 2000u)) slide_apply_tx();
  persist_poll();
  scan_poll();

  batt_poll(uptime_s, !busy && ps == P_IDLE && up_idle() && !fl_busy() && !clean_len && (!ll_connected() || ll_free_us() > 2000u));       // not inside a connection event
  if (!demo_tour && cfg.enabled && due_s != 0xFFFFFFFFu && (int32_t)(uptime_s - due_s) >= 0 && !busy && batt_level() != BATT_CRIT) {
    if (slide_next() != SLR_OK) due_s = uptime_s + 30u;           // nothing to show or the flash is in use: look again soon
  }
  g_diag.sl_due_s = slide_next_in_s();
}
