// SES V300 (not badge154): e-paper panel update (PDi "BWRY Medium", chip 0x0D04) as a NON-blocking state machine.
// Ported from the verified bare-metal program epd_probe/ramtest/panel_draw.c (same sequence, same pins, bit-banged SPI).
// One update = power on, reset, OTP, init, image, DC/DC on, refresh, DC/DC off, power off (about 30 s). The picture comes from
//   - the boot splash (ble_splash.c, generated line by line),
//   - a slide slot of the external flash (a raw 46 080 byte frame, streamed 256 bytes at a time; its CRC is checked on the way),
//   - a plain colour (cleaning the panel).
// panel_poll() is called from the main loop, does a short step (a few ms at most) or only checks a timer / the BUSY pin.
// Bluetooth advertising and connections keep running meanwhile (steps that touch the SPI are only done when ll_free_us()
// leaves enough radio-free time while connected).
#ifndef BLE_PANEL_H
#define BLE_PANEL_H

#include <stdint.h>
#include <stdbool.h>

#define PANEL_SPLASH_ENABLE 1   // 0 = no boot splash

// panel pins (EFR32FG22 port C): SDA PC0, SCLK PC1, CS PC2, DC PC3, RST PC4, ON/OFF PC6; BUSY PA7 (input)

#define PANEL_FRAME_BYTES 46080u

typedef enum { PSRC_NONE = 0, PSRC_SPLASH = 1, PSRC_SLOT = 2, PSRC_FILL = 3 } panel_src_t;
typedef enum { PST_IDLE = 0, PST_RUNNING = 1, PST_DONE_OK = 2, PST_DONE_ERR = 3 } panel_status_t;

void panel_start(void);                              // main context, once after RAIL init: the boot splash
void panel_poll(void);                               // main context, as often as possible
bool panel_show_splash(void);                        // each returns false when an update is already running
bool panel_show_slot(uint8_t slot, uint32_t crc);    // slot 1..7 of the flash; crc = the record's CRC (checked while streaming)
bool panel_show_fill(uint8_t code);                  // 0 black, 1 white, 2 yellow, 3 red
void panel_set_temp(int8_t celsius);                 // handed to the panel for choosing the waveform (command 0xE6)

bool panel_busy(void);                               // an update is in progress
bool panel_done(void);                               // an update has finished (successfully or not)
panel_status_t panel_status(void);
uint8_t panel_error(void);                           // 0 = none, see ble_panel.c
panel_src_t panel_source(void);                      // what the last / current update shows
uint8_t panel_shown(void);                           // slot (PSRC_SLOT) or colour code (PSRC_FILL)
uint32_t panel_count(void);                          // finished updates since reset
uint32_t panel_last_ms(void);                        // duration of the last finished update
bool panel_flash_busy(void);                         // the update still needs to read the external flash (nobody else may write it)

#endif
