// SES V300 (not badge154): slideshow logic. Slide slots 1..7 of the external flash (see ble_up.h) hold raw 46 080 byte panel frames;
// this module knows which slots are valid, keeps the settings (persisted in the settings sector 0xF000 of the flash, as an append-only
// log of 16 entries, so a new save normally costs one page write and one erase per 16 saves), and every `interval` minutes
// shows the next slide with ble_panel.c. It also answers "show slot n now" / "next" from the phone (ble_ctl.c).
#ifndef BLE_SLIDE_H
#define BLE_SLIDE_H

#include <stdint.h>
#include <stdbool.h>

#define SLIDE_FRAME_BYTES 46080u

typedef struct {
  uint8_t  enabled;        // 1 = change the slide automatically
  uint8_t  mode;           // 0 = in order, 1 = random
  uint16_t interval_min;   // 1..1440 minutes between slide changes (measured from the end of the previous panel update)
  uint8_t  mask;           // bit n (1..7) = slot n takes part in the slideshow
  int8_t   temp_c;         // temperature handed to the panel (waveform choice)
  int8_t   tx_dbm;         // Bluetooth transmit power request, dBm (RAIL clips it to what the chip can do)
  uint8_t  nfc;            // 1 = a tap of a phone on the NFC chip shows the next slide (ble_nfc.h); older saved settings read 0
} slide_cfg_t;

typedef struct {
  bool     valid;          // a record 'SLD1' is present
  bool     show;           // valid and a complete frame (46 080 bytes): can be shown
  uint32_t total;
  uint32_t crc;
} slot_info_t;

// answers of slide_show() / slide_next()
#define SLR_OK        0    // update started
#define SLR_PANEL_BUSY 1
#define SLR_BAD_SLOT  2    // nothing showable in that slot / no slot to show
#define SLR_FLASH_BUSY 3   // an upload or another flash operation is running

void slide_init(void);                           // after RAIL init: flash pins, settings, slot table, TX power
void slide_poll(void);                           // main loop
const slide_cfg_t *slide_cfg(void);
void slide_set_cfg(const slide_cfg_t *c);        // takes effect now, saved in the background
uint8_t slide_show(uint8_t slot);                // SLR_*
uint8_t slide_next(void);                        // SLR_*: the next slide of the playlist
void slide_rescan(void);                         // read the slot records again (e.g. after an upload)
bool slide_scanning(void);
const slot_info_t *slide_slot(uint8_t slot);     // slot 1..7
uint8_t slide_current(void);                     // slot shown last (0 = none)
uint32_t slide_next_in_s(void);                  // seconds until the automatic change; 0xFFFFFFFF = off
uint32_t slide_uptime_s(void);
bool slide_flash_busy(void);                     // a settings write owns the flash right now
int16_t slide_tx_actual_ddbm(void);              // transmit power the radio really uses, tenth of dBm
void slide_apply_tx(void);                       // (re)apply the TX power request now (main context, radio idle or between events)
bool slide_panel_event(uint8_t *status, uint8_t *err, uint8_t *src, uint8_t *shown);   // true once per finished panel update

#endif
