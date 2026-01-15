# ESP32 Build & Flash Commands

## Quick Reference

### Prerequisites
Always source ESP-IDF environment first:
```bash
source $HOME/esp/esp-idf/export.sh
```

### Basic Commands

**1. Navigate to project:**
```bash
cd /path/to/your/esp32/project
```

**2. Set target (first time only, or if switching chips):**
```bash
idf.py set-target esp32s3
```

**3. Build:**
```bash
idf.py build
```

**4. Flash (replace PORT with your device):**
```bash
idf.py -p PORT flash
```

**5. Monitor (view serial output):**
```bash
idf.py -p PORT monitor
```

**6. Build + Flash + Monitor (all-in-one):**
```bash
idf.py -p PORT flash monitor
```

---

## Your Specific Setup

### Port
Your ESP32-S3 is on: `/dev/cu.usbmodem1101`

### Complete Workflow

```bash
# 1. Source ESP-IDF environment
source $HOME/esp/esp-idf/export.sh

# 2. Navigate to project
cd /Users/antonioli/Desktop/pipecat/esp32-luna
# OR for examples:
cd /Users/antonioli/Desktop/pipecat/ESP-IDF-v5.4.2/05_Spec_Analyzer

# 3. Set target (if not already set)
idf.py set-target esp32s3

# 4. Build
idf.py build

# 5. Flash
idf.py -p /dev/cu.usbmodem1101 flash

# 6. Monitor
idf.py -p /dev/cu.usbmodem1101 monitor
```

### One-Liner (Build + Flash + Monitor)
```bash
source $HOME/esp/esp-idf/export.sh && \
cd /path/to/project && \
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

---

## Finding Your Port

If your port changes, find it with:
```bash
# macOS
ls /dev/cu.usbmodem* /dev/cu.SLAB* /dev/tty.usbserial* 2>/dev/null

# Or check all USB devices
ls /dev/cu.* | grep -i usb
```

Common port names:
- `/dev/cu.usbmodem1101` (your current port)
- `/dev/cu.usbserial-0001`
- `/dev/cu.SLAB_USBtoUART`
- `/dev/cu.wchusbserial*`

---

## Useful Commands

### Clean Build
```bash
idf.py fullclean
idf.py build
```

### Menuconfig (Configuration)
```bash
idf.py menuconfig
```

### Check Project Size
```bash
idf.py size
```

### Flash Only (No Build)
```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

### Monitor Only (No Flash)
```bash
idf.py -p /dev/cu.usbmodem1101 monitor
```

### Erase Flash
```bash
idf.py -p /dev/cu.usbmodem1101 erase-flash
```

### Flash with Specific Baud Rate
```bash
idf.py -p /dev/cu.usbmodem1101 -b 921600 flash
```

---

## Troubleshooting

### Port Permission Denied
```bash
sudo chmod 666 /dev/cu.usbmodem1101
```

### Port Not Found
- Unplug and replug USB cable
- Try different USB port
- Check System Information (macOS) → USB

### Flash Fails
1. Hold BOOT button
2. Press and release RESET button
3. Release BOOT button
4. Try flashing again

### Monitor Shows Nothing
- Make sure device is powered on
- Check baud rate (usually 115200 or 2000000)
- Try resetting the device

### Build Fails
```bash
# Clean and rebuild
idf.py fullclean
idf.py build
```

---

## Creating a Helper Script

Create `flash.sh` in your project:
```bash
#!/bin/bash
cd "$(dirname "$0")"
source $HOME/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

Make it executable:
```bash
chmod +x flash.sh
```

Then just run:
```bash
./flash.sh
```

---

## Example: Complete Session

```bash
# Start fresh terminal session
cd /Users/antonioli/Desktop/pipecat/esp32-luna

# Source ESP-IDF
source $HOME/esp/esp-idf/export.sh

# Set target
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/cu.usbmodem1101 flash monitor

# Press Ctrl+] to exit monitor
```

---

## For Your ESP32-Luna Project

Once you're working on `esp32-luna/`:

```bash
cd /Users/antonioli/Desktop/pipecat/esp32-luna
source $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

