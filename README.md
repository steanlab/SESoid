# SES V300, Bluetooth stage 1

Does the radio of the EFR32FG22 (SES-imagotag V300 tag) do Bluetooth LE through Silicon Labs RAIL?
Project: SES V300 standalone slideshow. This repository is neither `badge154` nor `badge154-src`.

Files
- `v300_ble_probe.slcp`, `main.c`, `app.c`: the firmware (advertises "V300-probe" and logs what it hears).
- `build.sh`, `Dockerfile`, `.github/workflows/build.yml`: build with Gecko SDK 4.4.1 (GitHub Actions).
- `tools/ble_probe.py`: flashes the program over SWD and prints the counters from the tag's RAM.

The firmware touches neither the external flash nor any pin of the panel. It uses no bootloader and starts at flash address 0.
