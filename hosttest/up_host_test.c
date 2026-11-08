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
static uint8_t mem[0x80000]; static uint32_t lo, hi; static int deny_pct; static unsigned rnd(void);
void fl_init(void) { memset(mem, 0xFF, sizeof(mem)); lo = 0; hi = 0x10000; } bool fl_busy(void) { return false; } void fl_poll(void) {}
void fl_set_range(uint32_t l, uint32_t h) { lo = l; hi = h; }
bool fl_start_info(void){return true;}
bool fl_start_erase(uint32_t a){ if ((int)(rnd()%100) < deny_pct) return false; if (a<lo||a>=hi||(a&4095)) { printf("ERASE OUT OF RANGE %x\n",a); exit(9);} memset(mem+a,0xFF,4096); fl_res.result=0; fl_done=true; return true;}
bool fl_start_program(uint32_t a,const uint8_t*d){ if ((int)(rnd()%100) < deny_pct) return false; if (a<lo||a>=hi) { printf("PROG OUT OF RANGE %x\n",a); exit(9);} for(int i=0;i<256;i++){ if (mem[a+i]!=0xFF){ printf("PROG ON NON-BLANK %x\n",a+i); exit(9);} } memcpy(mem+a,d,256); fl_res.result=0; fl_done=true; return true;}
bool fl_start_scan(uint32_t a){(void)a;return true;}
bool fl_read_page(uint32_t a,uint8_t*b,uint32_t n){ if ((int)(rnd()%100) < deny_pct) return false; memcpy(b,mem+a,n); return true;}
uint32_t ll_free_us(void){return 100000;}
static uint8_t nq[64][20]; static uint8_t nl[64]; static int nh, nt;
bool att_notify(const uint8_t *d, uint8_t len) { memcpy(nq[nt % 64], d, len); nl[nt % 64] = len; nt++; return true; }
static unsigned rnd(void) { return (unsigned)rand(); }

static int run(uint32_t total, int p_loss_chunk, int seed, int mode)
{
  srand(seed);
  static uint8_t img[0x80000]; for (uint32_t i = 0; i < total; i++) img[i] = (uint8_t)(rnd() >> 8);
  uint32_t want = up_crc32(0, img, total);
  up_reset(); nh = nt = 0;
  deny_pct = mode ? 30 : 0;
  uint8_t b[7] = { 0x10, (uint8_t)mode, total, total >> 8, total >> 16, total >> 24, 3 };
  up_write(b, mode ? 7 : 6);
  for (int i = 0; i < 100000 && nt == 0; i++) up_poll();
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
    up_poll();
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
        if (r[1] != 0 || crc != want) return 3;
        if (mode) {
          if (memcmp(mem + 3 * 0x10000, img, total) != 0) return 5;
          nh = nt; uint8_t vv[2] = { 0x24, 3 }; up_write(vv, 2);
          for (int i = 0; i < 100000 && nh == nt; i++) up_poll();
          if (nh == nt || nq[nh % 64][0] != 0xA4 || nq[nh % 64][2] != 0) return 6;
          mem[3 * 0x10000 + total / 2] ^= 1; nh = nt; up_write(vv, 2);        // corrupt one byte: the verify must notice
          for (int i = 0; i < 100000 && nh == nt; i++) up_poll();
          if (nh == nt || nq[nh % 64][2] != 2) return 7;
        }
        return 0;
      }
    }
    if (base >= pages) { if (!ends++ || guard % 50 == 0) { uint8_t e = 0x13; up_write(&e, 1); } continue; }
    if (!progressed && sent >= base + 4 ) { uint8_t pg[3] = { 0x12, base, base >> 8 }; up_write(pg, 3); }   // timeout re-ask
  }
  return 2;
}
int main(void)
{
  int bad = 0; uint32_t sizes[] = { 1, 16, 17, 255, 256, 257, 4096, 4097, 46080, 46081, 61440 };
  for (unsigned s = 0; s < sizeof(sizes) / sizeof(*sizes); s++)
    for (int loss = 0; loss <= 30; loss += 10)
      for (int mode = 0; mode <= 1; mode++) for (int seed = 1; seed <= 3; seed++) { int r = run(sizes[s], loss, seed, mode); if (r) { printf("FAIL size %u loss %d seed %d mode %d code %d\n", sizes[s], loss, seed, mode, r); bad++; } }
  printf(bad ? "%d FAILED\n" : "ALL OK\n", bad);
  return bad != 0;
}
