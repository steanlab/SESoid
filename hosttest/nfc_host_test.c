// Host test of ble_nfc.c against a bit-level model of an NTAG I2C plus chip on PC5 (supply), PD2 (SDA), PD3 (SCL), address 0x55.
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
static int drive_high_viol;                  // SDA / SCL driven high by the master
static int slave_low, sda_m = 1, scl_m = 1;
static int ch_mode, bits, acked, phase, blk, rega, have_rega, txclk, txidx, master_ack, last_rw_read, tx_is_reg;
static uint8_t shift;
static int psda = 1, pscl = 1;

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

static void chip(int sda, int scl)
{
  if (!powered()) { ch_mode = 0; slave_low = 0; psda = sda; pscl = scl; return; }
  if (pscl && scl && psda && !sda) { ch_mode = 1; bits = 0; acked = 0; phase = 0; shift = 0; slave_low = 0; }        // start
  else if (pscl && scl && !psda && sda) { ch_mode = 0; slave_low = 0; }                                              // stop
  else if (!pscl && scl) {                                                                                           // rising edge
    if (ch_mode == 1 && bits < 8) { shift = (uint8_t)((shift << 1) | sda); bits++; }
    else if (ch_mode == 2) { txclk++; if (txclk == 9) master_ack = !sda; }
  } else if (pscl && !scl) {                                                                                         // falling edge
    if (ch_mode == 1 && bits == 8 && !acked) {
      int ack = 0;
      if (phase == 0) { ack = (shift >> 1) == 0x55; last_rw_read = shift & 1; }
      else if (phase == 1) { blk = shift; ack = (int)blk != nak_blk; have_rega = 0; }
      else if (phase == 2) { rega = shift; have_rega = 1; ack = 1; }
      slave_low = ack; acked = ack;
      if (!ack) ch_mode = 0;
    } else if (ch_mode == 1 && acked) {
      slave_low = 0; acked = 0; bits = 0;
      if (phase == 0 && last_rw_read) {                                                                              // read: the chip sends now
        ch_mode = 2; txclk = 0; txidx = 0; tx_is_reg = (blk == 0xFE && have_rega); drive_bit();
      } else if (phase == 0) phase = 1;
      else if (phase == 1) phase = (blk == 0xFE) ? 2 : 3;
      else if (phase == 2) phase = 3;
      if (ch_mode == 1 && phase == 3) { phase = 0; }                                                                 // a restart follows
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
  g_gpio.P[3].MODEL = 0x00001133u;
  for (int b = 0; b < 256; b++) for (int i = 0; i < 16; i++) mem[b][i] = (uint8_t)(b * 3 + i);
  memcpy(mem[0], "\x04\x45\x8C\x45\xEB\x20\x90\x00\x44\x00\xFF\xFF\xE1\x10\x6D\x0F", 16);
  mem[1][0] = 0x03; mem[1][1] = 0x1D;
  for (int i = 0; i < 8; i++) sreg[i] = (uint8_t)(0x10 + i);
  dead = 0; field = 0; nak_blk = -1; drive_high_viol = 0; slave_low = 0; sda_m = scl_m = 1; psda = pscl = 1; ch_mode = 0;
}
static void run(uint32_t limit_us)
{
  uint32_t t0 = now_us;
  while (nfc_state()->state == 1 && now_us - t0 < limit_us) { RAIL_GetTime(); nfc_poll(); }
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
  CHECK(g_gpio.P[2].MODEL == saved.P[2].MODEL && g_gpio.P[2].DOUT == saved.P[2].DOUT && g_gpio.P[3].MODEL == saved.P[3].MODEL && g_gpio.P[3].DOUT == saved.P[3].DOUT, "pins put back, the chip is off");
  CHECK(drive_high_viol == 0, "SDA and SCL were never driven high (%d)", drive_high_viol);
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
  CHECK(g_gpio.P[2].MODEL == saved.P[2].MODEL && g_gpio.P[3].MODEL == saved.P[3].MODEL, "pins put back after a failure");
}

static void test_live(void)
{
  printf("live\n");
  world();
  GpioT saved = g_gpio;
  CHECK(nfc_live_start(), "starts");
  run(1000000u);
  const nfc_t *c = nfc_state();
  CHECK(c->state == 1 && c->mode == 2 && c->ns_valid && !c->field_now && c->events == 0, "running, no field: valid %u field %u events %u", c->ns_valid, c->field_now, c->events);
  (void)nfc_changed();
  field = 1; run(500000u);
  CHECK(c->field_now && c->field_ever && c->events == 1, "field seen: now %u ever %u events %u", c->field_now, c->field_ever, c->events);
  CHECK(nfc_changed(), "the change is reported");
  field = 0; run(500000u); field = 1; run(500000u); field = 0; run(500000u);
  CHECK(!c->field_now && c->events == 2, "two taps counted: events %u", c->events);
  CHECK(c->state == 1, "still running");
  nfc_stop();
  CHECK(c->state == 2 && g_gpio.P[2].MODEL == saved.P[2].MODEL && g_gpio.P[2].DOUT == saved.P[2].DOUT && g_gpio.P[3].MODEL == saved.P[3].MODEL, "stopped, pins back");

  printf("live: runs out after two minutes\n");
  field = 0; CHECK(nfc_live_start(), "starts");
  run(200000000u);
  CHECK(c->state == 2 && c->live_s >= NFC_LIVE_MAX_S - 1 && g_gpio.P[2].MODEL == saved.P[2].MODEL, "ended by itself: state %u live_s %u", c->state, c->live_s);

  printf("live: chip does not answer\n");
  dead = 1; CHECK(nfc_live_start(), "starts");
  run(30000000u);
  CHECK(c->state == 3, "failed after repeated silence: state %u", c->state);

  printf("a new start ends the running one; reset ends it\n");
  dead = 0; CHECK(nfc_live_start(), "live starts");
  run(300000u);
  CHECK(nfc_snapshot_start() && c->mode == 1 && c->state == 1, "snapshot replaces live");
  run(2000000u);
  CHECK(c->state == 2 && c->regs_ok == 0x7F, "snapshot done");
  CHECK(nfc_live_start(), "live starts"); run(300000u); nfc_reset();
  CHECK(c->state == 2 && g_gpio.P[2].MODEL == saved.P[2].MODEL, "reset: stopped, pins back");
  CHECK(drive_high_viol == 0, "SDA and SCL never driven high (%d)", drive_high_viol);
}

int main(void)
{
  test_snapshot();
  test_live();
  printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
  return fails != 0;
}
