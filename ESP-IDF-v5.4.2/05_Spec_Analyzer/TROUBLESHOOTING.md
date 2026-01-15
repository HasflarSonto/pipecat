# Troubleshooting Guide

## ✅ Good News: Your ESP32 is Working!

The firmware is already running successfully on your ESP32-S3. You can see the spectrum analyzer working!

## Issue: Python Virtual Environment Not Found

If you see this error:
```
ERROR: ESP-IDF Python virtual environment "/Users/antonioli/.espressif/python_env/idf5.4_py3.10_env/bin/python" not found.
```

### Solution: Run the Install Script

The Python environment needs to be created. Run:

```bash
cd $HOME/esp/esp-idf
./install.sh esp32s3
```

This will:
1. Create the Python virtual environment for ESP-IDF v5.4.2
2. Install all required Python packages
3. Set up the toolchain

**Time:** ~2-3 minutes

### After Installation

Once the install completes, you should be able to run:

```bash
cd /Users/antonioli/Desktop/pipecat/ESP-IDF-v5.4.2/05_Spec_Analyzer
source $HOME/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 monitor
```

## Why This Happened

When we upgraded ESP-IDF from v5.2 to v5.4.2, it requires a new Python virtual environment. The install script creates this automatically, but it needs to be run once after the upgrade.

## Quick Test

To verify everything is working:

```bash
source $HOME/esp/esp-idf/export.sh
idf.py --version
```

You should see: `ESP-IDF v5.4.2`

## Alternative: Use the Pre-built Binary

Since your firmware is already running, you don't necessarily need to rebuild. But if you want to:
- Monitor serial output
- Flash new firmware
- Debug

You'll need the Python environment set up.

