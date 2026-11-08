// SES V300, BLE stage 2: L2CAP/ATT server with a tiny GATT database (device name + one private service).
//
//   Service  53455300-0300-4f45-5000-534c49444500   (handles 0x0004..0x0009)
//     RX char 53455301-...  write / write without response   (handle 0x0006)  - the phone sends data here
//     TX char 53455302-...  notify                            (handle 0x0008, CCCD 0x0009) - the tag answers here
//
// Test protocol of stage 2 (first byte of a write to RX):
//   0x01 ...  PING : the tag notifies 0x81 followed by the same bytes (up to 19)
//   0x02      STAT : the tag notifies 0x82, total bytes written (4 bytes LE), number of writes (2 bytes LE)
//   0x03      RESET: counters back to 0, notifies 0x83
//   anything else is counted as bulk data (this is how the speed of a future image upload is measured)
#ifndef BLE_ATT_H
#define BLE_ATT_H

#include <stdint.h>
#include <stdbool.h>

void att_reset(void);                              // new connection
void att_rx(const uint8_t *att, uint8_t len);      // one ATT PDU received on L2CAP channel 4
bool att_notify(const uint8_t *data, uint8_t len); // notification on the TX characteristic (len <= 20); false if not enabled/queue full

#endif
