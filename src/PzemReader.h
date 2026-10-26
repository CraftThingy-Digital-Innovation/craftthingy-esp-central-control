#pragma once

#include <Arduino.h>
#include "CentralConfig.h"
#include "HardwarePins.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Non-Blocking PZEM-004T v3/v4 Driver (Native 3.3V)
 * ============================================================================
 * Interfaces over ESP32 HardwareSerial2 at 9600 baud, 8N1.
 * Evaluates Modbus-RTU 16-bit CRC with zero dynamic heap allocations.
 */
class PzemReader {
public:
    PzemReader() : _lastPollMs(0), _consecutiveErrors(0) {
        memset(&_telemetry, 0, sizeof(PzemTelemetry));
    }

    void begin() {
        Serial2.begin(9600, SERIAL_8N1, PIN_PZEM_RX, PIN_PZEM_TX);
        _lastPollMs = 0;
        _consecutiveErrors = 0;
    }

    /**
     * @brief Non-blocking poll routine to be invoked in loop() or FreeRTOS task.
     * @param pollIntervalMs Interval between readings (default: 1000ms / 1 Hz)
     * @return true if fresh telemetry was updated
     */
    bool loop(uint32_t pollIntervalMs = 1000) {
        if (millis() - _lastPollMs < pollIntervalMs) {
            return false;
        }
        _lastPollMs = millis();

        // Drain any stale bytes
        while (Serial2.available()) {
            Serial2.read();
        }

        // Modbus-RTU Read Input Registers (0x04) starting at 0x0000 for 10 registers
        uint8_t cmd[8] = { 0x01, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x70, 0x0D };
        Serial2.write(cmd, 8);
        Serial2.flush();

        // Wait up to 150ms for 25-byte response
        uint8_t response[25];
        uint8_t received = 0;
        uint32_t startMs = millis();

        while ((millis() - startMs < 150) && (received < 25)) {
            if (Serial2.available()) {
                response[received++] = Serial2.read();
            } else {
                delay(1);
            }
        }

        if (received < 25) {
            handleError();
            return false;
        }

        // Validate Modbus header
        if (response[0] != 0x01 || response[1] != 0x04 || response[2] != 0x14) {
            handleError();
            return false;
        }

        // Verify CRC16
        uint16_t expectedCrc = calculateCRC(response, 23);
        uint16_t receivedCrc = response[23] | (response[24] << 8);
        if (expectedCrc != receivedCrc) {
            handleError();
            return false;
        }

        // Parse 10 Registers (20 payload bytes, Big-Endian)
        // Reg 0: Voltage (0.1V)
        _telemetry.voltage = (float)((response[3] << 8) | response[4]) * 0.1f;

        // Reg 1..2: Current (0.001A, 32-bit: Low word in Reg 1, High word in Reg 2)
        uint32_t currentRaw = ((uint32_t)((response[7] << 8) | response[8]) << 16) |
                              ((response[5] << 8) | response[6]);
        _telemetry.current = (float)currentRaw * 0.001f;

        // Reg 3..4: Power (0.1W, 32-bit: Low word in Reg 3, High word in Reg 4)
        uint32_t powerRaw = ((uint32_t)((response[11] << 8) | response[12]) << 16) |
                            ((response[9] << 8) | response[10]);
        _telemetry.power = (float)powerRaw * 0.1f;

        // Reg 5..6: Energy (1Wh, 32-bit: Low word in Reg 5, High word in Reg 6)
        uint32_t energyRaw = ((uint32_t)((response[15] << 8) | response[16]) << 16) |
                             ((response[13] << 8) | response[14]);
        _telemetry.energy = (float)energyRaw * 0.001f; // Convert Wh -> kWh

        // Reg 7: Frequency (0.1Hz)
        _telemetry.frequency = (float)((response[17] << 8) | response[18]) * 0.1f;

        // Reg 8: Power Factor (0.01)
        _telemetry.powerFactor = (float)((response[19] << 8) | response[20]) * 0.01f;

        _telemetry.isOnline = true;
        _telemetry.lastReadMillis = millis();
        _consecutiveErrors = 0;
        return true;
    }

    const PzemTelemetry& getTelemetry() const {
        return _telemetry;
    }

    bool isOnline() const {
        return _telemetry.isOnline;
    }

private:
    PzemTelemetry _telemetry;
    uint32_t      _lastPollMs;
    uint8_t       _consecutiveErrors;

    void handleError() {
        _consecutiveErrors++;
        if (_consecutiveErrors > 3) {
            _telemetry.isOnline = false;
        }
    }

    static uint16_t calculateCRC(const uint8_t* buffer, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t pos = 0; pos < len; pos++) {
            crc ^= (uint16_t)buffer[pos];
            for (uint8_t bit = 8; bit != 0; bit--) {
                if ((crc & 0x0001) != 0) {
                    crc >>= 1;
                    crc ^= 0xA001;
                } else {
                    crc >>= 1;
                }
            }
        }
        return crc;
    }
};
