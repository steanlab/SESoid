// Host test of the demo pictures: determinism, all four colours used, line generator = pixel function, PNG source dump.
#include <stdio.h>
#include <string.h>
#include "ble_demo.h"
#include "ble_splash.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
static void hw_sample(void)
{
  demo_hw_set(0, "PROCESSOR", "EFR32FG22C121F256GM40, M33 38.4 MHz");
  demo_hw_set(1, "PANEL", "5.9 in, 720 x 256, BWRY, PDi 0x0D04");
  demo_hw_set(2, "TEMPERATURE", "panel 24.0 C, chip 25.1 C");
  demo_hw_set(3, "BATTERY", "5 x CR2450, 3.00 V, 94 %");
  demo_hw_set(4, "FLASH CHIP", "Winbond EF 40 13, 512 KiB, slides 3/10");
  demo_hw_set(5, "CRYSTALS", "38.4 MHz, 32.768 kHz (+33 ppm)");
  demo_hw_set(6, "RADIO", "BLE 2.4 GHz, TX 0.0 dBm, RSSI -62 dBm");
  demo_hw_set(7, "NFC", "NTAG I2C plus, NDEF 112 bytes");
  demo_hw_set(8, "FIRMWARE", "SESoid 2.0, up 1 h 23 min, 12 screens");
}
int main(int argc, char **argv)
{
  hw_sample();
  for (int p = 0; p < DEMO_COUNT; p++) {
    long cnt[4] = {0, 0, 0, 0};
    uint8_t line[64];
    for (int l = 0; l < PANEL_LINES; l++) {
      demo_gen_line((uint8_t)p, l, line);
      for (int i = 0; i < 256; i++) {
        int c = (line[i >> 2] >> (6 - 2 * (i & 3))) & 3;
        cnt[c]++;
        if (c != demo_px((uint8_t)p, l, 255 - i)) { CHECK(0, "line generator differs from the pixel function"); l = PANEL_LINES; break; }
      }
    }
    printf("pattern %d: K %ld W %ld Y %ld R %ld\n", p, cnt[0], cnt[1], cnt[2], cnt[3]);
    if (p != 1 && p != 2 && p != 3) { for (int c = 0; c < 4; c++) CHECK(cnt[c] > 1000, "a colour is (almost) missing"); }
    CHECK(cnt[0] + cnt[1] > 20000, "too little black/white");
    if (argc > 1) {                                  // dump: 720x256 index bytes
      char name[256]; snprintf(name, sizeof name, "%s/demo%d.idx", argv[1], p);
      FILE *f = fopen(name, "wb");
      for (int v = 0; v < 256; v++) for (int u = 0; u < 720; u++) fputc(demo_px((uint8_t)p, u, v), f);
      fclose(f);
    }
  }
  if (argc > 1) {
    char name[256]; snprintf(name, sizeof name, "%s/demo_hw.idx", argv[1]);
    FILE *f = fopen(name, "wb");
    for (int v = 0; v < 256; v++) for (int u = 0; u < 720; u++) fputc(demo_px(DEMO_PAT_HW, u, v), f);
    fclose(f);
  }
  printf(fails ? "FAILED\n" : "ALL OK\n");
  return fails != 0;
}
