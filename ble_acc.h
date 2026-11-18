// SES V300 (not badge154): identity and access window of the Bluetooth side.
//   Identity: the advertising address (static random) and the name "V300-XXXX" are derived from the chip's unique 64-bit ID and a random 64-bit secret
//             made on the first start and kept in the settings log, so every tag has its own and two tags in one room never collide. Without the secret
//             nobody can compute the ID from the address. (Erasing the settings flash makes a new secret and so a new address and name.)
//   Window:   the tag advertises only while the window is open: for ACC_BOOT_S seconds after power-up / reset, ACC_TAP_S seconds after each tap of a
//             phone on the NFC chip, and ACC_LINK_S seconds after a connection ended. Outside the window it stays silent (no radio at all),
//             so nobody can connect to it without being at the tag, and the battery is spared.
#ifndef BLE_ACC_H
#define BLE_ACC_H

#include <stdint.h>
#include <stdbool.h>

#define ACC_BOOT_S  180u                       // 3 minutes after power-up
#define ACC_TAP_S   90u
#define ACC_LINK_S  30u
#define ACC_DANGER_S 60u                       // dangerous commands (reboot, NFC chip writes, flash tests) work this long after a tap

void acc_init(const uint8_t eui[8], const uint8_t secret[8]);   // computes address and name from the chip ID mixed with a per-tag random secret (kept in the settings flash, never sent): the address alone does not reveal the ID
const uint8_t *acc_addr(void);                // 6 bytes, LSB first, top two bits of the last byte = 11 (static random)
const char *acc_name(void);                   // "V300-XXXX"
uint8_t acc_name_len(void);
void acc_open(uint32_t now_s, uint32_t secs); // open the window until at least now + secs (uptime seconds)
bool acc_is_open(uint32_t now_s);
uint32_t acc_left_s(uint32_t now_s);          // seconds left (0 = closed)
void acc_danger_arm(uint32_t now_s);          // a tap: dangerous commands are allowed for ACC_DANGER_S seconds
bool acc_danger_ok(uint32_t now_s);

#endif
