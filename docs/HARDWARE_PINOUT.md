# CraftThingy Central Control — ESP32 38-Pin Hardware Pinout

## 1. Peripheral Pin Allocation Table

This assignment avoids all ESP32 strapping pins (GPIO 0, 2, 12, 15) and Wi-Fi ADC2 conflicts:

| Subsystem | Signal Name | ESP32 GPIO | Direction | Level | Notes |
|---|---|---|---|---|---|
| **PZEM-004T v3/v4** | `ESP32 RX2` $\leftarrow$ PZEM TX | **GPIO 16** | Input | 3.3V Native | Hardware UART2. Native 3.3V with 3.3V PZEM logic jumper |
| | `ESP32 TX2` $\rightarrow$ PZEM RX | **GPIO 17** | Output | 3.3V Native | Hardware UART2 |
| **INA226 DC Sensor** | `I2C SDA` | **GPIO 21** | Bidirectional | 3.3V | I2C Data (4.7kΩ pull-up on breakout) |
| | `I2C SCL` | **GPIO 22** | Output | 3.3V | I2C Clock (4.7kΩ pull-up on breakout) |
| | `ALERT` | **GPIO 35** | Input | 3.3V | Optional over-current / voltage alert interrupt |
| **MicroSD Module** | `VSPI SCK` | **GPIO 18** | Output | 3.3V | Hardware SPI Clock |
| | `VSPI MISO` | **GPIO 19** | Input | 3.3V | Hardware SPI Master In |
| | `VSPI MOSI` | **GPIO 23** | Output | 3.3V | Hardware SPI Master Out |
| | `MicroSD CS` | **GPIO 5** | Output | 3.3V | Chip Select (Active LOW) |
| **User Interface** | `Pairing Button` | **GPIO 32** | Input | 3.3V | Momentary button to GND (Internal Pull-Up enabled) |
| | `Status LED (RGB/Bicolor)` | **GPIO 25, 26** | Output | 3.3V | Green: Normal / Red: Power Warning / Blue: Pairing |
| | `Buzzer / Alarm Output` | **GPIO 27** | Output | 3.3V | PWM Piezo buzzer for breaker trip warning |
| **Auxiliary Relays** | `Shed Relay 1 (Optional)` | **GPIO 4** | Output | 3.3V | Local contactor / relay output |
| | `Shed Relay 2 (Optional)` | **GPIO 14** | Output | 3.3V | Local contactor / relay output |

---

## 2. PZEM-004T v3/v4 Connection (3.3V Native Mode)

```
[ ESP32 38-Pin DevKit ]                      [ PZEM-004T v3.0 / v4.0 ]
         3V3 ────────────────────────────────────── VCC (3.3V Jumper closed)
         GND ────────────────────────────────────── GND
 GPIO 16 (RX2) ◄─────────────────────────────────── TXD (3.3V Logic)
 GPIO 17 (TX2) ───────────────────────────────────► RXD (3.3V Logic)
```

> **Note on 3.3V Operation:** 
> When the PZEM-004T is operated with its 3.3V solder bridge or powered at 3.3V VCC, both TX and RX lines interface directly with the ESP32 with **zero voltage dividers or level shifters**.

---

## 3. Battery System & XW228DKFR4 LED Meter

```
                      ┌────────────────────────────────────────────────────────┐
                      │                   Battery (+)                          │
                      └───┬───────────────────────────────┬────────────────────┘
                          │                               │
                          ▼                               ▼
                 ┌──────────────────┐           ┌──────────────────┐
                 │  INA226 VBUS     │           │ Momentary Button │
                 │  & Shunt (+)     │           └─────────┬────────┘
                 └────────┬─────────┘                     │
                          │                               ▼
                          │                      [ XW228DKFR4 (+) ]
                          │                              │
                          ▼                      [ XW228DKFR4 (-) ]
                 ┌──────────────────┐                     │
                 │  INA226 Shunt(-) │                     │
                 │  (Load / 5V Reg) │                     │
                 └──────────────────┘                     ▼
                          │                            [ GND ]
                          ▼
                 ┌──────────────────┐
                 │ 5V Buck Booster  │
                 │ (Powers ESP32)   │
                 └──────────────────┘
```

* **INA226**: Connected via I2C (`GPIO 21 SDA`, `GPIO 22 SCL`) on the 3.3V rail. Reads continuous high-precision battery mV, charge/discharge mA, and power.
* **XW228DKFR4**: Passive battery indicator board wired in parallel across the battery through a **momentary tactile push button**. Pressing the button lights up the 4 LED segments without consuming power during standby.

---

## 4. MicroSD Card Adapter (VSPI)

```
[ ESP32 38-Pin DevKit ]                      [ MicroSD Card Adapter ]
         3V3 / 5V ───────────────────────────────── VCC (Depending on module LDO)
         GND ────────────────────────────────────── GND
 GPIO 18 (SCK) ───────────────────────────────────► SCK / CLK
 GPIO 19 (MISO) ◄────────────────────────────────── MISO / DO
 GPIO 23 (MOSI) ──────────────────────────────────► MOSI / DI
  GPIO 5 (CS) ────────────────────────────────────► CS / SS
```
