# Bill of Materials (BOM)

Complete parts list for building your own Satoshi Pet.

## Core Components

### 1. Heltec WiFi Kit 32 V3

**The main board - ESP32-S3 with built-in OLED display**

| Spec | Value |
|------|-------|
| MCU | ESP32-S3 |
| Display | 0.96" 128x64 OLED (SSD1306) |
| WiFi | 2.4GHz 802.11 b/g/n |
| USB | USB-C (programming & charging) |
| Battery | Built-in LiPo charger (JST-PH 2.0) |

**Where to Buy:**
- [AliExpress - Official Heltec Store](https://www.aliexpress.com/item/1005006439587092.html) - ~$15
- [Amazon US](https://www.amazon.com/dp/B0BKVKVQ4S) - ~$20
- [Amazon UK](https://www.amazon.co.uk/dp/B0BKVKVQ4S) - ~£18

**⚠️ Important:** Make sure to get the **V3** version (ESP32-S3), not V2 or V1.

---

### 2. LiPo Battery

**3.7V Lithium Polymer battery with JST-PH 2.0mm connector**

| Spec | Recommended |
|------|-------------|
| Voltage | 3.7V (single cell) |
| Capacity | 1000-2000mAh |
| Connector | JST-PH 2.0mm (2-pin) |
| Size | ~50x30x8mm or smaller |

**Where to Buy:**
- Search "3.7V LiPo JST-PH 2.0" on Amazon/AliExpress
- [Adafruit 2000mAh](https://www.adafruit.com/product/2011) - $12
- [SparkFun 1000mAh](https://www.sparkfun.com/products/13813) - $10

**Battery Life Estimates:**
| Capacity | Active Use | Sleep Mode |
|----------|------------|------------|
| 1000mAh | ~8 hours | ~3 days |
| 2000mAh | ~16 hours | ~7 days |

---

### 3. Piezo Buzzer

**5V passive piezo buzzer for sound effects**

| Spec | Value |
|------|-------|
| Type | Passive (tone-driven) |
| Voltage | 3.3V-5V |
| Size | ~12mm diameter |

**Where to Buy:**
- Amazon: Search "passive piezo buzzer 5V" - pack of 10 for ~$5
- [Adafruit](https://www.adafruit.com/product/160) - $1.50

**Note:** Get a **passive** buzzer, not active. Passive buzzers can play different tones, active buzzers only beep.

---

## Optional Components

### 4. External Button (Optional)

**Additional tactile button for easier pressing**

The Heltec board has a built-in PRG button (GPIO 0), but you can add an external button for a better case design.

| Spec | Value |
|------|-------|
| Type | Tactile momentary switch |
| Size | 6mm x 6mm |
| Connection | Normally open (NO) |

**Where to Buy:**
- Amazon: Pack of 100 for ~$5
- Any electronics store

**Wiring:** Connect between GPIO 2 and GND (pull-up is internal).

---

### 5. 3D Printed Case

**Enclosure to protect and display your Satoshi Pet**

| Spec | Recommended |
|------|-------------|
| Material | PLA or PETG |
| Layer Height | 0.2mm |
| Infill | 15-20% |
| Supports | May be needed for button cutout |

**Options:**
1. **Print yourself** - STL files in `hardware/3d-models/`
2. **Use a service** - Upload to JLCPCB, Shapeways, or local maker space
3. **Skip it** - The bare board works fine for testing!

---

## Tools Needed

| Tool | Purpose |
|------|---------|
| Soldering iron | Connect buzzer wires |
| Solder | For buzzer connection |
| Wire (22-26 AWG) | Connect buzzer to board |
| USB-C cable | Programming and charging |
| Computer | For flashing firmware |

---

## Total Cost Estimate

| Item | Low | High |
|------|-----|------|
| Heltec WiFi Kit 32 V3 | $15 | $25 |
| LiPo Battery | $5 | $15 |
| Piezo Buzzer | $0.50 | $2 |
| External Button | $0 | $0.50 |
| 3D Printed Case | $2 | $10 |
| **Total** | **~$23** | **~$53** |

Most builders spend around **$25-35** for a complete kit.

---

## Sourcing Tips

1. **AliExpress** - Cheapest, but 2-4 week shipping
2. **Amazon** - Fast shipping, slightly more expensive
3. **Adafruit/SparkFun** - Best quality, good documentation
4. **Local electronics store** - Immediate, but limited selection

## Alternative Boards

The firmware is designed for Heltec WiFi Kit 32 V3, but could be adapted for:

| Board | Compatibility | Notes |
|-------|--------------|-------|
| Heltec WiFi Kit 32 V2 | ⚠️ Partial | Different pin mapping |
| LILYGO T-Display S3 | ⚠️ Partial | Different display library |
| Generic ESP32 + SSD1306 | ⚠️ Partial | Manual wiring needed |

For best results, use the recommended Heltec WiFi Kit 32 V3.
