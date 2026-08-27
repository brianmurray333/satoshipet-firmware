<h1 align="center">🐱⚡ Satoshi Pet Firmware</h1>

<p align="center">
  <strong>A cute hardware companion that reacts to your Bitcoin activity</strong>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#hardware">Hardware</a> •
  <a href="#build-your-own">Build Your Own</a> •
  <a href="#flashing">Flashing</a> •
  <a href="#connecting">Connecting</a> •
  <a href="#license">License</a>
</p>

---

## What is Satoshi Pet?

Satoshi Pet is an open-source hardware device that makes Bitcoin tangible and fun. It's a virtual pet that lives on a small OLED screen and reacts to your Bitcoin balance:

- 🎉 **Celebrates** when you earn sats
- 🍕 **Needs feeding** with coins you earn
- 🎮 **Plays games** to stay happy
- 💤 **Sleeps** to save battery
- 📱 **Syncs** with your [Ganamos](https://ganamos.earth) wallet

Perfect for teaching kids about Bitcoin through interactive play, or just having a fun desk companion that shows your stack!

## Features

- **Real-time Balance Updates** - Your pet knows when you earn or spend sats
- **Virtual Economy** - Earn coins by earning Bitcoin, spend them on pet care
- **Mini Games** - Lightning-themed games to keep your pet happy
- **Job Notifications** - Get pinged when new jobs are posted in your groups
- **Battery Powered** - USB-C rechargeable, lasts for days
- **WiFi Connected** - Syncs automatically with your Ganamos account
- **Open Source** - Build your own from off-the-shelf parts!

## Hardware

### Bill of Materials (BOM)

| Part | Specification | Est. Cost | Where to Buy |
|------|---------------|-----------|--------------|
| **Heltec WiFi Kit 32 V3** | ESP32-S3 with 0.96" OLED | $15-20 | [AliExpress](https://www.aliexpress.com/item/1005006439587092.html), [Amazon](https://www.amazon.com/dp/B0BKVKVQ4S) |
| **LiPo Battery** | 3.7V, 1000-2000mAh, JST-PH 2.0 connector | $5-10 | Amazon, AliExpress |
| **Tactile Button** | 6mm through-hole (optional, for external button) | $0.50 | Any electronics store |
| **Piezo Buzzer** | 5V passive buzzer | $1 | Amazon, AliExpress |
| **3D Printed Case** | PLA filament | $2-5 | Print yourself or use a service |
| **USB-C Cable** | For charging and flashing | $3 | Anywhere |

**Total estimated cost: $25-40**

### Wiring Diagram

```
Heltec WiFi Kit 32 V3
├── GPIO 0  ← Built-in PRG button (primary button)
├── GPIO 2  ← External button (optional)
├── GPIO 48 → Piezo buzzer positive (+)
├── GND     → Piezo buzzer negative (-)
└── Battery → 3.7V LiPo via JST connector
```

The Heltec board has a built-in OLED display, so no additional wiring needed for the screen!

### 3D Printed Case

STL files for the enclosure are in the [`hardware/3d-models/`](hardware/3d-models/) directory:
- `case-top.stl` - Top shell
- `case-bottom.stl` - Bottom shell with battery compartment
- `button-cap.stl` - Optional button cap

Print settings: PLA, 0.2mm layer height, 20% infill

## Build Your Own

### Prerequisites

1. **Arduino IDE 2.x** or **PlatformIO**
2. **ESP32 Board Support** - Add Heltec ESP32 to your Arduino boards manager
3. **Required Libraries**:
   - `WiFiManager` by tzapu
   - `ArduinoJson` by Benoit Blanchon
   - `Heltec ESP32 Dev-Boards` board package

### Arduino IDE Setup

1. **Add Heltec Board Manager URL:**
   ```
   File → Preferences → Additional Board Manager URLs:
   https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/releases/download/0.0.9/package_heltec_esp32_index.json
   ```

2. **Install Board Package:**
   ```
   Tools → Board → Boards Manager → Search "Heltec" → Install
   ```

3. **Select Board:**
   ```
   Tools → Board → Heltec ESP32 Series Dev-Boards → WiFi Kit 32(V3)
   ```

4. **Install Libraries:**
   ```
   Tools → Manage Libraries → Search and install:
   - WiFiManager
   - ArduinoJson
   ```

## Flashing

1. Connect your Heltec board via USB-C

2. Open `firmware/satoshi_pet_heltec.ino` in Arduino IDE

3. Select the correct port:
   ```
   Tools → Port → /dev/cu.usbserial-* (Mac) or COM* (Windows)
   ```

4. Click **Upload** (→ button)

5. Wait for flashing to complete (~30 seconds)

## Connecting

### First-Time Setup

1. **Power on** your freshly flashed device
2. **Connect to WiFi**: The device creates a hotspot called `SatoshiPet-Setup`
   - Connect to this WiFi from your phone/laptop
   - A captive portal opens - enter your home WiFi credentials
3. **Get Pairing Code**: Device displays a 6-character code
4. **Link to Ganamos**:
   - Go to [ganamos.earth/connect-pet](https://ganamos.earth/connect-pet)
   - Create an account if you don't have one
   - Enter the pairing code to link your device

### Reconnecting

If you change WiFi networks:
1. Hold the button for 5+ seconds
2. Device enters WiFi setup mode
3. Connect to `SatoshiPet-Setup` and enter new credentials

## How It Works

```
┌─────────────────┐         ┌─────────────────┐
│  Satoshi Pet    │ ←WiFi→  │   Ganamos.earth │
│  (ESP32 Device) │         │   (Backend)     │
└────────┬────────┘         └────────┬────────┘
         │                           │
         │  1. Fetch config          │
         │  ──────────────────────→  │
         │  (balance, coins, jobs)   │
         │  ←──────────────────────  │
         │                           │
         │  2. Spend coins           │
         │  ──────────────────────→  │
         │  (feed, play game)        │
         │  ←──────────────────────  │
         │                           │
         │  3. Submit game score     │
         │  ──────────────────────→  │
         │  (leaderboard update)     │
         │  ←──────────────────────  │
         │                           │
```

The device polls the Ganamos API every 20-30 seconds to:
- Check your current balance
- Receive new coins when you earn Bitcoin
- Get notified of new jobs
- Sync pet care actions

## API Endpoints

The firmware communicates with these public endpoints:

| Endpoint | Purpose |
|----------|---------|
| `GET /api/device/config` | Fetch balance, coins, notifications |
| `POST /api/device/spend-coins` | Spend coins for feeding/games |
| `POST /api/device/game-score` | Submit game high scores |
| `GET /api/device/jobs` | Fetch available jobs |
| `POST /api/device/job-complete` | Mark a job as completed |

Note: Device identity is now sent in HTTP headers (`X-Device-Id`, `X-Pairing-Code`). The `deviceId` query parameter remains temporarily for one release to maintain compatibility with existing devices and backend versions.
## Troubleshooting

### Device won't connect to WiFi
- Hold button 5+ seconds to enter WiFi setup mode
- Make sure you're connecting to 2.4GHz network (not 5GHz)

### Pairing code not accepted
- Make sure you're entering the code exactly as shown
- Try power cycling the device to generate a new code

### Pet shows wrong balance
- Balance syncs every 20-30 seconds
- Check your internet connection
- Try pressing the button to force a refresh

### Screen stays off
- Device enters power-save mode after inactivity
- Press any button to wake up
- Check battery charge level

## Contributing

We welcome contributions! Please:

1. Fork this repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Community

- 🌐 **Website**: [satoshipet.com](https://satoshipet.com)
- 💬 **Discord**: Coming soon
- 🐦 **Twitter**: [@SatoshiPet](https://twitter.com/SatoshiPet)

## License

This project uses dual licensing:

- **Firmware** (`/firmware/*`): [Apache License 2.0](LICENSE) - Free to use, modify, and distribute with attribution
- **Hardware** (`/hardware/*`): [CERN-OHL-P v2](hardware/LICENSE-HARDWARE) - Open hardware license with patent protection

## Disclaimer

Satoshi Pet is a fun educational device. It does NOT store any Bitcoin private keys or have access to your funds. It only displays your Ganamos balance and reacts to changes.

---

<p align="center">
  Made with ❤️ by <a href="https://ganamos.earth">Ganamos</a>
</p>
