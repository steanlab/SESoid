// SES V300 (not badge154): control protocol for the web page (slideshow, settings, system). Opcodes 0x30..0x3F on the RX characteristic;
// the answers are notifications on the TX characteristic (at most 20 bytes, several fragments are paced out of ctl_poll()).
//
// Phone -> tag:
//   30                      STATUS     -> B0
//   31                      SLOTS      -> B1 x7 (slot table is read from the flash first)
//   32 slot                 SHOW       show slot 1..7 now (a panel update takes about 30 s)           -> B3
//   33                      NEXT       show the next slide of the playlist now                          -> B3
//   34 en mode int(2) mask temp tx   SET SETTINGS  enabled, order (0 = in order, 1 = random), minutes (1..1440), slot mask (bit n = slot n),
//                           panel temperature (signed, degrees C), Bluetooth TX power request (signed dBm)   -> B4
//   35                      GET SETTINGS                                                                 -> B4
//   36 A5                   REBOOT     (the 0xA5 is a safety byte)                                      -> B5, then the tag resets
//   37                      HW INFO    -> B6 x4
//   38 code                 FILL       panel plain colour 0 black, 1 white, 2 yellow, 3 red (cleans ghost images)   -> B3
//   39                      SPLASH     show the boot picture again                                      -> B3
//   3A                      PANEL DIAGNOSTICS  (about 5 s: panel off 3 s, on, reset, measure BUSY / SDA / chip ID)   -> B3, later B8 by itself
//   3B                      PANEL DIAGNOSTICS REPORT  (the last one)                                     -> B8
//   3C                      CLOCK TEST  (about 1 s with a crystal, 2.3 s without: starts the 32.768 kHz oscillator, measures LFXO / LFRCO / ULFRCO against the 38.4 MHz crystal)  -> B3, later B9 by itself
//   3D                      CLOCK TEST REPORT  (the last one)                                            -> B9
//   3E mode [mask(4)]       PIN MONITOR  mode 0 off, 1 floating inputs, 2 pull-up, 3 pull-down (+0x80: also watch PA1..PA3); the optional mask (bit n = pin n)
//                           limits the watched pins, so a single pin can be pulled up while the others stay untouched; clears the history; stops by itself
//                           after 5 minutes and when the connection ends                                  -> BA (then once a second and whenever a pin got both levels)
//   3F [1]                  PIN MONITOR REPORT; with 1 the history is cleared first                      -> BA
//   3F 02 opt mask(4)       I2C SCAN on the free pins of the mask (2..6 pins, every ordered pair is tried as SDA / SCL); opt 0 the other
//                           pins float, 1 pulled up, 2 pulled down                                        -> B3, then BB (twice a second, and at the end)
//   3F 05 opt mask(4) pwr(4) I2C SCAN with power pins: each pin of pwr is driven high in turn (push-pull) during a whole scan of mask (2..4 pins);
//                           stops at the first device and reports the power pin                              -> B3, then BB
//   3F 10                   NFC CHIP SNAPSHOT (read only): power on, read the blocks and session registers, power off  -> B3, then BC x7 and BD
//   3F 11                   NFC CHIP LIVE: power on, poll the status register 5 times a second for 2 minutes   -> B3, then BD on every change and every second
//   3F 12                   NFC CHIP OFF                                                                     -> BD
//   3F 13                   NFC CHIP REPORT again                                                            -> BC x7 (snapshot only) and BD
//   3F 03                   I2C SCAN REPORT                                                              -> BB
//   3F 04                   I2C SCAN ABORT                                                               -> BB
// Tag -> phone:
//   B0 pstat perr src shown step next_s(4) uptime_s(4) count(2) crc_bad flags       STATUS (pstat 0 idle 1 running 2 done 3 failed; flags bit0 auto on,
//                           bit1 flash idle, bit2 scanning)
//   B1 slot flags total(4) crc(4)       flags: bit0 record valid, bit1 showable (46 080 bytes), bit2 in the playlist, bit3 shown last
//   B3 op status            result of 32/33/38/39: 0 started, 1 panel busy, 2 nothing to show, 3 flash busy
//   B4 ver en mode int(2) mask temp tx_req tx_actual(2, tenth of dBm)
//   B5                      reboot acknowledged
//   B6 idx data...          hardware info fragments (idx 0..3), see ble_ctl.c
//   B7 pstat perr src shown panel update finished (sent on its own)
//   B8 state busy_off sda_pd_off sda_pu_off busy_on sda_pd_on sda_pu_on busy_rstlow busy_10 busy_100 busy_500 busy_ms(2) id1(2) id2(2) id_slow(2)
//                           panel diagnostics report (state 0 never run, 1 running, 2 finished), fields as in panel_diag_t
//   B9 state flags sel ctune_xi ctune_xo fact_xi fact_xo lfxo_ms(2) cnt_lfxo(3) cnt_lfrco(3) cnt_ulfrco(3) captune
//                           clock test report, fields as in hwtest_clock_t (ble_hwtest.h); a count is HFXO cycles per CT_DOWN_x / 2 cycles of the clock
//   BC idx data(16)         NFC chip block idx 0..5 (blocks 00 01 02 38 3A FE, see nfc_block_addr), then BC 6 regs_ok r0..r6 = the session registers
//   BD state mode fail flags ns events(2) live_s(2) polls(2) regs_ok
//                           NFC chip status, fields as in nfc_t (ble_nfc.h); flags bit0 NS_REG valid, bit1 field present now, bit2 field seen
//   BB state|opt<<4 pairs_total pairs_done stuck hits sda scl addr pwr pwr_now 0 0(spare) high(4) held(4)   (pwr, pwr_now: pin index, 0xFF none)
//                           I2C scan report, fields as in hwtest_scan_t (state 0 never, 1 running, 2 finished, 3 aborted; pin indices as in BA; addr = 7 bit)
//   BA mode elapsed_s(2) level(4) seen_hi(4) seen_lo(4) edges(2) last_pin 0
//                           pin monitor report, fields as in hwtest_pins_t (pin index: PA0..PA8 = 0..8, PB0..PB4 = 9..13, PC0..PC7 = 14..21, PD0..PD3 = 22..25)
#ifndef BLE_CTL_H
#define BLE_CTL_H

#include <stdint.h>
#include <stdbool.h>

void ctl_reset(void);                              // new connection / disconnect
bool ctl_write(const uint8_t *v, uint8_t len);     // a write to the RX characteristic; true if the opcode belongs to this module
void ctl_poll(void);                               // main loop (also runs the hardware tests)

#endif
