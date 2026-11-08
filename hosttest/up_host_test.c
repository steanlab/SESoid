// Host test of ble_up.c: a model of the phone-side sender (same algorithm as web/index.html) against the tag state machine,
// with random loss/reordering of chunk writes and of notifications. Build: gcc -O1 -I.. -Istub up_host_test.c ../ble_up.c -o t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ble_up.h"
#include "ble_diag.h"
#include "ble_flash.h"
volatile ble_diag_t g_diag;
volatile fl_result_t fl_res; volatile bool fl_done;
void fl_init(void) {} bool fl_busy(void) { return false; } void fl_poll(void) {}
bool fl_start_info(void){return true;} bool fl_start_erase(uint32_t a){(void)a;return true;}
bool fl_start_program(uint32_t a,const uint8_t*d){(void)a;(void)d;return true;} bool fl_start_scan(uint32_t a){(void)a;return true;}
uint32_t ll_free_us(void){return 100000;}
static uint8_t nq[64][20]; static uint8_t nl[64]; static int nh, nt;
bool att_notify(const uint8_t *d, uint8_t len) { memcpy(nq[nt % 64], d, len); nl[nt % 64] = len; nt++; return true; }
static unsigned rnd(void) { return (unsigned)rand(); }

static int run(uint32_t total, int p_loss_chunk, int seed)
{
  srand(seed);
  static uint8_t img[0x80000]; for (uint32_t i = 0; i < total; i++) img[i] = (uint8_t)(rnd() >> 8);
  uint32_t want = up_crc32(0, img, total);
  up_reset(); nh = nt = 0;
  uint8_t b[6] = { 0x10, 0, total, total >> 8, total >> 16, total >> 24 };
  up_write(b, 6);
  if (nq[0][0] != 0x90 || nq[0][1] != 0) return 1;
  nh = nt;
  uint32_t pages = (total + 255) / 256, base = 0, sent = 0;
  int guard = 0, ends = 0;
  while (guard++ < 200000) {
    while (sent < pages && sent < base + 4) {
      uint32_t n = total - sent * 256; if (n > 256) n = 256; uint32_t ch = (n + 15) / 16;
      for (uint32_t k = 0; k < ch; k++) {
        if ((int)(rnd() % 100) < p_loss_chunk) continue;       // chunk lost (should not happen over BLE, tested for robustness)
        uint8_t m[19]; uint32_t idx = sent * 16 + k; m[0] = 0x11; m[1] = idx; m[2] = idx >> 8;
        memset(m + 3, 0, 16); uint32_t cn = total - idx * 16; if (cn > 16) cn = 16; memcpy(m + 3, img + idx * 16, cn);
        up_write(m, 19);
      }
      uint8_t pg[3] = { 0x12, sent, sent >> 8 }; up_write(pg, 3); sent++;
    }
    bool progressed = false;
    while (nh < nt) {
      uint8_t *r = nq[nh % 64]; nh++;
      if (r[0] == 0x92 && r[3] == 0) { uint32_t p = r[1] | (r[2] << 8); if (p == base) { base++; progressed = true; } }
      else if (r[0] == 0x92 && r[3] == 1) { uint32_t e = r[4] | (r[5] << 8); base = e; sent = e; progressed = true; }
      else if (r[0] == 0x93) {
        uint32_t p = r[1] | (r[2] << 8); uint16_t mask = r[3] | (r[4] << 8);
        uint32_t n = total - p * 256; if (n > 256) n = 256; uint32_t ch = (n + 15) / 16;
        for (uint32_t k = 0; k < ch; k++) if (!(mask >> k & 1)) {
          uint8_t m[19]; uint32_t idx = p * 16 + k; m[0] = 0x11; m[1] = idx; m[2] = idx >> 8;
          memset(m + 3, 0, 16); uint32_t cn = total - idx * 16; if (cn > 16) cn = 16; memcpy(m + 3, img + idx * 16, cn);
          up_write(m, 19);
        }
        uint8_t pg[3] = { 0x12, p, p >> 8 }; up_write(pg, 3);
      } else if (r[0] == 0x94) {
        uint32_t crc = r[2] | (r[3] << 8) | (r[4] << 16) | ((uint32_t)r[5] << 24);
        return (r[1] == 0 && crc == want) ? 0 : 3;
      }
    }
    if (base >= pages) { if (!ends++ || guard % 50 == 0) { uint8_t e = 0x13; up_write(&e, 1); } continue; }
    if (!progressed && sent >= base + 4 ) { uint8_t pg[3] = { 0x12, base, base >> 8 }; up_write(pg, 3); }   // timeout re-ask
  }
  return 2;
}
int main(void)
{
  int bad = 0; uint32_t sizes[] = { 1, 16, 17, 255, 256, 257, 4096, 46080, 46081, 100000 };
  for (unsigned s = 0; s < sizeof(sizes) / sizeof(*sizes); s++)
    for (int loss = 0; loss <= 30; loss += 10)
      for (int seed = 1; seed <= 3; seed++) { int r = run(sizes[s], loss, seed); if (r) { printf("FAIL size %u loss %d seed %d code %d\n", sizes[s], loss, seed, r); bad++; } }
  printf(bad ? "%d FAILED\n" : "ALL OK\n", bad);
  return bad != 0;
}
