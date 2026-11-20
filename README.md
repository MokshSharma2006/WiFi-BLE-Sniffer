# RF Sniffer — ESP32-C3 WiFi/BLE Passive Monitoring Tool

A handheld WiFi + BLE passive reconnaissance tool built on the ESP32-C3 Mini, using its native radio for WiFi monitoring and an NRF24L01+ for BLE advertisement sniffing, with an OLED display and joystick for on-device navigation.

**By Moksh Sharma**

> ⚠️ **For authorized security research only.** Use this only on networks and devices you own or have explicit permission to test. This tool is strictly **passive** — it never transmits deauthentication frames, injects packets, or jams anything. It observes and reports; it does not disrupt.

---

## Features

### WiFi Scan
- Channel-hopping (1–13) promiscuous-mode scan of nearby access points
- Per-AP: SSID, RSSI, channel, **encryption type** (Open / WEP / WPA / WPA2 / WPA3), **WPS flag**, and a **weak-network marker**
- Select an AP to open its submenu:
  - **Graph** — live RSSI plot, locked onto that AP's channel for continuous coverage
  - **Deauth Monitor** — passively counts deauth/disassoc frames targeting that AP (detection only — useful for noticing if someone *else* is attacking the network)
  - **Clients** — MACs seen associated with that AP, with a best-effort vendor guess

### Probe Log
Logs probe-request frames from nearby devices (MAC + the SSID they're looking for) — useful for understanding what's broadcasting nearby.

### Export Report
Dumps everything collected (APs, encryption/WPS/weak flags, per-AP clients, probe log) as a plain-text report over Serial (115200 baud) — paste straight into an assessment write-up.

### BLE Sniff
Uses the NRF24L01+ in a promiscuous mode hack to listen on BLE's three advertising channels (37/38/39), software-dewhitening and parsing what comes in to show advertiser MAC, packet count, and device name when available.

### Joystick Debug
A live raw-ADC readout screen for diagnosing joystick wiring/orientation issues.

---

## Hardware Required

| Component | Notes |
|---|---|
| ESP32-C3 Mini / SuperMini | Native WiFi + BLE radio |
| NRF24L01+ module | For BLE advertisement sniffing |
| 1.3" OLED (SH1106, I2C, 128×64) | Swap library if yours is SSD1306 |
| Analog joystick module (X/Y + press switch) | |

## Wiring

**OLED (I2C)**

| OLED pin | ESP32-C3 pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

**NRF24L01+**

| NRF24 pin | ESP32-C3 pin |
|---|---|
| VCC | **3V3 only** — 5V will damage the module |
| GND | GND |
| CE | GPIO2 |
| CSN | GPIO3 |
| SCK | GPIO4 |
| MOSI | GPIO6 |
| MISO | GPIO5 |
| IRQ | not connected |

**Joystick**

| Joystick pin | ESP32-C3 pin |
|---|---|
| VCC | 3V3 (not 5V — see note below) |
| GND | GND |
| VRx | GPIO0 (ADC) |
| VRy | GPIO1 (ADC) |
| SW | GPIO10 |

> The joystick module is commonly labeled "5V" but is just a voltage-divider potentiometer — power it at 3.3V to match the ESP32-C3's ADC range and avoid exceeding its non-5V-tolerant GPIO limits.

> If your NRF24L01+ brownout-resets or fails to initialize, add a 10–100µF capacitor across its VCC/GND — it's notorious for power-supply glitches on long jumper wires.

---

## Libraries Required

Install via Arduino Library Manager:

- **RF24** by TMRh20
- **Adafruit SH110X** (swap for **Adafruit_SSD1306** if your OLED uses that driver instead)
- **Adafruit GFX Library**

## Board Setup

In Arduino IDE: select **"ESP32C3 Dev Module"** (or your board's specific entry) under Tools → Board.

## Flashing

1. Open `wifi_ble_sniffer.ino` in Arduino IDE
2. Install the libraries above
3. Select your board and port
4. Upload

---

## Controls

Navigation is fully axis-independent of joystick mounting orientation — if your module is rotated, flip `JOY_SWAP_XY` / `JOY_INVERT_UPDOWN` near the top of the file (see **Joystick Debug** screen for live raw values to diagnose this).

| Input | Action |
|---|---|
| Up / Down | Move cursor / scroll |
| Single tap (SW) | Select / enter |
| Double tap (SW), within ~350ms | Go back one level |

---

## Configuration

All user-editable settings are in the `USER CONFIG` block at the top of the `.ino` file:

```cpp
#define OLED_SDA, OLED_SCL, OLED_ADDR   // OLED wiring/address
#define JOY_VRX, JOY_VRY, JOY_SW        // Joystick pins
#define JOY_DEADZONE, JOY_REPEAT_MS     // Joystick feel tuning
#define DOUBLE_TAP_MS                   // Back-gesture timing window
#define JOY_SWAP_XY, JOY_INVERT_UPDOWN  // Fix a rotated/mirrored joystick
#define NRF_CE, NRF_CSN, NRF_SCK, ...   // NRF24L01 pins
```

---

## Honesty Note on BLE Sniffing

BLE sniffing via the NRF24L01+ is a well-documented but unofficial hack — the radio isn't natively a BLE receiver. This implementation follows the standard published approach (bit-reversed access address + software LFSR dewhitening), but results can vary by NRF24 clone. Raw packet data is printed to Serial so you can verify/tune it against a known BLE device. WiFi sniffing, by contrast, uses the ESP32-C3's official promiscuous-mode API and is reliable out of the box.

## Known Limitations

- Vendor OUI lookup is a small, illustrative table (~15 common vendors) — not the full IEEE registry. Unknown MACs will show "Unknown."
- The per-AP client list caps at 4 entries and the AP list at 24, to keep RAM usage predictable on the ESP32-C3.
- Deauth/disassoc monitoring is detection-only. This tool will never send these frames — that capability has been deliberately excluded.

## Legal

Passive reception of publicly broadcast WiFi/BLE packets is legal in most jurisdictions, but laws vary by region. You are responsible for ensuring your use complies with local law and that you have authorization for any network or device you monitor. This project does not include, and will not add, active attack capabilities (packet injection, jamming, deauthentication).

## License

Add your preferred license here (e.g., MIT).
