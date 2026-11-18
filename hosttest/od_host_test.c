// Host test of ble_od.c together with ble_up.c: an OpenDisplay client (messages 00 cmd ...) against the tag, flash modelled in RAM.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ble_od.h"
#include "ble_up.h"
#include "ble_diag.h"
#include "ble_flash.h"
#include "ble_slide.h"
#include "ble_panel.h"
#include "ble_batt.h"
volatile ble_diag_t g_diag;
volatile fl_result_t fl_res; volatile bool fl_done;
static uint8_t mem[0x80000]; static uint32_t lo, hi;
void fl_init(void) { memset(mem, 0xFF, sizeof(mem)); lo = FL_TEST_LO; hi = FL_TEST_END; } bool fl_busy(void) { return false; } void fl_poll(void) {}
void fl_set_range(uint32_t l, uint32_t h) { lo = l; hi = h; }
bool fl_start_info(void){return true;}
bool fl_start_erase(uint32_t a){ if (a<lo||a>=hi||(a&4095)) { printf("ERASE OUT OF RANGE %x\n",a); exit(9);} memset(mem+a,0xFF,4096); fl_res.result=0; fl_done=true; return true;}
bool fl_start_program(uint32_t a,const uint8_t*d){ if (a<lo||a>=hi) { printf("PROG OUT OF RANGE %x\n",a); exit(9);} for(int i=0;i<256;i++) if (mem[a+i]!=0xFF){ printf("PROG ON NON-BLANK %x\n",a+i); exit(9);} memcpy(mem+a,d,256); fl_res.result=0; fl_done=true; return true;}
bool fl_start_scan(uint32_t a){(void)a;return true;}
bool fl_read_page(uint32_t a,uint8_t*b,uint32_t n){ memcpy(b,mem+a,n); return true;}
uint32_t ll_free_us(void){return 100000;}
bool slide_flash_busy(void) { return false; } bool panel_flash_busy(void) { return false; }
static slide_cfg_t cfgv; const slide_cfg_t *slide_cfg(void) { return &cfgv; }
static int scan_left, shows, busy_left; static uint8_t shown_slot;
void slide_rescan(void) { scan_left = 3; }
bool slide_scanning(void) { return scan_left > 0; }
uint8_t slide_show(uint8_t s) { shown_slot = s; shows++; busy_left = 5; return SLR_OK; }
bool panel_busy(void) { return busy_left > 0; }
uint16_t batt_mv(void) { return 2930; }
static uint32_t now_us;
uint32_t RAIL_GetTime(void) { return now_us; }
bool att_notify(const uint8_t *d, uint8_t len) { (void)d; (void)len; printf("page-channel notify leaked: %02x\n", d[0]); exit(9); }
static uint8_t nq[256][20]; static uint8_t nl[256]; static int nh, nt;
bool att_notify_od(const uint8_t *d, uint8_t len) { memcpy(nq[nt % 256], d, len); nl[nt % 256] = len; nt++; return true; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void tick(int n) { for (int i = 0; i < n; i++) { now_us += 20000; up_poll(); od_poll(); if (scan_left) scan_left--; else if (busy_left) busy_left--; } }
static void wait_msg(int cmd, int *status) { for (int g = 0; g < 2000 && nh == nt; g++) tick(1); *status = -1; if (nh == nt) return; (void)cmd; *status = nq[nh % 256][0]; }

static int expect(uint8_t cmd, int want_status)
{
  int s; wait_msg(cmd, &s);
  if (s != want_status || nq[nh % 256][1] != cmd) { printf("  got status %d cmd %02x, want %02x %02x\n", s, nh == nt ? 0 : nq[nh % 256][1], want_status, cmd); nh = nt; return 0; }
  nh++; return 1;
}

int main(void)
{
  srand(7);
  fl_init(); up_reset(); od_reset(); memset(&cfgv, 0, sizeof(cfgv)); cfgv.flags = 2; cfgv.temp_c = 22;
  // configuration read: chunks add up to the length in the first message and the CRC is right
  uint8_t c40[2] = { 0, 0x40 }; od_rx(c40, 2);
  tick(50);
  CHECK(nt >= 8, "config chunks: %d", nt);
  static uint8_t cfg[512]; int got = 0, total = nq[0][4] | nq[0][5] << 8, idx = 0;
  for (int i = 0; i < nt; i++) { uint8_t *m = nq[i]; int hdr = i == 0 ? 6 : 4; CHECK(m[0] == 0 && m[1] == 0x40 && m[2] == idx, "config header %d", i); memcpy(cfg + got, m + hdr, nl[i] - hdr); got += nl[i] - hdr; idx++; }
  CHECK(got == total && total > 100, "config length %d / %d", got, total);
  nh = nt;
  uint8_t c44[2] = { 0, 0x44 }; od_rx(c44, 2); CHECK(expect(0x44, 0), "0x44"); 
  // refused things
  uint8_t bad[3] = { 0, 0x71, 1 }; od_rx(bad, 3); CHECK(expect(0x71, 0xFF), "data before start refused");
  uint8_t zip[4] = { 0, 0x70, 1, 2 }; od_rx(zip, 4); CHECK(expect(0x70, 0xFF), "size field (compressed) refused");
  uint8_t unk[2] = { 0, 0x66 }; od_rx(unk, 2); CHECK(expect(0x66, 0xFF), "unknown command refused");
  // a whole frame
  static uint8_t img[46080]; for (int i = 0; i < 46080; i++) img[i] = (uint8_t)(rand() >> 5);
  uint8_t c70[2] = { 0, 0x70 }; od_rx(c70, 2);
  CHECK(expect(0x70, 0), "start answered ok");
  uint32_t o = 0; int msgs = 0;
  while (o < 46080) {
    uint8_t m[20]; int n = 1 + rand() % 18; if (o + n > 46080) n = 46080 - o;
    m[0] = 0; m[1] = 0x71; memcpy(m + 2, img + o, n); od_rx(m, (uint8_t)(n + 2));
    int s; wait_msg(0x71, &s); if (s != 0 || nq[nh % 256][1] != 0x71) { CHECK(0, "data message %d at %u: status %d", msgs, o, s); break; } nh++;
    o += n; msgs++;
  }
  uint8_t c72[2] = { 0, 0x72 }; od_rx(c72, 2);
  CHECK(expect(0x72, 0), "end answered ok");
  CHECK(shows == 1 && shown_slot == OD_SLOT, "slot 10 shown: %d %d", shows, shown_slot);
  CHECK(expect(0x73, 0), "refresh done message");
  CHECK(memcmp(mem + FL_SLOT_BASE(OD_SLOT), img, 46080) == 0, "frame in slot 10");
  CHECK(memcmp(mem + FL_SLOT_BASE(OD_SLOT) + FL_SLOT_REC, "SLD1", 4) == 0, "record written");
  for (uint32_t a = 0; a < 0x80000; a++) if ((a < FL_SLOT_BASE(OD_SLOT) || a >= FL_SLOT_BASE(OD_SLOT) + FL_SLOT_SIZE) && mem[a] != 0xFF) { CHECK(0, "write outside slot 10 at %x", a); break; }
  CHECK(!od_busy(), "idle again");
  // too much data: refused, upload aborted
  od_rx(c70, 2); CHECK(expect(0x70, 0), "second start");
  uint8_t big[20] = { 0, 0x71 }; int s2 = 0;
  for (int i = 0; i < 3000 && s2 == 0; i++) { od_rx(big, 20); int s; wait_msg(0x71, &s); if (s != 0) s2 = s; else nh++; }
  CHECK(s2 == 0xFF, "overflow refused (%d)", s2);
  // setting off: the characteristic is dead
  tick(20); nh = nt; cfgv.flags = 0; od_rx(c44, 2); tick(5); CHECK(nh == nt, "setting off: no answer");
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails != 0;
}
