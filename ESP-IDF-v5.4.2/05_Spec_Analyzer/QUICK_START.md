# Quick Start Guide - Spec_Analyzer

## ✅ Setup Complete!

Your ESP-IDF v5.4.2 environment is now fully configured.

## Monitor Serial Output

To view the serial output from your ESP32-S3:

```bash
cd /Users/antonioli/Desktop/pipecat/ESP-IDF-v5.4.2/05_Spec_Analyzer
source $HOME/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 monitor
```

**Or use the helper script:**
```bash
./monitor.sh
```

Press `Ctrl+]` to exit the monitor.

## Common Commands

### Build
```bash
source $HOME/esp/esp-idf/export.sh
idf.py build
```

### Flash
```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

### Build + Flash + Monitor (All-in-One)
```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

### Clean Build
```bash
idf.py fullclean
idf.py build
```

## Important: Always Source ESP-IDF First!

**Every time you open a new terminal**, you need to source the ESP-IDF environment:

```bash
source $HOME/esp/esp-idf/export.sh
```

Or add this to your `~/.zshrc` to make it automatic:
```bash
alias get_idf=". $HOME/esp/esp-idf/export.sh"
```

Then just run `get_idf` in any terminal.

## Port Information

Your ESP32-S3 is on: `/dev/cu.usbmodem1101`

If the port changes, find it with:
```bash
ls /dev/cu.usbmodem* /dev/cu.SLAB* /dev/tty.usbserial* 2>/dev/null
```

## What You Should See

When you run the monitor, you should see:
- ESP32-S3 boot messages
- Audio FFT initialization
- Display initialization
- Real-time audio spectrum data

The display should show a live audio spectrum analyzer visualization.

