# Flashing Guide

This guide walks you through flashing the Satoshi Pet firmware to a Heltec WiFi Kit 32 V3 board.

## Prerequisites

### Option 1: Arduino IDE (Recommended for beginners)

1. **Download Arduino IDE 2.x**
   - [arduino.cc/en/software](https://www.arduino.cc/en/software)

2. **Add Heltec Board Support**
   - Open Arduino IDE
   - Go to `File → Preferences`
   - In "Additional Board Manager URLs", add:
     ```
     https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/0.0.9/package_heltec_esp32_index.json
     ```
   - Click OK

3. **Install Heltec ESP32 Board Package**
   - Go to `Tools → Board → Boards Manager`
   - Search for "Heltec"
   - Install "Heltec ESP32 Series Dev-Boards"

4. **Install Required Libraries**
   - Go to `Tools → Manage Libraries`
   - Install these libraries:
     - **WiFiManager** by tzapu (version 2.0.x)
     - **ArduinoJson** by Benoit Blanchon (version 7.x)

### Option 2: PlatformIO (Recommended for developers)

1. Install [VS Code](https://code.visualstudio.com/)
2. Install [PlatformIO Extension](https://platformio.org/install/ide?install=vscode)
3. Libraries will be installed automatically from `platformio.ini`

## Flashing Steps

### Step 1: Connect Your Board

1. Connect the Heltec WiFi Kit 32 V3 to your computer via USB-C
2. Wait for drivers to install (usually automatic)

### Step 2: Select Board and Port

**Arduino IDE:**
- `Tools → Board → Heltec ESP32 Series Dev-Boards → WiFi Kit 32(V3)`
- `Tools → Port → Select your port`
  - Mac: `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*`
  - Windows: `COM3`, `COM4`, etc.
  - Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`

**PlatformIO:**
- The board is auto-detected, just click the Upload button

### Step 3: Open the Firmware

1. Open the `firmware/satoshi_pet_heltec.ino` file in Arduino IDE
2. All the `.cpp` and `.h` files in the same folder will be included automatically

### Step 4: Upload

**Arduino IDE:**
- Click the Upload button (→ arrow icon)
- Wait for compilation and upload (~30-60 seconds)
- You should see "Done uploading" when complete

**PlatformIO:**
- Click the Upload button in the bottom toolbar
- Or run `pio run --target upload`

### Step 5: Verify

1. Open Serial Monitor (`Tools → Serial Monitor`)
2. Set baud rate to `115200`
3. You should see boot messages and "Setup complete!"

## Troubleshooting

### "Port not found" or "No device detected"

1. Try a different USB cable (some cables are charge-only)
2. Try a different USB port
3. Install CH340 drivers manually:
   - [Windows Driver](http://www.wch.cn/download/CH341SER_EXE.html)
   - [Mac Driver](http://www.wch.cn/download/CH341SER_MAC_ZIP.html)

### "Upload failed" or "Timed out waiting for packet header"

1. Hold the `PRG` button on the board while clicking Upload
2. Release the button after you see "Connecting..."
3. Some boards require this manual boot mode entry

### "Board not supported" error

Make sure you installed the Heltec ESP32 board package, NOT the generic ESP32 package.

### Compilation errors

1. Make sure all files are in the same folder as `satoshi_pet_heltec.ino`
2. Make sure you installed both required libraries (WiFiManager, ArduinoJson)
3. Try `Sketch → Verify/Compile` first to see detailed errors

## First Boot

After successful flashing:

1. The board will display a splash screen
2. Then it will create a WiFi hotspot called `SatoshiPet-Setup`
3. Connect to this WiFi from your phone/laptop
4. A captive portal will open - enter your home WiFi credentials
5. The device will restart and display a 6-character pairing code

See the main README for pairing instructions.

## Updating Firmware

To update an already-configured device:

1. The device will remember WiFi credentials after update
2. Just flash the new firmware using the same steps above
3. Your pairing will be preserved (stored in flash memory)

## Factory Reset

To completely reset the device:

1. Hold the external button for 10+ seconds
2. The device will prompt "Factory Reset?"
3. Select "Yes" to confirm
4. All settings (WiFi, pairing) will be erased
