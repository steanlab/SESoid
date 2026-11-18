// SES V300 (not badge154): slideshow logic, see ble_slide.h
#include <string.h>
#include "ble_slide.h"
#include "ble_batt.h"
#include "ble_panel.h"
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
  c->flags &= 3u;
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
  }
  was_busy = busy;
  if (clean_len && !busy) {                            // the cleaning sequence goes on step by step, then the slide follows
    if (clean_pos < clean_len) {
      if (panel_show_fill(clean_seq[clean_pos])) clean_pos++;
    } else {
      clean_len = 0; clean_pos = 0;
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
  if (cfg.enabled && due_s != 0xFFFFFFFFu && (int32_t)(uptime_s - due_s) >= 0 && !busy && batt_level() != BATT_CRIT) {
    if (slide_next() != SLR_OK) due_s = uptime_s + 30u;           // nothing to show or the flash is in use: look again soon
  }
  g_diag.sl_due_s = slide_next_in_s();
}
