#!/bin/sh
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -DBATT_HOST -I.. -o batt_host_test batt_host_test.c ../ble_batt.c
./batt_host_test
