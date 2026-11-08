// SES V300 (not badge154): clock test and pin monitor, see ble_hwtest.h
#include <string.h>
#include "ble_hwtest.h"
#include "rail.h"
#include "em_device.h"

// =====================================================================================================================
// clock test
// =====================================================================================================================
// The radio runs on the 38.4 MHz crystal (HFXO), so it is the reference. CMU calibration counts HFXO cycles while a counter
// runs down a given number of cycles of the clock under test. Whether the counter runs N or N+1 cycles does not matter:
// every clock is measured twice (N and N/2 cycles) and the difference of the two counts is used (HFXO cycles per N/2 cycles).
#define LFXO_GAIN       2u            // oscillator gain setting (2 is the usual one for 32.768 kHz crystals)
#define LFXO_CAPTUNE    0x30u         // load capacitance setting, 0.25 pF per step
#define LFXO_START_US   2000000u      // give the crystal 2 s to start
#define CAL_MAX_US      150000u

typedef enum { CT_IDLE = 0, CT_LFXO_WAIT, CT_LFRCO_WAIT, CT_MEAS, CT_CLEAN } ct_state_t;

static hwtest_clock_t ck;
static ct_state_t ct;
static uint32_t ct_t0;
static uint8_t meas_i, meas_started;
static uint32_t cnt[6];
static bool cnt_ok[6];
static bool lfxo_ready, lfrco_ready;
static bool we_clk_lfxo, we_clk_lfrco, we_force_lfrco, we_force_lfxo;

static const struct { uint32_t downsel; uint32_t cycles; uint8_t clk; } meas[6] = {
  { CMU_CALCTRL_DOWNSEL_LFXO,   CT_DOWN_LFXO,        0 }, { CMU_CALCTRL_DOWNSEL_LFXO,   CT_DOWN_LFXO / 2,   0 },
  { CMU_CALCTRL_DOWNSEL_LFRCO,  CT_DOWN_LFRCO,       1 }, { CMU_CALCTRL_DOWNSEL_LFRCO,  CT_DOWN_LFRCO / 2,  1 },
  { CMU_CALCTRL_DOWNSEL_ULFRCO, CT_DOWN_ULFRCO,      2 }, { CMU_CALCTRL_DOWNSEL_ULFRCO, CT_DOWN_ULFRCO / 2, 2 },
};

static uint32_t us_since(uint32_t t0) { return (uint32_t)(RAIL_GetTime() - t0); }

bool hwtest_clock_start(void)
{
  if (ct != CT_IDLE) return false;
  memset(&ck, 0, sizeof(ck));
  memset(cnt, 0, sizeof(cnt));
  memset(cnt_ok, 0, sizeof(cnt_ok));
  lfxo_ready = lfrco_ready = false;
  we_clk_lfxo = we_clk_lfrco = we_force_lfrco = we_force_lfxo = false;
  meas_i = 0; meas_started = 0;
  ck.state = 1;
  ck.lfxo_ms = 0xFFFF;
  ck.captune = LFXO_CAPTUNE;

  if (HFXO0->STATUS & HFXO_STATUS_RDY) ck.flags |= 0x01;
  ck.sel = (uint8_t)((CMU->SYSCLKCTRL & 7u) | ((CMU->EM23GRPACLKCTRL & 3u) << 3) | ((CMU->RTCCCLKCTRL & 3u) << 5));
  ck.ctune_xi = (uint8_t)((HFXO0->XTALCTRL & _HFXO_XTALCTRL_CTUNEXIANA_MASK) >> _HFXO_XTALCTRL_CTUNEXIANA_SHIFT);
  ck.ctune_xo = (uint8_t)((HFXO0->XTALCTRL & _HFXO_XTALCTRL_CTUNEXOANA_MASK) >> _HFXO_XTALCTRL_CTUNEXOANA_SHIFT);
  ck.fact_xi = (uint8_t)((DEVINFO->MODXOCAL >> _DEVINFO_MODXOCAL_HFXOCTUNEXIANA_SHIFT) & 0xFFu);
  ck.fact_xo = (uint8_t)((DEVINFO->MODXOCAL >> _DEVINFO_MODXOCAL_HFXOCTUNEXOANA_SHIFT) & 0xFFu);

  // LFXO: use it as it is if something already runs it, otherwise start it with our own settings
  if ((CMU->CLKEN0 & CMU_CLKEN0_LFXO) && (LFXO->STATUS & LFXO_STATUS_ENS)) {
    ck.flags |= 0x02;
    we_clk_lfxo = false;
  } else {
    if (!(CMU->CLKEN0 & CMU_CLKEN0_LFXO)) { CMU->CLKEN0 |= CMU_CLKEN0_LFXO; we_clk_lfxo = true; }
    LFXO->LOCK = LFXO_LOCK_LOCKKEY_UNLOCK;
    LFXO->CTRL |= LFXO_CTRL_DISONDEMAND;
    LFXO->CTRL &= ~LFXO_CTRL_FORCEEN;
    for (uint32_t i = 0; i < 20000u && (LFXO->STATUS & LFXO_STATUS_ENS); i++) { }
    LFXO->CAL = (LFXO_GAIN << _LFXO_CAL_GAIN_SHIFT) | (LFXO_CAPTUNE << _LFXO_CAL_CAPTUNE_SHIFT);
    LFXO->CFG = LFXO_CFG_MODE_XTAL | (_LFXO_CFG_TIMEOUT_CYCLES32K << _LFXO_CFG_TIMEOUT_SHIFT);
    LFXO->IF_CLR = LFXO_IF_FAIL;
    LFXO->CTRL = LFXO_CTRL_FAILDETEN | LFXO_CTRL_DISONDEMAND | LFXO_CTRL_FORCEEN;
    we_force_lfxo = true;
  }
  ct_t0 = RAIL_GetTime();
  ct = CT_LFXO_WAIT;
  return true;
}

static void cal_begin(uint32_t downsel, uint32_t cycles)
{
  CMU->CALCMD = CMU_CALCMD_CALSTOP;
  CMU->IF_CLR = CMU_IF_CALRDY;                           // the flag is cleared by hand: no leftover of the previous measurement
  uint32_t c = CMU->CALCTRL & ~(_CMU_CALCTRL_UPSEL_MASK | _CMU_CALCTRL_DOWNSEL_MASK | _CMU_CALCTRL_CALTOP_MASK | CMU_CALCTRL_CONT);
  CMU->CALCTRL = c | CMU_CALCTRL_UPSEL_HFXO | downsel | cycles;
  CMU->CALCMD = CMU_CALCMD_CALSTART;
}

static void ct_finish(void)
{
  // differences of the two counts of each clock
  static const uint8_t flag_bit[3] = { 0x10, 0x20, 0x40 };
  uint32_t out[3] = { 0, 0, 0 };
  for (int k = 0; k < 3; k++) {
    if (cnt_ok[2 * k] && cnt_ok[2 * k + 1] && cnt[2 * k] > cnt[2 * k + 1]) { out[k] = cnt[2 * k] - cnt[2 * k + 1]; ck.flags |= flag_bit[k]; }
  }
  ck.cnt_lfxo = out[0]; ck.cnt_lfrco = out[1]; ck.cnt_ulfrco = out[2];
  ck.state = 2;
  ct = CT_IDLE;
}

static void ct_poll(void)
{
  switch (ct) {
    case CT_IDLE: break;

    case CT_LFXO_WAIT:
      if (LFXO->STATUS & LFXO_STATUS_RDY) {
        lfxo_ready = true;
        ck.flags |= 0x04;
        uint32_t ms = us_since(ct_t0) / 1000u;
        ck.lfxo_ms = (uint16_t)(ms > 0xFFFEu ? 0xFFFEu : ms);
      } else if (LFXO->IF & LFXO_IF_FAIL) {
        ck.flags |= 0x08;
      } else if (us_since(ct_t0) < LFXO_START_US) {
        break;                                            // still waiting
      } else {
        ck.flags |= 0x80;
      }
      // LFRCO: needs its bus clock and a force-enable if nothing else keeps it running
      if (!(CMU->CLKEN0 & CMU_CLKEN0_LFRCO)) { CMU->CLKEN0 |= CMU_CLKEN0_LFRCO; we_clk_lfrco = true; }
      if (!(LFRCO->STATUS & LFRCO_STATUS_ENS)) { LFRCO->CTRL |= LFRCO_CTRL_FORCEEN; we_force_lfrco = true; }
      ct_t0 = RAIL_GetTime();
      ct = CT_LFRCO_WAIT;
      break;

    case CT_LFRCO_WAIT:
      if (LFRCO->STATUS & LFRCO_STATUS_RDY) lfrco_ready = true;
      else if (us_since(ct_t0) < 50000u) break;
      meas_i = 0; meas_started = 0;
      ct = CT_MEAS;
      break;

    case CT_MEAS:
      if (meas_i >= 6) { ct = CT_CLEAN; break; }
      if ((meas[meas_i].clk == 0 && !lfxo_ready) || (meas[meas_i].clk == 1 && !lfrco_ready)) { meas_i++; break; }
      if (!meas_started) {
        cal_begin(meas[meas_i].downsel, meas[meas_i].cycles);
        ct_t0 = RAIL_GetTime();
        meas_started = 1;
        break;
      }
      {
        uint32_t dt = us_since(ct_t0);
        if (CMU->IF & CMU_IF_CALRDY) {
          cnt[meas_i] = CMU->CALCNT & _CMU_CALCNT_CALCNT_MASK;
          cnt_ok[meas_i] = true;
          meas_i++; meas_started = 0;
        } else if (dt > CAL_MAX_US) {                     // the clock under test does not run
          CMU->CALCMD = CMU_CALCMD_CALSTOP;
          meas_i++; meas_started = 0;
        }
      }
      break;

    case CT_CLEAN:
      CMU->CALCMD = CMU_CALCMD_CALSTOP;
      if (we_force_lfxo) {
        LFXO->CTRL |= LFXO_CTRL_DISONDEMAND;
        LFXO->CTRL &= ~LFXO_CTRL_FORCEEN;
        LFXO->IF_CLR = LFXO_IF_FAIL;
      }
      if (we_clk_lfxo) CMU->CLKEN0 &= ~CMU_CLKEN0_LFXO;
      if (we_force_lfrco) LFRCO->CTRL &= ~LFRCO_CTRL_FORCEEN;
      if (we_clk_lfrco) CMU->CLKEN0 &= ~CMU_CLKEN0_LFRCO;
      ct_finish();
      break;
  }
}

const hwtest_clock_t *hwtest_clock(void) { return &ck; }

// =====================================================================================================================
// pin monitor
// =====================================================================================================================
static const uint8_t port_first[4] = { 0, 9, 14, 22 };
static const uint8_t port_size[4]  = { 9, 5, 8, 4 };

#define PIN(port, n) (1u << (port_first[port] + (n)))
// pins this firmware drives or reads: flash (PA0 CLK, PA4 MISO, PA5 CS, PB0 MOSI), panel (PA7 BUSY, PC0..PC4, PC6)
#define OWNED_MASK (PIN(0, 0) | PIN(0, 4) | PIN(0, 5) | PIN(0, 7) | PIN(1, 0) | PIN(2, 0) | PIN(2, 1) | PIN(2, 2) | PIN(2, 3) | PIN(2, 4) | PIN(2, 6))
#define SWD_MASK   (PIN(0, 1) | PIN(0, 2) | PIN(0, 3))
#define ALL_MASK   ((1u << PM_PINS) - 1u)
#define SETTLE_US  3000u

static hwtest_pins_t pm;
static uint32_t pm_mask, pm_prev, pm_t0, pm_settle_until;
static bool pm_run, pm_baseline, pm_changed;
static uint32_t sv_model[4], sv_modeh[4], sv_dout[4];

uint32_t hwtest_pins_owned(void) { return OWNED_MASK; }
uint32_t hwtest_pins_mask(void) { return pm_mask; }
const hwtest_pins_t *hwtest_pins(void) { return &pm; }
bool hwtest_pins_changed(void) { bool c = pm_changed; pm_changed = false; return c; }

static void set_field(volatile uint32_t *model, volatile uint32_t *modeh, int pin, uint32_t mode)
{
  if (pin < 8) *model = (*model & ~(0xFu << (pin * 4))) | (mode << (pin * 4));
  else         *modeh = (*modeh & ~(0xFu << ((pin - 8) * 4))) | (mode << ((pin - 8) * 4));
}

static uint32_t pm_read(void)
{
  uint32_t lv = 0;
  for (int p = 0; p < 4; p++) lv |= ((uint32_t)GPIO->P[p].DIN & ((1u << port_size[p]) - 1u)) << port_first[p];
  return lv & pm_mask;
}

static void pins_restore(uint32_t mask)
{
  for (int p = 0; p < 4; p++) {
    for (int n = 0; n < port_size[p]; n++) {
      if (!(mask & PIN(p, n))) continue;
      uint32_t m = n < 8 ? (sv_model[p] >> (n * 4)) & 0xFu : (sv_modeh[p] >> ((n - 8) * 4)) & 0xFu;
      uint32_t d = (sv_dout[p] >> n) & 1u;
      if (d) GPIO->P[p].DOUT |= 1u << n; else GPIO->P[p].DOUT &= ~(1u << n);
      set_field(&GPIO->P[p].MODEL, &GPIO->P[p].MODEH, n, m);
    }
  }
}

static void pm_restore(void) { pins_restore(pm_mask); }

void hwtest_pins_clear(void)
{
  if (!pm_run) return;
  pm.seen_hi = pm.seen_lo = 0; pm.edges = 0; pm.last = 0xFF;
  pm_baseline = false;
  pm_t0 = RAIL_GetTime();
  pm_settle_until = pm_t0 + SETTLE_US;
  pm_changed = true;
}

bool hwtest_pins_start(uint8_t mode, uint32_t only)
{
  uint8_t m = mode & 0x7Fu;
  if (m > PM_MODE_PULLDN) return false;
  hwtest_scan_abort();                                       // the scanner and the monitor never use the pins together
  if (pm_run) { pm_restore(); pm_run = false; }
  memset(&pm, 0, sizeof(pm));
  pm.last = 0xFF;
  pm_changed = true;
  if (m == PM_MODE_OFF) { pm_mask = 0; return true; }

  pm_mask = ALL_MASK & ~OWNED_MASK & ~((mode & PM_FLAG_SWD) ? 0u : SWD_MASK) & only;
  if (pm_mask == 0) return false;
  CMU->CLKEN0 |= CMU_CLKEN0_GPIO;
  for (int p = 0; p < 4; p++) { sv_model[p] = GPIO->P[p].MODEL; sv_modeh[p] = GPIO->P[p].MODEH; sv_dout[p] = GPIO->P[p].DOUT; }
  for (int p = 0; p < 4; p++) {
    for (int n = 0; n < port_size[p]; n++) {
      if (!(pm_mask & PIN(p, n))) continue;
      if (m == PM_MODE_PULLUP) GPIO->P[p].DOUT |= 1u << n; else GPIO->P[p].DOUT &= ~(1u << n);
      set_field(&GPIO->P[p].MODEL, &GPIO->P[p].MODEH, n, m == PM_MODE_FLOAT ? 1u : 2u);   // 1 input, 2 input with pull (DOUT picks up/down)
    }
  }
  pm.mode = mode;
  pm_run = true;
  hwtest_pins_clear();
  return true;
}

static void pm_poll(void)
{
  if (!pm_run) return;
  uint32_t now = RAIL_GetTime();
  if ((int32_t)(now - pm_settle_until) < 0) return;           // let the pulls settle
  pm.elapsed_s = (uint16_t)((now - pm_t0) / 1000000u);
  if (pm.elapsed_s >= PM_MAX_S) { pm_restore(); pm_run = false; pm.mode = PM_MODE_OFF; pm_changed = true; return; }

  uint32_t lv = pm_read();
  pm.level = lv;
  uint32_t before = pm.seen_hi & pm.seen_lo;
  pm.seen_hi |= lv; pm.seen_lo |= ~lv & pm_mask;
  if (!pm_baseline) { pm_baseline = true; pm_prev = lv; return; }
  uint32_t ch = lv ^ pm_prev;
  if (ch) {
    for (int i = 0; i < PM_PINS; i++) if (ch & (1u << i)) { pm.last = (uint8_t)i; if (pm.edges < 0xFFFF) pm.edges++; }
    pm_prev = lv;
  }
  if ((pm.seen_hi & pm.seen_lo) != before) pm_changed = true;
}


// =====================================================================================================================
// I2C scanner
// =====================================================================================================================
#define SC_HALF_US    5u            // half a clock period: about 60 kHz
#define SC_ACK_US     150u          // the acknowledge is read at the end of this wait: a line that only rises slowly is not mistaken for an answer
#define SC_STRETCH_US 200u          // how long a clock that does not rise is waited for
#define SC_SETTLE_MAX_US 60000u     // a released line that is still low after this is called stuck
#define SC_PACE_US    1000u         // one address per call, one call per millisecond at most
#define SC_ADDR_FIRST 0x08u
#define SC_ADDR_LAST  0x77u

typedef enum { SS_IDLE = 0, SS_SETUP, SS_SETTLE, SS_ADDR } ss_t;

static hwtest_scan_t sc;
static ss_t ss;
static uint8_t sc_pin[SC_MAX_PINS], sc_n, sc_i, sc_j, sc_addr, sc_sda, sc_scl, sc_acks, sc_ackaddr[3];
static uint32_t sc_mask, sc_t0, sc_next;
static bool sc_changed;

const hwtest_scan_t *hwtest_scan(void) { return &sc; }
bool hwtest_scan_changed(void) { bool c = sc_changed; sc_changed = false; return c; }

static void pin_split(int idx, int *port, int *n)
{
  int p = 3;
  while (p > 0 && idx < port_first[p]) p--;
  *port = p; *n = idx - port_first[p];
}

// pin modes of the series 2 GPIO: 0 off, 1 input, 2 input with pull (DOUT selects up or down), 4 push-pull
static void pin_cfg(int idx, uint32_t mode, uint32_t dout)
{
  int p, n; pin_split(idx, &p, &n);
  if (dout) GPIO->P[p].DOUT |= 1u << n; else GPIO->P[p].DOUT &= ~(1u << n);
  set_field(&GPIO->P[p].MODEL, &GPIO->P[p].MODEH, n, mode);
}
static uint32_t pin_read(int idx) { int p, n; pin_split(idx, &p, &n); return ((uint32_t)GPIO->P[p].DIN >> n) & 1u; }

static void wait_us(uint32_t us) { uint32_t t0 = RAIL_GetTime(); while ((uint32_t)(RAIL_GetTime() - t0) < us) { } }

static void sda_rel(void) { pin_cfg(sc_sda, 2, 1); }
static void sda_low(void) { pin_cfg(sc_sda, 4, 0); }
static void scl_low(void) { pin_cfg(sc_scl, 4, 0); }
static bool scl_rel(void)                                   // false if the clock does not rise (a stuck line or a device that stretches too long)
{
  pin_cfg(sc_scl, 2, 1);
  uint32_t t0 = RAIL_GetTime();
  while (!pin_read(sc_scl)) { if ((uint32_t)(RAIL_GetTime() - t0) > SC_STRETCH_US) return false; }
  return true;
}

// one address write transaction; ok = false if the bus did not behave, *ack = a device answered
static bool i2c_probe(uint8_t addr, bool *ack)
{
  bool ok = true;
  *ack = false;
  sda_rel(); ok &= scl_rel(); wait_us(SC_HALF_US);
  sda_low(); wait_us(SC_HALF_US); scl_low(); wait_us(SC_HALF_US);                     // start
  for (int k = 7; k >= 0; k--) {
    uint8_t bit = (uint8_t)(((addr << 1) >> k) & 1u);                                   // the last bit (k = 0) is the write flag, 0
    if (bit) sda_rel(); else sda_low();
    wait_us(SC_HALF_US); ok &= scl_rel(); wait_us(SC_HALF_US); scl_low(); wait_us(SC_HALF_US);
  }
  sda_rel(); wait_us(SC_HALF_US);
  ok &= scl_rel(); wait_us(SC_HALF_US);
  bool early = pin_read(sc_sda) == 0;
  wait_us(SC_ACK_US);
  *ack = early && pin_read(sc_sda) == 0;                                                // a device holds the line low the whole clock high time
  scl_low(); wait_us(SC_HALF_US);
  sda_low(); wait_us(SC_HALF_US); ok &= scl_rel(); wait_us(SC_HALF_US); sda_rel(); wait_us(SC_HALF_US);   // stop
  return ok;
}

static void sc_finish(uint8_t state)
{
  pins_restore(sc_mask);
  ss = SS_IDLE; sc.state = state; sc_changed = true;
}

void hwtest_scan_abort(void) { if (ss != SS_IDLE) sc_finish(3); }

bool hwtest_scan_start(uint8_t opt, uint32_t mask)
{
  if (opt > SC_OPT_PULLDN) return false;
  mask &= ALL_MASK & ~OWNED_MASK & ~SWD_MASK;
  int n = 0;
  for (int i = 0; i < PM_PINS; i++) if (mask & (1u << i)) { if (n < SC_MAX_PINS) sc_pin[n] = (uint8_t)i; n++; }
  if (n < 2 || n > SC_MAX_PINS) return false;
  hwtest_scan_abort();
  if (pm_run) { pm_restore(); pm_run = false; pm.mode = PM_MODE_OFF; pm_changed = true; }
  memset(&sc, 0, sizeof(sc));
  sc.state = 1; sc.opt = opt; sc.pairs_total = (uint8_t)(n * (n - 1));
  sc_n = (uint8_t)n; sc_mask = mask; sc_i = 0; sc_j = 1;
  CMU->CLKEN0 |= CMU_CLKEN0_GPIO;
  for (int p = 0; p < 4; p++) { sv_model[p] = GPIO->P[p].MODEL; sv_modeh[p] = GPIO->P[p].MODEH; sv_dout[p] = GPIO->P[p].DOUT; }
  ss = SS_SETUP;
  sc_next = RAIL_GetTime();
  sc_changed = true;
  return true;
}

static void sc_commit_pair(void)                              // a pair that answered at up to three addresses: those are devices
{
  for (int k = 0; k < sc_acks; k++) {
    if (sc.hits < 2) { sc.hit[sc.hits].sda = sc_sda; sc.hit[sc.hits].scl = sc_scl; sc.hit[sc.hits].addr = sc_ackaddr[k]; }
    if (sc.hits < 255) sc.hits++;
    sc_changed = true;
  }
}

static void sc_next_pair(void)
{
  sc.pairs_done++;
  do {
    sc_j++;
    if (sc_j >= sc_n) { sc_i++; sc_j = 0; }
  } while (sc_i < sc_n && sc_i == sc_j);
  ss = sc_i >= sc_n ? SS_IDLE : SS_SETUP;
  if (ss == SS_IDLE) sc_finish(2);
}

static void sc_poll(void)
{
  if (ss == SS_IDLE) return;
  uint32_t now = RAIL_GetTime();
  if ((int32_t)(now - sc_next) < 0) return;
  sc_next = now + SC_PACE_US;
  switch (ss) {
    case SS_IDLE: break;
    case SS_SETUP:
      sc_sda = sc_pin[sc_i]; sc_scl = sc_pin[sc_j];
      for (int k = 0; k < sc_n; k++) {
        if (sc_pin[k] == sc_sda || sc_pin[k] == sc_scl) pin_cfg(sc_pin[k], 2, 1);       // released: pulled up
        else if (sc.opt == SC_OPT_PULLUP) pin_cfg(sc_pin[k], 2, 1);
        else if (sc.opt == SC_OPT_PULLDN) pin_cfg(sc_pin[k], 2, 0);
        else pin_cfg(sc_pin[k], 1, 0);
      }
      sc_t0 = now; sc_next = now; sc_acks = 0;
      ss = SS_SETTLE;
      break;
    case SS_SETTLE: {
      bool sda_h = pin_read(sc_sda), scl_h = pin_read(sc_scl);
      if (sda_h) sc.high |= 1u << sc_sda;
      if (scl_h) sc.high |= 1u << sc_scl;
      if (sda_h && scl_h) { sc_addr = SC_ADDR_FIRST; ss = SS_ADDR; }
      else if ((uint32_t)(now - sc_t0) > SC_SETTLE_MAX_US) { sc.stuck++; sc_changed = true; sc_next_pair(); }
      break; }
    case SS_ADDR: {
      bool ack, ok = i2c_probe(sc_addr, &ack);
      if (!ok) { sc.stuck++; sc_changed = true; sc_next_pair(); break; }                  // the clock did not behave: give up on this pair
      if (ack) {
        if (sc_acks >= 3) { sc.held |= 1u << sc_sda; sc_changed = true; sc_next_pair(); break; }   // "answers" at 4 addresses: the data line is held low, not a device
        sc_ackaddr[sc_acks++] = sc_addr;
      }
      if (sc_addr >= SC_ADDR_LAST) { sc_commit_pair(); sc_next_pair(); } else sc_addr++;
      break; }
  }
}

// =====================================================================================================================
void hwtest_reset(void)
{
  hwtest_scan_abort();
  if (pm_run) { pm_restore(); pm_run = false; pm.mode = PM_MODE_OFF; }
}

void hwtest_poll(void)
{
  ct_poll();
  pm_poll();
  sc_poll();
}
