#!/bin/sh
# Host test of ble_nfc.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -Ihwtest_stubs -I.. -o nfc_host_test nfc_host_test.c ../ble_nfc.c
./nfc_host_test
