#!/bin/bash
# Quick monitor script for Spec_Analyzer example

cd "$(dirname "$0")"
source $HOME/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 monitor

