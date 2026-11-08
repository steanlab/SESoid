#!/bin/bash
# Build inside the ghcr.io/nabucasa/silabs-firmware-builder image (it brings slc and the ARM compiler).
# The Gecko SDK is taken from the image too (the Git LFS download of the SDK repository fails in CI).
set -e

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
echo "=== looking for a Gecko SDK in the image ==="
find / -xdev -maxdepth 4 \( -name gecko_sdk.slcs -o -name simplicity_sdk.slcs \) 2>/dev/null | while read -r f; do
  echo "  $f: $(grep -m1 sdk_version "$f")"
done
GSDK_PATH=""
for f in $(find / -xdev -maxdepth 4 -name gecko_sdk.slcs 2>/dev/null); do
  d=$(dirname "$f")
  lib="$d/platform/radio/rail_lib/autogen/librail_release/librail_efr32xg22_gcc_release.a"
  sz=$(stat -c %s "$lib" 2>/dev/null || echo 0)
  echo "  candidate $d: $(grep -m1 sdk_version "$f"), RAIL library $sz bytes"
  if [ "$sz" -gt 100000 ] && [ -z "$GSDK_PATH" ]; then GSDK_PATH="$d"; fi
done
test -n "$GSDK_PATH" || { echo "no usable Gecko SDK (with a real RAIL library) found in the image"; ls /; exit 1; }
SDKVER=$(grep -m1 sdk_version "$GSDK_PATH/gecko_sdk.slcs" | sed 's/[^0-9.]//g')
echo "using $GSDK_PATH, version $SDKVER"
# make the project ask for the version that is really there
sed -i "s/^sdk: {id: gecko_sdk, version: .*}/sdk: {id: gecko_sdk, version: $SDKVER}/" v300_ble_probe.slcp
grep '^sdk:' v300_ble_probe.slcp

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
