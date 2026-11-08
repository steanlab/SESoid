// SES V300 (not badge154): hardware tests started from the "Инструменты" tab of the web page.
//   1. Clock test: is there a 32.768 kHz crystal (LFXO)? Measures LFXO, LFRCO and ULFRCO against the 38.4 MHz crystal (HFXO) that the radio runs on.
//   2. Pin monitor: reads the GPIO pins that nothing on this tag uses, to find where the NFC chip is wired (a field detect / interrupt line
//      changes level when a phone is held to the tag).
// Both are non-blocking (one small step per hwtest_poll() call) and put every register back when they are done.
#ifndef BLE_HWTEST_H
#define BLE_HWTEST_H

#include <stdint.h>
#include <stdbool.h>

// ---- clock test (answer 0xB9, see ble_ctl.h). The counts are HFXO cycles during CT_DOWN_* cycles of the measured clock.
#define CT_DOWN_LFXO   512u       // 512 cycles of 32.768 kHz = 15.6 ms -> about 600 000 HFXO counts (20-bit counter, max 1 048 575)
#define CT_DOWN_LFRCO  512u
#define CT_DOWN_ULFRCO 16u        // 16 cycles of about 1 kHz = about 16 ms

typedef struct {
  uint8_t  state;                 // 0 never run, 1 running, 2 finished
  uint8_t  flags;                 // bit0 HFXO ready, bit1 LFXO was already on, bit2 LFXO started, bit3 LFXO failure flag, bit4 LFXO count valid,
                                  // bit5 LFRCO count valid, bit6 ULFRCO count valid, bit7 LFXO start timed out
  uint8_t  sel;                   // clock selections: bits0-2 SYSCLK source (1 FSRCO, 2 HFRCODPLL, 3 HFXO, 4 CLKIN0), bits3-4 EM23 group A, bits5-6 RTCC (1 LFRCO, 2 LFXO, 3 ULFRCO)
  uint8_t  ctune_xi, ctune_xo;    // HFXO tuning capacitors now in use
  uint8_t  fact_xi, fact_xo;      // the factory values from the device information page
  uint16_t lfxo_ms;               // LFXO start-up time, 0xFFFF if it never became ready
  uint32_t cnt_lfxo, cnt_lfrco, cnt_ulfrco;
  uint8_t  captune;               // LFXO load capacitance setting used by the test
} hwtest_clock_t;

// ---- pin monitor. Pin index: PA0..PA8 = 0..8, PB0..PB4 = 9..13, PC0..PC7 = 14..21, PD0..PD3 = 22..25.
#define PM_PINS        26
#define PM_MODE_OFF    0
#define PM_MODE_FLOAT  1
#define PM_MODE_PULLUP 2
#define PM_MODE_PULLDN 3
#define PM_FLAG_SWD    0x80       // in the mode byte: also watch PA1..PA3 (SWD pads), off by default
#define PM_MAX_S       300u       // the monitor stops by itself after 5 minutes

typedef struct {
  uint8_t  mode;                  // PM_MODE_*, with PM_FLAG_SWD
  uint16_t elapsed_s;
  uint32_t level;                 // present level of every monitored pin (bit n = pin n)
  uint32_t seen_hi, seen_lo;      // pins that were seen high / low since the start (or since the history was cleared)
  uint16_t edges;                 // level changes seen
  uint8_t  last;                  // pin index of the last change, 0xFF none
} hwtest_pins_t;

void hwtest_reset(void);                                  // new connection / disconnect: stops the pin monitor and puts the pins back
void hwtest_poll(void);                                   // main loop
bool hwtest_clock_start(void);                            // false if a test is already running
const hwtest_clock_t *hwtest_clock(void);
bool hwtest_pins_start(uint8_t mode);                     // PM_MODE_* (0 stops); a new start clears the history
void hwtest_pins_clear(void);
const hwtest_pins_t *hwtest_pins(void);
bool hwtest_pins_changed(void);                           // true once after a new pin got both levels (or the state changed): the answer should be sent
uint32_t hwtest_pins_mask(void);                          // pins watched in the present mode (bit n = pin n)
uint32_t hwtest_pins_owned(void);                         // pins used by this firmware or reserved (never touched), for the page

#endif
