// Host test of ble_slide.c (SES V300, not badge154): settings log in the flash (append, erase when full, corrupt entries), slot table,
// playlist (in order / random), timer, manual show / next, busy answers. The flash, the panel and the radio are small models.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_slide.h"
#include "ble_panel.h"
#include "ble_flash.h"
#include "ble_up.h"
#include "ble_ll.h"
#include "ble_diag.h"

volatile ble_diag_t g_diag;
volatile fl_result_t fl_res; volatile bool fl_done;
static uint8_t mem[0x80000];
static uint32_t lo, hi;
static int fl_state;            // 0 idle, 1 erasing, 2 programming
static uint32_t fl_addr; static uint8_t fl_page[256];
static int erase_count, prog_count, range_violations;
static uint32_t now_us;
static bool g_up_idle = true, g_connected;
static int8_t last_tx;

uint32_t RAIL_GetTime(void) { return now_us; }
int RAIL_SetTxPowerDbm(void *h, int16_t p) { (void)h; last_tx = (int8_t)(p / 10); return 0; }
int16_t RAIL_GetTxPowerDbm(void *h) { (void)h; return (int16_t)(last_tx * 10 > 60 ? 60 : last_tx * 10); }
void *sl_rail_util_get_handle(int h) { (void)h; return 0; }
bool ll_connected(void) { return g_connected; }
uint32_t ll_free_us(void) { return 100000; }
bool up_idle(void) { return g_up_idle; }
uint32_t up_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
  static const uint32_t t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
  crc = ~crc; while (n--) { crc ^= *p++; crc = (crc >> 4) ^ t[crc & 15u]; crc = (crc >> 4) ^ t[crc & 15u]; } return ~crc;
}
// flash model
void fl_init(void) { fl_state = 0; fl_done = false; lo = 0; hi = FL_TEST_END; }
void fl_set_range(uint32_t l, uint32_t h) { lo = l; hi = h; }
bool fl_busy(void) { return fl_state != 0; }
bool fl_start_erase(uint32_t a)
{
  if (fl_state || (a & 4095) || a < lo || a >= hi) { if (!fl_state) range_violations++; return false; }
  fl_addr = a; fl_state = 1; fl_res.result = FLR_RUNNING; fl_done = false; return true;
}
bool fl_start_program(uint32_t a, const uint8_t *d)
{
  if (fl_state || (a & 255) || a < lo || a >= hi) { if (!fl_state) range_violations++; return false; }
  memcpy(fl_page, d, 256); fl_addr = a; fl_state = 2; fl_res.result = FLR_RUNNING; fl_done = false; return true;
}
void fl_poll(void)
{
  static int delay; if (!fl_state) return; if (++delay < 3) return; delay = 0;
  if (fl_state == 1) { memset(mem + fl_addr, 0xFF, 4096); erase_count++; }
  else { for (int i = 0; i < 256; i++) mem[fl_addr + i] &= fl_page[i]; prog_count++; }
  fl_res.result = FLR_OK; fl_state = 0; fl_done = true;
}
bool fl_read_page(uint32_t a, uint8_t *b, uint32_t n) { if (fl_state) return false; memcpy(b, mem + a, n); return true; }
bool fl_start_info(void) { return true; }
bool fl_start_scan(uint32_t a) { (void)a; return true; }
// panel model: an update takes 30 s
static uint32_t panel_end_us; static bool pbusy; static panel_src_t psrc; static uint8_t pshown, perr; static int shows; static uint8_t last_shown_slot; static uint32_t n_done;
static void pmodel(void) { if (pbusy && (int32_t)(now_us - panel_end_us) >= 0) { pbusy = false; n_done++; } }
bool panel_busy(void) { pmodel(); return pbusy; }
bool panel_show_slot(uint8_t s, uint32_t crc) { (void)crc; pmodel(); if (pbusy) return false; pbusy = true; panel_end_us = now_us + 30000000u; psrc = PSRC_SLOT; pshown = s; last_shown_slot = s; shows++; return true; }
bool panel_show_splash(void) { pmodel(); if (pbusy) return false; pbusy = true; panel_end_us = now_us + 30000000u; psrc = PSRC_SPLASH; return true; }
void panel_set_temp(int8_t c) { (void)c; }
panel_status_t panel_status(void) { return pbusy ? PST_RUNNING : PST_DONE_OK; }
uint8_t panel_error(void) { return perr; }
panel_src_t panel_source(void) { return psrc; }
uint8_t panel_shown(void) { return pshown; }
bool panel_flash_busy(void) { return false; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void tick(uint32_t ms) { for (uint32_t i = 0; i < ms; i += 10) { now_us += 10000; slide_poll(); fl_poll(); } }
static void make_record(int slot, uint32_t total, uint32_t crc)
{
  uint8_t r[256]; memset(r, 0xFF, 256); memcpy(r, "SLD1", 4);
  r[4] = (uint8_t)total; r[5] = (uint8_t)(total >> 8); r[6] = (uint8_t)(total >> 16); r[7] = (uint8_t)(total >> 24);
  r[8] = (uint8_t)crc; r[9] = (uint8_t)(crc >> 8); r[10] = (uint8_t)(crc >> 16); r[11] = (uint8_t)(crc >> 24);
  memcpy(mem + slot * 0x10000 + 0xF000, r, 256);
}

int main(void)
{
  memset(mem, 0xFF, sizeof mem);
  now_us = 1000;
  // blank flash: defaults
  slide_init();
  const slide_cfg_t *c = slide_cfg();
  CHECK(c->enabled == 1 && c->mode == 0 && c->interval_min == 60 && c->mask == 0xFE && c->temp_c == 25 && c->tx_dbm == 6, "defaults wrong");
  for (int i = 1; i <= 7; i++) CHECK(!slide_slot(i)->show, "slot %d should be empty", i);

  // slots 2, 4, 5 are complete frames, slot 6 is a 10 KB test upload, slot 1 has a record with a bad size
  make_record(2, 46080, 0x11111111); make_record(4, 46080, 0x22222222); make_record(5, 46080, 0x33333333); make_record(6, 10240, 0x44444444); make_record(1, 0x20000, 5);
  slide_rescan(); tick(100);
  CHECK(!slide_scanning(), "scan did not finish");
  CHECK(slide_slot(2)->show && slide_slot(4)->show && slide_slot(5)->show, "complete slots not recognised");
  CHECK(slide_slot(6)->valid && !slide_slot(6)->show, "10 KB slot must be valid but not showable");
  CHECK(!slide_slot(1)->valid, "oversize record must be rejected");
  CHECK(slide_slot(4)->crc == 0x22222222, "crc not read");
  CHECK(g_diag.sl_valid == ((1u << 2) | (1u << 4) | (1u << 5)), "diag valid mask %x", g_diag.sl_valid);

  // boot splash running: the countdown starts when it is finished
  panel_show_splash();
  tick(1000);
  CHECK(slide_next_in_s() == 0xFFFFFFFFu, "countdown must not run during the first update (%u)", slide_next_in_s());
  tick(30000);
  uint32_t left = slide_next_in_s();
  CHECK(left > 3500 && left <= 3600, "countdown after the splash: %u s", left);
  // 60 minutes later the first slide is shown, in order 2 4 5 2
  shows = 0;
  uint8_t seq[4]; int n = 0;
  for (int k = 0; k < 4; k++) {
    int before = shows;
    for (int ms = 0; ms < 70 * 60 * 1000 && shows == before; ms += 10000) tick(10000);
    CHECK(shows == before + 1, "slide %d was not shown", k);
    seq[n++] = last_shown_slot;
    tick(31000);                                           // the update finishes
  }
  CHECK(seq[0] == 2 && seq[1] == 4 && seq[2] == 5 && seq[3] == 2, "order %d %d %d %d", seq[0], seq[1], seq[2], seq[3]);

  // manual commands
  tick(31000);
  CHECK(slide_show(3) == SLR_BAD_SLOT, "empty slot");
  CHECK(slide_show(9) == SLR_BAD_SLOT, "slot 9");
  g_up_idle = false; CHECK(slide_show(4) == SLR_FLASH_BUSY, "flash busy while uploading"); g_up_idle = true;
  CHECK(slide_show(4) == SLR_OK, "show 4");
  CHECK(slide_show(5) == SLR_PANEL_BUSY && slide_next() == SLR_PANEL_BUSY, "panel busy");
  tick(31000);

  // settings: save, reload, wear log
  slide_cfg_t n2 = *slide_cfg(); n2.interval_min = 5; n2.mask = (1u << 4) | (1u << 5); n2.mode = 1; n2.temp_c = -3; n2.tx_dbm = -8;
  slide_set_cfg(&n2); tick(200);
  CHECK(last_tx == -8, "TX power not applied (%d)", last_tx);
  CHECK(prog_count == 1 && erase_count == 0, "first save: %d programs %d erases", prog_count, erase_count);
  CHECK(slide_next_in_s() <= 300 && slide_next_in_s() > 290, "countdown restarted with the new interval: %u", slide_next_in_s());
  slide_init();                                            // "reboot"
  c = slide_cfg();
  CHECK(c->interval_min == 5 && c->mask == 0x30 && c->mode == 1 && c->temp_c == -3 && c->tx_dbm == -8 && c->enabled == 1, "settings not reloaded");
  // random mode never repeats the current slot when there is a choice (after a reboot the splash comes first; the countdown starts after it)
  tick(31000);
  panel_show_splash(); tick(31000);
  int rep = 0, last = 0;
  for (int k = 0; k < 30; k++) { int b = shows; for (int g = 0; g < 100 && shows == b; g++) tick(10000); CHECK(shows == b + 1, "random step %d: no slide", k); if (last_shown_slot == last) rep++; last = last_shown_slot; tick(31000); CHECK(last_shown_slot == 4 || last_shown_slot == 5, "random picked slot %d outside the mask", last_shown_slot); }
  CHECK(rep == 0, "random repeated the same slide %d times", rep);
  // 16 more saves: the 17th write needs an erase
  for (int k = 0; k < 17; k++) { slide_cfg_t x = *slide_cfg(); x.interval_min = (uint16_t)(10 + k); slide_set_cfg(&x); tick(200); }
  CHECK(erase_count == 1, "expected exactly one erase of the settings sector, got %d", erase_count);
  slide_init();
  CHECK(slide_cfg()->interval_min == 26, "after wrap: interval %u", slide_cfg()->interval_min);
  // a corrupt newest entry is skipped
  uint32_t used = g_diag.sl_log_next; CHECK(used >= 1 && used < 16, "log index %u", used);
  mem[FL_CFG_SECTOR + (used - 1) * 256 + 8] ^= 0x55;
  slide_init();
  CHECK(slide_cfg()->interval_min != 26 && slide_cfg()->interval_min >= 10, "corrupt entry not skipped: %u", slide_cfg()->interval_min);
  // nothing outside the settings sector was written
  CHECK(range_violations == 0, "%d writes outside the allowed range", range_violations);
  for (uint32_t a = 0; a < 0x10000; a++) if (a < 0xF000 && mem[a] != 0xFF) { CHECK(0, "test area modified at %x", a); break; }
  // out of range values are clamped
  slide_cfg_t bad = { 2, 3, 5000, 0xFF, 120, 100 }; slide_set_cfg(&bad);
  CHECK(slide_cfg()->enabled == 1 && slide_cfg()->mode == 1 && slide_cfg()->interval_min == 1440 && slide_cfg()->mask == 0xFE && slide_cfg()->temp_c == 60 && slide_cfg()->tx_dbm == 20, "clamping");
  // auto mode off
  bad.enabled = 0; bad.interval_min = 1; slide_set_cfg(&bad); CHECK(slide_next_in_s() == 0xFFFFFFFFu, "auto off must disable the countdown");
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails != 0;
}
