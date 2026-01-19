# Setup Guide

Complete guide to setting up your Satoshi Pet from scratch.

## Part 1: Hardware Assembly

### Required Parts

| Part | Notes |
|------|-------|
| Heltec WiFi Kit 32 V3 | Main board with built-in OLED |
| 3.7V LiPo Battery | 1000-2000mAh recommended |
| Piezo Buzzer | 5V passive buzzer for sounds |
| 3D Printed Case | Optional but recommended |

### Wiring

```
Heltec Board          Component
────────────         ──────────
GPIO 48  ──────────► Buzzer (+)
GND      ──────────► Buzzer (-)
Battery Connector ◄── LiPo Battery (JST-PH 2.0)
```

**Note:** The Heltec board has a built-in battery connector and charging circuit. Just plug in the battery!

### Assembly

1. **Test before assembly** - Flash the firmware and test everything works
2. **Connect buzzer** - Solder wires to GPIO 48 (+) and GND (-)
3. **Connect battery** - Plug into the JST connector on the board
4. **Place in case** - Secure with screws or friction fit
5. **Done!**

## Part 2: First Boot

### Step 1: Power On

Press the reset button or plug in USB-C. You'll see:

1. **Ganamos splash logo** (2.5 seconds)
2. **"Fix your community / Earn Bitcoin"** tagline (2.5 seconds)
3. **WiFi Setup screen**

### Step 2: Configure WiFi

1. On your phone/laptop, look for WiFi network: `SatoshiPet-Setup`
2. Connect to it (no password needed)
3. A captive portal will automatically open
   - If it doesn't, open a browser and go to `192.168.4.1`
4. Click "Configure WiFi"
5. Select your home WiFi network
6. Enter the password
7. Click "Save"

The device will restart and connect to your WiFi.

### Step 3: Get Pairing Code

Once connected to WiFi, the device displays:

```
┌────────────────────┐
│   Pairing Code:    │
│                    │
│      ABC123        │
│                    │
│ Enter in Ganamos   │
└────────────────────┘
```

Write down this 6-character code.

### Step 4: Create Ganamos Account

1. Go to [ganamos.earth](https://ganamos.earth)
2. Click "Sign Up" and create an account
3. Verify your email

### Step 5: Connect Your Pet

1. Go to [ganamos.earth/connect-pet](https://ganamos.earth/connect-pet)
2. Enter the 6-character pairing code from your device
3. Choose a name for your pet
4. Select a pet type (cat, dog, rabbit, etc.)
5. Click "Connect"

Your device will show "Connected!" and your pet will appear!

## Part 3: Daily Use

### Button Controls

| Action | What Happens |
|--------|--------------|
| **Short press** | Open menu |
| **Hold (3 sec)** | Select menu item / WiFi setup (if disconnected) |
| **Hold (10 sec)** | Factory reset prompt |

### Menu Options

- **Home** - Return to pet view
- **Play** - Play the Lightning Game (costs coins)
- **Feed** - Feed your pet (costs coins)
- **Jobs** - View available jobs

### Earning Coins

Coins are earned automatically when you:
- Receive Bitcoin deposits
- Get paid for completing jobs
- Receive internal transfers

Every sat you earn = coins for your pet!

### Pet Stats

- **Hunger** - Decreases over time, feed to restore
- **Happiness** - Decreases over time, play games to restore
- **Coins** - Your spendable balance for pet care

If both hunger and happiness hit 0, your pet "dies" (can be revived by feeding/playing).

## Part 4: Troubleshooting

### Device won't connect to WiFi

1. Make sure you're using a 2.4GHz network (not 5GHz)
2. Check that the password is correct
3. Try power cycling the device
4. Hold button 5 seconds to re-enter WiFi setup

### Pairing code not accepted

- Codes are case-insensitive (ABC123 = abc123)
- Make sure you're typing it exactly as shown
- If the code changed, use the new code on the device screen
- Power cycle to generate a fresh code

### Pet shows 0 balance but I have Bitcoin

- Balance syncs every 20-30 seconds
- Check your Ganamos account to verify the balance
- Try pressing the button to wake the device and force sync

### Screen is black

1. Press any button to wake from sleep mode
2. Check battery charge (plug in USB-C)
3. If still black, try holding reset button

### Sounds not working

- Check buzzer wiring (GPIO 48 and GND)
- Sounds are muted during "quiet hours" (8pm-8am)

## Part 5: Changing WiFi Networks

If you move or change routers:

1. Hold the button for 5+ seconds (when device is on)
2. Device enters WiFi setup mode
3. Connect to `SatoshiPet-Setup` hotspot
4. Enter new WiFi credentials
5. Device reconnects with same pairing

Your pet and coins are preserved!

## Part 6: Factory Reset

To completely wipe the device:

1. Hold external button for 10+ seconds
2. Screen shows "Factory Reset?"
3. Use short press to toggle between No/Yes
4. Hold to confirm selection
5. If "Yes", device erases all data and restarts

After factory reset:
- WiFi credentials are erased
- Pairing is erased
- A new pairing code is generated
- You'll need to pair again
