// host test stub: GPIO / CMU registers as C++ objects whose reads and writes call the panel model in panel_host_test.cpp
#pragma once
#include <stdint.h>
extern uint32_t sim_rd(uint32_t id);
extern void sim_wr(uint32_t id, uint32_t v);
struct Reg {
  uint32_t id;
  Reg &operator=(uint32_t x) { sim_wr(id, x); return *this; }
  operator uint32_t() const { return sim_rd(id); }
  Reg &operator|=(uint32_t x) { sim_wr(id, sim_rd(id) | x); return *this; }
};
struct PortRegs { Reg MODEL, MODEH, DOUT, DIN; };
struct GpioT { PortRegs P[4]; PortRegs P_SET[4]; PortRegs P_CLR[4]; };
struct CmuT { Reg CLKEN0; };
extern GpioT g_gpio;
extern CmuT g_cmu;
#define GPIO (&g_gpio)
#define CMU (&g_cmu)
#define CMU_CLKEN0_GPIO (1u << 26)
extern uint32_t SystemCoreClock;
