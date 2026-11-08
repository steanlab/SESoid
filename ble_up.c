// SES V300, BLE stage 3: slide upload protocol, see ble_up.h
#include <string.h>
#include "ble_up.h"
#include "ble_att.h"
#include "ble_flash.h"
#include "ble_diag.h"

#define PAGE   256u
#define CHUNK  16u
#define WIN    8u                 // pages buffered at once (2 KiB of RAM)

typedef enum { F_NONE, F_ERASING, F_READY, F_PROG, F_REC, F_DONE, F_ERR } fstate_t;

typedef struct {
  bool     active;
  uint8_t  mode;
  uint8_t  slot;                  // flash mode: slot 1..7 at slot * 64 KiB
  fstate_t fs;
  uint32_t base;                  // flash address of the slot
  uint32_t es, nsec;              // erase progress
  uint8_t  req;                   // bit n: the phone asked to commit the page in ring slot n
  uint32_t pend_page;             // page being programmed
  uint32_t total;                 // image length in bytes
  uint32_t next_page;             // next page to commit; pages below it are done
  uint32_t crc;                   // running CRC of the committed pages
  uint16_t mask[WIN];             // chunks received, per ring slot
  uint8_t  buf[WIN][PAGE];
} up_t;

static up_t u;
static bool begin_ack_due;          // flash mode: the BEGIN answer is sent after the erase
static bool end_wanted;
static bool vfy_active;            // slot verify (command 0x24) running
static uint32_t vfy_off, vfy_crc, vfy_total, vfy_rec_crc;
static uint8_t vfy_slot;
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

static bool up_busy(void)                         // an upload is in progress (not finished, not failed)
{
  return u.active && u.fs != F_DONE && u.fs != F_ERR && !(u.mode == 0 && u.next_page * PAGE >= u.total);
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
  begin_ack_due = end_wanted = vfy_active = false;
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

static void page_done(uint32_t page)           // the page is stored: checksum, free the ring slot, tell the phone
{
  uint32_t s = page % WIN, n = u.total - page * PAGE;
  uint8_t r[4];
  if (n > PAGE) n = PAGE;
  u.crc = up_crc32(u.crc, u.buf[s], n);
  u.mask[s] = 0;
  u.req &= (uint8_t)~(1u << s);
  u.next_page++;
  g_diag.up_bytes += n;
  r[0] = 0x92; wr16(r + 1, page); r[3] = 0;
  att_notify(r, 4);
  sync_diag();
}

static void page_fail(uint32_t page)
{
  uint8_t r[4] = { 0x92, (uint8_t)page, (uint8_t)(page >> 8), 2 };
  u.fs = F_ERR;
  att_notify(r, 4);
}

// Commit the pages the phone asked for, strictly in order. RAM sink: at once. Flash: one page at a time (program + verify).
static void commit_poll(void)
{
  while (u.active && u.fs != F_ERR && u.fs != F_PROG && u.fs != F_REC && u.fs != F_ERASING) {
    uint32_t page = u.next_page, s = page % WIN;
    if (!(u.req & (1u << s)) || page * PAGE >= u.total) return;
    uint16_t need = need_mask(page);
    if ((u.mask[s] & need) != need) {
      uint8_t r[5] = { 0x93, (uint8_t)page, (uint8_t)(page >> 8), (uint8_t)u.mask[s], (uint8_t)(u.mask[s] >> 8) };
      u.req &= (uint8_t)~(1u << s);
      att_notify(r, 5);
      DIAG_INC(up_incomplete);
      return;
    }
    if (u.mode == 0) { page_done(page); continue; }
    uint32_t n = u.total - page * PAGE;
    if (n < PAGE) memset(&u.buf[s][n], 0xFF, PAGE - n);
    if (fl_start_program(u.base + page * PAGE, u.buf[s])) { u.fs = F_PROG; u.pend_page = page; }
    return;                                         // otherwise retried from up_poll
  }
}

static void on_page(uint32_t page)
{
  uint8_t r[6];
  r[0] = 0x92; wr16(r + 1, page);
  if (page < u.next_page) { r[3] = 0; att_notify(r, 4); return; }                 // repeated question: already done
  if (page >= WIN + u.next_page || page * PAGE >= u.total) { r[3] = 1; wr16(r + 4, u.next_page); att_notify(r, 6); DIAG_INC(up_ooo); return; }
  u.req |= (uint8_t)(1u << (page % WIN));
  commit_poll();
}

static void end_answer(void)
{
  uint8_t e[10];
  e[0] = 0x94; e[1] = (u.active && u.next_page * PAGE >= u.total && (u.mode == 0 || u.fs == F_DONE)) ? 0 : 1;
  if (u.fs == F_ERR) e[1] = 2;
  wr32(e + 2, u.crc); wr32(e + 6, u.total);
  att_notify(e, 10);
}

static void send_slot_answer(uint8_t slot, uint8_t status, uint32_t total, uint32_t crc)
{
  uint8_t r[11] = { 0xA4, slot, status };
  wr32(r + 3, total); wr32(r + 7, crc);
  att_notify(r, 11);
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
  if (u.active && u.fs == F_ERR && (v[0] == 0x12 || v[0] == 0x13)) { end_answer(); return true; }   // tell the phone the upload failed
  uint8_t r[8];
  switch (v[0]) {
    case 0x10:   // BEGIN
      r[0] = 0x90; r[2] = len > 1 ? v[1] : 0;
      if (flash_q_op || flash_pending || fl_busy() || vfy_active) { r[1] = 1; att_notify(r, 3); return true; }
      if ((len != 6 && len != 7) || v[1] > 1 || rd32(v + 2) == 0 || rd32(v + 2) > 0x80000u ||
          (v[1] == 1 && (len != 7 || v[6] < 1 || v[6] > 7 || rd32(v + 2) > 0xF000u))) { r[1] = 1; att_notify(r, 3); return true; }
      memset(&u, 0, sizeof(u));
      u.active = true;
      u.mode = v[1];
      u.total = rd32(v + 2);
      g_diag.up_bytes = 0;
      begin_ack_due = false;
      end_wanted = false;
      if (u.mode == 1) {                             // erase the image sectors and the record sector first, then answer
        u.slot = v[6];
        u.base = (uint32_t)u.slot * FL_SLOT_SIZE;
        u.nsec = (u.total + FL_SECTOR - 1u) / FL_SECTOR + 1u;
        u.es = 0;
        u.fs = F_ERASING;
        fl_set_range(u.base, u.base + FL_SLOT_SIZE);
        begin_ack_due = true;
      } else {
        r[1] = 0;
        att_notify(r, 3);
      }
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
      if (u.active && u.mode == 1 && u.fs == F_REC) return true;                       // record being written: the answer follows by itself
      if (u.active && u.mode == 1 && u.fs == F_READY && u.next_page * PAGE >= u.total) { end_wanted = true; return true; }   // the record is written first
      end_answer();
      return true;
    case 0x14:   // ABORT
      memset(&u, 0, sizeof(u));
      begin_ack_due = end_wanted = false;
      fl_set_range(0, FL_TEST_END);
      sync_diag();
      return true;
    case 0x24:   // SLOT VERIFY: read the slot back from the flash and compare its CRC with the record
      if (up_busy() || flash_q_op || flash_pending || fl_busy() || vfy_active || len != 2 || v[1] < 1 || v[1] > 7) {
        send_slot_answer(len > 1 ? v[1] : 0, 9, 0, 0);
        return true;
      }
      vfy_active = true; vfy_slot = v[1]; vfy_off = 0xFFFFFFFFu; vfy_crc = 0;
      return true;
    case 0x20: case 0x21: case 0x22: case 0x23:
      if (flash_q_op || flash_pending || fl_busy() || up_busy() || vfy_active) { uint8_t e[3] = { 0xA3, FLR_PARAM, 0 }; att_notify(e, 3); return true; }   // one operation at a time
      fl_set_range(0, FL_TEST_END);
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

static void record_page(uint8_t *pg)
{
  memset(pg, 0xFF, PAGE);
  memcpy(pg, "SLD1", 4);
  wr32(pg + 4, u.total);
  wr32(pg + 8, u.crc);
  wr32(pg + 12, 0);                              // flags
}

static void slot_verify_poll(void)
{
  static uint8_t pg[PAGE];
  uint32_t base = (uint32_t)vfy_slot * FL_SLOT_SIZE;
  if (vfy_off == 0xFFFFFFFFu) {                  // first step: the record
    if (!fl_read_page(base + 0xF000u, pg, 16)) return;
    if (memcmp(pg, "SLD1", 4) != 0) { send_slot_answer(vfy_slot, 1, 0, 0); vfy_active = false; return; }
    vfy_total = rd32(pg + 4); vfy_rec_crc = rd32(pg + 8);
    if (vfy_total == 0 || vfy_total > 0xF000u) { send_slot_answer(vfy_slot, 1, 0, 0); vfy_active = false; return; }
    vfy_off = 0; vfy_crc = 0;
    return;
  }
  uint32_t n = vfy_total - vfy_off;
  if (n > PAGE) n = PAGE;
  if (!fl_read_page(base + vfy_off, pg, n)) return;
  vfy_crc = up_crc32(vfy_crc, pg, n);
  vfy_off += n;
  if (vfy_off >= vfy_total) {
    send_slot_answer(vfy_slot, vfy_crc == vfy_rec_crc ? 0 : 2, vfy_total, vfy_crc);
    vfy_active = false;
  }
}

void up_poll(void)
{
  if (flash_q_op && !flash_pending) {
    flash_try_start();
  }
  if (vfy_active) slot_verify_poll();
  fl_poll();

  if (u.active && u.mode == 1) {
    switch (u.fs) {
      case F_ERASING:
        if (!fl_busy()) {
          if (fl_done) {
            fl_done = false;
            if (fl_res.result != FLR_OK) { u.fs = F_ERR; uint8_t r[3] = { 0x90, 4, 1 }; att_notify(r, 3); begin_ack_due = false; break; }
            u.es++;
          }
          if (u.es >= u.nsec) {
            u.fs = F_READY;
            if (begin_ack_due) { uint8_t r[4] = { 0x90, 0, 1, u.slot }; att_notify(r, 4); begin_ack_due = false; }
          } else {
            uint32_t sec = (u.es + 1u == u.nsec) ? 0xF000u : u.es * FL_SECTOR;     // the last one is the record sector
            fl_start_erase(u.base + sec);
          }
        }
        break;
      case F_PROG:
        if (fl_done && !fl_busy()) {
          fl_done = false;
          g_diag.fl_pages_ok += (fl_res.result == FLR_OK);
          g_diag.fl_pages_bad += (fl_res.result != FLR_OK);
          if (fl_res.result == FLR_OK) { u.fs = F_READY; page_done(u.pend_page); commit_poll(); }
          else page_fail(u.pend_page);
        }
        break;
      case F_REC:
        if (fl_done && !fl_busy()) {
          fl_done = false;
          if (fl_res.result == FLR_OK) { u.fs = F_DONE; end_answer(); } else { u.fs = F_ERR; end_answer(); }
        }
        break;
      case F_READY:
        commit_poll();                                              // a start that had to wait for a radio-free moment
        if (end_wanted && u.fs == F_READY && u.next_page * PAGE >= u.total) {
          uint8_t pg[PAGE];
          record_page(pg);
          if (fl_start_program(u.base + 0xF000u, pg)) { u.fs = F_REC; end_wanted = false; }
        }
        break;
      default:
        break;
    }
  }

  if (flash_pending && fl_done && !fl_busy()) {
    fl_done = false;
    flash_pending = false;
    g_diag.fl_pages_ok += (fl_res.result == FLR_OK);
    g_diag.fl_pages_bad += (fl_res.result != FLR_OK);
    send_flash_result();
  }
}
