// SES V300 (not badge154): boot splash picture generator (black background, white text), see ble_splash.h.
// The text is stored as two small 1-bit column bitmaps (ble_splash_data.h, made by tools/make_splash.py).
#include "ble_splash.h"
#include "ble_splash_data.h"

#define CODE_BLACK 0u
#define CODE_WHITE 1u

static inline uint32_t bm_get(const uint8_t *d, int hb, int du, int dv)
{
  return (d[du * hb + (dv >> 3)] >> (dv & 7)) & 1u;
}

// is the picture pixel (u, v) white?
static uint32_t white(int u, int v)
{
  int du = u - SPL_TITLE_X, dv = v - SPL_TITLE_Y;
  if (du >= 0 && du < SPL_TITLE_W && dv >= 0 && dv < SPL_TITLE_H) {
    return bm_get(spl_title, SPL_TITLE_HB, du, dv);
  }
  du = u - SPL_SUB_X;
  dv = v - SPL_SUB_Y;
  if (du >= 0 && du < SPL_SUB_W && dv >= 0 && dv < SPL_SUB_H) {
    return bm_get(spl_sub, SPL_SUB_HB, du, dv);
  }
  return 0;
}

void splash_gen_line(int line, uint8_t *out)
{
#ifdef SPLASH_FPC_RIGHT
  int u = PANEL_LINES - 1 - line;
#else
  int u = line;
#endif
  int in_title = (u >= SPL_TITLE_X && u < SPL_TITLE_X + SPL_TITLE_W);
  int in_sub = (u >= SPL_SUB_X && u < SPL_SUB_X + SPL_SUB_W);
  if (!in_title && !in_sub) {          // most lines are plain black
    for (int i = 0; i < PANEL_LINE_BYTES; i++) out[i] = 0;
    return;
  }
  for (int i = 0; i < PANEL_LINE_BYTES; i++) {
    uint32_t b = 0;
    for (int j = 0; j < 4; j++) {
      int p = i * 4 + j;
#ifdef SPLASH_FPC_RIGHT
      int v = p;
#else
      int v = PANEL_PIX - 1 - p;
#endif
      b = (b << 2) | (white(u, v) ? CODE_WHITE : CODE_BLACK);
    }
    out[i] = (uint8_t)b;
  }
}
