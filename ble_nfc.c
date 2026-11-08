// SES V300 (not badge154): NFC chip over I2C, read only, see ble_nfc.h
#include <string.h>
#include "ble_nfc.h"
#include "em_device.h"
#include "rail.h"

#define ADDR          0x55u
#define HALF_US       5u            // half a clock period: about 60 kHz, the same as the scanner that found the chip
#define STRETCH_US    200u
#define PWRUP_US      30000u        // after the supply is switched on
#define PACE_US       1000u
#define LIVE_PERIOD_US 200000u
#define LIVE_NAK_MAX  10u

// the three pins: PC5 supply (port 2, pin 5), PD2 SDA and PD3 SCL (port 3, pins 2 and 3)
#define PWR_P 2
#define PWR_N 5
#define SDA_P 3
#define SDA_N 2
#define SCL_P 3
#define SCL_N 3

const uint8_t nfc_block_addr[NFC_BLOCKS] = { 0x00, 0x01, 0x02, 0x38, 0x3A, 0xFE };

typedef enum { NS_IDLE = 0, NS_PWRUP, NS_SNAP, NS_LIVE } ns_t;

static nfc_t nf;
static ns_t ns;
static bool nf_changed;
static uint8_t step;                // snapshot: 0..5 blocks, 6..12 registers
static uint32_t nf_next, nf_t0, live_last_s;
static uint8_t nak_run;
static uint32_t sv_mode[3], sv_dout[3];
static const uint8_t pin_p[3] = { PWR_P, SDA_P, SCL_P };
static const uint8_t pin_n[3] = { PWR_N, SDA_N, SCL_N };

const nfc_t *nfc_state(void) { return &nf; }
bool nfc_changed(void) { bool c = nf_changed; nf_changed = false; return c; }

static uint32_t get_mode(int p, int n) { return n < 8 ? (GPIO->P[p].MODEL >> (n * 4)) & 0xFu : (GPIO->P[p].MODEH >> ((n - 8) * 4)) & 0xFu; }
static void set_mode(int p, int n, uint32_t m)
{
  if (n < 8) GPIO->P[p].MODEL = (GPIO->P[p].MODEL & ~(0xFu << (n * 4))) | (m << (n * 4));
  else       GPIO->P[p].MODEH = (GPIO->P[p].MODEH & ~(0xFu << ((n - 8) * 4))) | (m << ((n - 8) * 4));
}
// pin modes of the series 2 GPIO: 1 input, 2 input with pull (DOUT selects up or down), 4 push-pull
static void cfg(int p, int n, uint32_t mode, uint32_t dout)
{
  if (dout) GPIO->P[p].DOUT |= 1u << n; else GPIO->P[p].DOUT &= ~(1u << n);
  set_mode(p, n, mode);
}
static uint32_t rd(int p, int n) { return ((uint32_t)GPIO->P[p].DIN >> n) & 1u; }

static void wait_us(uint32_t us) { uint32_t t0 = RAIL_GetTime(); while ((uint32_t)(RAIL_GetTime() - t0) < us) { } }

// ---- bit-banged I2C master: a released line is an input with the internal pull-up, a low is a push-pull low, nothing is ever driven high
static bool bus_err;
static void sda_rel(void) { cfg(SDA_P, SDA_N, 2, 1); }
static void sda_low(void) { cfg(SDA_P, SDA_N, 4, 0); }
static void scl_low(void) { cfg(SCL_P, SCL_N, 4, 0); }
static void scl_rel(void)
{
  cfg(SCL_P, SCL_N, 2, 1);
  uint32_t t0 = RAIL_GetTime();
  while (!rd(SCL_P, SCL_N)) { if ((uint32_t)(RAIL_GetTime() - t0) > STRETCH_US) { bus_err = true; return; } }
}
static void i2c_start(void) { sda_rel(); scl_rel(); wait_us(HALF_US); sda_low(); wait_us(HALF_US); scl_low(); wait_us(HALF_US); }   // also a repeated start
static void i2c_stop(void) { sda_low(); wait_us(HALF_US); scl_rel(); wait_us(HALF_US); sda_rel(); wait_us(HALF_US); }
static bool i2c_wbyte(uint8_t b)                                    // true if the chip acknowledged
{
  for (int k = 7; k >= 0; k--) {
    if ((b >> k) & 1u) sda_rel(); else sda_low();
    wait_us(HALF_US); scl_rel(); wait_us(HALF_US); scl_low(); wait_us(HALF_US);
  }
  sda_rel(); wait_us(HALF_US); scl_rel(); wait_us(HALF_US);
  bool ack = rd(SDA_P, SDA_N) == 0;
  scl_low(); wait_us(HALF_US);
  return ack;
}
static uint8_t i2c_rbyte(bool ack)
{
  uint8_t v = 0;
  sda_rel();
  for (int k = 0; k < 8; k++) {
    wait_us(HALF_US); scl_rel(); wait_us(HALF_US);
    v = (uint8_t)((v << 1) | rd(SDA_P, SDA_N));
    scl_low();
  }
  if (ack) sda_low(); else sda_rel();
  wait_us(HALF_US); scl_rel(); wait_us(HALF_US); scl_low(); wait_us(HALF_US);
  sda_rel();
  return v;
}

// read len bytes: address (write), mem, restart, address (read). Returns false if the chip did not acknowledge the first three bytes.
static bool read_mem(uint8_t first, bool two, uint8_t second, uint8_t *buf, uint8_t len)
{
  bool ok;
  bus_err = false;
  i2c_start();
  ok = i2c_wbyte((uint8_t)(ADDR << 1));
  if (ok) ok = i2c_wbyte(first);
  if (ok && two) ok = i2c_wbyte(second);
  if (ok) { i2c_start(); ok = i2c_wbyte((uint8_t)((ADDR << 1) | 1u)); }
  if (ok) for (uint8_t i = 0; i < len; i++) buf[i] = i2c_rbyte(i + 1 < len);
  i2c_stop();
  return ok && !bus_err;
}
static bool read_block(uint8_t blk, uint8_t *buf) { return read_mem(blk, false, 0, buf, 16); }
static bool read_reg(uint8_t rega, uint8_t *v) { return read_mem(0xFE, true, rega, v, 1); }       // session register: block 0xFE, register address, 1 byte

// ---- power and pins
static void pins_save_on(void)
{
  CMU->CLKEN0 |= CMU_CLKEN0_GPIO;
  for (int k = 0; k < 3; k++) { sv_mode[k] = get_mode(pin_p[k], pin_n[k]); sv_dout[k] = (GPIO->P[pin_p[k]].DOUT >> pin_n[k]) & 1u; }
  sda_rel(); scl_rel();
  cfg(PWR_P, PWR_N, 4, 1);                                            // supply on
}
static void pins_back(void)
{
  for (int k = 0; k < 3; k++) cfg(pin_p[k], pin_n[k], sv_mode[k], sv_dout[k]);
}

static void finish(uint8_t state)
{
  if (ns != NS_IDLE) pins_back();
  ns = NS_IDLE; nf.state = state; nf_changed = true;
}

void nfc_stop(void) { if (ns != NS_IDLE) finish(2); }
void nfc_reset(void) { nfc_stop(); }

static bool begin(uint8_t mode)
{
  nfc_stop();
  memset(&nf, 0, sizeof(nf));
  nf.state = 1; nf.mode = mode;
  pins_save_on();
  ns = NS_PWRUP; step = 0; nak_run = 0; live_last_s = 0;
  nf_t0 = RAIL_GetTime(); nf_next = nf_t0 + PWRUP_US;
  nf_changed = true;
  return true;
}
bool nfc_snapshot_start(void) { return begin(1); }
bool nfc_live_start(void) { return begin(2); }

static void snap_step(void)
{
  if (step < NFC_BLOCKS) {
    if (!read_block(nfc_block_addr[step], nf.blocks[step])) nf.fail |= (uint8_t)(1u << step);
  } else {
    uint8_t r = (uint8_t)(step - NFC_BLOCKS), v = 0;
    if (read_reg(r, &v)) { nf.regs[r] = v; nf.regs_ok |= (uint8_t)(1u << r); } else nf.fail |= 0x40u;
  }
  step++;
  if (step >= NFC_BLOCKS + NFC_REGS) {
    bool any = (nf.fail & 0x3Fu) != 0x3Fu || nf.regs_ok;
    finish(any ? 2 : 3);
  }
}

static void live_step(uint32_t now)
{
  uint8_t v = 0;
  nf.polls++;
  if (read_reg(NFC_LIVE_NS_REG, &v)) {
    nak_run = 0;
    uint8_t f = v & 1u;                                               // NS_REG bit 0: RF field present
    if (!nf.ns_valid || nf.regs[NFC_LIVE_NS_REG] != v || nf.field_now != f) nf_changed = true;
    if (f && !nf.field_now) nf.events++;
    if (f) nf.field_ever = 1;
    nf.field_now = f; nf.regs[NFC_LIVE_NS_REG] = v; nf.regs_ok |= (uint8_t)(1u << NFC_LIVE_NS_REG); nf.ns_valid = 1;
  } else if (++nak_run >= LIVE_NAK_MAX) { finish(nf.ns_valid ? 2 : 3); return; }
  uint32_t s = (uint32_t)(now - nf_t0) / 1000000u;
  if (s != live_last_s) { live_last_s = s; nf.live_s = (uint16_t)s; }
  if (s >= NFC_LIVE_MAX_S) finish(2);
}

void nfc_poll(void)
{
  if (ns == NS_IDLE) return;
  uint32_t now = RAIL_GetTime();
  if ((int32_t)(now - nf_next) < 0) return;
  switch (ns) {
    case NS_IDLE: break;
    case NS_PWRUP: ns = nf.mode == 1 ? NS_SNAP : NS_LIVE; nf_next = now; break;
    case NS_SNAP: nf_next = now + PACE_US; snap_step(); break;
    case NS_LIVE: nf_next = now + LIVE_PERIOD_US; live_step(now); break;
  }
}
