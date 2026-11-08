// SES V300, BLE stage 3: slide upload protocol, see ble_up.h
#include <string.h>
#include "ble_up.h"
#include "ble_att.h"
#include "ble_flash.h"
#include "ble_diag.h"

#define PAGE   256u
#define CHUNK  16u
#define WIN    8u                 // pages buffered at once (2 KiB of RAM)

typedef struct {
  bool     active;
  uint8_t  mode;
  uint32_t total;                 // image length in bytes
  uint32_t next_page;             // next page to commit; pages below it are done
  uint32_t crc;                   // running CRC of the committed pages
  uint16_t mask[WIN];             // chunks received, per ring slot
  uint8_t  buf[WIN][PAGE];
} up_t;

static up_t u;
static bool flash_pending;        // flash test: send the answer when the operation is done
static uint8_t flash_q_op;        // test command waiting for a radio-free moment: 0 none
static uint32_t flash_q_arg;

uint32_t up_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
  static const uint32_t t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ t[crc & 15u];
    crc = (crc >> 4) ^ t[crc & 15u];
  }
  return ~crc;
}

static void sync_diag(void)
{
  g_diag.up_state = u.active ? 1u : 0u;
  g_diag.up_mode = u.mode;
  g_diag.up_total = u.total;
  g_diag.up_pages = u.next_page;
  g_diag.up_crc = u.crc;
}

void up_reset(void)
{
  memset(&u, 0, sizeof(u));
  flash_pending = false;
  flash_q_op = 0;
  fl_init();
  sync_diag();
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

static uint16_t need_mask(uint32_t page)
{
  uint32_t start = page * PAGE;
  if (start >= u.total) return 0;
  uint32_t n = u.total - start;
  if (n >= PAGE) return 0xFFFFu;
  uint32_t chunks = (n + CHUNK - 1u) / CHUNK;
  return (uint16_t)((1u << chunks) - 1u);
}

static bool store_page(uint32_t page, const uint8_t *data)
{
  if (u.mode == 0) return true;                // RAM sink
  (void)page; (void)data;
  return false;                                // flash mode comes in stage 3b
}

static void on_page(uint32_t page)
{
  uint8_t r[6];
  r[0] = 0x92; wr16(r + 1, page);
  if (page < u.next_page) { r[3] = 0; att_notify(r, 4); return; }                 // repeated question: already done
  if (page != u.next_page) { r[3] = 1; wr16(r + 4, u.next_page); att_notify(r, 6); DIAG_INC(up_ooo); return; }
  if (page >= WIN + u.next_page || page * PAGE >= u.total) { r[3] = 1; wr16(r + 4, u.next_page); att_notify(r, 6); DIAG_INC(up_ooo); return; }
  uint32_t s = page % WIN;
  uint16_t need = need_mask(page);
  if ((u.mask[s] & need) != need) {
    r[0] = 0x93; wr16(r + 3, u.mask[s]);
    att_notify(r, 5);
    DIAG_INC(up_incomplete);
    return;
  }
  uint32_t n = u.total - page * PAGE;
  if (n > PAGE) n = PAGE;
  if (!store_page(page, u.buf[s])) { r[3] = 2; att_notify(r, 4); return; }
  u.crc = up_crc32(u.crc, u.buf[s], n);
  u.mask[s] = 0;
  u.next_page++;
  g_diag.up_bytes += n;
  r[3] = 0;
  att_notify(r, 4);
  sync_diag();
}

static void send_flash_result(void)
{
  uint8_t r[16];
  fl_result_t f;
  memcpy(&f, (const void *)&fl_res, sizeof(f));
  if (f.op == 1) {
    r[0] = 0xA0; r[1] = (uint8_t)(f.jedec >> 16); r[2] = (uint8_t)(f.jedec >> 8); r[3] = (uint8_t)f.jedec;
    r[4] = f.sr1_last; r[5] = f.sr2; r[6] = f.sr3;
    att_notify(r, 7);
  } else if (f.op == 2 || f.op == 3) {
    r[0] = (f.op == 2) ? 0xA1 : 0xA2; r[1] = f.result; wr32(r + 2, f.busy_us);
    r[6] = f.sr1_first; r[7] = f.sr1_last; r[8] = f.sr1_wel; wr16(r + 9, f.nonff > 0xFFFFu ? 0xFFFFu : f.nonff);
    att_notify(r, 11);
  } else {
    r[0] = 0xA3; r[1] = f.result; wr32(r + 2, f.nonff);
    att_notify(r, 6);
  }
}

static void flash_try_start(void)
{
  bool ok = false;
  uint8_t pat[PAGE];
  switch (flash_q_op) {
    case 0x20: ok = fl_start_info(); break;
    case 0x21: ok = fl_start_erase(flash_q_arg * FL_SECTOR); break;
    case 0x22:
      for (uint32_t i = 0; i < PAGE; i++) pat[i] = (uint8_t)(i ^ (flash_q_arg * 0x35u) ^ 0xA5u);
      ok = fl_start_program(flash_q_arg * FL_SECTOR, pat);
      break;
    case 0x23: ok = fl_start_scan(flash_q_arg * FL_SECTOR); break;
    default: break;
  }
  if (ok) { flash_q_op = 0; flash_pending = true; }
}

bool up_write(const uint8_t *v, uint8_t len)
{
  if (len == 0 || v[0] < 0x10 || v[0] > 0x2F) return false;
  uint8_t r[8];
  switch (v[0]) {
    case 0x10:   // BEGIN
      r[0] = 0x90; r[2] = len > 1 ? v[1] : 0;
      if (len != 6 || v[1] > 1 || rd32(v + 2) == 0 || rd32(v + 2) > 0x80000u) { r[1] = 1; att_notify(r, 3); return true; }
      if (v[1] == 1) { r[1] = 3; att_notify(r, 3); return true; }
      memset(&u, 0, sizeof(u));
      u.active = true;
      u.mode = v[1];
      u.total = rd32(v + 2);
      g_diag.up_bytes = 0;
      r[1] = 0;
      att_notify(r, 3);
      sync_diag();
      return true;
    case 0x11:   // CHUNK
      if (!u.active || len != 19) return true;
      {
        uint32_t idx = rd16(v + 1), page = idx / 16u, k = idx % 16u;
        DIAG_INC(up_chunks);
        if (page < u.next_page) { DIAG_INC(up_dup); return true; }
        if (page >= u.next_page + WIN) { DIAG_INC(up_ahead); return true; }
        memcpy(&u.buf[page % WIN][k * CHUNK], v + 3, CHUNK);
        u.mask[page % WIN] |= (uint16_t)(1u << k);
      }
      return true;
    case 0x12:   // PAGE
      if (u.active && len == 3) on_page(rd16(v + 1));
      return true;
    case 0x13:   // END
      r[0] = 0x94;
      r[1] = (u.active && u.next_page * PAGE >= u.total) ? 0 : 1;
      {
        uint8_t e[10];
        e[0] = 0x94; e[1] = r[1]; wr32(e + 2, u.crc); wr32(e + 6, u.total);
        att_notify(e, 10);
      }
      return true;
    case 0x14:   // ABORT
      memset(&u, 0, sizeof(u));
      sync_diag();
      return true;
    case 0x20: case 0x21: case 0x22: case 0x23:
      if (flash_q_op || flash_pending || fl_busy()) return true;      // one test at a time
      flash_q_op = v[0];
      flash_q_arg = (v[0] == 0x23 && len >= 3) ? rd16(v + 1) : (len >= 2 ? v[1] : 0u);
      if ((v[0] == 0x23 && flash_q_arg > 127u) || (v[0] != 0x23 && v[0] != 0x20 && flash_q_arg > 15u)) {
        flash_q_op = 0;
        uint8_t e[3] = { 0xA3, FLR_PARAM, 0 };
        att_notify(e, 3);
      }
      return true;
    default:
      return true;
  }
}

void up_poll(void)
{
  if (flash_q_op && !flash_pending) {
    flash_try_start();
    if (!flash_q_op && !flash_pending) { }
  }
  fl_poll();
  if (flash_pending && fl_done && !fl_busy()) {
    fl_done = false;
    flash_pending = false;
    g_diag.fl_pages_ok += (fl_res.result == FLR_OK);
    g_diag.fl_pages_bad += (fl_res.result != FLR_OK);
    send_flash_result();
  }
}
