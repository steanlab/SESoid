#!/bin/bash
# Build inside the silabs-builder container (see Dockerfile and .github/workflows/build.yml).
# Same steps as the OpenEPaperLink EFR32xG22 build_firmware.sh, minus everything that belongs to OEPL.
set -e
GSDK_PATH=${GSDK_PATH:-/gecko_sdk/}

rm -rf ./out
slc generate -s ${GSDK_PATH} -p v300_ble_probe.slcp -o makefile -d ./out -cp
cd out

# The generated makefile does not allow C define overrides; harmless, but keep the same patch as upstream.
sed -i 's/C_DEFS   =/C_DEFS ?=/' v300_ble_probe.Makefile

# The SDK insists on the 32.768 kHz crystal (LFXO). Whether the V300 has one is not known: use the internal LFRCO.
sed -i 's/, LFXO/, LFRCO/' autogen/sl_device_init_clocks.c
sed -i 's/ sl_device_init_lfxo/\/\/ sl_device_init_lfxo/' autogen/sl_event_handler.c

make -f v300_ble_probe.Makefile -j8

cd ..
arm-none-eabi-objcopy -O binary out/build/debug/v300_ble_probe.out v300_ble_probe.bin
cp out/build/debug/v300_ble_probe.out v300_ble_probe.elf
arm-none-eabi-size out/build/debug/v300_ble_probe.out
ls -l v300_ble_probe.bin
