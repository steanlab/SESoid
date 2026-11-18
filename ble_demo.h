// SES V300: demo pictures, generated line by line (no flash needed; the frame of 46 080 bytes does not fit into RAM).
#ifndef BLE_DEMO_H
#define BLE_DEMO_H

#include <stdint.h>

#define DEMO_COUNT 9
#define DEMO_PAT_HW 100   // the TAG HARDWARE screen (text from demo_hw_set)
// 0 calibration chart, 1 chessboards, 2 Julia set, 3 Sierpinski / XOR, 4 dithered tone ramps, 5 Truchet arcs, 6 TV test chart, 7 Sierpinski carpet, 8 Newton fractal
// line = panel line 0..719, same mapping as the boot splash (ble_splash.h); out = 64 bytes, 2 bits per pixel, 00 black 01 white 10 yellow 11 red
void demo_gen_line(uint8_t pattern, int line, uint8_t *out);
uint8_t demo_px(uint8_t pattern, int u, int v);   // picture pixel u = column 0..719, v = row 0..255 (for the host test)

// TAG HARDWARE screen: up to DEMO_HW_ITEMS items (label in red, value in black), filled by the caller before the screen is shown
#define DEMO_HW_ITEMS 9
void demo_hw_set(uint8_t i, const char *label, const char *value);   // label up to 15, value up to 46 characters (ASCII)
void demo_hw_clear(void);

#endif
