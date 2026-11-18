// SES V300 (not badge154): system services, see ble_sys.h
#include "ble_sys.h"
#include "ble_diag.h"
#include "em_device.h"

static uint32_t s_cause;

void sys_init(void)
{
  s_cause = EMU->RSTCAUSE;
  EMU->CMD = EMU_CMD_RSTCAUSECLR;
  g_diag.sys_reset = s_cause;
}

void sys_watchdog_start(void)
{
#if WDOG_ENABLE
  CMU->CLKEN0 |= CMU_CLKEN0_WDOG0;
  CMU->WDOG0CLKCTRL = (CMU->WDOG0CLKCTRL & ~_CMU_WDOG0CLKCTRL_CLKSEL_MASK) | CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO;   // 1 kHz, runs without any crystal
  WDOG0->CFG = (10u << _WDOG_CFG_PERSEL_SHIFT);       // period 2^(3+10) + 1 = 8193 ticks = about 8.2 s; software clear only; stops while the core is halted by a debugger
  WDOG0->EN = WDOG_EN_EN;
  g_diag.sys_wdog = 1;
#endif
}

void sys_feed(void)
{
#if WDOG_ENABLE
  if (!(WDOG0->SYNCBUSY & WDOG_SYNCBUSY_CMD)) {
    WDOG0->CMD = WDOG_CMD_CLEAR;
  }
#endif
}

void sys_reboot(void)
{
  NVIC_SystemReset();
  for (;;) { }
}

uint32_t sys_reset_cause(void) { return s_cause; }

void sys_eui64(uint8_t out[8])
{
  uint32_t l = DEVINFO->EUI64L, h = DEVINFO->EUI64H;
  for (int i = 0; i < 4; i++) { out[i] = (uint8_t)(l >> (8 * i)); out[4 + i] = (uint8_t)(h >> (8 * i)); }
}

uint32_t sys_part(void) { return DEVINFO->PART; }

uint32_t sys_mem_kb(void)
{
  uint32_t m = DEVINFO->MSIZE;
  return (m & 0xFFFFu) | (((m >> 16) & 0x7FFu) << 16);
}

// EFR32xG22: EMU->TEMP holds the die temperature in 0.25 K steps (bits 10:0), 0 = no measurement yet. The die is the tag's ambient temperature
// while the tag sleeps; a few tenths warmer right after radio or panel activity.
int16_t sys_temp_dc(void)
{
  uint32_t v = EMU->TEMP & 0x7FFu;
  if (v == 0u) return SYS_TEMP_NA;
  return (int16_t)(((int32_t)v * 10 + 2) / 4 - 2731);        // tenths of K minus 273.15 K, rounded
}
