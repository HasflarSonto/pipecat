# ESP32-S3 Complete Guide: Pipecat Migration to ESP-IDF

This comprehensive guide covers everything you need to migrate your Pipecat voice bot from running on your computer to an ESP32-S3 development board.

---

## Table of Contents

1. [Quick Answers](#quick-answers)
2. [Quick Start](#quick-start)
3. [Detailed Setup Instructions](#detailed-setup-instructions)
4. [Migration Strategy](#migration-strategy)
5. [Working with Examples](#working-with-examples)
6. [Troubleshooting](#troubleshooting)
7. [Project Structure](#project-structure)
8. [Next Steps](#next-steps)

---

## Quick Answers

### ✅ Question 1: Can I use Cursor IDE instead of VS Code?

**YES!** Cursor IDE works with ESP-IDF because:
- Cursor is based on VS Code, so most VS Code extensions work, including the Espressif IDF plugin
- Terminal commands (`idf.py build`, `idf.py flash`, etc.) work identically
- File editing, syntax highlighting, and IntelliSense work the same
- CMake-based builds are IDE-agnostic

**Recommendation:** Install the Espressif IDF plugin in Cursor and test. If you encounter issues, you can always use the command line (`idf.py`) which works identically in both IDEs.

### ✅ Question 2: How much can be automated?

**Highly Automated:**
- ✅ ESP-IDF installation and environment setup
- ✅ Project structure creation
- ✅ CMake configuration
- ✅ Build/flash/monitor scripts
- ✅ Board-specific pin definitions
- ✅ Basic component scaffolding

**Partially Automated:**
- ⚠️ Hardware driver integration (needs testing/tuning)
- ⚠️ WebRTC client implementation (needs ESP32-specific libraries)

**Manual Work Required:**
- 🔧 Porting Python logic to C/C++
- 🔧 Testing and debugging hardware interactions
- 🔧 Optimizing for ESP32-S3 memory constraints

---

## Quick Start

### Step 1: Run Setup Script

```bash
cd /Users/antonioli/Desktop/pipecat
chmod +x scripts/esp32_setup.sh
./scripts/esp32_setup.sh
```

This will:
1. Install ESP-IDF v5.4.2 (if not already installed)
2. Set up environment variables
3. Create project structure
4. Configure for ESP32-S3
5. Set up Cursor IDE configuration

**Time:** ~10-15 minutes (first time, includes ESP-IDF download)

### Step 2: Install Espressif IDF Extension in Cursor

1. Open Cursor IDE
2. Press `Cmd+Shift+X` to open Extensions
3. Search for "Espressif IDF"
4. Install the official "Espressif IDF" extension by Espressif Systems
5. Reload Cursor if prompted

### Step 3: Build & Flash

```bash
cd esp32-luna
source setup_env.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

---

## Detailed Setup Instructions

### Prerequisites

- macOS (you're on darwin 24.5.0)
- Python 3.10+ (for ESP-IDF tools)
- Git
- Cursor IDE installed

### Step 1: Automated Setup

Run the automated setup script:

```bash
cd /Users/antonioli/Desktop/pipecat
./scripts/esp32_setup.sh
```

### Step 2: Source ESP-IDF Environment

Before building, you need to source the ESP-IDF environment. You have two options:

**Option A: Use the project's setup script (Recommended)**
```bash
cd esp32-luna
source setup_env.sh
```

**Option B: Use ESP-IDF's export script**
```bash
. $HOME/esp/esp-idf/export.sh
```

**Important:** You need to do this in every new terminal session, or add it to your `~/.zshrc`:

```bash
echo 'alias get_idf=". $HOME/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

Then you can just run `get_idf` in any terminal.

### Step 3: Initial Build

1. Navigate to project:
   ```bash
   cd /Users/antonioli/Desktop/pipecat/esp32-luna
   ```

2. Source environment:
   ```bash
   source setup_env.sh
   ```

3. Set target (first time only):
   ```bash
   idf.py set-target esp32s3
   ```

4. Build:
   ```bash
   idf.py build
   ```

**Expected output:** Build should complete successfully (may take a few minutes the first time).

### Step 4: Connect Your ESP32-S3 Board

1. Connect your ESP32-S3-Touch-AMOLED-2.06 board via USB-C
2. Find the serial port:
   ```bash
   ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/tty.usbserial* 2>/dev/null
   ```
   
   Common names:
   - `/dev/cu.usbmodem1101` (your current port)
   - `/dev/cu.usbserial-0001`
   - `/dev/cu.SLAB_USBtoUART`
   - `/dev/cu.wchusbserial*`

3. Flash the firmware:
   ```bash
   idf.py -p /dev/cu.usbmodem1101 flash
   ```

4. Monitor serial output:
   ```bash
   idf.py -p /dev/cu.usbmodem1101 monitor
   ```
   
   Press `Ctrl+]` to exit monitor.

### Step 5: Using Cursor IDE Features

**Build Tasks:**
- Press `Cmd+Shift+B` to build
- Or use Command Palette (`Cmd+Shift+P`): "Tasks: Run Task" → "Build"

**Menuconfig:**
- Command Palette → "ESP-IDF: SDK Configuration editor (menuconfig)"
- Or terminal: `idf.py menuconfig`

**Serial Monitor:**
- Command Palette → "ESP-IDF: Monitor your device"
- Or terminal: `idf.py monitor`

---

## Migration Strategy

### Phase 1: Hardware Setup ✅ (Automated)
- Display initialization
- Audio codec setup
- Button handling
- Basic I/O

### Phase 2: WebRTC Client 🔄 (Partially Automated)
- Use ESP32 WebRTC libraries (esp_webrtc or similar)
- Connect to your Pipecat server
- Handle SDP munging (already supported in Pipecat with `--esp32` flag)

### Phase 3: Voice Pipeline 🔧 (Manual)
- Port audio capture/playback
- Integrate with Pipecat's WebRTC transport
- Handle audio frames

### Phase 4: Display Integration 🔧 (Manual)
- Port Luna face rendering to C/C++
- Optimize for ESP32-S3 constraints
- Handle emotions and animations

### Connecting to Your Pipecat Server

Your current setup runs:
```bash
python my_bot.py -t webrtc --esp32 --host 192.168.1.100
```

The ESP32 client will connect to this server. The `--esp32` flag enables SDP munging for ESP32 compatibility.

---

## Working with Examples

### Running the Spec_Analyzer Example

We've successfully built and flashed the `05_Spec_Analyzer` example, which demonstrates:
- Audio capture from ES8311 microphone
- FFT (Fast Fourier Transform) analysis
- Real-time audio spectrum visualization on AMOLED display
- LVGL for graphical interface

**Location:** `ESP-IDF-v5.4.2/05_Spec_Analyzer/`

**Quick Commands:**
```bash
cd ESP-IDF-v5.4.2/05_Spec_Analyzer
source $HOME/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

**Helper Script:**
```bash
./monitor.sh
```

### Other Available Examples

The `ESP-IDF-v5.4.2/` directory contains several examples:
- `01_AXP2101` - Power management demo
- `02_lvgl_demo_v9` - LVGL graphics demo
- `03_esp-brookesia` - UI framework demo
- `04_Immersive_block` - Gyroscope demo
- `05_Spec_Analyzer` - Audio spectrum analyzer ✅ (Working!)
- `06_videoplayer` - Video playback from TF card

---

## Troubleshooting

### "idf.py: command not found"
- You need to source the ESP-IDF environment first
- Run: `source setup_env.sh` or `. $HOME/esp/esp-idf/export.sh`

### "ESP-IDF Python virtual environment not found"
If you see:
```
ERROR: ESP-IDF Python virtual environment "/Users/antonioli/.espressif/python_env/idf5.4_py3.10_env/bin/python" not found.
```

**Solution:** Run the install script:
```bash
cd $HOME/esp/esp-idf
./install.sh esp32s3
```

This creates the Python virtual environment for ESP-IDF v5.4.2.

### "Permission denied" on serial port
```bash
sudo chmod 666 /dev/cu.usbmodem*
```

### Build fails with "CMake Error"
- Try: `idf.py fullclean`
- Then: `idf.py build`

### Extension doesn't work in Cursor
- The command-line tools (`idf.py`) work identically
- You can use terminal tasks instead of the extension UI
- Check `.vscode/tasks.json` for available tasks

### Can't find serial port
- Make sure USB cable is connected
- Check System Information (macOS)
- Try different USB ports
- Install USB-to-Serial drivers if needed (CP2102, CH340, etc.)

### Port changed
If your port changes, find it with:
```bash
ls /dev/cu.usbmodem* /dev/cu.SLAB* /dev/tty.usbserial* 2>/dev/null
```

---

## Project Structure

After setup, you'll have:

```
esp32-luna/
├── main/
│   ├── CMakeLists.txt
│   ├── main.c
│   └── Kconfig.projbuild
├── components/
│   ├── display/          # AMOLED display driver
│   ├── audio/            # ES8311 codec driver
│   ├── webrtc_client/    # WebRTC client for Pipecat
│   └── luna_face/        # Face rendering logic
├── CMakeLists.txt
├── sdkconfig.defaults
├── setup_env.sh
└── .vscode/              # Cursor/VS Code config
    ├── settings.json
    ├── tasks.json
    └── launch.json
```

### Example Project Structure

```
ESP-IDF-v5.4.2/
├── 05_Spec_Analyzer/     # Working audio spectrum analyzer
│   ├── main/
│   ├── components/
│   ├── monitor.sh        # Helper script
│   └── README_BUILD.md
└── [other examples]
```

---

## Next Steps

1. ✅ **Setup complete** - ESP-IDF v5.4.2 installed and configured
2. ✅ **Example working** - Spec_Analyzer successfully built and flashed
3. 🔄 **Study examples** - Understand audio/display integration
4. 🔄 **Implement WebRTC** - Connect to Pipecat server
5. 🔄 **Port Luna logic** - Migrate face rendering to C/C++
6. 🔄 **Optimize** - Memory and performance tuning for ESP32-S3

---

## Additional Resources

- **Board Documentation:** `ESP32S3Display.md`
- **ESP-IDF Examples:** `ESP-IDF-v5.4.2/`
- **Setup Script:** `scripts/esp32_setup.sh`
- **ESP-IDF Official Docs:** https://docs.espressif.com/projects/esp-idf/

---

## Quick Reference Commands

```bash
# Source ESP-IDF environment
source $HOME/esp/esp-idf/export.sh

# Build project
idf.py build

# Flash firmware
idf.py -p /dev/cu.usbmodem1101 flash

# Monitor serial output
idf.py -p /dev/cu.usbmodem1101 monitor

# Build, flash, and monitor (all-in-one)
idf.py -p /dev/cu.usbmodem1101 flash monitor

# Clean build
idf.py fullclean

# Menuconfig
idf.py menuconfig

# Set target
idf.py set-target esp32s3
```

---

**Last Updated:** January 2025  
**ESP-IDF Version:** v5.4.2  
**Target:** ESP32-S3  
**Board:** ESP32-S3-Touch-AMOLED-2.06

