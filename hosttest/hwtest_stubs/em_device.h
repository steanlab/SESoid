// host test stub: the registers the hardware tests use as plain memory (macro values copied from the Gecko SDK 4.4.1 headers of the EFR32FG22)
#pragma once
#include <stdint.h>
#define _LFXO_LOCK_LOCKKEY_UNLOCK 0x00001A20UL
#define _LFXO_CFG_MODE_XTAL 0x00000000UL
#define _CMU_CALCTRL_DOWNSEL_LFXO 0x00000004UL
#define _CMU_CALCTRL_DOWNSEL_LFRCO 0x0000000AUL
#define _CMU_CALCTRL_DOWNSEL_ULFRCO 0x0000000BUL
#define _CMU_CALCTRL_UPSEL_HFXO 0x00000002UL
#define CMU_CLKEN0_LFXO (0x1UL << 22)
#define CMU_CLKEN0_LFRCO (0x1UL << 21)
#define CMU_CLKEN0_GPIO (0x1UL << 26)
#define HFXO_STATUS_RDY (0x1UL << 0)
#define _HFXO_XTALCTRL_CTUNEXIANA_MASK 0xFF00UL
#define _HFXO_XTALCTRL_CTUNEXIANA_SHIFT 8
#define _HFXO_XTALCTRL_CTUNEXOANA_MASK 0xFF0000UL
#define _HFXO_XTALCTRL_CTUNEXOANA_SHIFT 16
#define _DEVINFO_MODXOCAL_HFXOCTUNEXIANA_SHIFT 0
#define _DEVINFO_MODXOCAL_HFXOCTUNEXOANA_SHIFT 8
#define LFXO_STATUS_ENS (0x1UL << 16)
#define LFXO_STATUS_RDY (0x1UL << 0)
#define LFXO_LOCK_LOCKKEY_UNLOCK (_LFXO_LOCK_LOCKKEY_UNLOCK << 0)
#define LFXO_CTRL_DISONDEMAND (0x1UL << 1)
#define LFXO_CTRL_FORCEEN (0x1UL << 0)
#define LFXO_CTRL_FAILDETEN (0x1UL << 4)
#define _LFXO_CAL_GAIN_SHIFT 8
#define _LFXO_CAL_CAPTUNE_SHIFT 0
#define LFXO_CFG_MODE_XTAL (_LFXO_CFG_MODE_XTAL << 4)
#define _LFXO_CFG_TIMEOUT_CYCLES32K 0x00000007UL
#define _LFXO_CFG_TIMEOUT_SHIFT 8
#define LFXO_IF_FAIL (0x1UL << 3)
#define LFRCO_STATUS_RDY (0x1UL << 0)
#define LFRCO_STATUS_ENS (0x1UL << 16)
#define LFRCO_CTRL_FORCEEN (0x1UL << 0)
#define CMU_CALCTRL_DOWNSEL_LFXO (_CMU_CALCTRL_DOWNSEL_LFXO << 28)
#define CMU_CALCTRL_DOWNSEL_LFRCO (_CMU_CALCTRL_DOWNSEL_LFRCO << 28)
#define CMU_CALCTRL_DOWNSEL_ULFRCO (_CMU_CALCTRL_DOWNSEL_ULFRCO << 28)
#define CMU_CALCTRL_UPSEL_HFXO (_CMU_CALCTRL_UPSEL_HFXO << 24)
#define CMU_CALCTRL_CONT (0x1UL << 23)
#define _CMU_CALCTRL_UPSEL_MASK 0xF000000UL
#define _CMU_CALCTRL_DOWNSEL_MASK 0xF0000000UL
#define _CMU_CALCTRL_CALTOP_MASK 0xFFFFFUL
#define CMU_CALCMD_CALSTART (0x1UL << 0)
#define CMU_CALCMD_CALSTOP (0x1UL << 1)
#define CMU_IF_CALRDY (0x1UL << 0)
#define _CMU_CALCNT_CALCNT_MASK 0xFFFFFUL

typedef struct { volatile uint32_t STATUS, CTRL; } LfrcoT;
typedef struct { volatile uint32_t LOCK, CTRL, CAL, CFG, STATUS, IF, IF_CLR; } LfxoT;
typedef struct { volatile uint32_t STATUS, XTALCTRL; } HfxoT;
typedef struct { volatile uint32_t MODXOCAL; } DevinfoT;
typedef struct { volatile uint32_t CLKEN0, STATUS, CALCMD, CALCTRL, CALCNT, IF, IF_CLR, SYSCLKCTRL, EM23GRPACLKCTRL, RTCCCLKCTRL; } CmuT;
typedef struct { volatile uint32_t MODEL, MODEH, DOUT, DIN; } GpioPortT;
typedef struct { GpioPortT P[4]; } GpioT;
extern LfrcoT g_lfrco; extern LfxoT g_lfxo; extern HfxoT g_hfxo; extern DevinfoT g_devinfo; extern CmuT g_cmu; extern GpioT g_gpio;
#define LFRCO (&g_lfrco)
#define LFXO (&g_lfxo)
#define HFXO0 (&g_hfxo)
#define DEVINFO (&g_devinfo)
#define CMU (&g_cmu)
#define GPIO (&g_gpio)
