// SES V300 (not badge154): the NFC chip on I2C, found with the I2C scanner (see ble_hwtest.h).
// NXP NTAG I2C plus compatible (GET_VERSION over NFC answers 00 04 04 05 02 02 13 03): supply PC5 (high = on, a GPIO switch), SDA = PD2, SCL = PD3,
// 7-bit address 0x55. Everything is non-blocking (one small I2C transaction per nfc_poll() call) and puts the three pins back when it ends.
//   tap:      background mode (switch "NFC tap" in the settings): the chip stays powered, its status register NS_REG is read 5 times a second
//             (bit 0 = RF field present); a phone arriving after at least 400 ms without a field is one tap (nfc_tap_take()).
//   snapshot: read some blocks and the session registers once.      live: poll NS_REG (5 Hz, 50 Hz with FD watch) for NFC_LIVE_MAX_S seconds.
//   FD watch: during live, the given free pins get the internal pull-up and are sampled every millisecond together with the field flag, to find which
//             one (if any) is the chip's open-drain FD output.
//   backup:   read blocks 0x00..0x3A (944 bytes) into RAM for the page to download.
//   write:    write 1..7 blocks from block 1 on (the NDEF area) over I2C with ACK polling and read-back verification.
//   unlock:   clear the static lock bytes (block 0, bytes 10 and 11) and the dynamic lock bytes (block 0x38, bytes 8..10) over I2C; the other bytes
//             are written back as they were read; byte 0 of block 0 is the I2C address (0xAA = 0x55 << 1) and is written as such.
// Only write and unlock ever write to the chip's memory, and only when the caller passes the arming key (see ble_ctl.c).
#ifndef BLE_NFC_H
#define BLE_NFC_H

#include <stdint.h>
#include <stdbool.h>

#define NFC_BLOCKS      6          // blocks read by the snapshot (order below), 16 bytes each
#define NFC_REGS        7          // session registers NC_REG, LAST_NDEF_BLOCK, SRAM_MIRROR_BLOCK, WDT_LS, WDT_MS, I2C_CLOCK_STR, NS_REG
#define NFC_LIVE_MAX_S  120u
#define NFC_LIVE_NS_REG 6
#define NFC_BK_BLOCKS   0x3Bu      // backup: blocks 0x00..0x3A
#define NFC_WBUF_BLOCKS 7          // write buffer: NDEF area blocks 1..7
#define NFC_FD_MAX      12

// nf.mode
#define NFC_M_SNAP   1
#define NFC_M_LIVE   2
#define NFC_M_BACKUP 3
#define NFC_M_WRITE  4
#define NFC_M_UNLOCK 5
// nf.op_res (write, unlock, backup)
#define NFC_R_OK      0
#define NFC_R_NOACK   1            // the chip did not acknowledge (op_blk = block)
#define NFC_R_VERIFY  2            // read-back differs, e.g. the block is write-protected (op_blk = block)
#define NFC_R_REFUSED 3            // precondition failed, nothing written
#define NFC_R_GONE    4            // the chip no longer answers at its address after a write

extern const uint8_t nfc_block_addr[NFC_BLOCKS];   // 0x00 UID / lock bytes / CC, 0x01 and 0x02 start of the NDEF area, 0x38 dynamic lock bytes, 0x3A configuration, 0xFE session registers

typedef struct {
  uint8_t  state;                  // 0 never run, 1 running, 2 finished, 3 failed (the chip never answered / the operation failed)
  uint8_t  mode;                   // NFC_M_*
  uint8_t  fail;                   // snapshot: bit n = block n did not answer, bit 6 = a session register did not answer
  uint8_t  regs_ok;                // bit n = session register n was read
  uint8_t  blocks[NFC_BLOCKS][16];
  uint8_t  regs[NFC_REGS];
  uint8_t  ns_valid, field_now, field_ever;
  uint16_t events;                 // live: times the field went from absent to present
  uint16_t live_s, polls;
  uint8_t  op_res, op_blk;
  uint8_t  fd_n;                   // pins watched in the last live run
} nfc_t;

typedef struct {                   // FD watch: samples per watched pin, split by the field flag at the time of the sample
  uint8_t  n, pin[NFC_FD_MAX];     // pin index PA0..PA8 = 0..8, PB0..PB4 = 9..13, PC0..PC7 = 14..21, PD0..PD3 = 22..25
  uint32_t lo_f[NFC_FD_MAX], hi_f[NFC_FD_MAX], lo_n[NFC_FD_MAX], hi_n[NFC_FD_MAX];
} nfc_fd_t;

bool nfc_snapshot_start(void);
bool nfc_live_start(uint32_t fd_mask);      // fd_mask 0 = no FD watch; the pins of the chip itself (PC5, PD2, PD3) are never taken
bool nfc_backup_start(void);
bool nfc_wbuf_set(uint8_t idx, const uint8_t *d16);   // idx 0..6 = block 1..7
bool nfc_write_start(uint8_t nblocks);      // false if the buffer is incomplete or is not an NDEF TLV that fits
bool nfc_unlock_start(void);
void nfc_stop(void);                        // ends whatever runs and puts the pins back
void nfc_poll(void);                        // main loop
void nfc_reset(void);                       // new connection / disconnect
const nfc_t *nfc_state(void);
const nfc_fd_t *nfc_fd(void);
const uint8_t *nfc_backup_block(uint8_t blk);   // 16 bytes, valid when a backup finished with op_res 0
bool nfc_changed(void);                     // true once when something changed that the page should be told

// tap mode
void nfc_tap_enable(bool on);
void nfc_hold(bool hold);                   // true while the pin monitor or the I2C scanner own the pins: the tap mode stays out
bool nfc_tap_take(void);                    // true once per detected tap
uint8_t nfc_tap_count(void);                // taps since start (wraps at 256)
bool nfc_tap_running(void);

#endif
