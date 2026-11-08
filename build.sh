#!/bin/bash
# Build inside the ghcr.io/nabucasa/silabs-firmware-builder image (it brings slc and the ARM compiler).
# The Gecko SDK 4.4.1 is checked out by the workflow and mounted at /gecko_sdk.
set -e
GSDK_PATH=${GSDK_PATH:-/gecko_sdk}

echo "=== environment ==="
uname -a
echo "slc:     $(command -v slc || echo 'not in PATH')"
echo "gcc:     $(command -v arm-none-eabi-gcc || echo 'not in PATH')"
ls /opt 2>/dev/null || true

# Look for the tools if they are not in PATH.
if ! command -v slc >/dev/null; then
  f=$(find / -xdev \( -name slc -o -name slc-cli \) -type f -perm -u+x 2>/dev/null | head -1)
  test -n "$f" && export PATH="$PATH:$(dirname "$f")"
fi
if ! command -v arm-none-eabi-gcc >/dev/null; then
  f=$(find / -xdev -name arm-none-eabi-gcc -type f 2>/dev/null | head -1)
  test -n "$f" && export PATH="$PATH:$(dirname "$f")"
fi
command -v slc || { echo "slc not found"; exit 1; }
command -v arm-none-eabi-gcc || { echo "arm-none-eabi-gcc not found"; exit 1; }
arm-none-eabi-gcc --version | head -1
test -f "$GSDK_PATH/gecko_sdk.slcs" || { echo "no Gecko SDK in $GSDK_PATH"; exit 1; }
grep sdk_version "$GSDK_PATH/gecko_sdk.slcs"
# the RAIL library must be a real file, not a Git LFS pointer
sz=$(stat -c %s "$GSDK_PATH/platform/radio/rail_lib/autogen/librail_release/librail_efr32xg22_gcc_release.a")
echo "RAIL library size: $sz bytes"
test "$sz" -gt 100000 || { echo "RAIL library is a Git LFS pointer"; exit 1; }

echo "=== slc ==="
slc configuration --sdk="$GSDK_PATH" || true
slc signature trust --sdk "$GSDK_PATH" || true
slc configuration --gcc-toolchain="$(dirname "$(dirname "$(command -v arm-none-eabi-gcc)")")/" || true

rm -rf ./out
slc generate -s "$GSDK_PATH" -p v300_ble_probe.slcp -o makefile -d ./out -cp
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
