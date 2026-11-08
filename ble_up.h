// SES V300, BLE stage 3: slide upload protocol on top of the RX/TX characteristics of ble_att.c.
//
// Everything is in pages of 256 bytes, sent as 16 chunks of 16 bytes. All multi-byte values are little endian.
// Phone -> tag (writes to the RX characteristic, first byte = opcode; opcodes 0x01..0x03 are the old stage-2 tests):
//   10 mode total(4)        BEGIN  mode 0 = RAM sink (data is only counted and checksummed), mode 1 = external flash (stage 3b)
//   11 idx(2) data(16)      CHUNK  idx = 16-byte chunk number from the start of the image; idempotent (may be repeated)
//   12 page(2)              PAGE   the phone has sent all chunks of this page: the tag checks them and commits the page
//   13                      END    all pages sent: the tag answers with the CRC32 of the whole image
//   14                      ABORT
//   20                      FLASH INFO  (JEDEC id and status registers)
//   21 sector               FLASH ERASE TEST   sector 0..15 of the test area (4 KiB each), measured BUSY time, blank check
//   22 sector               FLASH PROGRAM TEST one test page (pattern) at the start of that sector, verified
//   23 sector(2)            FLASH SCAN   (read only, any sector 0..127) number of bytes that are not FF
// Tag -> phone (notifications on the TX characteristic):
//   90 status mode          BEGIN answer (status 0 = ok, 1 = bad parameters, 3 = mode not available)
//   92 page(2) status       page committed (0), 1 = out of order (then expected page follows as 2 more bytes), 2 = storage error
//   93 page(2) mask(2)      page incomplete: bit n set = chunk n was received; the phone repeats the missing chunks and the PAGE message
//   94 status crc(4) total(4)  END answer (status 0 = all pages committed; 1 = pages missing)
//   A0 jedec(3) sr1 sr2 sr3 ; A1/A2 result busy_us(4) sr_first sr_last sr_wel nonff(2) ; A3 result nonff(4)
#ifndef BLE_UP_H
#define BLE_UP_H

#include <stdint.h>
#include <stdbool.h>

void up_reset(void);                              // new connection / disconnect
bool up_write(const uint8_t *v, uint8_t len);     // a write to the RX characteristic; true if the opcode belongs to this module
void up_poll(void);                               // main loop

// CRC-32 (IEEE, as zlib / the JavaScript side)
uint32_t up_crc32(uint32_t crc, const uint8_t *p, uint32_t n);

#endif
