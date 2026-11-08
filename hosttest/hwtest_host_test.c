// Host test of ble_hwtest.c (clock test and pin monitor) against a small model of the oscillators, the calibration counter and the GPIO pins.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "em_device.h"
#include "rail.h"
#include "ble_hwtest.h"

LfrcoT g_lfrco; LfxoT g_lfxo; HfxoT g_hfxo; DevinfoT g_devinfo; CmuT g_cmu; GpioT g_gpio;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- model
static uint32_t now_us, step_us = 50;
static struct { int xtal, fail; double hf_ppm, lfxo_ppm, lfrco_hz, ulfrco_hz; } sc;
static uint32_t lfxo_en_t, lfrco_en_t, cal_end;
static int cal_run, lfxo_en, lfrco_en;
static uint32_t cal_cnt;
static int ext[4][16];                       // external drive per pin: -1 nothing, 0 or 1
static int cal_starts;

static const int psize[4] = { 9, 5, 8, 4 };

static void sim(void)
{
  // LFXO
  if ((g_cmu.CLKEN0 & CMU_CLKEN0_LFXO) && (g_lfxo.CTRL & LFXO_CTRL_FORCEEN)) {
    if (!lfxo_en) { lfxo_en = 1; lfxo_en_t = now_us; g_lfxo.STATUS |= LFXO_STATUS_ENS; }
    if (sc.xtal && now_us - lfxo_en_t >= 400000u) g_lfxo.STATUS |= LFXO_STATUS_RDY;
    if (sc.fail && now_us - lfxo_en_t >= 300000u) g_lfxo.IF |= LFXO_IF_FAIL;
  } else if (lfxo_en && !(g_lfxo.CTRL & LFXO_CTRL_FORCEEN)) {
    lfxo_en = 0; g_lfxo.STATUS &= ~(LFXO_STATUS_ENS | LFXO_STATUS_RDY);
  }
  if (g_lfxo.IF_CLR & LFXO_IF_FAIL) { g_lfxo.IF &= ~LFXO_IF_FAIL; g_lfxo.IF_CLR = 0; }
  // LFRCO
  if ((g_cmu.CLKEN0 & CMU_CLKEN0_LFRCO) && (g_lfrco.CTRL & LFRCO_CTRL_FORCEEN)) {
    if (!lfrco_en) { lfrco_en = 1; lfrco_en_t = now_us; g_lfrco.STATUS |= LFRCO_STATUS_ENS; }
    if (now_us - lfrco_en_t >= 1000u) g_lfrco.STATUS |= LFRCO_STATUS_RDY;
  } else if (lfrco_en && !(g_lfrco.CTRL & LFRCO_CTRL_FORCEEN)) {
    lfrco_en = 0; g_lfrco.STATUS &= ~(LFRCO_STATUS_ENS | LFRCO_STATUS_RDY);
  }
  // calibration counter (counts one cycle more than asked, like some chips do)
  if (g_cmu.IF_CLR & CMU_IF_CALRDY) { g_cmu.IF &= ~CMU_IF_CALRDY; g_cmu.IF_CLR = 0; }
  if (g_cmu.CALCMD & CMU_CALCMD_CALSTOP) { cal_run = 0; g_cmu.CALCMD = 0; }
  if (g_cmu.CALCMD & CMU_CALCMD_CALSTART) {
    g_cmu.CALCMD = 0;
    uint32_t down = g_cmu.CALCTRL & _CMU_CALCTRL_DOWNSEL_MASK, cycles = g_cmu.CALCTRL & _CMU_CALCTRL_CALTOP_MASK;
    double fd = 0;
    if (down == CMU_CALCTRL_DOWNSEL_LFXO && (g_lfxo.STATUS & LFXO_STATUS_RDY)) fd = 32768.0 * (1 + sc.lfxo_ppm * 1e-6);
    if (down == CMU_CALCTRL_DOWNSEL_LFRCO && (g_lfrco.STATUS & LFRCO_STATUS_ENS)) fd = sc.lfrco_hz;
    if (down == CMU_CALCTRL_DOWNSEL_ULFRCO) fd = sc.ulfrco_hz;
    cal_starts++;
    if (fd > 0) {
      double fh = 38.4e6 * (1 + sc.hf_ppm * 1e-6);
      cal_run = 1; cal_end = now_us + (uint32_t)((cycles + 1) / fd * 1e6); cal_cnt = (uint32_t)floor(fh * (cycles + 1) / fd + 0.5);
    } else cal_run = 0;                                    // dead clock: never finishes
  }
  if (cal_run && (int32_t)(now_us - cal_end) >= 0) { g_cmu.IF |= CMU_IF_CALRDY; g_cmu.CALCNT = cal_cnt; cal_run = 0; }
  // pins
  for (int p = 0; p < 4; p++) {
    uint32_t din = 0;
    for (int n = 0; n < psize[p]; n++) {
      uint32_t mode = (n < 8 ? g_gpio.P[p].MODEL >> (n * 4) : g_gpio.P[p].MODEH >> ((n - 8) * 4)) & 0xFu;
      int v = 0;
      if (ext[p][n] >= 0) v = ext[p][n];
      else if (mode == 2) v = (g_gpio.P[p].DOUT >> n) & 1u;
      if (v) din |= 1u << n;
    }
    g_gpio.P[p].DIN = din;
  }
}

uint32_t RAIL_GetTime(void) { now_us += step_us; sim(); return now_us; }

static void world_reset(void)
{
  memset(&g_lfrco, 0, sizeof(g_lfrco)); memset(&g_lfxo, 0, sizeof(g_lfxo)); memset(&g_hfxo, 0, sizeof(g_hfxo));
  memset(&g_cmu, 0, sizeof(g_cmu)); memset(&g_gpio, 0, sizeof(g_gpio));
  g_hfxo.STATUS = HFXO_STATUS_RDY; g_hfxo.XTALCTRL = (0x8Cu << 8) | (0x7Au << 16);
  g_devinfo.MODXOCAL = 0x55u | (0x61u << 8);
  g_cmu.SYSCLKCTRL = 3; g_cmu.EM23GRPACLKCTRL = 1; g_cmu.RTCCCLKCTRL = 1;
  lfxo_en = lfrco_en = cal_run = 0; cal_starts = 0;
  for (int p = 0; p < 4; p++) for (int n = 0; n < 16; n++) ext[p][n] = -1;
  sc.xtal = 1; sc.fail = 0; sc.hf_ppm = 5; sc.lfxo_ppm = 20; sc.lfrco_hz = 32768.0 * 0.99; sc.ulfrco_hz = 1100.0;
}

static void run_clock(void)
{
  uint32_t t0 = now_us;
  CHECK(hwtest_clock_start(), "start accepted");
  CHECK(!hwtest_clock_start(), "second start while running refused");
  while (hwtest_clock()->state != 2 && now_us - t0 < 6000000u) hwtest_poll();
  CHECK(hwtest_clock()->state == 2, "finished");
}

static void expect_count(uint32_t got, double f_lf, uint32_t down, const char *what)
{
  double want = 38.4e6 * (1 + sc.hf_ppm * 1e-6) * (down / 2.0) / f_lf;
  CHECK(fabs((double)got - want) <= 1.01, "%s count %u want %.1f", what, got, want);
}

static void test_clock_ok(void)
{
  printf("clock: crystal present\n");
  world_reset();
  uint32_t t0 = now_us;
  run_clock();
  const hwtest_clock_t *c = hwtest_clock();
  CHECK(c->flags & 0x01, "HFXO ready flag");
  CHECK(!(c->flags & 0x02), "LFXO was not on before");
  CHECK((c->flags & 0x04) && !(c->flags & 0x08) && !(c->flags & 0x80), "LFXO started, flags %02x", c->flags);
  CHECK((c->flags & 0x70) == 0x70, "all three counts valid, flags %02x", c->flags);
  CHECK(c->lfxo_ms >= 399 && c->lfxo_ms <= 420, "start time %u ms", c->lfxo_ms);
  expect_count(c->cnt_lfxo, 32768.0 * (1 + sc.lfxo_ppm * 1e-6), CT_DOWN_LFXO, "LFXO");
  expect_count(c->cnt_lfrco, sc.lfrco_hz, CT_DOWN_LFRCO, "LFRCO");
  expect_count(c->cnt_ulfrco, sc.ulfrco_hz, CT_DOWN_ULFRCO, "ULFRCO");
  CHECK(c->ctune_xi == 0x8C && c->ctune_xo == 0x7A && c->fact_xi == 0x55 && c->fact_xo == 0x61, "HFXO tuning %02x %02x %02x %02x", c->ctune_xi, c->ctune_xo, c->fact_xi, c->fact_xo);
  CHECK(c->sel == (3 | (1 << 3) | (1 << 5)), "clock selections %02x", c->sel);
  CHECK(!(g_cmu.CLKEN0 & CMU_CLKEN0_LFXO) && !(g_cmu.CLKEN0 & CMU_CLKEN0_LFRCO), "bus clocks put back");
  CHECK(!(g_lfxo.CTRL & LFXO_CTRL_FORCEEN) && !(g_lfrco.CTRL & LFRCO_CTRL_FORCEEN), "oscillators stopped again");
  CHECK(now_us - t0 < 1000000u, "took %u us", now_us - t0);
  CHECK(cal_starts == 6, "six measurements, %d", cal_starts);
}

static void test_clock_nocrystal(void)
{
  printf("clock: no crystal\n");
  world_reset(); sc.xtal = 0;
  uint32_t t0 = now_us;
  run_clock();
  const hwtest_clock_t *c = hwtest_clock();
  CHECK((c->flags & 0x80) && !(c->flags & 0x04) && !(c->flags & 0x10), "timeout flag, no LFXO count: %02x", c->flags);
  CHECK(c->lfxo_ms == 0xFFFF, "no start time");
  CHECK((c->flags & 0x60) == 0x60, "LFRCO and ULFRCO still measured: %02x", c->flags);
  CHECK(c->cnt_lfxo == 0, "LFXO count zero");
  CHECK(!(g_cmu.CLKEN0 & CMU_CLKEN0_LFXO) && !(g_lfxo.CTRL & LFXO_CTRL_FORCEEN), "LFXO off again");
  CHECK(now_us - t0 > 2000000u && now_us - t0 < 2400000u, "took %u us", now_us - t0);
  CHECK(cal_starts == 4, "LFXO skipped, %d measurements", cal_starts);
}

static void test_clock_fail(void)
{
  printf("clock: failure flag\n");
  world_reset(); sc.xtal = 0; sc.fail = 1;
  uint32_t t0 = now_us;
  run_clock();
  const hwtest_clock_t *c = hwtest_clock();
  CHECK((c->flags & 0x08) && !(c->flags & 0x80), "failure flag instead of timeout: %02x", c->flags);
  CHECK(now_us - t0 < 800000u, "stopped waiting early, %u us", now_us - t0);
  RAIL_GetTime();                                              // let the model see the clear request
  CHECK(!(g_lfxo.IF & LFXO_IF_FAIL), "failure flag cleared");
}

static void test_clock_already_on(void)
{
  printf("clock: LFXO already used by someone else\n");
  world_reset();
  g_cmu.CLKEN0 = CMU_CLKEN0_LFXO; g_lfxo.CTRL = LFXO_CTRL_FORCEEN; g_lfxo.CAL = 0x1234; g_lfxo.STATUS = LFXO_STATUS_ENS | LFXO_STATUS_RDY;
  lfxo_en = 1;
  run_clock();
  const hwtest_clock_t *c = hwtest_clock();
  CHECK(c->flags & 0x02, "already-on flag");
  CHECK((c->flags & 0x70) == 0x70, "counts valid %02x", c->flags);
  CHECK((g_cmu.CLKEN0 & CMU_CLKEN0_LFXO) && (g_lfxo.CTRL & LFXO_CTRL_FORCEEN), "left running");
  CHECK(g_lfxo.CAL == 0x1234, "its settings not touched");
  expect_count(c->cnt_lfxo, 32768.0 * (1 + sc.lfxo_ppm * 1e-6), CT_DOWN_LFXO, "LFXO");
}

static void test_clock_dead_lfrco(void)
{
  printf("clock: a clock that does not tick\n");
  world_reset(); sc.lfrco_hz = 0;                       // LFRCO never counts
  run_clock();
  CHECK(!(hwtest_clock()->flags & 0x20), "no LFRCO count");
  CHECK((hwtest_clock()->flags & 0x50) == 0x50, "the others are fine %02x", hwtest_clock()->flags);
}

// ---- pins
static uint32_t PA(int n) { return 1u << n; }
static uint32_t PB(int n) { return 1u << (9 + n); }
static uint32_t PC(int n) { return 1u << (14 + n); }
static uint32_t PD(int n) { return 1u << (22 + n); }

static void settle(uint32_t us) { uint32_t t0 = now_us; while (now_us - t0 < us) { RAIL_GetTime(); hwtest_poll(); } }

static void test_pins(void)
{
  printf("pins\n");
  world_reset();
  // the pins of this firmware carry a recognisable setting that must survive
  g_gpio.P[0].MODEL = 0x4111A1A4u; g_gpio.P[0].DOUT = 0x0A0u; g_gpio.P[1].MODEL = 0x00044444u; g_gpio.P[2].MODEL = 0x74444444u; g_gpio.P[2].DOUT = 0x1Fu;
  g_gpio.P[3].MODEL = 0x00003333u; g_gpio.P[3].DOUT = 0x5u;
  GpioT saved = g_gpio;

  CHECK(hwtest_pins_start(PM_MODE_FLOAT, 0xFFFFFFFFu), "start float");
  uint32_t mask = hwtest_pins_mask();
  uint32_t want = PA(6) | PA(8) | PB(1) | PB(2) | PB(3) | PB(4) | PC(5) | PC(7) | PD(0) | PD(1) | PD(2) | PD(3);
  CHECK(mask == want, "watched mask %08x want %08x", mask, want);
  CHECK((hwtest_pins_owned() & (PA(0) | PA(4) | PA(5) | PA(7) | PB(0) | PC(0) | PC(1) | PC(2) | PC(3) | PC(4) | PC(6))) == (PA(0) | PA(4) | PA(5) | PA(7) | PB(0) | PC(0) | PC(1) | PC(2) | PC(3) | PC(4) | PC(6)), "owned mask");
  settle(10000);
  // owned pins and SWD pins untouched while running
  CHECK((g_gpio.P[1].MODEL & 0xFu) == (saved.P[1].MODEL & 0xFu), "PB0 untouched");
  CHECK((g_gpio.P[0].MODEL & 0xF0FFFFFFu) == (saved.P[0].MODEL & 0xF0FFFFFFu), "PA0..PA5 and PA7 untouched (PA6 is watched)");
  CHECK((g_gpio.P[0].MODEH & 0xFu) == 1u && ((g_gpio.P[0].MODEL >> 24) & 0xFu) == 1u, "PA6 and PA8 are floating inputs");
  CHECK((g_gpio.P[2].MODEL & 0x0F0FFFFFu) == (saved.P[2].MODEL & 0x0F0FFFFFu), "PC0..PC4 and PC6 untouched");
  CHECK((g_gpio.P[2].MODEL >> 28) == 1u && ((g_gpio.P[2].MODEL >> 20) & 0xFu) == 1u, "PC5 and PC7 are floating inputs: %08x", g_gpio.P[2].MODEL);
  CHECK(((g_gpio.P[3].MODEL) & 0xFFFFu) == 0x1111u, "PD0..PD3 are floating inputs: %08x", g_gpio.P[3].MODEL);
  CHECK(hwtest_pins()->level == 0 && hwtest_pins()->edges == 0, "quiet at first");
  CHECK(hwtest_pins_changed(), "start is reported");
  CHECK(!hwtest_pins_changed(), "reported once");

  ext[3][2] = 1; settle(5000);                                  // PD2 goes high
  CHECK(hwtest_pins()->level == PD(2), "level shows PD2");
  CHECK(hwtest_pins()->edges == 1 && hwtest_pins()->last == 24, "one edge on pin 24, edges %u last %u", hwtest_pins()->edges, hwtest_pins()->last);
  CHECK((hwtest_pins()->seen_hi & PD(2)) && hwtest_pins_changed(), "low at the start, now high: seen both ways, reported");
  ext[3][2] = 0; settle(5000);
  CHECK(!hwtest_pins_changed(), "not reported a second time");
  CHECK(((hwtest_pins()->seen_hi & hwtest_pins()->seen_lo) & PD(2)) != 0, "PD2 in both masks");
  CHECK(hwtest_pins()->edges == 2, "two edges");
  hwtest_pins_clear(); settle(8000);
  CHECK(hwtest_pins()->edges == 0 && (hwtest_pins()->seen_hi & hwtest_pins()->seen_lo) == 0 && hwtest_pins()->last == 0xFF, "history cleared");

  // pull-up: undriven pins read high, a pin driven low shows
  ext[3][2] = -1;
  CHECK(hwtest_pins_start(PM_MODE_PULLUP, 0xFFFFFFFFu), "start pull-up");
  settle(8000);
  CHECK(hwtest_pins()->level == mask, "all high with pull-ups: %08x", hwtest_pins()->level);
  ext[1][3] = 0; settle(3000);
  CHECK(!(hwtest_pins()->level & PB(3)) && hwtest_pins()->last == 12, "PB3 pulled low, last %u", hwtest_pins()->last);
  ext[1][3] = -1;
  CHECK(hwtest_pins_start(PM_MODE_PULLDN, 0xFFFFFFFFu), "start pull-down");
  settle(8000);
  CHECK(hwtest_pins()->level == 0, "all low with pull-downs");

  // SWD pads only when asked
  CHECK(hwtest_pins_start(PM_MODE_FLOAT | PM_FLAG_SWD, 0xFFFFFFFFu), "start with SWD");
  CHECK(hwtest_pins_mask() == (want | PA(1) | PA(2) | PA(3)), "SWD pins added: %08x", hwtest_pins_mask());

  // stop puts everything back
  CHECK(hwtest_pins_start(PM_MODE_OFF, 0xFFFFFFFFu), "stop");
  CHECK(g_gpio.P[0].MODEL == saved.P[0].MODEL && g_gpio.P[1].MODEL == saved.P[1].MODEL && g_gpio.P[2].MODEL == saved.P[2].MODEL && g_gpio.P[3].MODEL == saved.P[3].MODEL, "modes restored");
  CHECK(g_gpio.P[0].DOUT == saved.P[0].DOUT && g_gpio.P[1].DOUT == saved.P[1].DOUT && g_gpio.P[2].DOUT == saved.P[2].DOUT && g_gpio.P[3].DOUT == saved.P[3].DOUT, "outputs restored");
  CHECK(hwtest_pins()->mode == PM_MODE_OFF, "mode off");

  // connection ends -> restored as well
  CHECK(hwtest_pins_start(PM_MODE_PULLUP, 0xFFFFFFFFu), "restart");
  settle(5000);
  hwtest_reset();
  CHECK(g_gpio.P[3].MODEL == saved.P[3].MODEL && g_gpio.P[3].DOUT == saved.P[3].DOUT && g_gpio.P[0].MODEL == saved.P[0].MODEL, "restored by reset");
  CHECK(hwtest_pins()->mode == PM_MODE_OFF, "off after reset");

  // five minutes limit
  CHECK(hwtest_pins_start(PM_MODE_FLOAT, 0xFFFFFFFFu), "restart for the time limit");
  hwtest_pins_changed();
  step_us = 5000; settle((PM_MAX_S + 2) * 1000000u); step_us = 50;
  CHECK(hwtest_pins()->mode == PM_MODE_OFF && hwtest_pins_changed(), "stopped by itself and reported");
  CHECK(g_gpio.P[3].MODEL == saved.P[3].MODEL, "restored after the limit");

  CHECK(!hwtest_pins_start(9, 0xFFFFFFFFu), "bad mode refused");

  // only some pins: the others keep their setting
  g_gpio = saved;
  CHECK(hwtest_pins_start(PM_MODE_PULLUP, PD(2) | PD(3) | PA(1)), "start with a pin selection");
  CHECK(hwtest_pins_mask() == (PD(2) | PD(3)), "selection limited to watched pins, SWD pin dropped: %08x", hwtest_pins_mask());
  CHECK((g_gpio.P[3].MODEL & 0xFFu) == saved.P[3].MODEL % 256 && ((g_gpio.P[3].MODEL >> 8) & 0xFu) == 2u && ((g_gpio.P[3].MODEL >> 12) & 0xFu) == 2u, "only PD2 and PD3 became pull-up inputs: %08x", g_gpio.P[3].MODEL);
  CHECK((g_gpio.P[1].MODEL & 0xFFFFFFF0u) == (saved.P[1].MODEL & 0xFFFFFFF0u) && g_gpio.P[0].MODEL == saved.P[0].MODEL, "other ports untouched");
  CHECK(hwtest_pins_start(PM_MODE_OFF, 0), "stop again");
  CHECK(g_gpio.P[3].MODEL == saved.P[3].MODEL && g_gpio.P[3].DOUT == saved.P[3].DOUT, "selection restored");
  CHECK(!hwtest_pins_start(PM_MODE_FLOAT, PA(1)), "a selection with nothing watchable is refused");

}

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  test_clock_ok();
  test_clock_nocrystal();
  test_clock_fail();
  test_clock_already_on();
  test_clock_dead_lfrco();
  test_pins();
  if (fails) { printf("%d FAILED\n", fails); return 1; }
  printf("ALL OK\n");
  return 0;
}
