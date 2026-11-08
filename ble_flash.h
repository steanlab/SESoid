// SES V300, BLE stage 3: external SPI NOR flash (Winbond EF 40 13, 512 KiB) driven by bit-banging, in small steps that fit between
// BLE connection events. Pins (found earlier): CS PA5, CLK PA0, MOSI PB0, MISO PA4.
// Writes (erase / program) are only allowed with the lock opened by fl_unlock(); until stage 3b only the test area is writable.
#ifndef BLE_FLASH_H
#define BLE_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#define FL_SIZE        0x80000u
#define FL_SECTOR      4096u
#define FL_PAGE        256u
#define FL_TEST_END    0x10000u     // writes are accepted below this address only (blank area 0x00000..0x2FFFF of the dump)

// result codes (also sent to the phone)
#define FLR_OK         0
#define FLR_BUSY_TMO   1            // BUSY did not clear in time
#define FLR_NOT_BLANK  2            // sector not all FF after the erase
#define FLR_NO_WEL     3            // write enable latch did not set after WREN
#define FLR_VERIFY     4            // read-back differs from what was programmed
#define FLR_PARAM      5            // bad address / another operation running / out of the test area
#define FLR_RUNNING    0xFF

void fl_init(void);                                  // pins; called on every new connection
bool fl_busy(void);                                  // an operation is running
void fl_poll(void);                                  // main loop: advances the running operation when the radio leaves enough time
bool fl_start_info(void);                            // JEDEC id + status registers -> fl_result_t
bool fl_start_erase(uint32_t addr);                  // 4 KiB sector; busy time and status register samples are recorded
bool fl_start_program(uint32_t addr, const uint8_t *data);   // one 256 byte page (page aligned), verified by reading back
bool fl_start_scan(uint32_t addr);                   // read-only: count bytes != FF in the 4 KiB sector

typedef struct {
  uint8_t  op;            // 1 info, 2 erase, 3 program, 4 scan
  uint8_t  result;        // FLR_*
  uint32_t jedec;         // manufacturer << 16 | type << 8 | capacity
  uint8_t  sr1_first, sr1_last, sr1_wel;   // status samples: first poll after the command, last poll, SR1 right after WREN
  uint8_t  sr2, sr3;
  uint32_t busy_us;       // time from the command until BUSY cleared
  uint32_t nonff;         // scan / blank check: bytes != FF
} fl_result_t;

extern volatile fl_result_t fl_res;
extern volatile bool fl_done;                        // set when an operation finishes, cleared by the user of the result

// For the page writer of stage 3b: blocking-free interface used by ble_up.c to store received pages.
#endif
