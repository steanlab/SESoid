#!/bin/sh
# Host test of the panel state machine (needs g++ or clang++). Run from the folder that contains this script.
set -e
cd "$(dirname "$0")"
${CXX:-g++} -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-function -fpermissive -Ipanel_stubs -I.. -o panel_host_test panel_host_test.cpp
./panel_host_test
