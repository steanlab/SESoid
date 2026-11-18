// SES V300: main loop. While nothing needs the CPU (not connected, no panel update, no flash work) the core sleeps in EM1 and wakes every 2 ms
// (sleep timer); EM1 keeps the 38.4 MHz clock and the radio timer running, so all time bases of the firmware stay valid. EM2 is used only with the
// setting "deep sleep", while the Bluetooth window is closed and nothing runs (see app_deep_ok): the radio timer (RAIL_GetTime) stops there and is set
// forward by the time slept. While connected or while a panel update runs the loop spins as before (timing is tight).
#include <stdbool.h>
#include "sl_system_init.h"
#include "sl_system_process_action.h"
#include "sl_power_manager.h"
#include "sl_sleeptimer.h"
#include "rail.h"

void app_init(void);
void app_process_action(void);
bool app_sleep_ok(void);
bool app_deep_ok(void);

#define DEEP_MS 100u            // wake-up period in EM2: also the longest delay of a phone tap being noticed (the NFC chip is read every 200 ms)
static sl_sleeptimer_timer_handle_t wake_timer;
static void wake_cb(sl_sleeptimer_timer_handle_t *t, void *d) { (void)t; (void)d; }

int main(void)
{
  sl_system_init();
  sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);        // never deeper than EM1, see above
  app_init();
  while (1) {
    sl_system_process_action();
    app_process_action();
    if (app_deep_ok()) {                                             // EM2 (setting "deep sleep"): the radio timer stops, so it is set forward by the time slept
      uint32_t t0 = RAIL_GetTime();
      uint64_t k0 = sl_sleeptimer_get_tick_count64();
      if (sl_sleeptimer_restart_timer_ms(&wake_timer, DEEP_MS, wake_cb, NULL, 0, 0) == SL_STATUS_OK) {
        sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
        sl_power_manager_sleep();
        sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
        uint32_t slept = sl_sleeptimer_tick_to_ms((uint32_t)(sl_sleeptimer_get_tick_count64() - k0)) * 1000u;
        if ((uint32_t)(RAIL_GetTime() - t0) + 2000u < slept) RAIL_SetTime(t0 + slept);   // RAIL did not count the sleep (it normally does not in EM2)
      }
      continue;
    }
    if (app_sleep_ok() && sl_sleeptimer_restart_timer_ms(&wake_timer, 2, wake_cb, NULL, 0, 0) == SL_STATUS_OK) {
      sl_power_manager_sleep();
    }
  }
}
