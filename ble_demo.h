// SES V300: demo pictures, generated line by line (no flash needed; the frame of 46 080 bytes does not fit into RAM).
#ifndef BLE_DEMO_H
#define BLE_DEMO_H

#include <stdint.h>

#define DEMO_COUNT 8
// 0 calibration chart, 1 chessboards, 2 Mandelbrot set, 3 Julia set, 4 Sierpinski / XOR, 5 zone plates (moire), 6 dithered tone ramps, 7 Truchet arcs
// line = panel line 0..719, same mapping as the boot splash (ble_splash.h); out = 64 bytes, 2 bits per pixel, 00 black 01 white 10 yellow 11 red
void demo_gen_line(uint8_t pattern, int line, uint8_t *out);
uint8_t demo_px(uint8_t pattern, int u, int v);   // picture pixel u = column 0..719, v = row 0..255 (for the host test)

#endif
