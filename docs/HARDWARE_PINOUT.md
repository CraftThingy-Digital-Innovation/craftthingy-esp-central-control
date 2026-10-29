# CraftThingy Central Control — ESP32 38-Pin Hardware Pinout

## 1. Peripheral Pin Allocation Table

This assignment avoids all ESP32 strapping pins (GPIO 0, 2, 12, 15) and Wi-Fi ADC2 conflicts:

| Subsystem | Signal Name | ESP32 GPIO | Direction | Level | Notes |
|---|---|---|---|---|---|
| **PZEM-004T v3/v4** | `ESP32 RX2` $\leftarrow$ PZEM TX | **GPIO 16** | Input | 3.3V Native | Hardware UART2. Native 3.3V with 3.3V PZEM logic jumper |
| | `ESP32 TX2` $\rightarrow$ PZEM RX | **GPIO 17** | Output | 3.3V Native | Hardware UART2 |
| **INA226 DC Sensor** | `I2C SDA` | **GPIO 21** | Bidirectional | 3.3V | I2C Data (4.7kΩ pull-up on breakout) |
| | `I2C SCL` | **GPIO 22** | Output | 3.3V | I2C Clock (4.7kΩ pull-up on breakout) |
| | `VCC / GND` | **3V3 / GND** | Power / Ground | 3.3V | Sensor chip logic supply from ESP32 3V3 |
| | `VBUS` | **Bridge to IN+** | Input | 0 - 36V | **Bus Voltage Sensing**: Wire jumper directly to `IN+` (Battery +) |
| | `IN+ (Vin+)` | **Battery (+)** | Analog Sense | 3.7V - 4.2V | Shunt Resistor (+) side (Battery positive terminal) |
| | `IN- (Vin-)` | **Booster In (+)** | Analog Sense | 3.7V - 4.2V | Shunt Resistor (-) side (5V Boost input) |
| | `ALERT` | **GPIO 35** | Input | 3.3V | Optional over-current / voltage alert interrupt |
| **MicroSD Module** | `VSPI SCK` | **GPIO 18** | Output | 3.3V | Hardware SPI Clock |
| | `VSPI MISO` | **GPIO 19** | Input | 3.3V | Hardware SPI Master In |
| | `VSPI MOSI` | **GPIO 23** | Output | 3.3V | Hardware SPI Master Out |
| | `MicroSD CS` | **GPIO 5** | Output | 3.3V | Chip Select (Active LOW) |
| **Optional UI** | `Pairing Button (Optional)` | **GPIO 32** (or **-1**) | Input | 3.3V | **100% Optional**. Pairing is digital via Web/API. Set to -1 if omitted. |
| | `Status LED (Optional)` | **GPIO 25** (or **-1**) | Output | 3.3V | **100% Optional**. Status is live in Web UI / REST API. Set to -1 if omitted. |
| | `Buzzer Alarm (Optional)` | **GPIO 27** (or **-1**) | Output | 3.3V | **100% Optional**. Breaker alerts broadcast over Web/API. Set to -1 if omitted. |
| **Auxiliary Relays** | `Shed Relay 1 (Optional)` | **GPIO 4** | Output | 3.3V | Local contactor / relay output (Optional) |
| | `Shed Relay 2 (Optional)` | **GPIO 14** | Output | 3.3V | Local contactor / relay output (Optional) |
| **Power & Stability** | `Reset / Enable (EN)` | **EN Pin** | Input | 3.3V | 10kΩ pull-up to 3.3V + 10µF cap to GND (100ms POR delay) |
| | `5V / VIN Rail` | **5V Pin** | Power In | 5.0V | 220µF low-ESR bulk reservoir capacitor across 5V & GND |

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

## 3. Battery System & INA226 Power vs Sensing Clarification

The INA226 module has **two completely separate circuits**:
1. **Chip Power & I2C Logic (`VCC` & `GND`)**:
   * **Power it directly from 3.3V (`ESP32 3V3` pin)**.
   * INA226 supports $2.7\text{V} - 5.5\text{V}$ logic. Powering it from 3.3V is the **safest and recommended configuration** because the I2C pull-up resistors will pull `SDA` and `SCL` directly to 3.3V, matching the ESP32 GPIOs without needing any level shifters.
2. **High-Side Sensing Shunt (`IN+`, `IN-`, and `VBUS`)**:
   * The shunt resistor measures current flowing out of the battery.
   * `IN+` & `VBUS` connect to the **Battery Positive terminal** ($3.0\text{V} - 4.2\text{V}$).
   * `IN-` connects to the **input of the 5V booster / regulator** (the circuit load). The shunt resistor measures whatever current the booster draws from the battery.

```
                          ┌────────────────────────────────────────────────────────┐
                          │                   Battery (+) (3.7V - 4.2V)            │
                          └───┬───────────────────────────────┬────────────────────┘
                              │                               │
                              ▼                               ▼
                     ┌──────────────────┐           ┌──────────────────┐
                     │    INA226 IN+    │           │ Momentary Button │
                     │   (Shunt High)   │           └─────────┬────────┘
                     └────────┬─────────┘                     │
                              ├─── Wire Jumper ───┐           ▼
                              │ (Battery Voltage) │  [ XW228DKFR4 (+) ]
                              │                   ▼           │
                              │          ┌────────────────┐   │
                              │          │  INA226 VBUS   │   │
                              │          │ (Voltage Sense)│   │
                              │          └────────────────┘   │
                              │ (Current Shunt)               │
                              ▼                               ▼
                     ┌──────────────────┐            [ XW228DKFR4 (-) ]
                     │    INA226 IN-    │                     │
                     │   (Shunt Low)    │                     │
                     └────────┬─────────┘                     │
                              │                               ▼
                              ▼                            [ GND ]
                     ┌──────────────────┐
                     │ 5V Boost Module  │ (Steps 3.7V Li-ion to 5.0V)
                     └────────┬─────────┘
                              │ 5V Rail
                              ├───────────────┐
                              │               │ (+)
                              │              ───
                              │             │   │ [ 220 µF Bulk Cap ]
                              │              ───
                              │               │ (-)
                              ▼               ▼
                     ┌──────────────────┐    GND
                     │ ESP32 5V/VIN Pin │
                     └────────┬─────────┘
                              │ Internal 3.3V LDO
                              ▼
                     ┌──────────────────┐
                     │ ESP32 3V3 Pin    │───┬──► [ INA226 VCC (Logic Power) ]
                     │                  │   └───[ 10 kΩ Resistor ]──┐
                     │ ESP32 EN Pin     │◄──────────────────────────┼──[ 10 µF Cap ]──► GND
                     │ ESP32 GND Pin    │──────► [ INA226 GND (Logic Ground) ]
                     │ ESP32 GPIO 21    │◄─────► [ INA226 SDA (3.3V I2C Data) ]
                     │ ESP32 GPIO 22    │──────► [ INA226 SCL (3.3V I2C Clock) ]
                     └──────────────────┘
```

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

---

## 5. Power Supply & Reset (EN) Hardware Stabilization

To guarantee 24/7 industrial-grade reliability near noisy inductive electrical panels and water pumps, the following passive stabilization networks are implemented:

### A. Chip Enable / Reset (EN) Power-On RC Delay ($10\text{k}\Omega + 10\mu\text{F}$)

When power is first applied, battery boost converters and grid supplies ramp up with a slight voltage slew rate. Without an RC delay, the ESP32 can exit reset before the internal 3.3V rail, clock crystal, and SPI flash memory stabilize, causing random boot crashes or flashing failures.

```
       3.3V Rail
           │
           ├───[ 10 kΩ Resistor ]───┐
                                    │
                                    ├───► [ ESP32 EN / CHIP_PU Pin ]
                                    │
           ├───[ 10 µF Capacitor ]──┘
           │     (Positive +)
          GND
```

* **RC Time Constant ($\tau$)**:
  $$\tau = R \times C = 10\,\text{k}\Omega \times 10\,\mu\text{F} = \mathbf{100\,\text{ms}}$$
* **Effect**: Holds the ESP32 `EN` pin LOW for ~100ms during initial power ramp-up, ensuring the 3.3V rail is 100% stable before the Xtensa dual-core CPU boots. Eliminates power-on lockups and flashing errors.

---

### B. 5V Rail Bulk Reservoir Capacitor ($220\mu\text{F}$)

The ESP32 Wi-Fi radio draws rapid, high-amplitude current pulses during RF transmission bursts (up to $450\text{mA} - 500\text{mA}$ lasting several milliseconds).

```
 [ 5V Boost Output ] ──────────┬──────────────────────────► [ ESP32 5V / VIN Pin ]
                               │
                              ─── +
                             │   │  [ 220 µF Bulk Electrolytic / Low-ESR ]
                              ─── -
                               │
 [ Common Ground ]   ──────────┴──────────────────────────► [ ESP32 GND Pin ]
```

* **Effect**: The $220\mu\text{F}$ capacitor acts as an immediate local energy reservoir directly at the input of the ESP32's onboard 3.3V Low-Dropout (LDO) regulator.
* **Result**: Prevents the 5V booster from sagging under sudden Wi-Fi transmit load steps, completely eliminating `Brownout detector was triggered` reboot loops.

