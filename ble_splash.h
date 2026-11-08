// SES V300 (not badge154): boot splash picture, generated line by line (the frame of 46 080 bytes does not fit into RAM).
#ifndef BLE_SPLASH_H
#define BLE_SPLASH_H

#include <stdint.h>

#define PANEL_LINES 720   // panel "lines" (the panel's long direction)
#define PANEL_PIX   256   // pixels per line
#define PANEL_LINE_BYTES 64

// Mapping of the landscape picture (u = column 0..719, v = row 0..255) onto panel lines, see README_hardware_SES-V300.md:
//   FPC end on the LEFT (default):  line = u,        pixel = 255 - v
//   FPC end on the RIGHT:           line = 719 - u,  pixel = v          (define SPLASH_FPC_RIGHT)
// out = 64 bytes of one line, 2 bits per pixel, first pixel in the high bits: 00 black, 01 white, 10 yellow, 11 red.
void splash_gen_line(int line, uint8_t *out);

#endif
