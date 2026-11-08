// SES V300, BLE stage 3: external SPI flash, see ble_flash.h
#include <string.h>
#include "ble_flash.h"
#include "ble_diag.h"
#include "ble_ll.h"
#include "rail.h"
#include "em_device.h"

// ---- pins: port (0 = A, 1 = B), pin number
#define CS_PORT   0
#define CS_PIN    5
#define CLK_PORT  0
#define CLK_PIN   0
#define MOSI_PORT 1
#define MOSI_PIN  0
#define MISO_PORT 0
#define MISO_PIN  4

#define SPI_HALF_LOOPS 2u              // delay loops per half clock period, about 1.5 MHz SPI clock at 38.4 MHz CPU

#define SLICE_PAGE_US  2600u           // radio-free time needed to program or read one page in one go (bit-banged)
#define SLICE_READ_US  2600u
#define BUSY_TMO_US    3000000u        // sector erase of this chip takes about 45 ms typically, 400 ms at most

volatile fl_result_t fl_res;
volatile bool fl_done;

typedef enum { S_IDLE, S_ERASE_WAIT, S_ERASE_VERIFY, S_PROG_WAIT, S_PROG_VERIFY, S_SCAN } fl_state_t;
static fl_state_t st;
static uint32_t wr_lo = 0, wr_hi = FL_TEST_END;
static uint32_t cur_addr, off, t0, last_sr_us;
static uint8_t page_buf[FL_PAGE];

static inline void pin_set(int port, int pin) { GPIO->P_SET[port].DOUT = 1u << pin; }
static inline void pin_clr(int port, int pin) { GPIO->P_CLR[port].DOUT = 1u << pin; }
static inline uint32_t miso(void) { return (GPIO->P[MISO_PORT].DIN >> MISO_PIN) & 1u; }

static void setmode(int port, int pin, uint32_t mode)
{
  if (pin < 8) {
    GPIO->P[port].MODEL = (GPIO->P[port].MODEL & ~(0xFu << (pin * 4))) | (mode << (pin * 4));
  } else {
    GPIO->P[port].MODEH = (GPIO->P[port].MODEH & ~(0xFu << ((pin - 8) * 4))) | (mode << ((pin - 8) * 4));
  }
}

static inline void dly(void) { for (volatile uint32_t i = 0; i < SPI_HALF_LOOPS; i++) { } }

static void wbyte(uint32_t b)
{
  for (int k = 7; k >= 0; k--) {
    if ((b >> k) & 1u) pin_set(MOSI_PORT, MOSI_PIN); else pin_clr(MOSI_PORT, MOSI_PIN);
    dly();
    pin_set(CLK_PORT, CLK_PIN);
    dly();
    pin_clr(CLK_PORT, CLK_PIN);
  }
}

static uint32_t rbyte(void)
{
  uint32_t v = 0;
  for (int k = 0; k < 8; k++) {
    pin_set(CLK_PORT, CLK_PIN);
    dly();
    v = (v << 1) | miso();
    pin_clr(CLK_PORT, CLK_PIN);
    dly();
  }
  return v;
}

static inline void cs_low(void) { pin_clr(CS_PORT, CS_PIN); }
static inline void cs_high(void) { pin_set(CS_PORT, CS_PIN); for (volatile int i = 0; i < 20; i++) { } }

static void cmd0(uint32_t c) { cs_low(); wbyte(c); cs_high(); }
static uint32_t rd_reg(uint32_t c) { cs_low(); wbyte(c); uint32_t v = rbyte(); cs_high(); return v; }
static void addr3(uint32_t a) { wbyte(a >> 16); wbyte(a >> 8); wbyte(a); }

void fl_set_range(uint32_t lo, uint32_t hi) { wr_lo = lo; wr_hi = hi; }

static void wait_us(uint32_t us) { uint32_t t = RAIL_GetTime(); while ((uint32_t)(RAIL_GetTime() - t) < us) { } }

void fl_init(void)
{
  wr_lo = 0; wr_hi = FL_TEST_END;
  CMU->CLKEN0 |= CMU_CLKEN0_GPIO;
  pin_set(CS_PORT, CS_PIN);
  pin_clr(CLK_PORT, CLK_PIN);
  setmode(CS_PORT, CS_PIN, 4);       // push-pull
  setmode(CLK_PORT, CLK_PIN, 4);
  setmode(MOSI_PORT, MOSI_PIN, 4);
  setmode(MISO_PORT, MISO_PIN, 1);   // input
  st = S_IDLE;
  fl_done = false;
  cmd0(0xAB);                        // release from deep power-down (harmless otherwise); the first JEDEC read after power-up was 00 00 00
  wait_us(60);
}

bool fl_busy(void) { return st != S_IDLE; }

static void finish(uint8_t result)
{
  fl_res.result = result;
  g_diag.fl_result = result;
  g_diag.fl_busy_us = fl_res.busy_us;
  st = S_IDLE;
  fl_done = true;
}

static void begin_op(uint8_t op)
{
  memset((void *)&fl_res, 0, sizeof(fl_res));
  fl_res.op = op;
  fl_res.result = FLR_RUNNING;
  g_diag.fl_op = op;
  g_diag.fl_result = FLR_RUNNING;
  fl_done = false;
}

bool fl_start_info(void)
{
  if (st != S_IDLE) return false;
  begin_op(1);
  cs_low(); wbyte(0x9F);
  uint32_t a = rbyte(), b = rbyte(), c = rbyte();
  cs_high();
  fl_res.jedec = (a << 16) | (b << 8) | c;
  fl_res.sr1_last = (uint8_t)rd_reg(0x05);
  fl_res.sr2 = (uint8_t)rd_reg(0x35);
  fl_res.sr3 = (uint8_t)rd_reg(0x15);
  g_diag.fl_jedec = fl_res.jedec;
  g_diag.fl_sr = fl_res.sr1_last | ((uint32_t)fl_res.sr2 << 8) | ((uint32_t)fl_res.sr3 << 16);
  finish(FLR_OK);
  return true;
}

bool fl_start_erase(uint32_t addr)
{
  if (st != S_IDLE || (addr & (FL_SECTOR - 1u)) || addr < wr_lo || addr >= wr_hi) return false;
  if (ll_free_us() < 1500u) return false;                // try again later
  begin_op(2);
  cur_addr = addr;
  cmd0(0x06);                                            // write enable
  fl_res.sr1_wel = (uint8_t)rd_reg(0x05);
  if (!(fl_res.sr1_wel & 2u)) { finish(FLR_NO_WEL); return true; }
  cs_low(); wbyte(0x20); addr3(addr); cs_high();
  t0 = RAIL_GetTime();
  last_sr_us = t0;
  fl_res.sr1_first = (uint8_t)rd_reg(0x05);              // first sample about 100 us after the command
  st = S_ERASE_WAIT;
  return true;
}

bool fl_start_program(uint32_t addr, const uint8_t *data)
{
  if (st != S_IDLE || (addr & (FL_PAGE - 1u)) || addr < wr_lo || addr >= wr_hi) return false;
  if (ll_free_us() < SLICE_PAGE_US) return false;
  begin_op(3);
  memcpy(page_buf, data, FL_PAGE);
  cur_addr = addr;
  cmd0(0x06);
  fl_res.sr1_wel = (uint8_t)rd_reg(0x05);
  if (!(fl_res.sr1_wel & 2u)) { finish(FLR_NO_WEL); return true; }
  cs_low(); wbyte(0x02); addr3(addr);
  for (uint32_t i = 0; i < FL_PAGE; i++) wbyte(page_buf[i]);
  cs_high();
  t0 = RAIL_GetTime();
  last_sr_us = t0;
  fl_res.sr1_first = (uint8_t)rd_reg(0x05);
  st = S_PROG_WAIT;
  return true;
}

bool fl_start_scan(uint32_t addr)
{
  if (st != S_IDLE || (addr & (FL_SECTOR - 1u)) || addr >= FL_SIZE) return false;
  begin_op(4);
  cur_addr = addr;
  off = 0;
  st = S_SCAN;
  return true;
}

// read 256 bytes at cur_addr + off and count non-FF bytes (or compare with page_buf when cmp)
static uint32_t read_slice(uint32_t a, const uint8_t *cmp)
{
  uint32_t bad = 0;
  cs_low(); wbyte(0x03); addr3(a);
  for (uint32_t i = 0; i < FL_PAGE; i++) {
    uint32_t v = rbyte();
    if (cmp ? (v != cmp[i]) : (v != 0xFFu)) bad++;
  }
  cs_high();
  return bad;
}

void fl_poll(void)
{
  if (st == S_IDLE) return;
  uint32_t now = RAIL_GetTime();
  switch (st) {
    case S_ERASE_WAIT:
    case S_PROG_WAIT:
      if ((uint32_t)(now - last_sr_us) < 1000u) return;      // one status read per millisecond at most (it takes about 30 us)
      last_sr_us = now;
      fl_res.sr1_last = (uint8_t)rd_reg(0x05);
      if (!(fl_res.sr1_last & 1u)) {
        fl_res.busy_us = now - t0;
        off = 0;
        st = (st == S_ERASE_WAIT) ? S_ERASE_VERIFY : S_PROG_VERIFY;
      } else if ((uint32_t)(now - t0) > BUSY_TMO_US) {
        fl_res.busy_us = now - t0;
        finish(FLR_BUSY_TMO);
      }
      return;
    case S_ERASE_VERIFY:
      if (ll_free_us() < SLICE_READ_US) { DIAG_INC(fl_blocked); return; }
      fl_res.nonff += read_slice(cur_addr + off, NULL);
      off += FL_PAGE;
      if (off >= FL_SECTOR) finish(fl_res.nonff ? FLR_NOT_BLANK : FLR_OK);
      return;
    case S_PROG_VERIFY:
      if (ll_free_us() < SLICE_READ_US) { DIAG_INC(fl_blocked); return; }
      fl_res.nonff = read_slice(cur_addr, page_buf);       // here: number of differing bytes
      finish(fl_res.nonff ? FLR_VERIFY : FLR_OK);
      return;
    case S_SCAN:
      if (ll_free_us() < SLICE_READ_US) { DIAG_INC(fl_blocked); return; }
      fl_res.nonff += read_slice(cur_addr + off, NULL);
      off += FL_PAGE;
      if (off >= FL_SECTOR) finish(FLR_OK);
      return;
    default:
      return;
  }
}

bool fl_read_page(uint32_t addr, uint8_t *buf, uint32_t n)
{
  if (st != S_IDLE || n > FL_PAGE || addr + n > FL_SIZE) return false;
  if (ll_free_us() < (n > 32u ? SLICE_READ_US : 600u)) { DIAG_INC(fl_blocked); return false; }
  cs_low(); wbyte(0x03); addr3(addr);
  for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)rbyte();
  cs_high();
  return true;
}
