# SES-imagotag V300 5.9" BWRY tag: hardware notes

Everything we found out about this tag, from the firmware and the tests on real hardware. What was **measured or verified on the tag** is stated plainly; what is only assumed is marked *(not verified)*. The tag's unique ID and the contents of its serial/UserData pages are deliberately not published here.

## Overview

| Part | What it is |
|---|---|
| Marking | EDG3-0590A (SES-imagotag V300), 5.9" e-paper, 4 colours |
| MCU | Silicon Labs EFR32FG22 (Cortex-M33, 38.4 MHz, 256 KB flash, 32 KB RAM); pyOCD target `efr32fg22c121f256gm40` |
| Crystals | HFXO 38.4 MHz (radio and core) and LFXO 32.768 kHz, both present on the board (photo); LFXO frequency measured by the owner: 32.76909 kHz |
| Radio | Bluetooth LE through Silicon Labs RAIL (own link layer, no Bluetooth stack); TX power up to 6 dBm is reported by the tag |
| Panel | PDi "BWRY Medium", controller ID 0x0D04, 720 x 256 pixels, black / white / yellow / red |
| External flash | Winbond SPI NOR, JEDEC EF 40 13 (512 KiB) |
| NFC | NXP NTAG I2C plus (NT3H2111 / NT3H2211 family), I2C address 0x55, FD (field detect) output wired to the MCU |
| Antenna | printed PCB trace antenna (F / inverted-F shape) at the left short edge of the board, outside the ground plane; fed from the MCU's RF pin through a small matching network (a row of 0402-size pads, some positions look unpopulated) and one blue SMD part that is not identified (from the board photo; tuning and gain not measured) |
| Power | five CR2450 coin cells **in parallel**; the MCU runs directly from the cells (no schematic available) |
| LED / buttons | none |
| Debug | SWD pads; the factory chip is debug-locked (see below) |

## MCU pin map (verified on the tag)

| Function | Pin |
|---|---|
| External flash: CS / CLK / MOSI / MISO | PA5 / PA0 / PB0 / PA4 |
| Panel: SDA (3-wire, bidirectional) / SCLK / CS / DC / RST | PC0 / PC1 / PC2 / PC3 / PC4 |
| Panel: ON/OFF (low = panel supply on) / BUSY (input, high = busy) | PC6 / PA7 |
| NFC chip: supply / SDA / SCL | PC5 / PD2 / PD3 |
| NFC chip: FD (field detect, open drain, low while a phone field is present) | PA8 |
| SWD | PA1..PA3 (debug pins; the pin monitor of the page can watch them only on request) |

The NFC chip is **powered from a GPIO (PC5)**. The firmware switches it on only for a job and drives the I2C bus by bit-banging.

## Panel

* Controller ID 0x0D04 is read with command 0x70 and checked after the OTP read; the controller keeps its configuration in an OTP area of 112 bytes (command 0x92 after the address bytes `00 15 00 00 E0`): the first byte must be 0xA5, the rest is replayed into the controller registers (commands 0x01, 0x00, 0x61, 0x06, 0x03, 0xE7, 0x65, 0x30, 0x50, 0x60, 0xE3, 0xEF, 0xDC..0xDE, 0xE8, 0xDA).
* SPI is bit-banged (about 2 us half period works; 2..200 us were all accepted), 3-wire with one bidirectional data line, DC low = command byte.
* Frame: 46 080 bytes = 720 lines of 64 bytes (the panel's native lines run along the long side, 256 pixels at 2 bits each), codes 0 black, 1 white, 2 yellow, 3 red. The image is sent with command 0x10, line after line.
* Update sequence (about 30 s in total, the refresh itself about 19 s): panel on (PC6 low) -> reset -> OTP read -> init with the temperature value (command 0xE6) -> image (0x10) -> DC/DC on (0x04) -> refresh (0x12, BUSY about 19 s) -> **mandatory DC/DC stop sequence** (0x02, 0x00 `07 2B 01`, then 0xEE writes with 4 ms / 3 ms waits, 0xFF/0xE3 toggles) -> panel off.
* The stop sequence must run after every refresh; the firmware does it also after a failed one.
* Full-colour cleaning cycles (fills in several colour combinations) remove ghost images; the number of changes between cleanings is a setting.
* The panel's own temperature: the firmware passes a fixed value to the waveform selection; **temperature compensation is intentionally not touched**. A 16-bit read with command 0x40 is tried during the page's panel diagnostics and shown as a raw value, but it is *(not verified)* that this controller answers it.
* **Chip temperature as a substitute:** the MCU has a die temperature sensor (EMU->TEMP, 0.25 K steps). With the setting "take the screen temperature from the tag's sensor" the firmware hands this value (rounded, limited to -20..60 C) to the panel with command 0xE6 before every update; without it the number from the page's "Panel temperature" field is used (default 25). At rest the die is close to the air temperature; right after radio or panel activity it is a little warmer. The page shows the chip temperature and the value sent at the last update.

## External flash

Winbond EF 40 13, 512 KiB, bit-banged SPI (commands 0x9F, 0x05/0x35/0x15 status, 0x06, 0x03, 0x02, 0x20 sector erase). Sector erase takes tens of ms; the firmware does it in steps between radio events.

| Address | Content |
|---|---|
| (n-1) x 0xC000 | slot n = 1..10, raw 46 080 byte frame (12 sectors per slot); record "SLD1" (length, CRC32, flags) at slot + 0xB400 |
| 0x78000 .. 0x7EFFF | free test area, used by the flash tools of the page |
| 0x7F000 | settings log: 16 entries of 256 bytes ("SES1", CRC32, version 4) |

The flash is a separate chip: unlocking the MCU does not erase it.

## NFC chip

* NTAG I2C plus: the phone sees an NFC Forum Type 2 tag (ISO 14443-3A, ATQA 0x0044, SAK 0x00); some apps label it Ultralight C / NTAG226 by its type bytes.
* The firmware reads and writes it from the MCU over I2C (address 0x55). The user memory used for the NDEF message starts at block 1; the firmware writes up to 7 blocks (112 bytes), always all seven so that nothing of an older record stays behind. A text record "Dev: sttag.co" was found in a chip that was not yet written by us (probably factory content).
* Session register NS_REG (block 0xFE, register 6): bit7 NDEF_DATA_READ, bit6 I2C_LOCKED, bit5 RF_LOCKED, bit4 SRAM_I2C_READY, bit3 SRAM_RF_READY, bit2 EEPROM_WR_ERR, bit1 EEPROM_WR_BUSY, bit0 RF_FIELD_PRESENT.
* The chip **stretches SCL** while it stores a block into its EEPROM (about 4 ms); the bit-banged master must wait for SCL (the firmware allows 8 ms), otherwise the write fails. Writes are verified by reading the block back.
* While a phone field holds the chip, writes over I2C may be refused; a stuck EEPROM write (EEPROM_WR_BUSY that never clears) clears only when the chip supply is switched off, so the firmware power-cycles the chip (200 ms) before each write.
* FD (PA8) is open drain and goes low while a phone field is present. Together with the NS_REG field bit (polled every 200 ms) it is used as the **tap detector**: a tap opens the Bluetooth window.
* The I2C address can be changed by writing byte 0 of block 0; a rescue and a lock-bit release exist in the page's tools (advanced), they need a tap and a chip backup.

## Power and clocks

* Five CR2450 in parallel. Battery voltage = the chip's own supply, read by the IADC (AVDD/4 input, internal 1.21 V reference, no calibration, about +-2 %), measured a few seconds after power-up and then every 10 minutes while the panel is idle. Default thresholds: warning 2.7 V, critical 2.5 V (at critical the firmware stops automatic slide changes and cleaning cycles; manual ones still work).
* HFXO 38.4 MHz (SMD crystal next to the MCU, marked 38.4) runs the core and the radio.
* **LFXO: a 32.768 kHz crystal is fitted** (the larger SMD crystal at the right of the MCU); the owner measured 32.76909 kHz (about 33 ppm above nominal; measured value as reported, method not recorded). The firmware used the internal LFRCO instead until the crystal was found; since the build after that the SDK's own LFXO setup (CTUNE 63 by default) runs the sleep timer and the EM2 wake-ups from the crystal. The "Crystal test" of the page shows the frequency that the crystal really has with the firmware's setting.
* Watchdog about 8 s from the 1 kHz ULFRCO. Deep sleep (EM2) is optional and off by default: the sleep timer then runs from the LFRCO; RAIL time stops in EM2 and is corrected after the sleep.
* On the bench the tag is powered from the debug probe, not from the cells.

## Factory tag and SWD

A factory tag is **debug-locked**: over SWD no core is found. The status read through the Secure Engine's Debug Challenge Interface of one factory tag was: device status 0x20, family EFR32xG22, SE firmware 0x1020e, debug lock applied, unlock by device erase allowed, authenticated unlock enabled (needs a key). Device erase removes the lock and the factory firmware for good, UserData survives. How to do it: `docs/FACTORY_TAG.md`. After that the firmware image goes to flash address 0 (there is no bootloader on this tag).

## Open questions

* Panel temperature sensor: command 0x40 answer not confirmed.
* Real current consumption (no measurements yet; deep sleep is estimated to extend battery life but has not been measured).
* Antenna tuning (S11, resonance) and radiated power: only the configured TX power is known; the antenna type is read from the board photo. The blue SMD part next to the matching network and the large dark-green rectangle marked "RFRT x035B" (imagotag, Graz) are not identified.
