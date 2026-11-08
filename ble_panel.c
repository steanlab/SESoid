// SES V300 (not badge154): e-paper panel update as a non-blocking state machine, see ble_panel.h.
// The command sequence is the one of epd_probe/ramtest/panel_draw.c (ran on this tag): power on, reset, OTP read (chip ID 0x0D04),
// initialisation from the OTP, image on command 0x10 (generated line by line), DC/DC on (0x04), refresh (0x12),
// then the mandatory DC/DC stop sequence and power off. Waits (timers, BUSY) do not block: every call of panel_poll() does one step.
#include <string.h>
#include "ble_panel.h"
#include "ble_splash.h"
#include "ble_diag.h"
#include "ble_ll.h"
#include "ble_flash.h"
#include "ble_up.h"
#include "rail.h"
#include "em_device.h"

#define HALF_US        2u         // SPI clock half period; the panel accepted 2 us .. 200 us (README), 2 us is the fastest verified
#define STEP_FREE_US   5000u      // while connected a step that touches the SPI needs this much radio-free time (longest step: OTP read ~4 ms)
#define IMG_LINES_ADV  2          // lines per call while advertising (about 2 ms each)

// ---- pins
#define PORT_C   2
#define SDA_PIN  0
#define SCLK_PIN 1
#define CS_PIN   2
#define DC_PIN   3
#define RST_PIN  4
#define ONOFF_PIN 6
#define BUSY_PORT 0
#define BUSY_PIN  7

// ---- error codes (g_diag.pn_err)
enum {
  E_NONE = 0, E_RESET = 1, E_CHIP = 2, E_OTP_ADDR = 3, E_OTP_END = 4, E_OTP_MARK = 5, E_INIT_RESET = 6, E_A5 = 7,
  E_DCDC_ON = 8, E_REFRESH = 9, E_DCDC_OFF = 10, E_FLASH = 11
};

typedef enum {
  S_IDLE = 0,
  S_PWR_A, S_PWR_B, S_PWR_C,
  S_RST1, S_RST2, S_RST3, S_RST4, S_RST5,
  S_CHIP, S_OTP_RD, S_OTP_R1, S_OTP_R2, S_OTP_END,
  S_INIT_A, S_INIT_B,
  S_IMG, S_ON, S_REFRESH,
  S_STOP_A, S_STOP_B, S_STOP_C, S_STOP_D, S_STOP_E, S_STOP_F,
  S_OFF_A, S_OFF_B,
  S_DONE
} pstate_t;

typedef enum { W_NONE = 0, W_TIME, W_BUSY } wait_t;

static pstate_t st = S_IDLE;
static wait_t wk;
static uint32_t t_end;          // end of the current wait (RAIL time, us)
static uint8_t busy_err;        // error to record when the BUSY wait times out
static bool busy_soft;          // on timeout only record the error and go on (DC/DC stop sequence)
static pstate_t rst_ret;        // where the reset sequence returns to
static uint8_t rst_err;
static uint8_t err;
static bool dcdc_on;
static int img_line;
static uint8_t otp[112];
static uint32_t t_start, t_refresh;

// what is drawn
static panel_src_t src = PSRC_NONE;
static panel_status_t status = PST_IDLE;
static uint8_t slot_no, fill_code, shown;
static uint32_t slot_crc, run_crc, n_done, last_ms;
static int8_t temp_c = 25;
static uint8_t pg[256];             // slot streaming: one flash page = 4 panel lines
static int pg_idx;
static uint32_t t_flash_wait;       // first failed flash read of the current stall (0 = none)
static bool img_open;
static bool inited;

static uint32_t half_cycles;
static bool use_dwt;

// ---- low level
#ifndef DWT_CYCCNT          // (the host test hosttest/panel_host_test.cpp defines its own)
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#endif

static void dwt_init(void)
{
  DEMCR |= (1u << 24);
  DWT_CTRL |= 1u;
  uint32_t a = DWT_CYCCNT;
  for (volatile int i = 0; i < 50; i++) { }
  use_dwt = (DWT_CYCCNT != a);
  uint32_t mhz = SystemCoreClock / 1000000u;
  if (mhz == 0) mhz = 38;
  half_cycles = HALF_US * mhz;
}

static inline void delay_cycles(uint32_t n)
{
  if (use_dwt) {
    uint32_t t0 = DWT_CYCCNT;
    while ((uint32_t)(DWT_CYCCNT - t0) < n) { }
  } else {
    for (volatile uint32_t i = 0; i < n / 6u; i++) { }
  }
}

static inline void pc_set(int pin) { GPIO->P_SET[PORT_C].DOUT = 1u << pin; }
static inline void pc_clr(int pin) { GPIO->P_CLR[PORT_C].DOUT = 1u << pin; }
static inline void pc_put(int pin, uint32_t v) { if (v) pc_set(pin); else pc_clr(pin); }
static inline uint32_t sda_in(void) { return (GPIO->P[PORT_C].DIN >> SDA_PIN) & 1u; }
static inline bool busy_high(void) { return ((GPIO->P[BUSY_PORT].DIN >> BUSY_PIN) & 1u) != 0; }

static void set_mode(int port, int pin, uint32_t mode)
{
  if (pin < 8) {
    GPIO->P[port].MODEL = (GPIO->P[port].MODEL & ~(0xFu << (pin * 4))) | (mode << (pin * 4));
  } else {
    GPIO->P[port].MODEH = (GPIO->P[port].MODEH & ~(0xFu << ((pin - 8) * 4))) | (mode << ((pin - 8) * 4));
  }
}

static void sda_input(void) { set_mode(PORT_C, SDA_PIN, 1); }
static void sda_output(void) { pc_clr(SDA_PIN); set_mode(PORT_C, SDA_PIN, 4); }

// SPI mode 0, MSB first
static void wbyte(uint32_t b)
{
  for (int k = 7; k >= 0; k--) {
    pc_clr(SCLK_PIN);
    pc_put(SDA_PIN, (b >> k) & 1u);
    delay_cycles(half_cycles);
    pc_set(SCLK_PIN);
    delay_cycles(half_cycles);
  }
  pc_clr(SCLK_PIN);
}

// SDA must be an input; sampled after the rising edge, as verified on the tag
static uint32_t rbyte(void)
{
  uint32_t v = 0;
  for (int k = 0; k < 8; k++) {
    pc_clr(SCLK_PIN);
    delay_cycles(half_cycles);
    pc_set(SCLK_PIN);
    delay_cycles(half_cycles);
    v = (v << 1) | sda_in();
  }
  pc_clr(SCLK_PIN);
  return v;
}

static void cmd(uint32_t c, const uint8_t *d, uint32_t n)
{
  pc_clr(CS_PIN);
  pc_clr(DC_PIN);
  wbyte(c);
  if (n) {
    pc_set(DC_PIN);
    for (uint32_t i = 0; i < n; i++) wbyte(d[i]);
  }
  pc_set(CS_PIN);
  pc_set(DC_PIN);
}

static void cmd1(uint32_t c, uint32_t v) { uint8_t b = (uint8_t)v; cmd(c, &b, 1); }

// ---- waits
static void wait_ms(uint32_t ms, pstate_t next)
{
  wk = W_TIME;
  t_end = RAIL_GetTime() + ms * 1000u;
  st = next;
}

static void wait_busy(uint32_t ms, pstate_t next, uint8_t e, bool soft)
{
  wk = W_BUSY;
  t_end = RAIL_GetTime() + ms * 1000u;
  busy_err = e;
  busy_soft = soft;
  st = next;
}

// any error: release the bus; if the DC/DC is running it is stopped properly first, then the panel is powered off
static void fail(uint8_t e)
{
  if (!err) err = e;
  pc_set(CS_PIN);
  pc_set(DC_PIN);
  wk = W_NONE;
  st = dcdc_on ? S_STOP_A : S_OFF_A;
}

static void start_reset(pstate_t ret, uint8_t e)
{
  rst_ret = ret;
  rst_err = e;
  st = S_RST1;
}

// one panel line (64 bytes) of the current picture; false = not available right now (flash busy)
static bool get_line(int l, uint8_t *out)
{
  switch (src) {
    case PSRC_SLOT: {
      int pi = l >> 2;
      if (pg_idx != pi) {
        if (!fl_read_page((uint32_t)slot_no * FL_SLOT_SIZE + (uint32_t)pi * 256u, pg, 256u)) return false;
        pg_idx = pi;
      }
      memcpy(out, pg + (l & 3) * PANEL_LINE_BYTES, PANEL_LINE_BYTES);
      run_crc = up_crc32(run_crc, out, PANEL_LINE_BYTES);
      return true;
    }
    case PSRC_FILL:
      memset(out, (int)(fill_code & 3u) * 0x55, PANEL_LINE_BYTES);
      return true;
    default:
      splash_gen_line(l, out);
      return true;
  }
}

// ---- one step of the sequence (the state says what to do now)
static void run(void)
{
  switch (st) {
    case S_PWR_A:
      pc_set(ONOFF_PIN);
      pc_clr(SCLK_PIN); pc_clr(CS_PIN); pc_clr(DC_PIN); pc_clr(RST_PIN); pc_clr(SDA_PIN);
      set_mode(PORT_C, SCLK_PIN, 4); set_mode(PORT_C, CS_PIN, 4); set_mode(PORT_C, DC_PIN, 4);
      set_mode(PORT_C, RST_PIN, 4); set_mode(PORT_C, ONOFF_PIN, 4); set_mode(PORT_C, SDA_PIN, 4);
      set_mode(BUSY_PORT, BUSY_PIN, 1);
      wait_ms(200, S_PWR_B);
      break;
    case S_PWR_B:
      pc_clr(ONOFF_PIN);                       // low = panel on (verified on this tag)
      wait_ms(20, S_PWR_C);
      break;
    case S_PWR_C:
      pc_set(DC_PIN); pc_set(RST_PIN); pc_set(CS_PIN);
      start_reset(S_CHIP, E_RESET);
      break;

    // reset sequence, returns to rst_ret
    case S_RST1: pc_set(RST_PIN); wait_ms(10, S_RST2); break;
    case S_RST2: pc_clr(RST_PIN); wait_ms(20, S_RST3); break;
    case S_RST3: pc_set(RST_PIN); wait_ms(10, S_RST4); break;
    case S_RST4: wait_busy(3000, S_RST5, rst_err, false); break;
    case S_RST5: wait_ms(10, rst_ret); break;

    case S_CHIP: {
      pc_clr(CS_PIN); pc_clr(DC_PIN);
      wbyte(0x70);
      pc_set(DC_PIN);
      sda_input();
      uint32_t chip = (rbyte() << 8) | rbyte();
      sda_output();
      g_diag.pn_chip = chip;
      if (chip != 0x0D04u) { fail(E_CHIP); break; }
      pc_clr(DC_PIN);
      wbyte(0x90);                              // index only
      wbyte(0xA2);
      pc_set(DC_PIN);
      static const uint8_t a2[5] = { 0x00, 0x15, 0x00, 0x00, 0xE0 };
      for (int i = 0; i < 5; i++) wbyte(a2[i]);
      wait_busy(2000, S_OTP_RD, E_OTP_ADDR, false);
      break;
    }
    case S_OTP_RD:
      pc_clr(DC_PIN);
      wbyte(0x92);
      pc_set(DC_PIN);
      sda_input();
      (void)rbyte();                            // dummy byte
      for (int i = 0; i < 112; i++) otp[i] = (uint8_t)rbyte();
      sda_output();
      pc_clr(RST_PIN);
      wait_ms(40, S_OTP_R1);
      break;
    case S_OTP_R1: pc_set(RST_PIN); wait_ms(10, S_OTP_R2); break;
    case S_OTP_R2: wait_busy(2000, S_OTP_END, E_OTP_END, false); break;
    case S_OTP_END:
      pc_set(CS_PIN);
      if (otp[0] != 0xA5) { fail(E_OTP_MARK); break; }
      start_reset(S_INIT_A, E_INIT_RESET);
      break;

    case S_INIT_A:
      cmd1(0xE6, (uint8_t)temp_c);
      cmd1(0xE0, 0x02);
      cmd(0xA5, 0, 0);
      wait_busy(2000, S_INIT_B, E_A5, false);
      break;
    case S_INIT_B:
      cmd(0x01, otp + 16, 1);
      cmd(0x00, otp + 26, 2);
      cmd(0x61, otp + 19, 4);
      cmd(0x00, otp + 17, 2);                   // PSR
      cmd(0x06, otp + 23, 3);
      cmd(0x03, otp + 30, 3);
      cmd(0xE7, otp + 33, 1);
      cmd(0x65, otp + 34, 4);
      cmd(0x30, otp + 38, 1);
      cmd(0x50, otp + 39, 1);
      cmd(0x60, otp + 40, 2);
      cmd(0xE3, otp + 42, 1);
      cmd1(0xFF, 0xA5);
      cmd(0xEF, otp + 43, 8);
      cmd(0xDC, otp + 59, 1);
      cmd(0xDD, otp + 60, 1);
      cmd(0xDE, otp + 61, 1);
      cmd(0xE8, otp + 62, 1);
      cmd(0xDA, otp + 63, 1);
      cmd1(0xFF, 0xE3);
      cmd1(0xE9, 0x01);
      img_line = 0;
      st = S_IMG;
      break;

    case S_IMG: {
      if (!img_open) {                          // CS stays low for the whole image: nothing else touches the panel meanwhile
        pc_clr(CS_PIN); pc_clr(DC_PIN);
        wbyte(0x10);
        pc_set(DC_PIN);
        img_open = true;
        run_crc = 0;
        pg_idx = -1;
      }
      int n = ll_connected() ? 1 : IMG_LINES_ADV;
      uint8_t line[PANEL_LINE_BYTES];
      while (n-- > 0 && img_line < PANEL_LINES) {
        if (!get_line(img_line, line)) {        // the flash is busy with something else: try again in the next call
          uint32_t now = RAIL_GetTime();
          if (!t_flash_wait) t_flash_wait = now | 1u;
          else if ((uint32_t)(now - t_flash_wait) > 5000000u) { img_open = false; fail(E_FLASH); }
          break;
        }
        t_flash_wait = 0;
        for (int i = 0; i < PANEL_LINE_BYTES; i++) wbyte(line[i]);
        img_line++;
      }
      g_diag.pn_line = (uint32_t)img_line;
      if (img_line >= PANEL_LINES) {
        pc_set(CS_PIN); pc_set(DC_PIN);
        img_open = false;
        if (src == PSRC_SLOT && run_crc != slot_crc) g_diag.pn_crc_bad++;     // shown anyway; the number says the flash content was not what the record promised
        st = S_ON;
      }
      break;
    }
    case S_ON:
      cmd(0x04, 0, 0);
      dcdc_on = true;
      wait_busy(3000, S_REFRESH, E_DCDC_ON, false);
      break;
    case S_REFRESH:
      cmd1(0x12, 0x00);
      t_refresh = RAIL_GetTime();
      wait_busy(60000, S_STOP_A, E_REFRESH, false);
      break;

    // DC/DC stop sequence, mandatory after every refresh
    case S_STOP_A:
      if (t_refresh) g_diag.pn_refresh_ms = (RAIL_GetTime() - t_refresh) / 1000u;
      cmd1(0x02, 0x00);
      wait_busy(5000, S_STOP_B, E_DCDC_OFF, true);
      break;
    case S_STOP_B: {
      static const uint8_t p0[3] = { 0x07, 0x2B, 0x01 };
      cmd(0x00, p0, 3);
      wait_ms(400, S_STOP_C);
      break;
    }
    case S_STOP_C: {
      static const uint8_t e1[2] = { 0xA0, 0x1E };
      cmd1(0xFF, 0xA5);
      cmd(0xEE, e1, 2);
      wait_ms(4, S_STOP_D);
      break;
    }
    case S_STOP_D: {
      static const uint8_t e2[2] = { 0x00, 0x00 };
      cmd(0xEE, e2, 2);
      wait_ms(3, S_STOP_E);
      break;
    }
    case S_STOP_E:
      cmd1(0xFF, 0xE3);
      wait_ms(6000, S_STOP_F);
      break;
    case S_STOP_F:
      dcdc_on = false;
      st = S_OFF_A;
      break;

    case S_OFF_A:
      pc_clr(DC_PIN); pc_clr(RST_PIN); pc_clr(CS_PIN); pc_clr(SCLK_PIN); pc_clr(SDA_PIN);
      wait_ms(200, S_OFF_B);
      break;
    case S_OFF_B:
      pc_set(ONOFF_PIN);
      wait_ms(120, S_DONE);
      break;
    case S_DONE:
    case S_IDLE:
    default:
      break;
  }
}

static bool begin(panel_src_t source)
{
  if (st != S_IDLE && st != S_DONE) return false;
  if (!inited) {
    CMU->CLKEN0 |= CMU_CLKEN0_GPIO;
    dwt_init();
    inited = true;
  }
  src = source;
  err = 0;
  dcdc_on = false;
  wk = W_NONE;
  t_refresh = 0;
  img_open = false;
  t_flash_wait = 0;
  g_diag.pn_err = 0;
  g_diag.pn_line = 0;
  g_diag.pn_refresh_ms = 0;
  g_diag.pn_src = (uint32_t)source;
  status = PST_RUNNING;
  t_start = RAIL_GetTime();
  st = S_PWR_A;
  g_diag.pn_state = (uint32_t)st;
  return true;
}

bool panel_show_splash(void) { shown = 0; return begin(PSRC_SPLASH); }

bool panel_show_slot(uint8_t slot, uint32_t crc)
{
  if (slot < 1 || slot > 7 || st == S_PWR_A || (st != S_IDLE && st != S_DONE)) return false;
  slot_no = slot; slot_crc = crc;
  if (!begin(PSRC_SLOT)) return false;
  shown = slot;
  return true;
}

bool panel_show_fill(uint8_t code)
{
  fill_code = code & 3u;
  if (!begin(PSRC_FILL)) return false;
  shown = fill_code;
  return true;
}

void panel_set_temp(int8_t celsius) { temp_c = celsius; }

void panel_start(void)
{
#if PANEL_SPLASH_ENABLE
  (void)panel_show_splash();
#endif
}

void panel_poll(void)
{
  if (st == S_IDLE || st == S_DONE) {
    return;
  }
  uint32_t now = RAIL_GetTime();
  if (wk == W_TIME) {
    if ((int32_t)(now - t_end) < 0) return;
    wk = W_NONE;
  } else if (wk == W_BUSY) {
    if (busy_high()) {
      wk = W_NONE;
    } else if ((int32_t)(now - t_end) >= 0) {
      wk = W_NONE;
      if (busy_soft) {
        if (!err) err = busy_err;
      } else {
        fail(busy_err);
        g_diag.pn_err = err;
        g_diag.pn_state = (uint32_t)st;
        return;
      }
    } else {
      return;
    }
  }
  // the next step touches the SPI / GPIO: while connected only when the radio leaves enough free time
  if (ll_connected() && ll_free_us() < STEP_FREE_US) {
    return;
  }
  run();
  g_diag.pn_state = (uint32_t)st;
  g_diag.pn_err = err;
  if (st == S_DONE) {
    last_ms = (RAIL_GetTime() - t_start) / 1000u;
    g_diag.pn_t_ms = last_ms;
    status = err ? PST_DONE_ERR : PST_DONE_OK;
    n_done++;
    g_diag.pn_count = n_done;
  }
}

bool panel_busy(void) { return st != S_IDLE && st != S_DONE; }
bool panel_done(void) { return st == S_DONE; }
panel_status_t panel_status(void) { return status; }
uint8_t panel_error(void) { return err; }
panel_src_t panel_source(void) { return src; }
uint8_t panel_shown(void) { return shown; }
uint32_t panel_count(void) { return n_done; }
uint32_t panel_last_ms(void) { return last_ms; }
bool panel_flash_busy(void) { return src == PSRC_SLOT && st >= S_PWR_A && st <= S_IMG; }
