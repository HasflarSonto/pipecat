# Touch Display Project

Raspberry Pi implementation for ILI9341 2.4" SPI TFT display with ADS7846 resistive touch controller.

## Quick Start

1. **Hardware Setup**
   - Connect display to SPI0 (see `display.md` for pinout)
   - Connect touch controller to SPI1 (see `displaytouch.md` for pinout)

2. **Software Setup**
   ```bash
   # Copy config.txt to /boot/firmware/config.txt
   sudo cp config.txt /boot/firmware/config.txt
   
   # Build and install touch overlay
   ./scripts/rebuild_touch_overlay.sh
   
   # Reboot
   sudo reboot
   ```

3. **Calibrate Touch**
   ```bash
   python3 calibrate_touch.py
   ```

4. **Run Calculator Demo**
   ```bash
   python3 calculator_touch.py
   ```

## Documentation

- **[display.md](display.md)** - Display setup and configuration
- **[displaytouch.md](displaytouch.md)** - Touch controller setup and troubleshooting
- **[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** - Complete guide for building applications
- **[docs/](docs/)** - Additional documentation and manuals

## Project Structure

```
.
├── calculator_touch.py      # Main calculator application (working example)
├── calibrate_touch.py       # Interactive touch calibration tool
├── config.txt               # Raspberry Pi boot configuration
├── ads7846-spil.dts         # Device tree overlay source for touch
├── display.md               # Display setup documentation
├── displaytouch.md          # Touch setup documentation
├── DEVELOPER_GUIDE.md       # Developer guide for building apps
├── scripts/                 # Utility scripts
│   ├── rebuild_touch_overlay.sh
│   ├── adjust_cal_for_rotation.py
│   └── fix_calibration.py
├── tests/                   # Test and diagnostic scripts
│   ├── test_interrupt.sh
│   ├── test_interrupt_edge.sh
│   ├── diagnose_touch.sh
│   └── test_spi_raw.py
└── docs/                    # Additional documentation
    ├── cursor.md
    ├── TOUCH_IMPROVEMENTS.md
    └── 2.4inch_SPI_Module_MSP2402_User_Manual_EN.md
```

## Key Features

- ✅ Direct framebuffer rendering (no X11/SDL required)
- ✅ Event-driven touch input with proper coordinate mapping
- ✅ Interactive calibration system
- ✅ Works over SSH (headless)
- ✅ Production-ready architecture

## Requirements

- Raspberry Pi (tested on 64-bit OS)
- Python 3 with PIL/Pillow and evdev
- ILI9341 display module
- ADS7846/XPT2046 touch controller

## License

See individual files for license information.

