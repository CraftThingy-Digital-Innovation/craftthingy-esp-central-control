#pragma once

#include <Arduino.h>

/**
 * ============================================================================
 * CraftThingy Central Control — ESP32 38-Pin DevKit Pin Map
 * ============================================================================
 * Strictly verified against ESP32 strapping pins and ADC2 limitations:
 * - Avoids boot strapping pins: GPIO 0, 2, 12, 15
 * - Avoids Wi-Fi ADC2 pins: GPIO 4, 0, 2, 15, 13, 12, 14, 27, 25, 26
 *   (ADC readings use ADC1 or dedicated I2C/SPI sensors)
 */

// ----------------------------------------------------------------------------
// Peacefair PZEM-004T v3.0 / v4.0 (Hardware UART2 at 9600 baud, 8N1)
// ----------------------------------------------------------------------------
// Wiring Architecture:
// - PZEM VCC (5V pin) -> 5V Rail (Boost Converter output / 5V Bus)
// - PZEM GND          -> System Common GND (ESP32 GND, Boost Converter GND, TP4056 GND)
// - PZEM RX           <- ESP32 GPIO 17 (TX2) direct connection (ESP32 3.3V sinks/drives optocoupler)
// - PZEM TX           -> 1N4148 Diode (Cathode facing PZEM TX, Anode facing ESP32 RX2 GPIO 16)
//                        ESP32 internal pullup (or external 4.7k-10k pullup to 3.3V) holds line at 3.3V.
//                        When PZEM TX is HIGH (5V): Diode is reverse-biased, isolating ESP32 from 5V.
//                        When PZEM TX is LOW (0V):  Diode conducts, pulling ESP32 RX2 LOW (~0.6V).
#define PIN_PZEM_RX         16      // ESP32 RX2 <- 1N4148 Anode (Cathode to PZEM TXD)
#define PIN_PZEM_TX         17      // ESP32 TX2 -> PZEM RXD (Direct)
#define PZEM_UART_NUM       2

// ----------------------------------------------------------------------------
// INA226 Bi-Directional DC Current & Power Sensor (Hardware I2C)
// ----------------------------------------------------------------------------
// Wiring Architecture:
// - VCC   -> 3.3V Logic Power from ESP32
// - GND   -> System Common GND
// - SCL   -> ESP32 GPIO 22 (Hardware I2C Clock)
// - SDA   -> ESP32 GPIO 21 (Hardware I2C Data)
// - IN+   -> High-side battery positive (TP4056 BAT+ / 1S Li-Ion cell)
// - IN-   -> Load positive (Boost converter input positive)
// - VBS   -> Jumpered directly to IN+ / Battery positive (Bus voltage sensing)
// - Shunt -> R100 (0.1 ohm) surface-mount sense resistor
#define PIN_I2C_SDA         21      // Hardware I2C Data
#define PIN_I2C_SCL         22      // Hardware I2C Clock
#define PIN_INA_ALERT       35      // Optional Alert interrupt (Input-only pin)
#define INA226_I2C_ADDR     0x44    // Active I2C address (A1=VS+, A0=GND on CJMCU-226)
#define INA226_SHUNT_OHMS   0.1f    // Onboard R100 (0.1 ohm) shunt resistor

// ----------------------------------------------------------------------------
// MicroSD Card Adapter (Hardware VSPI)
// ----------------------------------------------------------------------------
#define PIN_SD_SCK          18      // VSPI Clock
#define PIN_SD_MISO         19      // VSPI Master In / Slave Out
#define PIN_SD_MOSI         23      // VSPI Master Out / Slave In
#define PIN_SD_CS           5       // Chip Select (Active LOW)

// ----------------------------------------------------------------------------
// User Controls, Visual Signals & Audio Alarms (ALL 100% OPTIONAL)
// Set to -1 if not wired. All features operate completely digitally via Web / API!
// ----------------------------------------------------------------------------
#define PIN_BUTTON_PAIR     -1      // Optional: Set to GPIO (e.g. 32) or -1 (Pairing is digital)
#define PIN_LED_STATUS      -1      // Optional: Set to GPIO (e.g. 25) or -1 (Digital status only)
#define PIN_BUZZER_ALARM    -1      // Optional: Set to GPIO (e.g. 27) or -1 (Digital alerts only)
