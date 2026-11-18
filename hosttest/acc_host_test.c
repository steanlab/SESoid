// Host test of ble_acc.c: identity derived from the unique ID and the access window.
#include <stdio.h>
#include <string.h>
#include "ble_acc.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
  static const uint8_t S1[8] = { 9, 8, 7, 6, 5, 4, 3, 2 }, S2[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
  uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
  acc_init(a, S1);
  uint8_t ad1[6]; memcpy(ad1, acc_addr(), 6); char nm1[16]; strcpy(nm1, acc_name());
  CHECK((ad1[5] & 0xC0) == 0xC0, "static random address needs the two top bits set");
  CHECK(strlen(nm1) == 9 && acc_name_len() == 9 && !strncmp(nm1, "V300-", 5), "name %s", nm1);
  for (int i = 5; i < 9; i++) CHECK((nm1[i] >= '0' && nm1[i] <= '9') || (nm1[i] >= 'A' && nm1[i] <= 'F'), "name hex");
  acc_init(a, S1); CHECK(!memcmp(ad1, acc_addr(), 6) && !strcmp(nm1, acc_name()), "same ID, same identity");
  acc_init(b, S1); CHECK(memcmp(ad1, acc_addr(), 6) != 0, "different ID, different address");
  // many IDs: no collisions of the 48-bit address in a sample, and the ID is not contained in the address
  int coll = 0; static uint8_t seen[4000][6];
  for (int i = 0; i < 4000; i++) { uint8_t e[8] = { 0x28, 0x6D, 0x97, 0xFF, 0xFE, (uint8_t)(i >> 8), (uint8_t)i, 0x11 }; acc_init(e, S1); memcpy(seen[i], acc_addr(), 6); for (int j = 0; j < i; j++) if (!memcmp(seen[j], seen[i], 6)) coll++; CHECK((seen[i][5] & 0xC0) == 0xC0, "top bits"); }
  CHECK(coll == 0, "%d address collisions in 4000 IDs", coll);
  acc_init(a, S2); CHECK(memcmp(ad1, acc_addr(), 6) != 0, "same ID, other secret: other address");
  // window
  acc_init(a, S1);
  CHECK(!acc_is_open(0), "closed before it was opened");
  acc_open(0, ACC_BOOT_S); CHECK(acc_is_open(0) && acc_is_open(ACC_BOOT_S - 1) && !acc_is_open(ACC_BOOT_S), "boot window");
  CHECK(acc_left_s(ACC_BOOT_S - 20) == 20, "left %u", acc_left_s(ACC_BOOT_S - 20));
  acc_open(ACC_BOOT_S - 20, ACC_TAP_S); CHECK(acc_is_open(ACC_BOOT_S + ACC_TAP_S - 21) && !acc_is_open(ACC_BOOT_S + ACC_TAP_S - 20), "tap extends");
  acc_open(ACC_BOOT_S + 30, ACC_LINK_S); CHECK(!acc_is_open(ACC_BOOT_S + ACC_TAP_S - 20), "a shorter request never shortens the window");
  acc_open(2000, 30); CHECK(acc_is_open(2029) && !acc_is_open(2030), "reopen later");
  // the uptime counter wraps: the window still works
  acc_init(a, S1); acc_open(0xFFFFFFF0u, 60); CHECK(acc_is_open(0xFFFFFFF5u) && acc_is_open(10) && !acc_is_open(60), "wrap");
  // dangerous commands: locked until a tap, open for 60 s, locked again after
  acc_init(a, S1); CHECK(!acc_danger_ok(0) && !acc_danger_ok(500), "locked after power-up");
  acc_danger_arm(100); CHECK(acc_danger_ok(100) && acc_danger_ok(159) && !acc_danger_ok(160), "open for 60 s after a tap");
  acc_danger_arm(150); CHECK(acc_danger_ok(209) && !acc_danger_ok(210), "a new tap restarts the 60 s");
  acc_danger_arm(0xFFFFFFF0u); CHECK(acc_danger_ok(5) && !acc_danger_ok(0x30u), "wrap");
  acc_init(a, S1); CHECK(!acc_danger_ok(0xFFFFFFF5u), "acc_init locks again");
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails != 0;
}
