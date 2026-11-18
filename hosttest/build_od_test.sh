#!/bin/sh
# Host test of ble_od.c with ble_up.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -Islide_stubs -I.. -o od_host_test od_host_test.c ../ble_od.c ../ble_up.c
./od_host_test
