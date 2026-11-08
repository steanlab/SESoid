// SES V300, BLE stage 2: L2CAP/ATT server, see ble_att.h
#include <string.h>
#include "ble_att.h"
#include "ble_ll.h"
#include "ble_diag.h"
#include "ble_up.h"
#include "ble_ctl.h"

#define ATT_MTU 23u            // default MTU: we never ask for more and answer the master's MTU request with 23

#define H_GAP_SVC  0x0001
#define H_NAME_CHR 0x0002
#define H_NAME_VAL 0x0003
#define H_APP_SVC  0x0004
#define H_RX_CHR   0x0005
#define H_RX_VAL   0x0006
#define H_TX_CHR   0x0007
#define H_TX_VAL   0x0008
#define H_TX_CCCD  0x0009
#define H_LAST     0x0009

#define UUID_PRIMARY_SERVICE 0x2800
#define UUID_CHARACTERISTIC  0x2803
#define UUID_CCCD            0x2902
#define UUID_GAP_SERVICE     0x1800
#define UUID_DEVICE_NAME     0x2A00

// 53455300-0300-4f45-5000-534c49444500 with the 4th byte set to the id; stored little endian as ATT wants it
#define APP_UUID(id) { 0x00, 0x45, 0x44, 0x49, 0x4c, 0x53, 0x00, 0x50, 0x45, 0x4f, 0x00, 0x03, (id), 0x53, 0x45, 0x53 }
static const uint8_t UUID_APP_SVC[16] = APP_UUID(0x00);
static const uint8_t UUID_APP_RX[16]  = APP_UUID(0x01);
static const uint8_t UUID_APP_TX[16]  = APP_UUID(0x02);

static const char DEVICE_NAME[] = "V300-probe";

static volatile uint8_t s_cccd;        // bit 0 = notifications enabled
static uint32_t s_total, s_writes;

void att_reset(void)
{
  s_cccd = 0;
  g_diag.cccd = 0;
  up_reset();
  ctl_reset();
}

// --------------------------------------------------------------------------------------------------------------
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void att_send(const uint8_t *att, uint8_t len)
{
  uint8_t out[4 + ATT_MTU];
  out[0] = len; out[1] = 0; out[2] = 4; out[3] = 0;       // L2CAP header: length, channel 4 (ATT)
  memcpy(out + 4, att, len);
  ll_send(2, out, (uint8_t)(len + 4u));
  for (uint8_t i = 0; i < sizeof(g_diag.last_att_tx); i++) g_diag.last_att_tx[i] = (i < len) ? att[i] : 0;
}

static void att_error(uint8_t op, uint16_t handle, uint8_t code)
{
  uint8_t r[5] = { 0x01, op, 0, 0, code };
  wr16(&r[2], handle);
  att_send(r, 5);
  DIAG_INC(att_err_sent);
}

// --------------------------------------------------------------------------------------------------------------
// The attribute table
// --------------------------------------------------------------------------------------------------------------
// 16-bit type of an attribute, or 0 if its type is a 128-bit UUID (then *u128 is set)
static uint16_t attr_type(uint16_t h, const uint8_t **u128)
{
  *u128 = 0;
  switch (h) {
    case H_GAP_SVC: case H_APP_SVC:                  return UUID_PRIMARY_SERVICE;
    case H_NAME_CHR: case H_RX_CHR: case H_TX_CHR:   return UUID_CHARACTERISTIC;
    case H_NAME_VAL:                                 return UUID_DEVICE_NAME;
    case H_RX_VAL:                                   *u128 = UUID_APP_RX; return 0;
    case H_TX_VAL:                                   *u128 = UUID_APP_TX; return 0;
    case H_TX_CCCD:                                  return UUID_CCCD;
    default:                                         return 0xFFFF;
  }
}

// Value of an attribute into out (max 23 bytes). Returns its length, or -1 if it cannot be read.
static int attr_value(uint16_t h, uint8_t *out)
{
  switch (h) {
    case H_GAP_SVC:  wr16(out, UUID_GAP_SERVICE); return 2;
    case H_APP_SVC:  memcpy(out, UUID_APP_SVC, 16); return 16;
    case H_NAME_CHR: out[0] = 0x02; wr16(out + 1, H_NAME_VAL); wr16(out + 3, UUID_DEVICE_NAME); return 5;
    case H_RX_CHR:   out[0] = 0x0C; wr16(out + 1, H_RX_VAL); memcpy(out + 3, UUID_APP_RX, 16); return 19;
    case H_TX_CHR:   out[0] = 0x10; wr16(out + 1, H_TX_VAL); memcpy(out + 3, UUID_APP_TX, 16); return 19;
    case H_NAME_VAL: memcpy(out, DEVICE_NAME, sizeof(DEVICE_NAME) - 1); return (int)sizeof(DEVICE_NAME) - 1;
    case H_TX_CCCD:  wr16(out, s_cccd); return 2;
    default:         return -1;
  }
}

// service groups: first handle, last handle, 16-bit uuid (or 0 and 128-bit)
typedef struct { uint16_t first, last, u16; const uint8_t *u128; } svc_t;
static const svc_t SVCS[2] = {
  { H_GAP_SVC, H_NAME_VAL, UUID_GAP_SERVICE, 0 },
  { H_APP_SVC, H_LAST, 0, UUID_APP_SVC },
};

static const uint16_t CHARS[3] = { H_NAME_CHR, H_RX_CHR, H_TX_CHR };

// request uuid (2 or 16 bytes at p) equals the 16-bit uuid v?
static bool uuid_is16(const uint8_t *p, uint8_t n, uint16_t v) { return n == 2 && rd16(p) == v; }

// --------------------------------------------------------------------------------------------------------------
static void on_app_write(const uint8_t *v, uint8_t len)
{
  DIAG_INC(att_write_n);
  g_diag.att_write_bytes += len;
  s_total += len;
  s_writes++;
  if (len == 0) return;
  if (up_write(v, len)) return;                      // stage 3: upload protocol and flash tests
  if (ctl_write(v, len)) return;                     // stage 4: slideshow, settings, system commands
  uint8_t r[20];
  switch (v[0]) {
    case 0x01: {
      uint8_t n = (uint8_t)((len - 1u) > 19u ? 19u : (len - 1u));
      r[0] = 0x81;
      memcpy(r + 1, v + 1, n);
      att_notify(r, (uint8_t)(1u + n));
      break;
    }
    case 0x02:
      r[0] = 0x82;
      r[1] = (uint8_t)s_total; r[2] = (uint8_t)(s_total >> 8); r[3] = (uint8_t)(s_total >> 16); r[4] = (uint8_t)(s_total >> 24);
      r[5] = (uint8_t)s_writes; r[6] = (uint8_t)(s_writes >> 8);
      att_notify(r, 7);
      break;
    case 0x03:
      s_total = 0; s_writes = 0;
      r[0] = 0x83;
      att_notify(r, 1);
      break;
    default:
      break;
  }
}

bool att_notify(const uint8_t *data, uint8_t len)
{
  if (!(s_cccd & 1u) || len > 20u) { DIAG_INC(notify_dropped); return false; }
  uint8_t r[3 + 20];
  r[0] = 0x1B;
  wr16(r + 1, H_TX_VAL);
  memcpy(r + 3, data, len);
  att_send(r, (uint8_t)(3u + len));
  DIAG_INC(notify_sent);
  return true;
}

// --------------------------------------------------------------------------------------------------------------
void att_rx(const uint8_t *p, uint8_t len)
{
  if (len == 0) return;
  DIAG_INC(att_rx);
  g_diag.att_op = p[0];
  for (uint8_t i = 0; i < sizeof(g_diag.last_att); i++) g_diag.last_att[i] = (i < len) ? p[i] : 0;

  uint8_t op = p[0];
  uint8_t r[ATT_MTU];
  uint16_t start = 0, end = 0;
  if (op == 0x04 || op == 0x06 || op == 0x08 || op == 0x10) {      // requests with a handle range
    if (len < 5) { att_error(op, 0, 0x04); return; }
    start = rd16(p + 1);
    end = rd16(p + 3);
    if (start == 0 || start > end) { att_error(op, start, 0x01); return; }
  }

  switch (op) {
    case 0x02: {   // Exchange MTU: we stay at the default 23
      r[0] = 0x03; wr16(r + 1, ATT_MTU);
      att_send(r, 3);
      break;
    }

    case 0x10: {   // Read By Group Type: only primary services
      uint8_t tl = (uint8_t)(len - 5u);
      if (!(uuid_is16(p + 5, tl, UUID_PRIMARY_SERVICE))) { att_error(op, start, 0x10); break; }
      uint8_t n = 2, esz = 0;
      for (int i = 0; i < 2; i++) {
        const svc_t *s = &SVCS[i];
        if (s->first < start || s->first > end) continue;
        uint8_t this_sz = s->u128 ? 20u : 6u;
        if (esz == 0) esz = this_sz;
        if (this_sz != esz || n + esz > ATT_MTU) break;
        wr16(r + n, s->first); wr16(r + n + 2, s->last);
        if (s->u128) memcpy(r + n + 4, s->u128, 16); else wr16(r + n + 4, s->u16);
        n = (uint8_t)(n + esz);
      }
      if (esz == 0) { att_error(op, start, 0x0A); break; }
      r[0] = 0x11; r[1] = esz;
      att_send(r, n);
      break;
    }

    case 0x08: {   // Read By Type
      uint8_t tl = (uint8_t)(len - 5u);
      uint8_t n = 2, esz = 0;
      if (uuid_is16(p + 5, tl, UUID_CHARACTERISTIC)) {
        for (int i = 0; i < 3; i++) {
          uint16_t h = CHARS[i];
          if (h < start || h > end) continue;
          uint8_t v[23];
          int vl = attr_value(h, v);
          uint8_t this_sz = (uint8_t)(2 + vl);
          if (esz == 0) esz = this_sz;
          if (this_sz != esz || n + esz > ATT_MTU) break;
          wr16(r + n, h); memcpy(r + n + 2, v, (size_t)vl);
          n = (uint8_t)(n + esz);
        }
      } else if (uuid_is16(p + 5, tl, UUID_DEVICE_NAME)) {
        if (H_NAME_VAL >= start && H_NAME_VAL <= end) {
          int vl = attr_value(H_NAME_VAL, r + n + 2);
          wr16(r + n, H_NAME_VAL);
          esz = (uint8_t)(2 + vl);
          n = (uint8_t)(n + esz);
        }
      } else if (uuid_is16(p + 5, tl, UUID_CCCD)) {
        if (H_TX_CCCD >= start && H_TX_CCCD <= end) {
          wr16(r + n, H_TX_CCCD); wr16(r + n + 2, s_cccd);
          esz = 4;
          n = (uint8_t)(n + esz);
        }
      } else if (uuid_is16(p + 5, tl, UUID_PRIMARY_SERVICE)) {
        for (int i = 0; i < 2; i++) {
          const svc_t *s = &SVCS[i];
          if (s->first < start || s->first > end) continue;
          uint8_t this_sz = s->u128 ? 18u : 4u;
          if (esz == 0) esz = this_sz;
          if (this_sz != esz || n + esz > ATT_MTU) break;
          wr16(r + n, s->first);
          if (s->u128) memcpy(r + n + 2, s->u128, 16); else wr16(r + n + 2, s->u16);
          n = (uint8_t)(n + esz);
        }
      }
      if (esz == 0) { att_error(op, start, 0x0A); break; }    // includes (0x2802) and everything else we do not have
      r[0] = 0x09; r[1] = esz;
      att_send(r, n);
      break;
    }

    case 0x04: {   // Find Information
      uint8_t n = 2, fmt = 0;
      for (uint16_t h = start; h <= end && h <= H_LAST; h++) {
        const uint8_t *u128;
        uint16_t t = attr_type(h, &u128);
        uint8_t this_fmt = u128 ? 2u : 1u;
        if (fmt == 0) fmt = this_fmt;
        uint8_t esz = (fmt == 1) ? 4u : 18u;
        if (this_fmt != fmt || n + esz > ATT_MTU) break;
        wr16(r + n, h);
        if (u128) memcpy(r + n + 2, u128, 16); else wr16(r + n + 2, t);
        n = (uint8_t)(n + esz);
      }
      if (fmt == 0) { att_error(op, start, 0x0A); break; }
      r[0] = 0x05; r[1] = fmt;
      att_send(r, n);
      break;
    }

    case 0x06: {   // Find By Type Value: service by uuid
      if (len < 9 || rd16(p + 5) != UUID_PRIMARY_SERVICE) { att_error(op, start, 0x0A); break; }
      uint8_t vl = (uint8_t)(len - 7u);
      bool found = false;
      for (int i = 0; i < 2 && !found; i++) {
        const svc_t *s = &SVCS[i];
        if (s->first < start || s->first > end) continue;
        bool match = s->u128 ? (vl == 16 && memcmp(p + 7, s->u128, 16) == 0) : (vl == 2 && rd16(p + 7) == s->u16);
        if (match) {
          r[0] = 0x07; wr16(r + 1, s->first); wr16(r + 3, s->last);
          att_send(r, 5);
          found = true;
        }
      }
      if (!found) att_error(op, start, 0x0A);
      break;
    }

    case 0x0A:     // Read
    case 0x0C: {   // Read Blob
      if (len < (op == 0x0A ? 3 : 5)) { att_error(op, 0, 0x04); break; }
      uint16_t h = rd16(p + 1);
      uint16_t off = (op == 0x0C) ? rd16(p + 3) : 0;
      if (h == 0 || h > H_LAST) { att_error(op, h, 0x01); break; }
      if (h == H_RX_VAL || h == H_TX_VAL) { att_error(op, h, 0x02); break; }
      uint8_t v[23];
      int vl = attr_value(h, v);
      if (vl < 0) { att_error(op, h, 0x02); break; }
      if (off > (uint16_t)vl) { att_error(op, h, 0x07); break; }
      uint8_t n = (uint8_t)(vl - off);
      if (n > ATT_MTU - 1u) n = ATT_MTU - 1u;
      r[0] = (op == 0x0A) ? 0x0B : 0x0D;
      memcpy(r + 1, v + off, n);
      att_send(r, (uint8_t)(1u + n));
      break;
    }

    case 0x12:     // Write Request
    case 0x52: {   // Write Command
      bool req = (op == 0x12);
      if (len < 3) { if (req) att_error(op, 0, 0x04); break; }
      uint16_t h = rd16(p + 1);
      const uint8_t *v = p + 3;
      uint8_t vl = (uint8_t)(len - 3u);
      if (h == H_RX_VAL) {
        on_app_write(v, vl);
        if (req) { r[0] = 0x13; att_send(r, 1); }
      } else if (h == H_TX_CCCD) {
        if (vl != 2) { if (req) att_error(op, h, 0x0D); break; }
        s_cccd = (uint8_t)(v[0] & 1u);
        g_diag.cccd = s_cccd;
        if (req) { r[0] = 0x13; att_send(r, 1); }
      } else if (req) {
        att_error(op, h, (h == 0 || h > H_LAST) ? 0x01 : 0x03);
      }
      break;
    }

    case 0x1E:     // Handle Value Confirmation
      break;

    default:
      if (!(op & 0x40u)) att_error(op, 0, 0x06);      // not supported (commands carry bit 6 and get no answer)
      break;
  }
}
