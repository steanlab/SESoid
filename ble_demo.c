// SES V300: demo pictures (see ble_demo.h). Integer arithmetic only.
#include "ble_demo.h"
#include "ble_splash.h"
#include "ble_demo_data.h"
#include <string.h>

extern const uint8_t demo_font7x14[95][14];

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

// ---- calibration chart (like the one on the page): solid fields, mixing ramps, thin lines, checks, frames
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

// ---- chessboards: 32 px black/white, 16 px four colours, then 4 px and 1 px black/white
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

// ---- Julia set in Q4.26 fixed point
#define FQ 26
static uint8_t px_julia(int u, int v)
{
  const int64_t step = (int64_t)(3.6 / 720 * (1 << FQ) + 0.5);
  int32_t zr = (int32_t)((int64_t)(u - 360) * step), zi = (int32_t)((int64_t)(v - 128) * step);
  const int32_t cr = (int32_t)(-0.8 * (1 << FQ)), ci = (int32_t)(0.156 * (1 << FQ));
  const int maxit = 40;
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

// ---- Sierpinski triangle / XOR texture
static uint8_t px_sierp(int u, int v)
{
  int x = u + 8, y = v;                              // a little offset so that the pattern does not start on the edge
  if ((x & y) == 0) return K;
  static const uint8_t c[3] = { W, Y, R };
  return c[(((x ^ y) >> 5) + (x >> 8)) % 3];
}

// ---- tone ramps: eight horizontal bands, every one a dithered gradient from one colour to another (64 levels), thin white marks every 1/8
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

// ---- Truchet tiles 16 x 16 with quarter circles (black and red) on white
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


// ---- TV test chart (same picture as the page's "TV chart"): grid, circles, diagonals, bars, gradations, line packets, mixing ramps, checks
static int in_ring(int u, int v, int cx, int cy, int r)       // |distance - r| < 1.1, integer arithmetic in tenths of a pixel
{
  int x = 10 * u + 5 - 10 * cx, y = 10 * v + 5 - 10 * cy, d2 = x * x + y * y, lo = 10 * r - 11, hi = 10 * r + 11;
  return d2 > lo * lo && d2 < hi * hi;
}
static int frame_hit(int u, int v, int x, int y, int w, int h) { return in_rect(u, v, x, y, w, h) && (u == x || u == x + w - 1 || v == y || v == y + h - 1); }
static uint8_t px_tv(int u, int v)
{
  if (frame_hit(u, v, 0, 0, 720, 256) || in_rect(u, v, 0, 0, 720, 2) || in_rect(u, v, 0, 254, 720, 2) || in_rect(u, v, 0, 0, 2, 256) || in_rect(u, v, 718, 0, 2, 256)) {
    if (in_rect(u, v, 2, 2, 14, 14) || in_rect(u, v, 704, 2, 14, 14) || in_rect(u, v, 2, 240, 14, 14) || in_rect(u, v, 704, 240, 14, 14)) return R;
    return K;
  }
  if (in_rect(u, v, 2, 2, 14, 14) || in_rect(u, v, 704, 2, 14, 14) || in_rect(u, v, 2, 240, 14, 14) || in_rect(u, v, 704, 240, 14, 14)) return R;
  // left block
  if (in_rect(u, v, 18, 18, 214, 220)) {
    if (frame_hit(u, v, 18, 18, 214, 220)) return K;
    if (in_rect(u, v, 22, 22, 204, 56)) { if (frame_hit(u, v, 22, 22, 204, 56)) return K; static const uint8_t bars[4] = { W, Y, R, K }; return bars[(u - 22) / 51]; }
    if (in_rect(u, v, 22, 84, 198, 26)) { if (frame_hit(u, v, 22, 84, 198, 26)) return K; return mix(u, v, K, W, ((u - 22) / 22) * 8); }
    static const uint8_t pitch[6] = { 2, 3, 4, 5, 6, 8 };
    for (int i = 0; i < 6; i++) {
      int x0 = 22 + i * 34;
      if (in_rect(u, v, x0, 116, 34, 56)) return (frame_hit(u, v, x0, 116, 34, 56) || ((u - x0) % pitch[i]) == 0) ? K : W;
      if (in_rect(u, v, x0, 178, 34, 56)) return (frame_hit(u, v, x0, 178, 34, 56) || ((v - 178) % pitch[i]) == 0) ? K : W;
    }
    return W;
  }
  // right block
  if (in_rect(u, v, 488, 18, 214, 220)) {
    if (frame_hit(u, v, 488, 18, 214, 220)) return K;
    static const uint8_t rp[4][2] = { {K, R}, {W, R}, {W, Y}, {Y, R} };
    for (int r = 0; r < 4; r++) if (in_rect(u, v, 492, 22 + r * 24, 198, 20)) return frame_hit(u, v, 492, 22 + r * 24, 198, 20) ? (uint8_t)K : mix(u, v, rp[r][0], rp[r][1], ((u - 492) / 22) * 8);
    static const uint8_t cc[4] = { K, K, R, Y }, cell[4] = { 1, 2, 1, 1 };
    for (int i = 0; i < 4; i++) {
      int x0 = 492 + i * 50;
      if (in_rect(u, v, x0, 124, 48, 56)) { if (frame_hit(u, v, x0, 124, 48, 56)) return K; return ((((u - x0) / cell[i]) + ((v - 124) / cell[i])) & 1) ? (uint8_t)W : cc[i]; }
      if (in_rect(u, v, x0, 186, 48, 48)) return frame_hit(u, v, x0, 186, 48, 48) ? (uint8_t)K : (uint8_t)i;
    }
    return W;
  }
  // centre: circles
  { int x = 10 * u + 5 - 3600, y = 10 * v + 5 - 1280;
    if (in_ring(u, v, 360, 128, 52)) return K;
    if (x * x + y * y < 520 * 520) { int dx = x < 0, dy = y < 0; return dx ? (dy ? W : R) : (dy ? Y : K); }
    if (in_ring(u, v, 360, 128, 118)) return K; }
  { int y0 = (u * 510 + 719) / 1438; if (v == y0 || v == 255 - y0) return K; }
  if (((u - 360) % 32) == 0 || v == 255 || (v >= 0 && ((v - 128) % 32) == 0)) return K;
  return W;
}

// ---- TAG HARDWARE screen
static char hw_label[DEMO_HW_ITEMS][16], hw_value[DEMO_HW_ITEMS][47];
void demo_hw_clear(void) { memset(hw_label, 0, sizeof hw_label); memset(hw_value, 0, sizeof hw_value); }
void demo_hw_set(uint8_t i, const char *label, const char *value)
{
  if (i >= DEMO_HW_ITEMS) return;
  strncpy(hw_label[i], label, 15); hw_label[i][15] = 0;
  strncpy(hw_value[i], value, 46); hw_value[i][46] = 0;
}
// a glyph pixel of string s at (x, y) relative to its top left corner, scale 1 or 2; 0 = outside / not set
static int glyph_hit(const char *s, int x, int y, int scale)
{
  if (x < 0 || y < 0 || y >= 14 * scale) return 0;
  int ci = x / (7 * scale), n = (int)strlen(s);
  if (ci >= n) return 0;
  unsigned char ch = (unsigned char)s[ci];
  if (ch < 32 || ch > 126) return 0;
  int gx = (x % (7 * scale)) / scale, gy = y / scale;
  return (demo_font7x14[ch - 32][gy] >> (7 - gx)) & 1;
}
static uint8_t px_hw(int u, int v)
{
  if (u < 3 || v < 3 || u >= 717 || v >= 253) return K;
  if (v < 36) {                                          // title bar
    if (glyph_hit("TAG HARDWARE", u - 16, v - 6, 2)) return W;
    if (glyph_hit("SES V300", u - 640, v - 11, 1)) return Y;
    return K;
  }
  if (v < 39) return Y;
  if (u == 359 || u == 360) return (v & 1) ? W : K;
  int col = u >= 360, x = u - (col ? 376 : 16), y0 = v - 46, row = y0 / 41, ly = y0 % 41;
  int idx = col ? 5 + row : row;
  if (y0 < 0 || x < 0 || row > 4 || idx >= DEMO_HW_ITEMS || !hw_label[idx][0]) return W;
  if (ly == 39) return ((u & 3) == 0) ? K : W;
  if (glyph_hit(hw_label[idx], x, ly, 1)) return R;
  if (glyph_hit(hw_value[idx], x, ly - 16, 1)) return K;
  return W;
}

static uint8_t px_newton(int u, int v)                       // Newton fractal of z^3 = 1, Q16 fixed point
{
  const int Q = 16;
  const int64_t st = (int64_t)(5.2 / 720 * (1 << Q) + 0.5);
  int32_t zr = (int32_t)((u - 360) * st), zi = (int32_t)((v - 128) * st);
  static const int32_t rootr[3] = { 1 << 16, -(1 << 15), -(1 << 15) }, rooti[3] = { 0, 56756, -56756 };   // 1, -1/2 +- i sqrt(3)/2
  for (int it = 0; it < 24; it++) {
    for (int k = 0; k < 3; k++) {
      int64_t dr = zr - rootr[k], di = zi - rooti[k];
      if ((dr * dr + di * di) >> Q < 40) {                  // converged: colour by root, dark rings by the iteration count
        static const uint8_t col[3] = { W, Y, R };
        return (it < 2) ? col[k] : ((it & 1) ? col[k] : (uint8_t)K);
      }
    }
    int64_t sr = ((int64_t)zr * zr - (int64_t)zi * zi) >> Q, si = ((int64_t)2 * zr * zi) >> Q;   // z^2
    int64_t den = (sr * sr + si * si) >> Q;                  // |z^2|^2
    if (den < 16) return K;
    int64_t ir = (sr << Q) / den, ii = (-si << Q) / den;   // 1 / z^2 = conj(z^2) / |z^2|^2
    zr = (int32_t)((2 * (int64_t)zr + ir) / 3); zi = (int32_t)((2 * (int64_t)zi + ii) / 3);
  }
  return K;
}
static uint8_t px_nova(int u, int v)                         // Nova fractal: z <- z - (z^3-1)/(3 z^2) + c, z0 = 1, c = pixel, Q20 fixed point
{
  const int Q = 20;
  const int64_t st = (int64_t)(3.4 / 720 * (1 << Q) + 0.5);
  const int64_t cr = (int64_t)(-0.2 * (1 << Q)) + (u - 360) * st, ci = (v - 128) * st;
  int64_t zr = 1 << Q, zi = 0;
  for (int it = 0; it < 60; it++) {
    int64_t sr = (zr * zr - zi * zi) >> Q, si = (2 * zr * zi) >> Q;      // z^2
    int64_t den = (sr * sr + si * si) >> Q;                              // |z^2|^2
    if (den < 64) return K;
    int64_t ir = (sr << Q) / den, ii = (-si << Q) / den;                 // 1 / z^2
    int64_t nr = (2 * zr + ir) / 3 + cr, ni = (2 * zi + ii) / 3 + ci;    // z - (z^3 - 1) / (3 z^2) = (2 z + 1 / z^2) / 3
    int64_t dr = nr - zr, di = ni - zi;
    zr = nr; zi = ni;
    if (zr > (50 << Q) || zr < -(50 << Q) || zi > (50 << Q) || zi < -(50 << Q)) return K;
    if (dr * dr + di * di < 10995) { int r = it % 3; return r == 0 ? Y : (r == 1 ? R : W); }   // |dz| < 1e-4 (Q40)
  }
  return K;
}
static uint8_t px_hilbert(int u, int v)                      // generalized Hilbert curve over the whole panel: 89 x 31 cells of 8 px, line 2 px wide
{
  int x = u - 4, y = v - 4;
  if (x < 0 || y < 0) return W;
  int cx = x / HIL_CELL, cy = y / HIL_CELL;
  if (cx >= HIL_GW || cy >= HIL_GH) return W;
  uint8_t t = hil_tab[cy * HIL_GW + cx];
  int dx = x % HIL_CELL, dy = y % HIL_CELL;
  int cxh = (dx == 3 || dx == 4), cyh = (dy == 3 || dy == 4);
  int on = (cxh && cyh) || ((t & 1) && dx >= 3 && cyh) || ((t & 4) && dx <= 4 && cyh) || ((t & 2) && dy >= 3 && cxh) || ((t & 8) && dy <= 4 && cxh);
  return on ? (((t >> 4) & 1) ? R : K) : W;
}
static uint8_t px_fern(int u, int v)                         // Barnsley fern, lying: stem on the left, tip on the right (1-bit bitmap from tools/make_demo_data.py)
{
  int x = u - FERN_X0, y = v - FERN_Y0;
  if (x < 0 || y < 0 || x >= FERN_W || y >= FERN_H) return W;
  int i = y * FERN_W + x;
  return ((fern_bm[i >> 3] >> (i & 7)) & 1) ? K : W;
}
static uint8_t px_carpet(int u, int v)
{
  int x = u + 24, y = v;                                   // 81 x 81 cells tiled: 27-pixel squares, three levels (27, 9, 3) and the pixel level
  for (int s = 27; s >= 1; s /= 3) if (((x / s) % 3 == 1) && ((y / s) % 3 == 1)) return (uint8_t)(((x / (s * 3) + y / (s * 3)) & 1) ? R : (s == 27 ? Y : K));
  return W;
}

uint8_t demo_px(uint8_t pattern, int u, int v)
{
  if (pattern == DEMO_PAT_HW) return px_hw(u, v);
  switch (pattern) {
    case 0: return px_calib(u, v);
    case 1: return px_chess(u, v);
    case 2: return px_julia(u, v);
    case 3: return px_sierp(u, v);
    case 4: return px_ramps(u, v);
    case 5: return px_truchet(u, v);
    case 6: return px_tv(u, v);
    case 7: return px_carpet(u, v);
    case 8: return px_newton(u, v);
    case 9: return px_nova(u, v);
    case 10: return px_hilbert(u, v);
    default: return px_fern(u, v);
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
