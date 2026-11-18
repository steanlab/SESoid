// SES V300: OpenDisplay compatibility (setting "OpenDisplay", off by default). Own code, written from the public protocol description.
// While the setting is on the tag also offers the service 0x2446 with one characteristic 0x2446 (write, write without response, notify) and
// advertises the service and the 16-byte record of the protocol. Messages start with 0x00 and the command; answers are [status][command][data]
// (status 0 = ok, 0xFF = refused). Supported: 0x40 read the configuration, 0x43 firmware version, 0x44 advertising record, 0x50 authenticate (no
// encryption), direct write 0x70 start / 0x71 data / 0x72 end (an uncompressed frame of 46 080 bytes, 2 bits per pixel, written into slot 10 and
// shown), then 0x73 when the screen refresh is over. Everything else is refused. Not verified with a real OpenDisplay client yet.
#ifndef BLE_OD_H
#define BLE_OD_H
#include <stdbool.h>
#include <stdint.h>

#define OD_SLOT 10u                  // the slot that receives OpenDisplay frames

bool od_enabled(void);                                // the setting is on
void od_rx(const uint8_t *d, uint8_t len);            // a write to the OpenDisplay characteristic
void od_poll(void);                                   // main loop: answers are sent one at a time, uploads advance
void od_reset(void);                                  // new connection / disconnect
bool od_busy(void);                                   // an OpenDisplay upload is running: answers of the upload code come to od_up_sink()
bool od_up_sink(const uint8_t *d, uint8_t len);       // an answer of ble_up.c (0x90, 0x92, 0x93, 0x94) instead of a notification to the page
void od_msd(uint8_t b[16]);                           // the 16-byte record (company id first); the advertisement carries it
#endif
