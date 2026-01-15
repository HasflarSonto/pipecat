# Spec_Analyzer Build Instructions

## ✅ Successfully Built and Flashed!

The project has been successfully built and flashed to your ESP32-S3 board.

## Quick Commands

### Build
```bash
cd /Users/antonioli/Desktop/pipecat/ESP-IDF-v5.4.2/05_Spec_Analyzer
source $HOME/esp/esp-idf/export.sh
idf.py build
```

### Flash
```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

### Monitor (View Serial Output)
```bash
idf.py -p /dev/cu.usbmodem1101 monitor
# Or use the helper script:
./monitor.sh
```

Press `Ctrl+]` to exit the monitor.

### Build, Flash, and Monitor (All-in-One)
```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

## What This Example Does

The Spec_Analyzer example:
- Captures audio from the ES8311 microphone
- Performs FFT (Fast Fourier Transform) analysis
- Displays a real-time audio spectrum analyzer on the AMOLED display
- Uses LVGL for the graphical interface

## Port Information

Your ESP32-S3 is connected to: `/dev/cu.usbmodem1101`

If the port changes, you can find it with:
```bash
ls /dev/cu.usbmodem* /dev/cu.SLAB* /dev/tty.usbserial* 2>/dev/null
```

## Troubleshooting

**Port not found:**
- Unplug and replug the USB cable
- Check Device Manager (Windows) or System Information (macOS)

**Flash fails:**
- Hold BOOT button, press RESET, release BOOT
- Try flashing again

**Monitor shows nothing:**
- Make sure the device is powered on
- Check baud rate (should be 115200 or 2000000)

## Next Steps

Now that you have a working example, you can:
1. Study the code in `main/main.c`
2. Modify the display or audio processing
3. Use this as a base for your Pipecat integration

