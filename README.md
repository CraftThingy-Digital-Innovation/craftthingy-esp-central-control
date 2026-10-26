# CraftThingy ESP Central Control & Energy Arbiter

**Intelligent Home Energy Management System (HEMS), 1300W Grid Arbiter & Central Coordinator**

Built for the **CraftThingy Digital Innovation** ecosystem on top of [`craftthingy-esp-core-ota`](../craftthingy-esp-core-ota).

---

## 🚀 Key Features

* **1300W Utility Grid Protection**: Real-time Peacefair PZEM-004T AC voltage, current, and active power monitoring via native 3.3V UART2. Prevents circuit breaker trips through dynamic load arbitration.
* **Dual-Mode Satellite Coordination**: Coordinates appliances (e.g. Water Pump, Washing Machine) with Token-Permit grants, priority queues, and dynamic load-siphoning. Automatically fails safe to local autonomous mode if the hub drops.
* **Zero-Trust Security**: Multi-source hardware entropy generation (RF thermal noise + ADC + eFuse silicon DNA whitened through SHA-256), rolling HMAC-SHA256 ratchets, 60-second time-gated pairing, and RSSI proximity filtering.
* **Continuous Datalogging (8GB MicroSD)**: Isolated FreeRTOS ring-buffered datalogger writing daily CSV records, breaker trip curves, and disaster recovery vaults without slowing control loops.
* **Resilient Ground Truth**: Primary encryption keys and configuration reside in onboard ESP32 NVS Flash; the system continues operating at 100% capacity even if the SD card is removed or damaged.
* **Built-in UPS & Battery Monitoring**: INA226 high-precision I2C DC bus and battery telemetry with XW228DKFR4 push-to-check physical LED bargraph.

---

## 🛠️ Hardware Requirements

* **Microcontroller**: ESP32 38-Pin DevKit (ESP-WROOM-32).
* **AC Energy Sensor**: Peacefair PZEM-004T v3.0 / v4.0 (operated at 3.3V logic).
* **Storage**: MicroSD Card Adapter (SPI) with 8GB FAT32 MicroSD card.
* **DC Battery Sensor**: INA226 I2C bi-directional current/power monitor.
* **Physical Battery Gauge**: XW228DKFR4 multi-segment LED board with momentary push-button.

For complete wiring schematics, see [docs/HARDWARE_PINOUT.md](docs/HARDWARE_PINOUT.md).  
For the detailed architecture, see [docs/SYSTEM_DESIGN.md](docs/SYSTEM_DESIGN.md).

---

## 📦 Building & Flashing

This project is built using PlatformIO:

```bash
# Build for ESP32 DevKit
pio run -e esp32dev

# Upload firmware via USB
pio run -e esp32dev -t upload

# Monitor serial output
pio device monitor -b 115200
```
