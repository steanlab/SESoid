// SES V300 (not badge154): battery monitoring. The five CR2450 cells are in parallel and the chip runs directly from them, so the chip's own supply
// (AVDD, read by the IADC through its AVDD/4 input) is the battery voltage. It is measured a few seconds after power-up and then every
// BATT_PERIOD_S seconds, only while the panel is idle (the panel draws current peaks that would pull the reading down).
//   level OK   : above the warning threshold
//   level LOW  : at or below the warning threshold            (the page shows a warning)
//   level CRIT : at or below the critical threshold           (automatic slide changes and cleaning cycles stop; manual ones still work)
// A level only improves again when the voltage is BATT_HYST_MV above its threshold (no flicker). Thresholds are set by the page (ble_slide settings).
// Accuracy: the IADC is read with its internal 1.21 V reference without a calibration, so the reading is good to about +-2 %.
#ifndef BLE_BATT_H
#define BLE_BATT_H

#include <stdint.h>
#include <stdbool.h>

#define BATT_PERIOD_S   600u
#define BATT_FIRST_S    3u
#define BATT_HYST_MV    40u

#define BATT_OK   0
#define BATT_LOW  1
#define BATT_CRIT 2

void batt_set_thresholds(uint16_t low_mv, uint16_t crit_mv);
void batt_poll(uint32_t now_s, bool panel_idle);      // main loop
uint8_t batt_level(void);
uint16_t batt_mv(void);                               // last reading, 0 = none yet
uint32_t batt_age_s(uint32_t now_s);                  // seconds since the last reading (0xFFFFFFFF = none)
void batt_measure_now(uint32_t now_s);                // forces a reading (the caller makes sure the panel is idle)

#endif
