# SESoid: standalone slideshow firmware for the SES-imagotag V300 tag

Firmware and control page for the SES-imagotag V300 5.9" BWRY GU510, a four-colour (black, white, yellow, red) e-paper shelf tag built around an EFR32FG22. The tag keeps up to 10 pictures in its external flash, shows them as a slideshow and is controlled from a phone or PC over Bluetooth LE with a web page (Web Bluetooth). No bootloader, no cloud, no gateway.

Control page ("Gecko Slide"): https://steanlab.github.io/SESoid (source: `docs/index.html`).
This repository is neither `badge154` nor `badge154-src`.

## What it does

- 10 slots of 720 x 256 pixels in the external flash, slideshow with a set interval, order and slot selection; panel cleaning cycles every N changes.
- The page converts any picture to the four panel colours (dithering with correction for the panel's dot gain) and uploads it over Bluetooth.
- Own Bluetooth LE link layer on top of Silicon Labs RAIL. The tag advertises only inside an access window (after power-up, after a phone is tapped on the NFC chip, and briefly after a connection); dangerous commands need a fresh NFC tap.
- NFC: a tap shows the next slide and opens the window; the page can read, back up, write (text, link, Wi-Fi) and restore the NFC chip's memory.
- Battery monitor (five CR2450 in parallel), panel temperature from the panel's own sensor (chip sensor as fallback), optional deep sleep (EM2) and OpenDisplay compatibility (both experimental).
- Demo tour for filming: splash, chosen test pictures drawn by the tag itself (calibration chart, chessboards, fractals, TV test chart and more), one slide, a TAG HARDWARE screen with live data.
- Diagnostics: panel, crystal, I²C/NFC and pin tests from the page.

## Repository

| Path | Content |
|---|---|
| `v300_ble_probe.slcp`, `main.c`, `app.c` | project file, start-up, radio set-up |
| `ble_ll.c`, `ble_att.c`, `ble_acc.c` | link layer, attribute layer, access window |
| `ble_ctl.c`, `ble_up.c`, `ble_slide.c` | commands, slot upload and erase, slideshow and settings |
| `ble_panel.c`, `ble_splash.c`, `ble_demo.c`, `ble_font7x14.c` | panel driver, boot splash, demo pictures and hardware screen |
| `ble_flash.c`, `ble_nfc.c`, `ble_batt.c`, `ble_sys.c`, `ble_hwtest.c`, `ble_od.c` | external flash, NFC chip, battery, chip helpers, hardware tests, OpenDisplay |
| `docs/index.html` | the control page (single file, also a PWA) |
| `docs/HARDWARE.md` | everything found out about the tag's hardware: pins, panel protocol, flash map, NFC chip, clocks |
| `docs/FACTORY_TAG.md` | first install on a factory (debug-locked) tag |
| `tools/` | `ble_probe.py` (flash the image and read diagnostics over SWD), `v300_unlock.py` (remove the debug lock), `make_splash.py`, `ll_sim.py` |
| `hosttest/`, `pagetest/` | host tests of the firmware modules and a headless test of the page |
| `.github/workflows/build.yml`, `Dockerfile` | build |

## Build

GitHub Actions (`.github/workflows/build.yml`) builds with Gecko SDK 4.4.1 and publishes `v300_ble_probe.bin` as the artifact `v300_ble_probe`. No local toolchain is needed. The firmware starts at flash address 0 and uses no bootloader.

## Flash

With a CMSIS-DAP debug probe on SWD and `pyocd`:

```
python tools/ble_probe.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --flash v300_ble_probe.bin --watch
```

A factory tag is debug-locked: follow `docs/FACTORY_TAG.md` first.

## Tests

```
sh hosttest/build_slide_test.sh      # also: up, acc, batt, nfc, od, hwtest, panel, demo
node pagetest/page_test.js "$PWD/docs/index.html" /tmp/pt
```

The host tests build the firmware modules on a PC; the page test runs the page in headless Chromium with a simulated tag.

## Status

Slideshow, upload, NFC window, battery and panel update are used on a real tag. The unlock script, deep sleep (EM2), OpenDisplay and the demo tour are new and not yet confirmed on hardware.
