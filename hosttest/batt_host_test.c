// host test of ble_batt.c: thresholds, hysteresis, schedule
#include <stdio.h>
#include "../ble_batt.h"
extern uint16_t batt_host_mv;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void)
{
  batt_set_thresholds(2700, 2500);
  CHECK(batt_mv() == 0 && batt_level() == BATT_OK && batt_age_s(10) == 0xFFFFFFFFu, "nothing measured yet");
  batt_host_mv = 3000; batt_poll(1, true); CHECK(batt_mv() == 0, "first reading only after %u s", BATT_FIRST_S);
  batt_poll(3, false); CHECK(batt_mv() == 0, "not while the panel works");
  batt_poll(3, true); CHECK(batt_mv() == 3000 && batt_level() == BATT_OK, "first reading");
  batt_host_mv = 2600; batt_poll(100, true); CHECK(batt_mv() == 3000, "next reading only after the period");
  batt_poll(3 + BATT_PERIOD_S, true); CHECK(batt_mv() == 2600 && batt_level() == BATT_LOW, "low (%u, %u)", batt_mv(), batt_level());
  batt_host_mv = 2720; batt_measure_now(2000); CHECK(batt_level() == BATT_LOW, "stays low inside the hysteresis band");
  batt_host_mv = 2800; batt_measure_now(2100); CHECK(batt_level() == BATT_OK, "back to ok above the band");
  batt_host_mv = 2400; batt_measure_now(2200); CHECK(batt_level() == BATT_CRIT, "critical");
  batt_host_mv = 2520; batt_measure_now(2300); CHECK(batt_level() == BATT_CRIT, "stays critical inside the band");
  batt_host_mv = 2560; batt_measure_now(2400); CHECK(batt_level() == BATT_LOW, "critical -> low above the band");
  batt_host_mv = 2900; batt_measure_now(2500); CHECK(batt_level() == BATT_OK, "ok");
  batt_set_thresholds(2500, 2700); batt_host_mv = 2400; batt_measure_now(2600); CHECK(batt_level() == BATT_CRIT, "critical threshold above low is clipped to low (%u)", batt_level());
  batt_host_mv = 0; batt_measure_now(2700); CHECK(batt_mv() == 2400, "a failed reading keeps the old one");
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails != 0;
}
