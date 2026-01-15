# ESP32-S3 Migration Guide: Pipecat to ESP-IDF

## Answers to Your Questions

### 1. Can I use Cursor IDE instead of VS Code for ESP-IDF?

**Yes, with some considerations:**

✅ **What works:**
- Cursor is based on VS Code, so most VS Code extensions work, including the Espressif IDF plugin
- Terminal commands (`idf.py build`, `idf.py flash`, etc.) work identically
- File editing, syntax highlighting, and IntelliSense work the same
- CMake-based builds are IDE-agnostic

⚠️ **Potential limitations:**
- The Espressif IDF plugin may have some UI features that assume VS Code's exact UI structure
- Some automated setup workflows in the plugin might need manual configuration
- Debugging integration might require additional setup

**Recommendation:** Install the Espressif IDF plugin in Cursor and test. If you encounter issues, you can always use the command line (`idf.py`) which works identically in both IDEs.

### 2. How much can be automated?

**Highly automated:**
- ✅ ESP-IDF installation and environment setup
- ✅ Project structure creation
- ✅ CMake configuration
- ✅ Build/flash/monitor scripts
- ✅ Board-specific pin definitions
- ✅ Basic component scaffolding

**Partially automated:**
- ⚠️ Hardware driver integration (needs testing/tuning)
- ⚠️ WebRTC client implementation (needs ESP32-specific libraries)

**Manual work required:**
- 🔧 Porting Python logic to C/C++
- 🔧 Testing and debugging hardware interactions
- 🔧 Optimizing for ESP32-S3 memory constraints

---

## Quick Start: Automated Setup

Run the setup script for macOS:

```bash
cd /Users/antonioli/Desktop/pipecat
chmod +x scripts/esp32_setup.sh
./scripts/esp32_setup.sh
```

This will:
1. Install ESP-IDF (if not already installed)
2. Set up environment variables
3. Create project structure
4. Configure for ESP32-S3
5. Set up Cursor IDE configuration

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
└── .vscode/              # Cursor/VS Code config
    ├── settings.json
    ├── tasks.json
    └── launch.json
```

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

---

## Connecting to Your Pipecat Server

Your current setup runs:
```bash
python my_bot.py -t webrtc --esp32 --host 192.168.1.100
```

The ESP32 client will connect to this server. The `--esp32` flag enables SDP munging for ESP32 compatibility.

---

## Next Steps

1. Run the setup script
2. Review the generated project structure
3. Test basic display/audio functionality
4. Implement WebRTC client connection
5. Port Luna face rendering logic

See `ESP32_SETUP_INSTRUCTIONS.md` for detailed step-by-step instructions.

