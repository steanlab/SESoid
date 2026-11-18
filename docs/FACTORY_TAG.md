# First install on a factory V300 tag

A factory tag (EFR32FG22) is **debug-locked**: over SWD it tells nothing about itself, and flashing fails with "No cores were discovered". The lock is removed by a device erase through the chip's Debug Challenge Interface. This **erases the factory firmware for good** (there is no way to copy it first); the UserData page survives. The external flash (slides) is a separate chip and is not touched.

You need: a CMSIS-DAP style debug probe (SWCLK, SWDIO, GND, nRESET, tag supply), Python with `pyocd` (`pip install pyocd`), the CMSIS pack `SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack` (keil.arm.com), and the firmware image `v300_ble_probe.bin`.

## 1. Check the tag (changes nothing)

```
python tools/v300_unlock.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack
```

A locked factory tag answers like this:

```
device status 0x20, family EFR32xG22, SE firmware 0x1020e
debug lock: APPLIED | unlock by device erase: allowed | authenticated unlock: allowed (needs a key, not supported here)
```

"No ACK" means the probe does not reach the chip: check the tag supply, the wires and lower `--freq` (default 100000).

## 2. Unlock (erases the factory firmware)

```
python tools/v300_unlock.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --unlock
```

Then power the tag off for 5 s and on again and run step 1: it must say `debug lock: none`.

## 3. Flash

```
python tools/ble_probe.py --pack ./SiliconLabs.GeckoPlatform_EFR32FG22_DFP.2025.12.1.pack --flash v300_ble_probe.bin --watch
```

The image goes to address 0 (there is no bootloader). From now on the tag is controlled from the page.

## What the script does

`tools/v300_unlock.py` talks to the DCI (access port 1, ID 0x54770002) through pyOCD: command `0xFE010000` reads the status (debug lock bits), command `0x430F0000` erases the main flash and removes the standard debug lock, then the tag is reset through nRESET. The same sequence is what OpenEPaperLink's `reflash.py -s` / `-u` does (OpenEPaperLink/Tag_FW_EFR32xG22); this script is the unlock part only, written anew, with a lower default SWD clock.
