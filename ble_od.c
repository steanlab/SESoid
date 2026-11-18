// SES V300: OpenDisplay compatibility, see ble_od.h
#include <string.h>
#include "ble_od.h"
#include "ble_att.h"
#include "ble_up.h"
#include "ble_slide.h"
#include "ble_panel.h"
#include "ble_batt.h"
#include "ble_sys.h"
#include "rail.h"

#define FRAME      46080u
#define CHUNKS     (FRAME / 16u)
#define PAGES      (FRAME / 256u)
#define WIN_MAX    6u                       // pages that may wait for their flash write (the upload code buffers 8)
#define QN         16u

typedef enum { S_IDLE, S_BEGIN, S_DATA, S_FINISH, S_END, S_SCAN, S_SHOW, S_REFRESH } st_t;

static st_t st;
static bool active;
static uint32_t off;                        // frame bytes taken
static uint8_t stage[16];
static uint8_t sn;
static uint32_t pages_sent, pages_done;
static bool ack_wait;                       // the 0x71 that completed a page is answered when the flash has caught up
static bool seen_busy;
static uint8_t q[QN][20];
static uint8_t ql[QN];
static uint8_t qh, qt;
static uint32_t q_last;
static uint8_t msd_loop;

bool od_enabled(void) { return (slide_cfg()->flags & 2u) != 0u; }
bool od_busy(void) { return active; }

void od_reset(void)
{
  st = S_IDLE; active = false; off = 0; sn = 0; pages_sent = pages_done = 0; ack_wait = false; seen_busy = false; qh = qt = 0;
}

static void put(uint8_t status, uint8_t cmd, const uint8_t *data, uint8_t n)
{
  uint8_t next = (uint8_t)((qt + 1u) % QN);
  if (next == qh || n > 18u) return;           // queue full: the answer is lost, the client times out
  q[qt][0] = status; q[qt][1] = cmd;
  if (n) memcpy(&q[qt][2], data, n);
  ql[qt] = (uint8_t)(2u + n);
  qt = next;
}
static void ok(uint8_t cmd) { put(0x00, cmd, NULL, 0); }
static void nack(uint8_t cmd) { put(0xFF, cmd, NULL, 0); }

static void abort_up(uint8_t cmd)
{
  nack(cmd);
  uint8_t a = 0x14;
  up_write(&a, 1);
  od_reset();
  // the answers already queued stay: od_reset() clears the queue, so put the refusal back
  nack(cmd);
}

static uint16_t crc16(const uint8_t *p, int n)
{
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*p++) << 8;
    for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

// configuration of the tag as the protocol describes it: system, manufacturer, power and one display packet
static uint8_t cfg[160];
static int cfg_build(void)
{
  int n = 0;
  memset(cfg, 0, sizeof(cfg));
  cfg[n++] = 0; cfg[n++] = 0;                    // length, patched below
  cfg[n++] = 1;                                  // protocol major version
  cfg[n++] = 0; cfg[n++] = 1;                    // packet 1: system_config (22 bytes)
  cfg[n++] = 5; cfg[n++] = 0;                    // ic_type (nearest entry of the list)
  cfg[n++] = 0x01;                               // communication_modes: BLE
  cfg[n++] = 0x00;                               // device_flags
  cfg[n++] = 0xFF;                               // pwr_pin
  n += 15;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  cfg[n++] = 1; cfg[n++] = 2;                    // packet 2: manufacturer_data (22 bytes): diy, custom board
  n += 22;
  cfg[n++] = 2; cfg[n++] = 4;                    // packet 4: power_option (30 bytes): battery, lithium primary, no sense pins
  cfg[n++] = 1;
  n += 3 + 2 + 1 + 1;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 1;
  cfg[n++] = 4;
  n += 2 + 4 + 2;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 1 + 2 + 1 + 4;
  cfg[n++] = 3; cfg[n++] = 32;                   // packet 32: display (46 bytes): 720 x 256, four colours, direct write only
  cfg[n++] = 0;                                  // instance
  cfg[n++] = 1;                                  // e-paper
  n += 2;                                        // panel_ic_type unknown
  cfg[n++] = (uint8_t)(720 & 255); cfg[n++] = (uint8_t)(720 >> 8);       // pixel_width
  cfg[n++] = (uint8_t)(256 & 255); cfg[n++] = (uint8_t)(256 >> 8);       // pixel_height
  n += 2 + 2 + 2;
  cfg[n++] = 0;                                  // rotation
  cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  cfg[n++] = 0;                                  // partial_update_support
  cfg[n++] = 3;                                  // color_scheme: bwry
  cfg[n++] = 0x08;                               // transmission_modes: direct_write only
  cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF; cfg[n++] = 0xFF;
  n += 2 + 13;
  int total = n + 2;
  cfg[0] = cfg[1] = 0;                           // the CRC is taken with the length bytes zeroed
  uint16_t c = crc16(cfg, n);
  cfg[0] = (uint8_t)total; cfg[1] = (uint8_t)(total >> 8);
  cfg[n++] = (uint8_t)c; cfg[n++] = (uint8_t)(c >> 8);
  return n;
}

static void config_read(void)
{
  int n = cfg_build(), off2 = 0, idx = 0;
  const int ch = 14;                              // MTU 23: 20 bytes of notification, 4 of header (6 in the first message)
  while (off2 < n) {
    uint8_t b[20];
    int k = 0;
    b[k++] = 0x00; b[k++] = 0x40; b[k++] = (uint8_t)idx; b[k++] = (uint8_t)(idx >> 8);
    if (idx == 0) { b[k++] = (uint8_t)n; b[k++] = (uint8_t)(n >> 8); }
    int m = n - off2 < ch ? n - off2 : ch;
    memcpy(b + k, cfg + off2, (size_t)m);
    k += m;
    uint8_t next = (uint8_t)((qt + 1u) % QN);
    if (next == qh) return;
    memcpy(q[qt], b, (size_t)k); ql[qt] = (uint8_t)k; qt = next;
    off2 += m; idx++;
  }
}

// layout of the reference firmware: 0..1 company id 0x2446, 2..12 dynamic data (0xFF = none), 13 temperature (T + 40) * 2, 14 battery in 10 mV (low 8 bits),
// 15 status: bit 0 battery bit 8, bits 4..7 loop counter
void od_msd(uint8_t b[16])
{
  uint32_t v = ((uint32_t)batt_mv() + 5u) / 10u;
  int t2 = ((int)slide_cfg()->temp_c + 40) * 2;
  if (v > 0x1FFu) v = 0x1FFu;
  if (t2 < 0) t2 = 0;
  if (t2 > 255) t2 = 255;
  b[0] = 0x46; b[1] = 0x24;
  for (int i = 2; i <= 12; i++) b[i] = 0xFF;
  b[13] = (uint8_t)t2;
  b[14] = (uint8_t)v;
  b[15] = (uint8_t)(((v >> 8) & 1u) | ((unsigned)(msd_loop & 15u) << 4));
  msd_loop++;
}

void od_rx(const uint8_t *d, uint8_t len)
{
  if (len < 2 || d[0] != 0x00 || !od_enabled()) return;
  uint8_t cmd = d[1];
  switch (cmd) {
    case 0x40: config_read(); break;
    case 0x43: { uint8_t b[8] = { 1, 0, 4, 'v', '3', '0', '0', FW_VERSION_MAJOR }; put(0x00, 0x43, b, 8); break; }
    case 0x44: { uint8_t b[16]; od_msd(b); put(0x00, 0x44, b, 16); break; }
    case 0x50: { uint8_t s = 0x03; put(0x00, 0x50, &s, 1); break; }      // no encryption configured
    case 0x70: {
      if (len > 2 || active || !up_idle()) { nack(0x70); break; }       // a size field would mean compressed data: not offered
      static const uint8_t begin[7] = { 0x10, 1, 0x00, 0xB4, 0x00, 0x00, OD_SLOT };      // flash upload of 46 080 bytes into slot 10
      od_reset();
      active = true; st = S_BEGIN;
      up_write(begin, 7);
      break;
    }
    case 0x71: {
      if (st != S_DATA || ack_wait) { nack(0x71); break; }
      uint8_t n = (uint8_t)(len - 2u);
      if (off + n > FRAME) { abort_up(0x71); break; }
      for (uint8_t i = 0; i < n; i++) {
        stage[sn++] = d[2 + i]; off++;
        if (sn == 16) {
          uint32_t idx = (off - 16u) / 16u;
          uint8_t m[19] = { 0x11, (uint8_t)idx, (uint8_t)(idx >> 8) };
          memcpy(m + 3, stage, 16);
          up_write(m, 19);
          sn = 0;
          if ((idx % 16u) == 15u) {
            uint32_t pg = idx / 16u;
            uint8_t pm[3] = { 0x12, (uint8_t)pg, (uint8_t)(pg >> 8) };
            up_write(pm, 3);
            pages_sent++;
            if (pages_sent - pages_done >= WIN_MAX) ack_wait = true;
          }
        }
      }
      if (!ack_wait) ok(0x71);
      break;
    }
    case 0x72:
      if (st != S_DATA || off != FRAME || sn != 0) { abort_up(0x72); break; }
      st = S_FINISH;
      break;
    default: nack(cmd); break;
  }
}

bool od_up_sink(const uint8_t *d, uint8_t len)
{
  if (!active || len == 0) return false;
  switch (d[0]) {
    case 0x90:
      if (st == S_BEGIN && len >= 2 && d[1] == 0) { st = S_DATA; ok(0x70); }
      else abort_up(0x70);
      break;
    case 0x92:
      if (len >= 4 && d[3] == 0) pages_done++; else abort_up(0x71);
      break;
    case 0x93: abort_up(0x71); break;
    case 0x94:
      if (st == S_END && len >= 2 && d[1] == 0) { slide_rescan(); st = S_SCAN; }
      else abort_up(0x72);
      break;
    default: break;
  }
  return true;
}

void od_poll(void)
{
  if (active) {
    if (ack_wait && pages_sent - pages_done < WIN_MAX) { ack_wait = false; ok(0x71); }
    if (st == S_FINISH && pages_done >= PAGES) { uint8_t e = 0x13; st = S_END; up_write(&e, 1); }
    if (st == S_SCAN && !slide_scanning()) {
      uint8_t r = slide_show(OD_SLOT);
      if (r == SLR_OK) { ok(0x72); st = S_REFRESH; seen_busy = false; }
      else if (r != SLR_PANEL_BUSY && r != SLR_FLASH_BUSY) { nack(0x72); od_reset(); }
    }
    if (st == S_REFRESH) {
      if (panel_busy()) seen_busy = true;
      else if (seen_busy) { uint8_t b = 0; put(0x00, 0x73, &b, 0); active = false; st = S_IDLE; }
    }
  }
  if (qh != qt) {
    uint32_t now = RAIL_GetTime();
    if ((uint32_t)(now - q_last) >= 15000u && att_notify_od(q[qh], ql[qh])) { q_last = now; qh = (uint8_t)((qh + 1u) % QN); }
  }
}
