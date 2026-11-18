#!/bin/sh
# Host test of ble_slide.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -DBATT_HOST -Wall -Wextra -Islide_stubs -I.. -o slide_host_test slide_host_test.c ../ble_slide.c ../ble_batt.c
./slide_host_test
