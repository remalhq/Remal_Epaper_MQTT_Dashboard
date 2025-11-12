# Remal E-paper MQTT Dashboard
A real-time **MQTT dashboard** built for the **Remal Shabakah v4 (ESP32-C3)** and **WeAct Studio 4.2″ e-paper (400×300)** display.  
It connects to Wi-Fi, subscribes to MQTT topics, displays incoming messages on the e-paper screen, and logs all activity to FFat flash storage with automatic 1 MB log rotation.

Most of this code is AI generated, so it doesn't really have our usual polish and coding style.. but hey, it works!

---

## Features
- 📡 **Wi-Fi + MQTT client**
- 🖥️ **E-paper display interface** (SSD1683 driver via GxEPD2)
- 💬 **Live message feed**  
  - Shows topic and payload text  
  - Wraps long messages across multiple lines  
  - Timestamped with local Dubai time
- 🌡️ **Built-in SHT30 sensor**  
  Displays current temperature (°C) and humidity (%RH) in the top bar
- 💾 **Persistent FFat logging**
  - Stores all MQTT messages in `/MQTT_log.txt`
  - Auto-rotates when file exceeds **1 MB**, keeping the newest ~900 KB
  - Remembers message index across reboots via `/MQTT_index.txt`
- 🧠 **Self-healing**
  - Automatically reconnects Wi-Fi and MQTT
  - Recovers gracefully after resets or power cycles

---

## 🧰 Hardware
| Component | Description |
|------------|-------------|
| **Board** | Remal Shabakah v4.0 (ESP32-C3-MINI) |
| **Display** | WeAct Studio 4.2″ e-paper (400 × 300 px, SSD1683 controller) |
| **Sensor** | Onboard SHT30 (Temp & Humidity, I²C) |
| **Power** | USB-C (5 V) or VIN (7–30 V) |
| **Logic voltage** | 3.3 V (non-5 V tolerant) |

---

## ⚙️ Pinout (Display Connections)
| Signal | ESP32-C3 Pin | Shabakah Label | Notes |
|---------|--------------|----------------|-------|
| **VCC** | 3.3 V | 3V3 | Power for e-paper |
| **GND** | GND | GND | Common ground |
| **SCK** | GPIO 4 | SPI SCK | Shared SPI bus |
| **MOSI** | GPIO 6 | SPI MOSI | Data line |
| **CS** | GPIO 7 | SPI CS | Chip Select |
| **DC** | GPIO 0 | DC | Data/Command |
| **RST** | GPIO 21 | RST | Reset |
| **BUSY** | GPIO 20 | BUSY | Panel busy signal |

---

### Required Libraries

| Library | Source |
|----------|--------|
| **Remal_SHT3X** | Installed automatically with Remal Boards |
| **GxEPD2** | Arduino Library Manager |
| **Adafruit GFX** | Arduino Library Manager |
| **PubSubClient** | Arduino Library Manager |

---

## 🧾 MQTT Configuration

| Setting | Value |
|----------|-------|
| **Server** | `mqtt.remal.io` |
| **Port** | `8885` |
| **Subscribe Topic** | `remal/<any topic>` <- Remember only topics under `remal/` will work! |

Each received message is:
- Displayed on the e-paper  
- Logged to `/MQTT_log.txt`  
- Indexed and timestamped  

---

## 📁 FFat Logging

- Log file path: `/MQTT_log.txt`  
- Index file path: `/MQTT_index.txt`
- Max size: **1 MB**
- Keeps newest **≈900 KB** and overwrites oldest logs automatically.

You can mount the FFat filesystem via serial tools or erase it by calling:
```cpp
FFat.format();
```

---

## 🧱 Example Output (on Display)

```
121 - Tue Nov 12 '25 09:49 PM
[Nadi/Laundry]: System up-time 5 days, 4 hours, 51 minutes, 10 seconds
```