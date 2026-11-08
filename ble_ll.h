// SES V300, BLE stage 2: own BLE link layer (slave role) on top of RAIL.
#ifndef BLE_LL_H
#define BLE_LL_H

#include <stdint.h>
#include <stdbool.h>
#include "rail.h"

#define LL_MAX_PAYLOAD 27   // no LE data length extension: every LL PDU carries at most 27 bytes

typedef struct {
  uint8_t llid;             // 1 = continuation/empty, 2 = start of L2CAP message, 3 = LL control
  uint8_t len;
  uint8_t data[LL_MAX_PAYLOAD];
} ll_pdu_t;

void ll_init(RAIL_Handle_t h);

// True from a successful ll_start() until the connection is closed (then the advertising loop takes over again).
bool ll_connected(void);

// Main context. ci = the received CONNECT_IND exactly as RAIL delivers it (2 byte header + 34 bytes),
// ci_end_us = RAIL time of the end of that packet. Returns false if the parameters are not acceptable.
bool ll_start(const uint8_t *ci, uint32_t ci_end_us);

// Main context, call as often as possible while connected: arms the next connection event, supervises the link,
// handles LL control procedures and passes L2CAP data on to the ATT layer.
void ll_poll(void);

// Main context: queue one LL PDU for the master (it leaves in the next connection event). False when the queue is full.
bool ll_send(uint8_t llid, const uint8_t *data, uint8_t len);

// Interrupt context: RAIL event handler while connected.
void ll_on_event(RAIL_Handle_t h, RAIL_Events_t events);

#endif
