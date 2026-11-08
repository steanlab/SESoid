// Host test of ble_nfc.c against a bit-level model of an NTAG I2C plus chip on PC5 (supply), PD2 (SDA), PD3 (SCL), address 0x55,
// with writes (4 ms busy time, block 0 rules, write protection) and an FD output on PA8.
#include <stdio.h>
#include <string.h>
#include "em_device.h"
#include "rail.h"
#include "ble_nfc.h"

LfrcoT g_lfrco; LfxoT g_lfxo; HfxoT g_hfxo; DevinfoT g_devinfo; CmuT g_cmu; GpioT g_gpio;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t now_us, step_us = 50;
static const int psize[4] = { 9, 5, 8, 4 };

// ---- the chip
static uint8_t mem[256][16];                 // blocks
static uint8_t sreg[8];                      // session registers
static int dead, field, nak_blk = -1;        // dead: never answers; field: RF field present; nak_blk: this block address is not acknowledged
static int wlock[256];                       // writes to this block are acknowledged but ignored
static int writes, mem_writes;               // write transactions seen / blocks stored
static int drive_high_viol;                  // SDA / SCL driven high by the master
static int fd_on;                            // PA8 is the FD output: low while the field is present
static int slave_low, sda_m = 1, scl_m = 1;
static int ch_mode, bits, acked, phase, blk, rega, have_rega, txclk, txidx, master_ack, last_rw_read, tx_is_reg, wr_idx, wr_active;
static uint8_t shift, wr_buf[16];
static int psda = 1, pscl = 1, addr7 = 0x55, amode;
static int map_addr(uint8_t v) { return amode == 0 ? v >> 1 : amode == 1 ? (v & 0x7F) : ((v * 5 + 11) & 0x7F); }   // what byte 0 of block 0 does to the I2C address: not known for the real chip
static uint32_t busy_until;

static int powered(void)
{
  uint32_t m = (g_gpio.P[2].MODEL >> 20) & 0xFu;
  return !dead && m == 4 && ((g_gpio.P[2].DOUT >> 5) & 1u);
}
static uint8_t tx_byte(void)
{
  if (tx_is_reg) { uint8_t r = (uint8_t)(rega + txidx); return r == 6 ? (uint8_t)(0x20 | (field ? 1 : 0)) : sreg[r & 7]; }
  return mem[(blk + txidx / 16) & 255][txidx % 16];
}
static void drive_bit(void) { slave_low = !((tx_byte() >> (7 - txclk)) & 1u); }
static void store_block(void)                // a complete 16 byte write: what the chip does with it
{
  mem_writes++;
  busy_until = now_us + 4000;
  if (wlock[blk]) return;
  if (blk == 0) {
    addr7 = map_addr(wr_buf[0]);                                 // byte 0 is the I2C address
    mem[0][10] = wr_buf[10]; mem[0][11] = wr_buf[11];            // static lock bytes can be reset over I2C
    for (int i = 12; i < 16; i++) mem[0][i] |= wr_buf[i];        // CC: one time programmable bits
    return;                                                      // bytes 1..9 (UID) are read only
  }
  memcpy(mem[blk], wr_buf, 16);
}

static void chip(int sda, int scl)
{
  if (!powered()) { ch_mode = 0; slave_low = 0; wr_active = 0; psda = sda; pscl = scl; return; }
  if (pscl && scl && psda && !sda) { if (wr_active && wr_idx == 16) store_block(); wr_active = 0; ch_mode = 1; bits = 0; acked = 0; phase = 0; shift = 0; slave_low = 0; }      // start
  else if (pscl && scl && !psda && sda) { if (wr_active && wr_idx == 16) store_block(); wr_active = 0; ch_mode = 0; slave_low = 0; }                                   // stop
  else if (!pscl && scl) {                                                                                           // rising edge
    if (ch_mode == 1 && bits < 8) { shift = (uint8_t)((shift << 1) | sda); bits++; }
    else if (ch_mode == 2) { txclk++; if (txclk == 9) master_ack = !sda; }
  } else if (pscl && !scl) {                                                                                         // falling edge
    if (ch_mode == 1 && bits == 8 && !acked) {
      int ack = 0;
      if (phase == 0) { ack = (shift >> 1) == addr7 && now_us >= busy_until; last_rw_read = shift & 1; }
      else if (phase == 1) { blk = shift; ack = (int)blk != nak_blk; have_rega = 0; }
      else if (phase == 2) { rega = shift; have_rega = 1; ack = 1; }
      else { ack = 1; if (wr_idx < 16) wr_buf[wr_idx++] = shift; }
      slave_low = ack; acked = ack;
      if (!ack) ch_mode = 0;
    } else if (ch_mode == 1 && acked) {
      slave_low = 0; acked = 0; bits = 0;
      if (phase == 0 && last_rw_read) {                                                                              // read: the chip sends now
        ch_mode = 2; txclk = 0; txidx = 0; tx_is_reg = (blk == 0xFE && have_rega); drive_bit();
      } else if (phase == 0) phase = 1;
      else if (phase == 1) { phase = (blk == 0xFE) ? 2 : 3; wr_idx = 0; wr_active = 1; writes++; }
      else if (phase == 2) phase = 3;
    } else if (ch_mode == 2) {
      if (txclk < 8) drive_bit();
      else if (txclk == 8) slave_low = 0;
      else if (txclk == 9) { if (master_ack) { txidx++; txclk = 0; drive_bit(); } else { ch_mode = 0; slave_low = 0; } }
    }
  }
  psda = sda; pscl = scl;
}

static void sim(void)
{
  for (int p = 0; p < 4; p++) {
    uint32_t din = 0;
    for (int n = 0; n < psize[p]; n++) {
      uint32_t mode = (n < 8 ? g_gpio.P[p].MODEL >> (n * 4) : g_gpio.P[p].MODEH >> ((n - 8) * 4)) & 0xFu;
      int dout = (g_gpio.P[p].DOUT >> n) & 1u;
      int v = (mode == 4 || mode == 2) ? dout : 1;
      if (p == 3 && (n == 2 || n == 3)) {
        if (mode == 4 && dout) drive_high_viol++;
        int m = (mode == 4) ? dout : (mode == 2 ? dout : 1);
        if (n == 2) { sda_m = m; v = m && !slave_low; } else { scl_m = m; v = m; }
      }
      if (p == 2 && n == 5) v = powered() ? 1 : 0;
      if (p == 0 && n == 8 && fd_on && mode == 2 && dout && field) v = 0;      // FD: open drain, low while the field is present, needs the pull-up
      if (v) din |= 1u << n;
    }
    g_gpio.P[p].DIN = din;
  }
  chip(sda_m && !slave_low, scl_m);
}
uint32_t RAIL_GetTime(void) { now_us += step_us; sim(); return now_us; }

static void world(void)
{
  memset(&g_gpio, 0, sizeof(g_gpio)); memset(&g_cmu, 0, sizeof(g_cmu));
  g_gpio.P[2].MODEL = 0x74444444u & ~(0xFu << 20); g_gpio.P[2].MODEL |= (0x1u << 20); g_gpio.P[2].DOUT = 0x1Fu;   // PC5 an input, as the firmware leaves it
  g_gpio.P[3].MODEL = 0x00001133u; g_gpio.P[0].MODEL = 0x11111111u; g_gpio.P[0].MODEH = 0x1u;
  for (int b = 0; b < 256; b++) for (int i = 0; i < 16; i++) mem[b][i] = (uint8_t)(b * 3 + i);
  memcpy(mem[0], "\x04\x45\x8C\x45\xEB\x20\x90\x00\x44\x00\xFF\xFF\xE1\x10\x6D\x0F", 16);
  mem[1][0] = 0x03; mem[1][1] = 0x1D;
  for (int i = 0; i < 8; i++) sreg[i] = (uint8_t)(0x10 + i);
  memset(wlock, 0, sizeof(wlock));
  dead = 0; field = 0; nak_blk = -1; drive_high_viol = 0; slave_low = 0; sda_m = scl_m = 1; psda = pscl = 1; ch_mode = 0; wr_active = 0;
  addr7 = 0x55; amode = 0; busy_until = 0; writes = mem_writes = 0; fd_on = 0;
}
static void run(uint32_t limit_us)
{
  uint32_t t0 = now_us;
  while (nfc_state()->state == 1 && now_us - t0 < limit_us) { RAIL_GetTime(); nfc_poll(); }
}
static void advance(uint32_t us) { uint32_t t0 = now_us; while (now_us - t0 < us) { RAIL_GetTime(); nfc_poll(); } }
static int pins_same(const GpioT *a)
{
  for (int p = 0; p < 4; p++) if (p != 1 && (a->P[p].MODEL != g_gpio.P[p].MODEL || a->P[p].MODEH != g_gpio.P[p].MODEH || a->P[p].DOUT != g_gpio.P[p].DOUT)) return 0;
  return 1;
}

static void test_snapshot(void)
{
  printf("snapshot\n");
  world();
  GpioT saved = g_gpio;
  CHECK(nfc_snapshot_start(), "starts");
  CHECK(((g_gpio.P[2].MODEL >> 20) & 0xFu) == 4 && ((g_gpio.P[2].DOUT >> 5) & 1u), "the supply pin PC5 is on");
  run(2000000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 2 && c->mode == 1 && c->fail == 0 && c->regs_ok == 0x7F, "finished: state %u fail %02x regs_ok %02x", c->state, c->fail, c->regs_ok);
  CHECK(memcmp(c->blocks[0], mem[0], 16) == 0 && memcmp(c->blocks[1], mem[1], 16) == 0 && memcmp(c->blocks[2], mem[2], 16) == 0, "blocks 0 1 2 read correctly");
  CHECK(memcmp(c->blocks[3], mem[0x38], 16) == 0 && memcmp(c->blocks[4], mem[0x3A], 16) == 0 && memcmp(c->blocks[5], mem[0xFE], 16) == 0, "blocks 38 3A FE read correctly");
  CHECK(c->regs[0] == 0x10 && c->regs[5] == 0x15 && c->regs[6] == 0x20, "session registers: %02x %02x %02x", c->regs[0], c->regs[5], c->regs[6]);
  CHECK(pins_same(&saved), "pins put back, the chip is off");
  CHECK(drive_high_viol == 0, "SDA and SCL were never driven high (%d)", drive_high_viol);
  CHECK(mem_writes == 0, "a snapshot never writes");
  CHECK(nfc_changed() && !nfc_changed(), "changes reported once");

  printf("snapshot: one block not acknowledged\n");
  nak_blk = 0x38;
  CHECK(nfc_snapshot_start(), "starts");
  run(2000000u);
  CHECK(c->state == 2 && c->fail == (1u << 3) && c->regs_ok == 0x7F, "only block 3 failed: fail %02x", c->fail);

  printf("snapshot: chip dead\n");
  dead = 1;
  CHECK(nfc_snapshot_start(), "starts");
  run(2000000u);
  CHECK(c->state == 3 && (c->fail & 0x3F) == 0x3F && c->regs_ok == 0, "failed: state %u fail %02x", c->state, c->fail);
  CHECK(pins_same(&saved), "pins put back after a failure");
}

static void test_live(void)
{
  printf("live\n");
  world();
  GpioT saved = g_gpio;
  CHECK(nfc_live_start(0), "starts");
  run(1000000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 1 && c->mode == 2 && c->ns_valid && !c->field_now && c->events == 0, "running, no field: valid %u field %u events %u", c->ns_valid, c->field_now, c->events);
  (void)nfc_changed();
  field = 1; run(500000u);
  CHECK(c->field_now && c->field_ever && c->events == 1, "field seen: now %u ever %u events %u", c->field_now, c->field_ever, c->events);
  CHECK(nfc_changed(), "the change is reported");
  field = 0; run(500000u); field = 1; run(500000u); field = 0; run(500000u);
  CHECK(!c->field_now && c->events == 2, "two taps counted: events %u", c->events);
  nfc_stop();
  CHECK(c->state == 2 && pins_same(&saved), "stopped, pins back");

  printf("live: runs out after two minutes\n");
  field = 0; CHECK(nfc_live_start(0), "starts");
  run(200000000u);
  CHECK(c->state == 2 && c->live_s >= NFC_LIVE_MAX_S - 1 && pins_same(&saved), "ended by itself: state %u live_s %u", c->state, c->live_s);

  printf("live: chip does not answer\n");
  dead = 1; CHECK(nfc_live_start(0), "starts");
  run(30000000u);
  CHECK(c->state == 3, "failed after repeated silence: state %u", c->state);

  printf("a new start ends the running one; reset ends it\n");
  dead = 0; CHECK(nfc_live_start(0), "live starts");
  run(300000u);
  CHECK(nfc_snapshot_start() && c->mode == 1 && c->state == 1, "snapshot replaces live");
  run(2000000u);
  CHECK(c->state == 2 && c->regs_ok == 0x7F, "snapshot done");
  CHECK(nfc_live_start(0), "live starts"); run(300000u); nfc_reset();
  CHECK(c->state == 2 && pins_same(&saved), "reset: stopped, pins back");
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);
}

static void test_fd(void)
{
  printf("FD watch\n");
  world(); fd_on = 1;
  GpioT saved = g_gpio;
  uint32_t mask = (1u << 6) | (1u << 8) | (1u << 10) | (1u << 19) | (1u << 24);     // PA6 PA8 PB1, and PC5 and PD2 which must be dropped
  CHECK(nfc_live_start(mask), "starts");
  const nfc_fd_t *f = nfc_fd();
  CHECK(f->n == 3 && f->pin[0] == 6 && f->pin[1] == 8 && f->pin[2] == 10, "watched pins: %u (%u %u %u)", f->n, f->pin[0], f->pin[1], f->pin[2]);
  run(1000000u); field = 1; run(1000000u); field = 0; run(1000000u); field = 1; run(500000u); field = 0; run(500000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 1 && c->events == 2, "two field periods: events %u", c->events);
  CHECK(f->lo_f[1] > 150 && f->lo_f[1] > 10 * f->hi_f[1], "PA8 is low while the field is there: lo_f %u hi_f %u (a few at the edges, the flag is polled)", f->lo_f[1], f->hi_f[1]);
  CHECK(f->hi_n[1] > 300 && f->hi_n[1] > 10 * f->lo_n[1], "PA8 is high without the field: hi_n %u lo_n %u", f->hi_n[1], f->lo_n[1]);
  CHECK(f->lo_f[0] == 0 && f->hi_f[0] > 100 && f->lo_n[0] == 0 && f->lo_f[2] == 0 && f->lo_n[2] == 0 && f->hi_f[2] > 100, "PA6 and PB1 never go low");
  nfc_stop();
  CHECK(pins_same(&saved), "all pins, also the watched ones, put back");
  CHECK(nfc_live_start(0) && nfc_fd()->n == 0, "a run without pins watches none");
  nfc_stop();
  CHECK(!nfc_live_start(0x3FFFFFFu & ~(1u << 19) & ~(1u << 24) & ~(1u << 25)) || nfc_fd()->n <= NFC_FD_MAX, "at most 12 pins");
  nfc_stop();
  CHECK(nfc_snapshot_start() && nfc_fd()->n == 0, "a snapshot after a watch restores nothing stale");
  run(2000000u);
  CHECK(pins_same(&saved), "pins intact after the snapshot");
}

static void test_tap(void)
{
  printf("tap mode\n");
  world(); now_us = 0x90000000u;                                    // beyond 2^31 us: the time comparisons must survive that
  GpioT saved = g_gpio;
  nfc_tap_enable(true); nfc_hold(false);
  advance(300000u);
  CHECK(nfc_tap_running(), "tap mode runs");
  CHECK(((g_gpio.P[2].MODEL >> 20) & 0xFu) == 4 && ((g_gpio.P[2].DOUT >> 5) & 1u), "chip powered in tap mode");
  advance(1000000u);
  CHECK(!nfc_tap_take() && nfc_tap_count() == 0, "no tap without a field");
  field = 1; advance(600000u);
  CHECK(nfc_tap_take() && nfc_tap_count() == 1, "a phone arrives: one tap");
  CHECK(!nfc_tap_take(), "taken once");
  advance(3000000u);
  CHECK(!nfc_tap_take() && nfc_tap_count() == 1, "a phone kept on the tag is not a second tap");
  for (int k = 0; k < 8; k++) { field = 0; advance(150000u); field = 1; advance(600000u); }
  CHECK(nfc_tap_count() == 1, "field dropping out for 150 ms (8 times): not a new tap (%u)", nfc_tap_count());
  field = 0; advance(800000u); field = 1; advance(600000u);
  CHECK(nfc_tap_take() && nfc_tap_count() == 2, "removed and put back again: a second tap (%u)", nfc_tap_count());

  printf("tap mode: held off by the pin tools, resumes\n");
  field = 0; advance(1000000u);
  nfc_hold(true); advance(500000u);
  CHECK(!nfc_tap_running() && pins_same(&saved), "hold: tap mode left, pins back");
  field = 1; advance(1000000u);
  CHECK(nfc_tap_count() == 2, "no taps counted while held");
  nfc_hold(false); advance(1000000u);
  CHECK(nfc_tap_running() && nfc_tap_count() == 2, "back after the hold, a phone already there is the starting point, not a tap");
  field = 0; advance(800000u); field = 1; advance(600000u);
  CHECK(nfc_tap_count() == 3, "a new tap after that (%u)", nfc_tap_count());

  printf("tap mode: a tool takes over and gives it back\n");
  field = 0; advance(500000u);
  CHECK(nfc_snapshot_start(), "snapshot starts during tap mode");
  CHECK(!nfc_tap_running(), "tap mode is not running during it");
  run(2000000u);
  CHECK(nfc_state()->state == 2 && nfc_state()->fail == 0, "the snapshot is complete");
  advance(500000u);
  CHECK(nfc_tap_running(), "tap mode resumes after the tool");

  printf("tap mode: off switch\n");
  nfc_tap_enable(false); advance(500000u);
  CHECK(!nfc_tap_running() && pins_same(&saved), "off: pins back");

  printf("tap mode: chip silent, retries later\n");
  dead = 1; nfc_tap_enable(true); advance(4000000u);
  CHECK(!nfc_tap_running(), "gave up for now");
  dead = 0; advance(7000000u);
  CHECK(nfc_tap_running(), "tried again after the pause");
  nfc_tap_enable(false); advance(500000u);
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);
  now_us = 1000;
}

static void test_backup_write(void)
{
  printf("backup\n");
  world();
  GpioT saved = g_gpio;
  nfc_tap_enable(false);
  CHECK(nfc_backup_start(), "starts");
  run(5000000u);
  const nfc_t *c = nfc_state();
  int same = 1; for (int b = 0; b < (int)NFC_BK_BLOCKS; b++) if (memcmp(nfc_backup_block((uint8_t)b), mem[b], 16)) same = 0;
  CHECK(c->state == 2 && c->mode == NFC_M_BACKUP && c->op_res == NFC_R_OK && same, "all 59 blocks copied: state %u op_res %u", c->state, c->op_res);
  CHECK(pins_same(&saved) && mem_writes == 0, "nothing written, pins back");
  nak_blk = 0x20;
  CHECK(nfc_backup_start(), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_NOACK && c->op_blk == 0x20, "a missing block is reported: state %u res %u blk %02x", c->state, c->op_res, c->op_blk);
  nak_blk = -1;

  printf("write\n");
  uint8_t b1[16] = { 0x03, 0x1B, 0xD1, 0x01, 0x17, 0x55, 0x04, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c' };
  uint8_t b2[16] = { 'o', 'm', '/', 'S', 'E', 'S', 'o', 'i', 'd', 0xFE, 0, 0, 0, 0, 0, 0 };
  CHECK(!nfc_write_start(2), "no buffer yet: refused");
  CHECK(nfc_wbuf_set(0, b1) && !nfc_write_start(2), "second block missing: refused");
  CHECK(nfc_wbuf_set(1, b2), "second block stored");
  CHECK(!nfc_write_start(0) && !nfc_write_start(8), "block counts 0 and 8 refused");
  CHECK(!nfc_write_start(1), "a message longer than the blocks written is refused");
  uint8_t before1[16], before3[16]; memcpy(before1, mem[1], 16); memcpy(before3, mem[3], 16); (void)before1;
  CHECK(nfc_write_start(2), "valid message accepted");
  run(5000000u);
  CHECK(c->state == 2 && c->op_res == NFC_R_OK, "written and verified: state %u res %u blk %02x", c->state, c->op_res, c->op_blk);
  CHECK(memcmp(mem[1], b1, 16) == 0 && memcmp(mem[2], b2, 16) == 0 && memcmp(mem[3], before3, 16) == 0, "blocks 1 and 2 changed, block 3 untouched");
  CHECK(mem_writes == 2 && memcmp(mem[0], "\x04\x45\x8C\x45\xEB\x20\x90\x00\x44\x00\xFF\xFF\xE1\x10\x6D\x0F", 16) == 0, "exactly two blocks written, block 0 untouched (%d)", mem_writes);
  CHECK(pins_same(&saved), "pins back");

  printf("write: protected block\n");
  uint8_t badbuf[16]; memcpy(badbuf, b1, 16); badbuf[7] = 'Z';
  uint8_t b2x[16]; memcpy(b2x, b2, 16); b2x[3] = 'Q';
  nfc_wbuf_set(0, badbuf); nfc_wbuf_set(1, b2x);
  wlock[2] = 1; mem_writes = 0;
  CHECK(nfc_write_start(2), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_VERIFY && c->op_blk == 2, "verify catches the ignored write: state %u res %u blk %02x", c->state, c->op_res, c->op_blk);
  wlock[2] = 0;

  printf("write: chip stops answering\n");
  dead = 1;
  CHECK(nfc_write_start(2), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_NOACK && c->op_blk == 1, "no answer: res %u blk %02x", c->op_res, c->op_blk);
  dead = 0;
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);
}

static void test_unlock(void)
{
  printf("unlock\n");
  world();
  GpioT saved = g_gpio;
  nfc_tap_enable(false);
  uint8_t uid[9]; memcpy(uid, mem[0], 9);
  uint8_t b38[16]; memcpy(b38, mem[0x38], 16); b38[8] = 0xFF; b38[9] = 0x3F; b38[10] = 0x7F; memcpy(mem[0x38], b38, 16);
  CHECK(nfc_unlock_start(0xAA), "starts");
  run(5000000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 2 && c->op_res == NFC_R_OK, "unlocked: state %u res %u blk %02x", c->state, c->op_res, c->op_blk);
  CHECK(mem[0][10] == 0 && mem[0][11] == 0, "static lock bytes cleared");
  CHECK(memcmp(mem[0], uid, 9) == 0 || (mem[0][0] == 0x04 && memcmp(mem[0] + 1, uid + 1, 8) == 0), "the first nine bytes unchanged");
  CHECK(mem[0][12] == 0xE1 && mem[0][13] == 0x10 && mem[0][14] == 0x6D && mem[0][15] == 0x0F, "CC unchanged");
  CHECK(addr7 == 0x55, "the I2C address is still 0x55 (%02x)", addr7);
  CHECK(mem[0x38][8] == 0 && mem[0x38][9] == 0 && mem[0x38][10] == 0 && memcmp(mem[0x38], b38, 8) == 0 && mem[0x38][11] == b38[11], "dynamic lock bytes cleared, the rest of block 38 as it was");
  CHECK(mem_writes == 2 && pins_same(&saved), "two blocks written, pins back (%d)", mem_writes);

  printf("unlock: refused when block 0 is not what is expected\n");
  world(); mem[0][12] = 0x00; mem_writes = 0;
  CHECK(nfc_unlock_start(0xAA), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_REFUSED && mem_writes == 0, "refused, nothing written: res %u writes %d", c->op_res, mem_writes);
  world(); mem[0][0] = 0x11; mem_writes = 0;
  CHECK(nfc_unlock_start(0xAA), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_REFUSED && mem_writes == 0, "refused on a wrong first byte");

  printf("unlock: write protected block 0 is caught by the read-back\n");
  world(); wlock[0] = 1; mem_writes = 0;
  CHECK(nfc_unlock_start(0xAA), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_VERIFY && c->op_blk == 0, "verify: res %u blk %02x", c->op_res, c->op_blk);
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);
}

static void test_find_rescue(void)
{
  printf("find\n");
  world(); nfc_tap_enable(false);
  CHECK(nfc_find_start(), "starts");
  run(5000000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 2 && c->op_blk == 0x55 && c->fail == 0 && c->fd_n == 1, "chip at 0x55: state %u blk %02x fail %02x n %u", c->state, c->op_blk, c->fail, c->fd_n);
  world(); addr7 = 0x2A; mem_writes = 0;
  GpioT saved = g_gpio;
  CHECK(nfc_find_start(), "starts");
  run(5000000u);
  CHECK(c->state == 2 && c->op_blk == 0x2A && c->fd_n == 1 && mem_writes == 0 && pins_same(&saved), "chip at 0x2A found, nothing written, pins back (blk %02x)", c->op_blk);
  world(); addr7 = 0x00;
  CHECK(nfc_find_start(), "starts");
  run(5000000u);
  CHECK(c->state == 2 && c->op_blk == 0x00 && c->fd_n == 1, "address 0x00 is probed too");
  world(); dead = 1;
  CHECK(nfc_find_start(), "starts");
  run(5000000u);
  CHECK(c->state == 3 && c->op_res == NFC_R_NOACK && c->fd_n == 0, "no chip: failed, none found");
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);

  printf("rescue\n");
  world(); mem_writes = 0;
  CHECK(nfc_rescue_start(), "starts");
  run(60000000u);
  CHECK(c->state == 2 && c->fail == 1 && mem_writes == 0, "already at 0x55: nothing written (fail %u writes %d)", c->fail, mem_writes);
  const int expect[3] = { 0x55, 0x55, 0x42 };               // the byte-0 value that gives 0x55 under each model: v >> 1 never reaches it with 0xAA excluded, so 0xAA is the answer there
  for (int m = 1; m <= 2; m++) {
    world(); amode = m; addr7 = map_addr(0xAA);
    uint8_t uid[9]; memcpy(uid, mem[0], 9);
    mem[0][10] = 0xFF; mem[0][11] = 0xFF; mem_writes = 0;
    saved = g_gpio;
    CHECK(addr7 != 0x55, "model %d: the chip starts away from 0x55 (%02x)", m, addr7);
    CHECK(nfc_rescue_start(), "starts");
    run(60000000u);
    CHECK(c->state == 2 && c->fail == 2 && addr7 == 0x55, "model %d: back at 0x55 (state %u fail %u addr %02x)", m, c->state, c->fail, addr7);
    CHECK(c->op_blk == expect[m], "model %d: the value found is %02x (want %02x), tries %u", m, c->op_blk, expect[m], c->fd_n);
    CHECK(mem_writes == c->fd_n, "model %d: one block write per try (%d / %u)", m, mem_writes, c->fd_n);
    CHECK(memcmp(mem[0] + 1, uid + 1, 8) == 0 && mem[0][10] == 0xFF && mem[0][11] == 0xFF && mem[0][12] == 0xE1 && mem[0][15] == 0x0F, "model %d: the rest of block 0 as it was", m);
    CHECK(pins_same(&saved) && drive_high_viol == 0, "model %d: pins back, nothing driven high", m);
    nfc_tap_enable(false);
    CHECK(nfc_snapshot_start(), "starts");
    run(5000000u);
    CHECK(c->state == 2 && !(c->fail & 0x3F), "model %d: a normal read works again", m);
  }
  printf("unlock with the value found by the rescue\n");
  world(); amode = 2;
  CHECK(nfc_unlock_start(0x42), "starts");
  run(5000000u);
  CHECK(c->state == 2 && c->op_res == NFC_R_OK && addr7 == 0x55 && mem[0][10] == 0, "unlock keeps the address with b0 = 0x42");
}

int main(void)
{
  test_snapshot();
  test_live();
  test_fd();
  test_tap();
  test_backup_write();
  test_unlock();
  test_find_rescue();
  printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
  return fails != 0;
}
