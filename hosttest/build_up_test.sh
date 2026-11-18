#!/bin/sh
# Host test of ble_up.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -Islide_stubs -I.. -o up_host_test up_host_test.c ../ble_up.c
./up_host_test
