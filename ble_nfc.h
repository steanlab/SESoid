// SES V300 (not badge154): the NFC chip on I2C, found with the I2C scanner (see ble_hwtest.h).
// NXP NTAG I2C plus compatible (GET_VERSION over NFC answers 00 04 04 05 02 02 13 03): supply PC5 (high = on, a GPIO switch), SDA = PD2, SCL = PD3,
// 7-bit address 0x55. Stage 1: READ ONLY. Nothing is ever written to the chip's memory; the only bytes sent are the address and the block / register
// number before a read. Both operations are non-blocking (one small I2C transaction per nfc_poll() call) and put the three pins back when they end.
//   snapshot: power on, read the blocks and the session registers once, power off.
//   live:     power on, read the status register NS_REG every 200 ms (bit 0 = RF field present), for NFC_LIVE_MAX_S seconds, power off.
#ifndef BLE_NFC_H
#define BLE_NFC_H

#include <stdint.h>
#include <stdbool.h>

#define NFC_BLOCKS      6          // blocks read by the snapshot (order below), 16 bytes each
#define NFC_REGS        7          // session registers NC_REG, LAST_NDEF_BLOCK, SRAM_MIRROR_BLOCK, WDT_LS, WDT_MS, I2C_CLOCK_STR, NS_REG
#define NFC_LIVE_MAX_S  120u
#define NFC_LIVE_NS_REG 6

extern const uint8_t nfc_block_addr[NFC_BLOCKS];   // 0x00 UID / lock bytes / CC, 0x01 and 0x02 start of the NDEF area, 0x38 dynamic lock bytes, 0x3A configuration, 0xFE session registers

typedef struct {
  uint8_t  state;                  // 0 never run, 1 running, 2 finished, 3 failed (the chip never answered)
  uint8_t  mode;                   // 1 snapshot, 2 live
  uint8_t  fail;                   // snapshot: bit n = block n did not answer, bit 6 = a session register did not answer
  uint8_t  regs_ok;                // bit n = session register n was read
  uint8_t  blocks[NFC_BLOCKS][16];
  uint8_t  regs[NFC_REGS];
  uint8_t  ns_valid, field_now, field_ever;
  uint16_t events;                 // live: times the field went from absent to present
  uint16_t live_s, polls;
} nfc_t;

bool nfc_snapshot_start(void);
bool nfc_live_start(void);
void nfc_stop(void);               // ends whatever runs and puts the pins back (state stays as it was, 2 if it was running)
void nfc_poll(void);               // main loop
void nfc_reset(void);              // new connection / disconnect
const nfc_t *nfc_state(void);
bool nfc_changed(void);            // true once when something changed that the page should be told

#endif
