// SES V300 (not badge154): NFC chip over I2C, see ble_nfc.h
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
#define LIVE_FD_PERIOD_US 20000u
#define FD_SAMPLE_US  1000u
#define LIVE_NAK_MAX  10u
#define TAP_PERIOD_US 200000u
#define TAP_ABSENT_POLLS 2u         // a tap needs at least 2 polls (400 ms) without a field before the phone arrives
#define TAP_RETRY_US  5000000u
#define ACK_POLL_MAX  25u           // 25 ms: a block write takes about 4 ms

// the three pins: PC5 supply (port 2, pin 5), PD2 SDA and PD3 SCL (port 3, pins 2 and 3)
#define PWR_P 2
#define PWR_N 5
#define SDA_P 3
#define SDA_N 2
#define SCL_P 3
#define SCL_N 3
#define OWN_PINS ((1u << 19) | (1u << 24) | (1u << 25))      // pin indices of PC5, PD2, PD3

static const uint8_t port_first[4] = { 0, 9, 14, 22 };

const uint8_t nfc_block_addr[NFC_BLOCKS] = { 0x00, 0x01, 0x02, 0x38, 0x3A, 0xFE };

typedef enum { NS_IDLE = 0, NS_PWRUP, NS_RUN, NS_TAP_UP, NS_TAP } ns_t;

static nfc_t nf;
static nfc_fd_t fd;
static ns_t ns;
static bool nf_changed;
static uint8_t step, sub, ackn;
static uint32_t nf_next, nf_t0, live_last_s, fd_next;
static uint8_t nak_run;
static uint32_t sv_mode[3], sv_dout[3];
static uint32_t fd_sv_mode[NFC_FD_MAX], fd_sv_dout[NFC_FD_MAX];
static const uint8_t pin_p[3] = { PWR_P, SDA_P, SCL_P };
static const uint8_t pin_n[3] = { PWR_N, SDA_N, SCL_N };
static uint8_t bk[NFC_BK_BLOCKS][16];
static uint8_t wbuf[NFC_WBUF_BLOCKS][16];
static uint8_t wbuf_have, wn;
static uint8_t ub0[16], ub38[16], tmp[16];
// tap
static bool tap_en, tap_hold, tap_base, tap_prev;
static uint8_t tap_absent, tap_pending, tap_count;
static uint32_t tap_retry_at;
static bool tap_wait;               // a retry time is pending (a flag, so the 32 bit microsecond clock may wrap)

const nfc_t *nfc_state(void) { return &nf; }
const nfc_fd_t *nfc_fd(void) { return &fd; }
const uint8_t *nfc_backup_block(uint8_t blk) { return blk < NFC_BK_BLOCKS ? bk[blk] : bk[0]; }
bool nfc_changed(void) { bool c = nf_changed; nf_changed = false; return c; }
bool nfc_tap_take(void) { if (!tap_pending) return false; tap_pending = 0; return true; }
uint8_t nfc_tap_count(void) { return tap_count; }
bool nfc_tap_running(void) { return ns == NS_TAP || ns == NS_TAP_UP; }
void nfc_tap_enable(bool on) { tap_en = on; }
void nfc_hold(bool hold) { tap_hold = hold; }

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
static void pin_split(int idx, int *p, int *n) { int q = 3; while (q > 0 && idx < port_first[q]) q--; *p = q; *n = idx - port_first[q]; }

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

// write one 16 byte block (address, block number, 16 data bytes); true if all 18 bytes were acknowledged
static bool write_block(uint8_t blk, const uint8_t *d)
{
  bool ok;
  bus_err = false;
  i2c_start();
  ok = i2c_wbyte((uint8_t)(ADDR << 1));
  if (ok) ok = i2c_wbyte(blk);
  for (int i = 0; ok && i < 16; i++) ok = i2c_wbyte(d[i]);
  i2c_stop();
  return ok && !bus_err;
}
static bool ack_poll(void)                                          // the chip does not answer while it stores a block (about 4 ms)
{
  bus_err = false;
  i2c_start();
  bool ok = i2c_wbyte((uint8_t)(ADDR << 1));
  i2c_stop();
  return ok;
}

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
static void fd_pins_on(void)
{
  for (int k = 0; k < fd.n; k++) {
    int p, n; pin_split(fd.pin[k], &p, &n);
    fd_sv_mode[k] = get_mode(p, n); fd_sv_dout[k] = (GPIO->P[p].DOUT >> n) & 1u;
    cfg(p, n, 2, 1);                                                  // input with pull-up: FD is an open-drain output
  }
}
static void fd_pins_back(void)
{
  for (int k = 0; k < fd.n; k++) { int p, n; pin_split(fd.pin[k], &p, &n); cfg(p, n, fd_sv_mode[k], fd_sv_dout[k]); }
}

static void finish(uint8_t state)
{
  if (ns != NS_IDLE) { fd_pins_back(); pins_back(); }
  ns = NS_IDLE; nf.state = state; nf_changed = true;
}
static void leave_tap(void) { pins_back(); ns = NS_IDLE; }

void nfc_stop(void)
{
  if (ns == NS_TAP || ns == NS_TAP_UP) leave_tap();
  else if (ns != NS_IDLE) finish(nf.state == 1 ? 2 : nf.state);
}
void nfc_reset(void) { nfc_stop(); }

static bool begin(uint8_t mode)
{
  nfc_stop();
  memset(&nf, 0, sizeof(nf));
  nf.state = 1; nf.mode = mode;
  pins_save_on();
  ns = NS_PWRUP; step = 0; sub = 0; ackn = 0; nak_run = 0; live_last_s = 0;
  fd.n = 0;                                                         // only a live run watches pins; its pins are set after this
  nf_t0 = RAIL_GetTime(); nf_next = nf_t0 + PWRUP_US;
  nf_changed = true;
  return true;
}
bool nfc_snapshot_start(void) { return begin(NFC_M_SNAP); }
bool nfc_backup_start(void) { return begin(NFC_M_BACKUP); }
bool nfc_unlock_start(void) { return begin(NFC_M_UNLOCK); }

bool nfc_live_start(uint32_t fd_mask)
{
  fd_mask &= 0x3FFFFFFu & ~OWN_PINS;
  uint8_t n = 0;
  uint8_t pins[NFC_FD_MAX];
  for (int i = 0; i < 26; i++) if (fd_mask & (1u << i)) { if (n >= NFC_FD_MAX) return false; pins[n++] = (uint8_t)i; }
  bool ok = begin(NFC_M_LIVE);
  memset(&fd, 0, sizeof(fd));
  fd.n = n; memcpy(fd.pin, pins, n);
  nf.fd_n = n;
  fd_pins_on();
  fd_next = RAIL_GetTime();
  return ok;
}

bool nfc_wbuf_set(uint8_t idx, const uint8_t *d16)
{
  if (idx >= NFC_WBUF_BLOCKS) return false;
  if (idx == 0) wbuf_have = 0;
  memcpy(wbuf[idx], d16, 16); wbuf_have |= (uint8_t)(1u << idx);
  return true;
}

bool nfc_write_start(uint8_t nblocks)
{
  if (nblocks < 1 || nblocks > NFC_WBUF_BLOCKS) return false;
  if ((wbuf_have & ((1u << nblocks) - 1u)) != ((1u << nblocks) - 1u)) return false;
  if (wbuf[0][0] != 0x03 || wbuf[0][1] == 0 || wbuf[0][1] == 0xFF || 2u + wbuf[0][1] > 16u * nblocks) return false;     // an NDEF message TLV that fits
  wn = nblocks;
  return begin(NFC_M_WRITE);
}

// ---- jobs: one transaction per call
static void job_snap(void)
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

static void job_backup(void)
{
  if (!read_block(step, bk[step]) && nf.op_res == NFC_R_OK) { nf.op_res = NFC_R_NOACK; nf.op_blk = step; }
  step++;
  if (step >= NFC_BK_BLOCKS) finish(nf.op_res == NFC_R_OK ? 2 : 3);
}

static void fail_op(uint8_t res, uint8_t blk) { nf.op_res = res; nf.op_blk = blk; finish(3); }

static void job_write(void)                                         // blocks 1..wn: sub 0 write, 1 wait for the chip, 2 read back
{
  uint8_t blk = (uint8_t)(1u + step);
  if (sub == 0) {
    if (!write_block(blk, wbuf[step])) { fail_op(NFC_R_NOACK, blk); return; }
    sub = 1; ackn = 0;
  } else if (sub == 1) {
    if (ack_poll()) sub = 2; else if (++ackn >= ACK_POLL_MAX) fail_op(NFC_R_GONE, blk);
  } else {
    if (!read_block(blk, tmp)) { fail_op(NFC_R_NOACK, blk); return; }
    if (memcmp(tmp, wbuf[step], 16) != 0) { fail_op(NFC_R_VERIFY, blk); return; }
    sub = 0; step++;
    if (step >= wn) finish(2);
  }
}

static void job_unlock(void)
{
  switch (step) {
    case 0:                                                         // block 0: read, check, build the new one
      if (!read_block(0x00, ub0)) { fail_op(NFC_R_NOACK, 0x00); return; }
      if (ub0[0] != 0x04 || ub0[12] != 0xE1) { fail_op(NFC_R_REFUSED, 0x00); return; }       // not what the chip is known to hold: nothing is written
      ub0[0] = (uint8_t)(ADDR << 1);                                // byte 0 is written as the I2C address, so the address stays
      ub0[10] = 0x00; ub0[11] = 0x00;                               // static lock bytes
      step = 1; break;
    case 1: if (!write_block(0x00, ub0)) { fail_op(NFC_R_NOACK, 0x00); return; } step = 2; ackn = 0; break;
    case 2: if (ack_poll()) step = 3; else if (++ackn >= ACK_POLL_MAX) fail_op(NFC_R_GONE, 0x00); break;
    case 3:
      if (!read_block(0x00, tmp)) { fail_op(NFC_R_GONE, 0x00); return; }
      if (tmp[10] != 0 || tmp[11] != 0 || tmp[12] != ub0[12] || tmp[13] != ub0[13] || tmp[14] != ub0[14]) { fail_op(NFC_R_VERIFY, 0x00); return; }
      step = 4; break;
    case 4:                                                         // block 0x38: the dynamic lock bytes
      if (!read_block(0x38, ub38)) { fail_op(NFC_R_NOACK, 0x38); return; }
      ub38[8] = 0x00; ub38[9] = 0x00; ub38[10] = 0x00;
      step = 5; break;
    case 5: if (!write_block(0x38, ub38)) { fail_op(NFC_R_NOACK, 0x38); return; } step = 6; ackn = 0; break;
    case 6: if (ack_poll()) step = 7; else if (++ackn >= ACK_POLL_MAX) fail_op(NFC_R_GONE, 0x38); break;
    default:
      if (!read_block(0x38, tmp)) { fail_op(NFC_R_NOACK, 0x38); return; }
      if (tmp[8] != 0 || tmp[9] != 0 || tmp[10] != 0) { fail_op(NFC_R_VERIFY, 0x38); return; }
      finish(2);
      break;
  }
}

static uint32_t live_period(void) { return fd.n ? LIVE_FD_PERIOD_US : LIVE_PERIOD_US; }

static void fd_sample(void)
{
  for (int k = 0; k < fd.n; k++) {
    int p, n; pin_split(fd.pin[k], &p, &n);
    uint32_t hi = rd(p, n);
    if (nf.field_now) { if (hi) fd.hi_f[k]++; else fd.lo_f[k]++; }
    else              { if (hi) fd.hi_n[k]++; else fd.lo_n[k]++; }
  }
}

static void job_live(uint32_t now)
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

// ---- tap mode
static void tap_step(uint32_t now)
{
  uint8_t v = 0;
  if (tap_hold || !tap_en) { leave_tap(); return; }
  if (read_reg(NFC_LIVE_NS_REG, &v)) {
    nak_run = 0;
    uint8_t f = v & 1u;
    if (!tap_base) { tap_base = true; tap_prev = f; tap_absent = f ? 0 : 100; }                  // the first reading is the starting point, never a tap
    else {
      if (f) { if (!tap_prev && tap_absent >= TAP_ABSENT_POLLS) { tap_count++; if (!tap_pending) tap_pending = 1; } tap_absent = 0; }
      else if (tap_absent < 100) tap_absent++;
      tap_prev = f;
    }
  } else if (++nak_run >= LIVE_NAK_MAX) { leave_tap(); tap_retry_at = now + TAP_RETRY_US; tap_wait = true; }
}

void nfc_poll(void)
{
  uint32_t now = RAIL_GetTime();
  if (ns == NS_IDLE) {
    if (tap_wait && (int32_t)(now - tap_retry_at) >= 0) tap_wait = false;
    if (tap_en && !tap_hold && !tap_wait) {
      pins_save_on(); ns = NS_TAP_UP; nf_next = now + PWRUP_US; tap_base = false; nak_run = 0;
    }
    return;
  }
  if (ns == NS_RUN && nf.mode == NFC_M_LIVE && fd.n && (int32_t)(now - fd_next) >= 0) { fd_next = now + FD_SAMPLE_US; fd_sample(); }
  if ((int32_t)(now - nf_next) < 0) return;
  switch (ns) {
    case NS_IDLE: break;
    case NS_PWRUP: ns = NS_RUN; nf_next = now; break;
    case NS_TAP_UP: ns = NS_TAP; nf_next = now; break;
    case NS_TAP: nf_next = now + TAP_PERIOD_US; tap_step(now); break;
    case NS_RUN:
      switch (nf.mode) {
        case NFC_M_SNAP:   nf_next = now + PACE_US; job_snap(); break;
        case NFC_M_BACKUP: nf_next = now + PACE_US; job_backup(); break;
        case NFC_M_WRITE:  nf_next = now + PACE_US; job_write(); break;
        case NFC_M_UNLOCK: nf_next = now + PACE_US; job_unlock(); break;
        default:           nf_next = now + live_period(); job_live(now); break;
      }
      break;
  }
}
