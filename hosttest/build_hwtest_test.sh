#!/bin/sh
# Host test of ble_hwtest.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -Ihwtest_stubs -I.. -o hwtest_host_test hwtest_host_test.c ../ble_hwtest.c -lm
./hwtest_host_test
