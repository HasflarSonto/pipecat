#!/bin/bash
# Quick flash script for ESP32-Luna
cd "$(dirname "$0")"
source $HOME/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash monitor
