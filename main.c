// SES V300 stage 1 probe: plain main loop, no sleeping (the point is to look at the radio, not at the current draw).
#include "sl_system_init.h"
#include "sl_system_process_action.h"

void app_init(void);
void app_process_action(void);

int main(void)
{
  sl_system_init();
  app_init();
  while (1) {
    sl_system_process_action();
    app_process_action();
  }
}
