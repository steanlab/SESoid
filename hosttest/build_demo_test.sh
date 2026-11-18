#!/bin/sh
# Host test of the demo pictures. Optional argument: a folder to write the pictures to (demoN.idx, 720x256 bytes of colour codes).
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O1 -Wall -Wextra -I.. -o demo_host_test demo_host_test.c ../ble_demo.c ../ble_font7x14.c
./demo_host_test "$@"
