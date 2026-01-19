# Wiring Guide

Detailed wiring instructions for Satoshi Pet.

## Pin Reference

### Heltec WiFi Kit 32 V3 Pinout

```
                    ┌─────────────┐
                    │   USB-C     │
                    │             │
              3.3V ─┤             ├─ GND
               EN ─┤             ├─ GPIO 35 (RGB LED)
           GPIO 36 ─┤             ├─ GPIO 0 (PRG Button)
           GPIO 26 ─┤             ├─ GPIO 4
           GPIO 33 ─┤             ├─ GPIO 16
           GPIO 32 ─┤             ├─ GPIO 17
           GPIO 35 ─┤             ├─ GPIO 5
           GPIO 34 ─┤             ├─ GPIO 18
               VIN ─┤             ├─ GPIO 19
               GND ─┤             ├─ GPIO 21
           GPIO 15 ─┤             ├─ GPIO 3
            GPIO 2 ─┤             ├─ GPIO 1
            GPIO 0 ─┤             ├─ GPIO 22
            GPIO 4 ─┤             ├─ GPIO 23
                    │   ┌─────┐   │
                    │   │OLED │   │
                    │   │     │   │
                    │   └─────┘   │
                    │             │
                    │  [RST][PRG] │
                    └─────────────┘
                      ↑       ↑
                   Reset   Button
                   Button  (GPIO 0)
```

## Connections

### Required: Piezo Buzzer

```
Heltec Board                    Buzzer
────────────                   ────────
GPIO 48  ───────────────────►  + (positive)
GND      ───────────────────►  - (negative)
```

**Soldering Steps:**
1. Cut two pieces of wire (~5cm each)
2. Strip both ends of each wire
3. Solder one wire to GPIO 48 pad
4. Solder other wire to any GND pad
5. Connect to buzzer (polarity matters for some buzzers)

### Required: Battery

```
Heltec Board                    Battery
────────────                   ────────
JST Connector ◄─────────────►  LiPo 3.7V (JST-PH 2.0)
```

**No soldering needed!** The battery plugs directly into the JST connector on the board.

**⚠️ Warning:** Double-check battery polarity. The connector should only fit one way, but forcing it backwards can damage the board.

### Optional: External Button

```
Heltec Board                    Button
────────────                   ────────
GPIO 2   ───────────────────►  Pin 1
GND      ───────────────────►  Pin 2
```

The internal pull-up resistor is enabled in firmware, so no external resistor needed.

## Schematic

```
                                    ┌─────────────────┐
                                    │                 │
                                    │  Heltec V3      │
                                    │  WiFi Kit 32    │
                                    │                 │
    ┌──────────┐                    │                 │
    │  LiPo    │◄──JST Connector───►│  Battery In     │
    │ Battery  │                    │                 │
    │  3.7V    │                    │                 │
    └──────────┘                    │                 │
                                    │  GPIO 48 ───────┼────► Buzzer (+)
    ┌──────────┐                    │                 │
    │  Button  │────GPIO 2─────────►│  GPIO 2         │
    │ (ext.)   │────GND────────────►│  GND ───────────┼────► Buzzer (-)
    └──────────┘                    │                 │
                                    │                 │
                                    │  Built-in:      │
                                    │  - 128x64 OLED  │
                                    │  - PRG Button   │
                                    │  - RGB LED      │
                                    │  - USB-C        │
                                    │                 │
                                    └─────────────────┘
```

## GPIO Summary

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | PRG Button (built-in) | Primary button, active LOW |
| 2 | External Button | Optional, active LOW with pull-up |
| 48 | Buzzer | PWM output for tones |
| 35 | RGB LED (built-in) | Used for notifications |
| 1 | Battery ADC | Reads battery voltage |
| 37 | ADC Control | Enables battery voltage divider |
| 21 | Vext | Display power control |

## Display (Built-in)

The OLED display is pre-wired on the Heltec board:

| Signal | GPIO |
|--------|------|
| SDA | Fixed (internal) |
| SCL | Fixed (internal) |
| RST | Fixed (internal) |

No external wiring needed for the display!

## Power

### USB-C Charging

- The Heltec board has a built-in LiPo charger
- Plug in USB-C to charge the battery
- Red LED = charging, off = charged

### Battery Life Tips

1. **Larger battery = longer life** (2000mAh recommended)
2. **Display auto-sleeps** after 3 minutes of inactivity
3. **WiFi power-saving** kicks in during sleep mode

## Testing Your Wiring

After assembly, test with Serial Monitor (115200 baud):

```
=== Battery Quick Test ===
Voltage: 4.05 V
==========================
```

If you see reasonable voltage (3.5-4.2V), battery is connected correctly.

For buzzer, the device will play sounds when:
- Button is pressed (chirp)
- New sats arrive (celebration tune)
- Pet needs attention (sad sound)

## Troubleshooting

### No display

- Check that the display ribbon cable is seated properly
- Try pressing the RST button
- Make sure you selected the correct board in Arduino IDE

### No sound

- Check buzzer polarity
- Verify GPIO 48 connection
- Some buzzers are polarized - try swapping + and -

### Battery not charging

- Check JST connector polarity
- Try a different USB-C cable
- The charging circuit may be damaged if polarity was reversed

### Button not working

- Check GPIO 2 connection for external button
- Built-in PRG button should always work
- Serial Monitor shows button presses if wired correctly
