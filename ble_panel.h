// SES V300 (not badge154): e-paper panel update (PDi "BWRY Medium", chip 0x0D04) as a NON-blocking state machine.
// Ported from the verified bare-metal program epd_probe/ramtest/panel_draw.c (same sequence, same pins, bit-banged SPI).
// panel_start() begins one complete update (power on, reset, OTP, init, image, DC/DC on, refresh, DC/DC off, power off);
// panel_poll() is called from the main loop, does a short step (a few ms at most) or only checks a timer / the BUSY pin.
// The whole update takes about 30 s; Bluetooth advertising and connections keep running meanwhile (steps that touch the
// SPI are only done when ll_free_us() leaves enough radio-free time).
#ifndef BLE_PANEL_H
#define BLE_PANEL_H

#include <stdint.h>
#include <stdbool.h>

#define PANEL_SPLASH_ENABLE 1   // 0 = no boot splash

// panel pins (EFR32FG22 port C): SDA PC0, SCLK PC1, CS PC2, DC PC3, RST PC4, ON/OFF PC6; BUSY PA7 (input)
// ble_panel.c: PANEL_TEMP_C = temperature handed to the panel for choosing the waveform (room temperature)

void panel_start(void);   // main context, once after RAIL init; draws the splash picture (ble_splash.c)
void panel_poll(void);    // main context, as often as possible
bool panel_busy(void);    // an update is in progress
bool panel_done(void);    // an update has finished (successfully or not; see g_diag.pn_err)

#endif
