// Host test of ble_panel.c + ble_splash.c (SES V300, not badge154). Build and run: see build_panel_test.sh.
// A model of the GPIO registers and of the panel (decodes the SPI bytes, answers chip ID / OTP reads, keeps BUSY low after some
// commands) runs the real state machine in simulated time. It checks: command sequence, BUSY respected, the longest blocking call
// of panel_poll(), the gating while "connected", the received image against the picture (splash PNG), and the pins at the end.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "em_device.h"
#include "rail.h"

// ---- simulated time and registers
static uint64_t cycles;                       // CPU cycles at 38.4 MHz
static uint64_t extra_us;                     // time spent "elsewhere" (the rest of the main loop)
static const uint32_t CPU_MHZ = 38;
uint32_t SystemCoreClock = 38400000u;
uint32_t RAIL_GetTime(void) { return (uint32_t)(cycles / CPU_MHZ + extra_us); }
static uint64_t now_us(void) { return cycles / CPU_MHZ + extra_us; }

static uint32_t sim_cyc(void) { cycles += 2; return (uint32_t)cycles; }
static uint32_t dummy_demcr, dummy_dwtctrl;
#define DEMCR dummy_demcr
#define DWT_CTRL dummy_dwtctrl
#define DWT_CYCCNT (sim_cyc())

// ---- the firmware under test (included as C++; it is plain C)
#include "../ble_diag.h"
volatile ble_diag_t g_diag;
static bool g_connected;
static uint32_t g_free_us;
bool ll_connected(void) { return g_connected; }
uint32_t ll_free_us(void) { return g_connected ? g_free_us : 1000000u; }
#include "../ble_splash.c"
#include "../ble_panel.c"

GpioT g_gpio; CmuT g_cmu;
static uint32_t clken, modeL[4], modeH[4], dout[4];

// ---- panel model
static uint64_t busy_until_us, pending_busy_us; static bool busy_forever;
static int prev_rst, prev_sclk, nbits, rd_rises, have_cmd, ndata, img_active, violations;
static uint32_t shift, last_cmd, nimg;
static uint8_t datab[64];
static uint8_t *img;
static uint8_t otp_model(int i) { return i == 0 ? 0xA5 : (uint8_t)(i * 7 + 3); }
static FILE *logf;
static int pin(int port, int n) { return (dout[port] >> n) & 1; }
static int mode_of(int port, int n) { uint32_t w = n < 8 ? modeL[port] : modeH[port]; return (w >> ((n & 7) * 4)) & 0xF; }
static int powered(void) { return mode_of(2, 6) == 4 && pin(2, 6) == 0; }
static int busy_now(void)
{
  if (!powered() || !pin(2, 4)) return 0;
  return !busy_forever && now_us() >= busy_until_us;
}
static void flush_cmd(void)
{
  if (!have_cmd) return;
  if (last_cmd == 0x10) fprintf(logf, "10 [%u bytes]\n", nimg);
  else { fprintf(logf, "%02X", last_cmd); for (int i = 0; i < ndata; i++) fprintf(logf, " %02X", datab[i]); fprintf(logf, "\n"); }
  have_cmd = 0; ndata = 0;
}
#define VIOL(...) do { violations++; printf("VIOLATION: " __VA_ARGS__); printf("\n"); } while (0)
static void byte_done(int dc, uint32_t v)
{
  if (!busy_now() && powered()) VIOL("byte %02X clocked while BUSY is low (t=%.3f s)", v, now_us() / 1e6);
  uint64_t t = now_us();
  if (!dc) {
    flush_cmd();
    have_cmd = 1; last_cmd = v; ndata = 0; rd_rises = 0;
    if (v == 0x10) { nimg = 0; img_active = 1; }
    else img_active = 0;
    pending_busy_us = 0;                        // BUSY goes low when the command ends (CS rises), not at the command byte
    if (v == 0x04) pending_busy_us = 50000;
    if (v == 0x12) pending_busy_us = 19400000;
    if (v == 0x02) pending_busy_us = 1500000;
    if (v == 0xA5) pending_busy_us = 5000;
  } else {
    if (last_cmd == 0x10) { if (nimg < 46080 + 64) img[nimg] = (uint8_t)v; nimg++; }
    else if (ndata < 64) datab[ndata++] = (uint8_t)v;
    if (last_cmd == 0xA2 && ndata == 5) busy_until_us = t + 2000;
  }
}
static void panel_update(void)
{
  int rst = pin(2, 4), sclk = pin(2, 1), cs = pin(2, 2), dc = pin(2, 3);
  if (!powered()) { prev_rst = rst; prev_sclk = sclk; return; }
  if (rst && !prev_rst) busy_until_us = now_us() + 5000;
  if (!rst) busy_forever = true; else if (busy_forever && prev_rst == 0) busy_forever = false;
  prev_rst = rst;
  if (cs) { nbits = 0; shift = 0; if (pending_busy_us) { busy_until_us = now_us() + pending_busy_us; pending_busy_us = 0; } }
  if (sclk && !prev_sclk && !cs) {
    if (mode_of(2, 0) == 4) { shift = (shift << 1) | (uint32_t)pin(2, 0); if (++nbits == 8) { byte_done(dc, shift & 0xFF); nbits = 0; shift = 0; } }
    else rd_rises++;
  }
  prev_sclk = sclk;
}
static int sda_bit(void)
{
  if (!have_cmd || rd_rises == 0) return 0;
  uint32_t n = (uint32_t)rd_rises - 1;
  if (last_cmd == 0x70) { static const int id[16] = { 0,0,0,0,1,1,0,1, 0,0,0,0,0,1,0,0 }; return n < 16 ? id[n] : 0; }
  if (last_cmd == 0x92) { if (n < 8) return 0; uint32_t m = n - 8; return m < 112 * 8 ? (otp_model((int)(m / 8)) >> (7 - m % 8)) & 1 : 0; }
  return 0;
}
// register ids: bits 12..15 kind (0 P, 1 P_SET, 2 P_CLR, 3 CMU), 8..11 port, 0..3 reg (0 MODEL 1 MODEH 2 DOUT 3 DIN)
uint32_t sim_rd(uint32_t id)
{
  cycles += 4;
  uint32_t kind = id >> 12, port = (id >> 8) & 15, reg = id & 15;
  if (kind == 3) return clken;
  if (kind == 0) {
    if (reg == 0) return modeL[port];
    if (reg == 1) return modeH[port];
    if (reg == 2) return dout[port];
    if (reg == 3) {
      uint32_t v = 0;
      for (int i = 0; i < 16; i++) {
        int bit = 0;
        if (mode_of((int)port, i) == 4) bit = pin((int)port, i);
        if (port == 0 && i == 7) bit = busy_now();
        if (port == 2 && i == 0 && mode_of(2, 0) != 4) bit = sda_bit();
        v |= (uint32_t)bit << i;
      }
      return v;
    }
  }
  return 0;
}
void sim_wr(uint32_t id, uint32_t v)
{
  cycles += 4;
  uint32_t kind = id >> 12, port = (id >> 8) & 15, reg = id & 15;
  if (kind == 3) { clken = v; return; }
  if (kind == 0) { if (reg == 0) modeL[port] = v; if (reg == 1) modeH[port] = v; if (reg == 2) dout[port] = v; }
  if (kind == 1 && reg == 2) dout[port] |= v;
  if (kind == 2 && reg == 2) dout[port] &= ~v;
  panel_update();
}
static void init_regs(void)
{
  for (int p = 0; p < 4; p++) {
    PortRegs *t[3] = { &g_gpio.P[p], &g_gpio.P_SET[p], &g_gpio.P_CLR[p] };
    for (int k = 0; k < 3; k++) {
      t[k]->MODEL.id = (uint32_t)(k << 12 | p << 8 | 0); t[k]->MODEH.id = (uint32_t)(k << 12 | p << 8 | 1);
      t[k]->DOUT.id = (uint32_t)(k << 12 | p << 8 | 2); t[k]->DIN.id = (uint32_t)(k << 12 | p << 8 | 3);
    }
  }
  g_cmu.CLKEN0.id = 3u << 12;
}

// ---- run
static int run_once(const char *name, bool connected, uint32_t free_pattern)
{
  memset(modeL, 0, sizeof modeL); memset(modeH, 0, sizeof modeH); memset(dout, 0, sizeof dout);
  cycles = 0; extra_us = 5000000; busy_forever = true; busy_until_us = 0; prev_rst = prev_sclk = 0; nbits = 0; have_cmd = 0; violations = 0; pending_busy_us = 0;
  img_active = 0; nimg = 0; memset(img, 0xEE, 46080 + 128);
  g_connected = connected; g_free_us = 0;
  char fn[64]; snprintf(fn, sizeof fn, "panel_cmds_%s.log", name);
  logf = fopen(fn, "w");
  memset((void *)&g_diag, 0, sizeof g_diag);
  panel_start();
  uint64_t t0 = now_us(), maxblock = 0; uint64_t polls = 0, gated_skips = 0;
  while (!panel_done() && now_us() - t0 < 120000000ull) {
    extra_us += 20;                             // the rest of the main loop
    if (connected) g_free_us = ((polls * 7) % 10 < free_pattern) ? 9000 : 1500;   // radio-free time varies
    uint64_t a = now_us();
    uint32_t line_before = g_diag.pn_line;
    panel_poll();
    uint64_t d = now_us() - a;
    if (connected && g_free_us < STEP_FREE_US && d > 100) VIOL("a step of %lu us ran with only %u us radio-free time", (unsigned long)d, g_free_us);
    (void)line_before;
    if (d > maxblock) maxblock = d;
    polls++;
    (void)gated_skips;
  }
  flush_cmd(); fclose(logf);
  printf("[%s] finished=%d error=%u chip=%04X lines=%u refresh=%u ms total sim=%.1f s | longest panel_poll call %lu us | polls %lu | image bytes %u | violations %d\n",
         name, panel_done(), g_diag.pn_err, g_diag.pn_chip, g_diag.pn_line, g_diag.pn_refresh_ms, (now_us() - t0) / 1e6, (unsigned long)maxblock,
         (unsigned long)polls, nimg, violations);
  printf("      pins at end: onoff=%d rst=%d cs=%d dc=%d sclk=%d sda=%d | OTP in firmware copy ok: ", pin(2, 6), pin(2, 4), pin(2, 2), pin(2, 3), pin(2, 1), pin(2, 0));
  int ok = 1; for (int i = 0; i < 112; i++) if (otp[i] != otp_model(i)) ok = 0;
  printf("%s\n", ok ? "yes" : "NO");
  return violations + (panel_done() ? 0 : 1) + (g_diag.pn_err ? 1 : 0) + (nimg != 46080);
}

int main(void)
{
  init_regs();
  img = (uint8_t *)malloc(46080 + 128);
  int bad = 0;
  bad += run_once("adv", false, 0);
  FILE *f = fopen("panel_image_adv.bin", "wb"); fwrite(img, 1, 46080, f); fclose(f);
  bad += run_once("conn", true, 4);              // connected, free time is large 40 % of the calls
  f = fopen("panel_image_conn.bin", "wb"); fwrite(img, 1, 46080, f); fclose(f);
  printf("%s\n", bad ? "FAILED" : "ALL OK");
  return bad != 0;
}
