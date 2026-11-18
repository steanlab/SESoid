// SES V300: demo pictures (see ble_demo.h). Integer arithmetic only.
#include "ble_demo.h"
#include "ble_splash.h"

enum { K = 0, W = 1, Y = 2, R = 3 };

static int bayer(int x, int y)                      // 8x8 ordered dither threshold 0..63
{
  int v = 0;
  for (int b = 0; b < 3; b++) { v = (v << 2) | (((x & 1) ^ (y & 1)) << 1) | (y & 1); x >>= 1; y >>= 1; }
  return v;
}

static uint8_t mix(int u, int v, uint8_t a, uint8_t b, int level64)   // level 0 = a ... 64 = b
{
  return bayer(u, v) < level64 ? b : a;
}

static int in_rect(int u, int v, int x, int y, int w, int h) { return u >= x && u < x + w && v >= y && v < y + h; }

// ---- 0: calibration chart (like the one on the page): solid fields, mixing ramps, thin lines, checks, frames
static uint8_t px_calib(int u, int v)
{
  if (u < 2 || v < 2 || u >= 718 || v >= 254) return K;
  static const int16_t ramp[6][4] = { {K, W, 24, 62}, {K, R, 24, 108}, {K, Y, 24, 154}, {W, R, 372, 62}, {W, Y, 372, 108}, {Y, R, 372, 154} };
  for (int i = 0; i < 4; i++) if (in_rect(u, v, 24 + i * 174, 8, 150, 46)) return (uint8_t)i;
  for (int r = 0; r < 6; r++) {
    int x0 = ramp[r][2], y0 = ramp[r][3];
    if (in_rect(u, v, x0, y0, 324, 40)) return mix(u, v, (uint8_t)ramp[r][0], (uint8_t)ramp[r][1], ((u - x0) / 36) * 8);
  }
  if (v >= 200 && v < 246) {
    int y = v - 200;
    if (u >= 24 && u < 96) return ((u - 24) & 1) ? W : K;
    if (u >= 104 && u < 176) return (((u - 104) >> 1) & 1) ? W : K;
    if (u >= 184 && u < 256) return (y & 1) ? W : K;
    if (u >= 264 && u < 336) return ((u + y) & 1) ? W : K;
    if (u >= 344 && u < 416) return ((u + y) & 1) ? W : R;
    if (u >= 424 && u < 496) return ((u + y) & 1) ? W : Y;
    if (u >= 504 && u < 576) return (((u - 504 + y) >> 2) & 1) ? R : Y;
    if (u >= 584 && u < 696) {
      for (int i = 0; i < 4; i++) {
        int x = 584 + i * 6, w = 112 - i * 12;
        if (in_rect(u, v, x, 200 + i * 6, w, 2) || in_rect(u, v, x, 200 + 46 - 2 - i * 6, w, 2)) return (i & 1) ? W : K;
      }
    }
  }
  return W;
}

// ---- 1: chessboards: 32 px black/white, 16 px four colours, then 4 px and 1 px black/white
static uint8_t px_chess(int u, int v)
{
  if (u < 3 || v < 3 || u >= 717 || v >= 253) return K;
  if (u < 240) return (((u >> 5) ^ (v >> 5)) & 1) ? W : K;
  if (u < 480) {
    int cx = (u - 240) >> 4, cy = v >> 4;
    return ((cx ^ cy) & 1) ? ((cx & 2) ? R : Y) : ((cy & 2) ? K : W);
  }
  if (u < 600) return ((((u - 480) >> 2) ^ (v >> 2)) & 1) ? W : K;
  return (((u - 600) ^ v) & 1) ? W : K;
}

// ---- 2 / 3: Mandelbrot and Julia sets in Q4.26 fixed point
#define FQ 26
static uint8_t px_fractal(int u, int v, int julia)
{
  int32_t cr, ci, zr, zi;
  int64_t step = julia ? (int64_t)(3.6 / 720 * (1 << FQ) + 0.5) : (int64_t)(3.4 / 720 * (1 << FQ) + 0.5);
  int64_t x = (int64_t)(u - 360) * step, y = (int64_t)(v - 128) * step;
  if (julia) { zr = (int32_t)x; zi = (int32_t)y; cr = (int32_t)(-0.8 * (1 << FQ)); ci = (int32_t)(0.156 * (1 << FQ)); }
  else { cr = (int32_t)(x - (int64_t)(0.55 * (1 << FQ))); ci = (int32_t)y; zr = 0; zi = 0; }
  const int maxit = julia ? 40 : 36;
  int it;
  for (it = 0; it < maxit; it++) {
    int64_t rr = ((int64_t)zr * zr) >> FQ, ii = ((int64_t)zi * zi) >> FQ;
    if (rr + ii > ((int64_t)4 << FQ)) break;
    int32_t nzi = (int32_t)((((int64_t)zr * zi) >> (FQ - 1)) + ci);
    zr = (int32_t)(rr - ii + cr); zi = nzi;
  }
  if (it >= maxit) return K;
  if (it < 3) return W;
  switch (it % 3) { case 0: return Y; case 1: return R; default: return W; }
}

// ---- 4: Sierpinski triangle / XOR texture
static uint8_t px_sierp(int u, int v)
{
  int x = u + 8, y = v;                              // a little offset so that the pattern does not start on the edge
  if ((x & y) == 0) return K;
  static const uint8_t c[3] = { W, Y, R };
  return c[(((x ^ y) >> 5) + (x >> 8)) % 3];
}

// ---- 5: two zone plates (interference / moire); r^2 / 256 steps around the centres, colours in the order K R Y W
static uint8_t px_zone(int u, int v)
{
  static const uint8_t ord[4] = { K, R, Y, W };
  int dv = v - 128, du1 = u - 240, du2 = u - 480;
  int a = (du1 * du1 + dv * dv) >> 8, b = (du2 * du2 + dv * dv) >> 8;
  return ord[(a + b) & 3];
}

// ---- 6: tone ramps: eight horizontal bands, every one a dithered gradient from one colour to another (64 levels), thin white marks every 1/8
static uint8_t px_ramps(int u, int v)
{
  static const uint8_t pair[8][2] = { {K, W}, {W, Y}, {Y, R}, {R, K}, {K, Y}, {K, R}, {W, R}, {Y, W} };
  int band = v >> 5, y = v & 31;
  if (u < 2 || u >= 718) return K;
  if (y < 2 && band) return K;
  if (y >= 28 && ((u % 90) < 2)) return K;           // ticks
  int level = (u * 65) / 720;
  return mix(u, v, pair[band][0], pair[band][1], level);
}

// ---- 7: Truchet tiles 16 x 16 with quarter circles (black and red) on white
static uint8_t px_truchet(int u, int v)
{
  int tx = u >> 4, ty = v >> 4, x = u & 15, y = v & 15;
  uint32_t h = (uint32_t)tx * 0x9E3779B1u ^ (uint32_t)ty * 0x85EBCA6Bu;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 13;
  if ((h >> 16) & 1) x = 15 - x;                    // mirrored tile
  int d1 = x * x + y * y;                           // distance^2 to the corner (0,0)
  int dx = 16 - x, dy = 16 - y;
  int d2 = dx * dx + dy * dy;                       // to the corner (16,16)
  if (d1 >= 42 && d1 <= 90) return K;
  if (d2 >= 42 && d2 <= 90) return R;
  if (d1 < 20 || d2 < 20) return Y;
  return W;
}

uint8_t demo_px(uint8_t pattern, int u, int v)
{
  switch (pattern) {
    case 0: return px_calib(u, v);
    case 1: return px_chess(u, v);
    case 2: return px_fractal(u, v, 0);
    case 3: return px_fractal(u, v, 1);
    case 4: return px_sierp(u, v);
    case 5: return px_zone(u, v);
    case 6: return px_ramps(u, v);
    default: return px_truchet(u, v);
  }
}

void demo_gen_line(uint8_t pattern, int line, uint8_t *out)
{
  for (int i = 0; i < PANEL_LINE_BYTES; i++) {
    uint32_t b = 0;
    for (int j = 0; j < 4; j++) {
      int p = i * 4 + j;
      b = (b << 2) | (demo_px(pattern, line, PANEL_PIX - 1 - p) & 3u);     // FPC end on the left: line = u, pixel = 255 - v
    }
    out[i] = (uint8_t)b;
  }
}
