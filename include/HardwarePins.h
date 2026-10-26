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
// Peacefair PZEM-004T v3.0 / v4.0 (Hardware UART2 at native 3.3V)
// ----------------------------------------------------------------------------
#define PIN_PZEM_RX         16      // ESP32 RX2 <- PZEM TXD (3.3V logic)
#define PIN_PZEM_TX         17      // ESP32 TX2 -> PZEM RXD (3.3V logic)
#define PZEM_UART_NUM       2

// ----------------------------------------------------------------------------
// INA226 Bi-Directional DC Current & Power Sensor (Hardware I2C)
// ----------------------------------------------------------------------------
#define PIN_I2C_SDA         21      // Hardware I2C Data
#define PIN_I2C_SCL         22      // Hardware I2C Clock
#define PIN_INA_ALERT       35      // Optional Alert interrupt (Input-only pin)
#define INA226_I2C_ADDR     0x40    // Default I2C address (A0=GND, A1=GND)

// ----------------------------------------------------------------------------
// MicroSD Card Adapter (Hardware VSPI)
// ----------------------------------------------------------------------------
#define PIN_SD_SCK          18      // VSPI Clock
#define PIN_SD_MISO         19      // VSPI Master In / Slave Out
#define PIN_SD_MOSI         23      // VSPI Master Out / Slave In
#define PIN_SD_CS           5       // Chip Select (Active LOW)

// ----------------------------------------------------------------------------
// User Controls, Visual Signals & Audio Alarms
// ----------------------------------------------------------------------------
#define PIN_BUTTON_PAIR     32      // Physical 60s Pairing Trigger (Active LOW, internal pull-up)
#define PIN_LED_STATUS      25      // Status Indicator LED
#define PIN_BUZZER_ALARM    27      // Piezo Buzzer for 1300W Trip Warning
