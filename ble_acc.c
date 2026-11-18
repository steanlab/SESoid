// SES V300 (not badge154): identity and access window, see ble_acc.h
#include <string.h>
#include "ble_acc.h"

static uint8_t addr[6];
static char name[12];
static uint32_t until_s;
static uint32_t danger_s;                     // end of the dangerous-command window (uptime seconds)
static bool danger_set;
static bool opened;

static uint32_t fnv(const uint8_t *p, uint32_t n, uint32_t h)
{
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

void acc_init(const uint8_t eui[8], const uint8_t secret[8])
{
  danger_set = false;
  uint8_t m[16];
  memcpy(m, secret, 8); memcpy(m + 8, eui, 8);
  uint32_t h1 = fnv(m, 16, 2166136261u);
  uint32_t h2 = fnv(m, 16, 0x9E3779B9u);
  h1 ^= h1 >> 16; h1 *= 0x85EBCA6Bu; h1 ^= h1 >> 13; h1 *= 0xC2B2AE35u; h1 ^= h1 >> 16;
  h2 ^= h2 >> 15; h2 *= 0x2C1B3C6Du; h2 ^= h2 >> 12;
  addr[0] = (uint8_t)h1; addr[1] = (uint8_t)(h1 >> 8); addr[2] = (uint8_t)(h1 >> 16); addr[3] = (uint8_t)(h1 >> 24);
  addr[4] = (uint8_t)h2; addr[5] = (uint8_t)((h2 >> 8) | 0xC0u);          // static random address: the two top bits are 1
  static const char hex[] = "0123456789ABCDEF";
  memcpy(name, "V300-", 5);
  name[5] = hex[(h2 >> 20) & 15u]; name[6] = hex[(h2 >> 16) & 15u]; name[7] = hex[(h1 >> 4) & 15u]; name[8] = hex[h1 & 15u];
  name[9] = 0;
  opened = false;
}

const uint8_t *acc_addr(void) { return addr; }
const char *acc_name(void) { return name; }
uint8_t acc_name_len(void) { return 9; }

void acc_open(uint32_t now_s, uint32_t secs)
{
  uint32_t t = now_s + secs;
  if (!opened || (int32_t)(t - until_s) > 0) until_s = t;
  opened = true;
}

uint32_t acc_left_s(uint32_t now_s) { return opened && (int32_t)(until_s - now_s) > 0 ? until_s - now_s : 0u; }
bool acc_is_open(uint32_t now_s) { return acc_left_s(now_s) != 0u; }

void acc_danger_arm(uint32_t now_s) { danger_s = now_s + ACC_DANGER_S; danger_set = true; }
bool acc_danger_ok(uint32_t now_s) { return danger_set && (int32_t)(danger_s - now_s) > 0; }
