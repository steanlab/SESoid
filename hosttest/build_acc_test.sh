#!/bin/sh
# Host test of ble_acc.c (needs gcc or clang). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -I.. -o acc_host_test acc_host_test.c ../ble_acc.c
./acc_host_test
