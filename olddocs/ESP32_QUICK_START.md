# ESP32-S3 Quick Start Guide

## Direct Answers to Your Questions

### ✅ Question 1: Can I use Cursor IDE instead of VS Code?

**YES!** Cursor IDE works with ESP-IDF because:
- Cursor is based on VS Code, so extensions work
- The Espressif IDF extension should install and work
- Command-line tools (`idf.py`) work identically
- If the extension has issues, you can use terminal commands

**What I've automated:**
- ✅ Cursor/VS Code configuration files (`.vscode/`)
- ✅ Build tasks (Cmd+Shift+B)
- ✅ Project structure
- ✅ ESP-IDF installation script

### ✅ Question 2: How much can be automated?

**Highly Automated (Done):**
- ✅ ESP-IDF installation
- ✅ Project structure creation
- ✅ CMake configuration
- ✅ Build/flash/monitor scripts
- ✅ Cursor IDE setup
- ✅ Board-specific configuration

**Partially Automated (Templates Created):**
- ⚠️ WebRTC client (skeleton created, needs implementation)
- ⚠️ Display driver (structure ready, needs hardware-specific code)
- ⚠️ Audio codec (structure ready, needs ES8311 driver)

**Manual Work Needed:**
- 🔧 Porting Python logic to C/C++
- 🔧 Testing hardware interactions
- 🔧 WebRTC library integration (ESP-IDF or third-party)

---

## Quick Start (3 Steps)

### Step 1: Run Setup Script
```bash
cd /Users/antonioli/Desktop/pipecat
./scripts/esp32_setup.sh
```

### Step 2: Install Extension in Cursor
1. Open Cursor
2. Cmd+Shift+X → Search "Espressif IDF" → Install
3. Open `esp32-luna` folder in Cursor

### Step 3: Build & Flash
```bash
cd esp32-luna
source setup_env.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

---

## What's Been Created

```
esp32-luna/
├── main/                    # Main application
│   ├── main.c              # Entry point (template)
│   └── CMakeLists.txt
├── components/
│   ├── webrtc_client/      # WebRTC → Pipecat connection
│   ├── display/            # AMOLED display driver
│   ├── audio/              # ES8311 codec driver
│   └── luna_face/         # Face rendering
├── .vscode/               # Cursor/VS Code config
│   ├── settings.json      # ESP-IDF paths
│   ├── tasks.json         # Build/flash tasks
│   └── launch.json        # Debug config
├── CMakeLists.txt         # Root CMake
├── sdkconfig.defaults      # ESP32-S3 defaults
└── setup_env.sh           # Environment setup
```

---

## Connecting to Your Pipecat Server

Your current setup:
```bash
python my_bot.py -t webrtc --esp32 --host 192.168.1.100
```

The ESP32 will connect to this. The `--esp32` flag enables SDP munging automatically.

---

## Next Steps

1. ✅ **Setup complete** - Run the script above
2. 🔄 **Test build** - Verify compilation works
3. 🔄 **Hardware test** - Flash and verify serial output
4. 🔄 **Implement WebRTC** - Connect to Pipecat server
5. 🔄 **Port Luna logic** - Migrate face rendering

---

## Documentation

- **Full setup guide:** `ESP32_SETUP_INSTRUCTIONS.md`
- **Migration strategy:** `ESP32_MIGRATION_GUIDE.md`
- **Board docs:** `ESP32S3Display.md`

---

## Troubleshooting

**"idf.py not found"**
→ Source environment: `source setup_env.sh`

**Extension doesn't work**
→ Use terminal: `idf.py build` works the same

**Can't find serial port**
→ Check: `ls /dev/cu.usbserial-*`

**Build fails**
→ Try: `idf.py fullclean && idf.py build`

