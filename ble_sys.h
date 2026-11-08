// SES V300 (not badge154): system services: reset cause, watchdog, reboot, device identification.
#ifndef BLE_SYS_H
#define BLE_SYS_H

#include <stdint.h>
#include <stdbool.h>

#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define WDOG_ENABLE 1            // 0 = no watchdog (e.g. for debugging with breakpoints)

// reset cause bits as in EMU->RSTCAUSE of the chip, captured before anything else runs
#define SYS_RST_POR   (1u << 0)
#define SYS_RST_PIN   (1u << 1)
#define SYS_RST_EM4   (1u << 2)
#define SYS_RST_WDOG  (1u << 3)
#define SYS_RST_LOCKUP (1u << 5)
#define SYS_RST_SYSREQ (1u << 6)

void sys_init(void);             // first thing in app_init: read and clear the reset cause
void sys_watchdog_start(void);   // last thing in app_init (8 s timeout, fed by sys_feed())
void sys_feed(void);             // main loop, every iteration
void sys_reboot(void);           // never returns
uint32_t sys_reset_cause(void);
void sys_eui64(uint8_t out[8]);  // the chip's unique 64-bit ID (never publish it)
uint32_t sys_part(void);         // DEVINFO PART word
uint32_t sys_mem_kb(void);       // flash KB in the low 16 bits, RAM KB in the high 16 bits

#endif
