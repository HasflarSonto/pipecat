# ESP32-S3 Setup Instructions for Cursor IDE

## Prerequisites

- macOS (you're on darwin 24.5.0)
- Python 3.10+ (for ESP-IDF tools)
- Git
- Cursor IDE installed

## Step 1: Automated Setup

Run the automated setup script:

```bash
cd /Users/antonioli/Desktop/pipecat
./scripts/esp32_setup.sh
```

This will:
- Install ESP-IDF v5.2 (if not already installed)
- Create project structure at `esp32-luna/`
- Configure for ESP32-S3
- Set up Cursor/VS Code configuration files

**Time:** ~10-15 minutes (first time, includes ESP-IDF download)

## Step 2: Install Espressif IDF Extension in Cursor

1. Open Cursor IDE
2. Press `Cmd+Shift+X` to open Extensions
3. Search for "Espressif IDF"
4. Install the official "Espressif IDF" extension by Espressif Systems
5. Reload Cursor if prompted

**Note:** If the extension doesn't appear or has issues:
- You can still use command-line tools (`idf.py`) which work identically
- The extension mainly provides UI for menuconfig, build buttons, etc.

## Step 3: Configure Cursor for ESP-IDF

1. Open the `esp32-luna` folder in Cursor:
   ```bash
   cd /Users/antonioli/Desktop/pipecat/esp32-luna
   cursor .
   ```

2. Set ESP-IDF path (if extension is installed):
   - Press `Cmd+Shift+P`
   - Type "ESP-IDF: Configure ESP-IDF extension"
   - Enter path: `$HOME/esp/esp-idf`
   - Select ESP32-S3 as target

3. Set target chip:
   - Press `Cmd+Shift+P`
   - Type "ESP-IDF: Set Espressif device target"
   - Select "esp32s3"

## Step 4: Source ESP-IDF Environment

Before building, you need to source the ESP-IDF environment. You have two options:

### Option A: Use the project's setup script (Recommended)
```bash
cd esp32-luna
source setup_env.sh
```

### Option B: Use ESP-IDF's export script
```bash
. $HOME/esp/esp-idf/export.sh
```

**Important:** You need to do this in every new terminal session, or add it to your `~/.zshrc`:

```bash
echo 'alias get_idf=". $HOME/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

Then you can just run `get_idf` in any terminal.

## Step 5: Initial Build

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

## Step 6: Connect Your ESP32-S3 Board

1. Connect your ESP32-S3-Touch-AMOLED-2.06 board via USB-C
2. Find the serial port:
   ```bash
   ls /dev/cu.usbserial-*
   # or
   ls /dev/tty.usbserial-*
   ```
   
   Common names:
   - `/dev/cu.usbserial-0001`
   - `/dev/cu.SLAB_USBtoUART`
   - `/dev/cu.wchusbserial*`

3. Flash the firmware:
   ```bash
   idf.py -p /dev/cu.usbserial-0001 flash
   ```
   (Replace with your actual port)

4. Monitor serial output:
   ```bash
   idf.py -p /dev/cu.usbserial-0001 monitor
   ```
   
   Press `Ctrl+]` to exit monitor.

## Step 7: Using Cursor IDE Features

### Build Tasks

You can use Cursor's task runner:

1. Press `Cmd+Shift+B` to build
2. Or use Command Palette (`Cmd+Shift+P`):
   - "Tasks: Run Task" → "Build"
   - "Tasks: Run Task" → "Build, Flash and Monitor"

### Menuconfig

Configure ESP-IDF options:

1. Command Palette → "ESP-IDF: SDK Configuration editor (menuconfig)"
2. Or terminal: `idf.py menuconfig`

### Serial Monitor

1. Command Palette → "ESP-IDF: Monitor your device"
2. Or terminal: `idf.py monitor`

## Troubleshooting

### "idf.py: command not found"
- You need to source the ESP-IDF environment first
- Run: `source setup_env.sh` or `. $HOME/esp/esp-idf/export.sh`

### "Permission denied" on serial port
```bash
sudo chmod 666 /dev/cu.usbserial-*
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
- Check Device Manager (Windows) or System Information (macOS)
- Try different USB ports
- Install USB-to-Serial drivers if needed (CP2102, CH340, etc.)

## Next Steps

1. ✅ Hardware setup complete
2. 🔄 Implement display driver (see `components/display/`)
3. 🔄 Implement audio codec (see `components/audio/`)
4. 🔄 Implement WebRTC client (see `components/webrtc_client/`)
5. 🔄 Port Luna face rendering logic

See `ESP32_MIGRATION_GUIDE.md` for migration strategy.

