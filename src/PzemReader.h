#pragma once

#include <Arduino.h>
#include "CentralConfig.h"
#include "HardwarePins.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Lightweight Zero-Dependency PZEM Driver (v3/v4)
 * ============================================================================
 * Interfaces over ESP32 HardwareSerial2 at 9600 baud, 8N1.
 * Standard Modbus-RTU 16-bit CRC with zero dynamic heap allocations.
 * Compatible with direct 3.3V connection or 5V logic-level shifter.
 */

class PzemReader {
public:
    PzemReader() : _lastPollMs(0), _consecutiveErrors(0), _polls(0), _okPolls(0),
                   _totalRxBytes(0), _lastRxLen(0), _targetAddr(0x01) {
        memset(&_telemetry, 0, sizeof(PzemTelemetry));
        memset(_lastRx, 0, sizeof(_lastRx));
    }

    void begin() {
        Serial2.begin(9600, SERIAL_8N1, PIN_PZEM_RX, PIN_PZEM_TX);
        gpio_pullup_en((gpio_num_t)PIN_PZEM_RX);
        gpio_pulldown_dis((gpio_num_t)PIN_PZEM_RX);
        _lastPollMs = 0;
        _consecutiveErrors = 0;
    }

    bool loop(uint32_t pollIntervalMs = 1000) {
        if (millis() - _lastPollMs < pollIntervalMs) {
            return false;
        }
        _lastPollMs = millis();
        _polls++;

        // Drain any stale bytes
        while (Serial2.available()) {
            Serial2.read();
        }

        // Target address 0x01 (alternate with general broadcast 0xF8 on consecutive failures)
        _targetAddr = (_consecutiveErrors > 2 && (_polls % 2 == 1)) ? 0xF8 : 0x01;

        // Modbus Read Input Registers (0x04), start 0x0000, 9 registers (universal v3/v4 compatibility)
        uint8_t cmd[8] = { _targetAddr, 0x04, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00 };
        uint16_t crc = calculateCRC(cmd, 6);
        cmd[6] = (uint8_t)(crc & 0xFF);
        cmd[7] = (uint8_t)((crc >> 8) & 0xFF);

        Serial2.write(cmd, 8);
        Serial2.flush();

        // Wait up to 300ms for response (23 bytes for 9 registers)
        uint8_t response[25];
        uint8_t received = 0;
        uint32_t startMs = millis();

        while ((millis() - startMs < 300) && (received < 23)) {
            if (Serial2.available()) {
                response[received++] = Serial2.read();
                _totalRxBytes++;
            } else {
                delay(2);
            }
        }

        _lastRxLen = received;
        if (received > 0) {
            memcpy(_lastRx, response, received > 25 ? 25 : received);
        }

        if (received < 23) {
            handleError();
            return false;
        }

        // Validate Modbus header (accepts address 0x01 or 0xF8, function 0x04, length 18 bytes / 0x12)
        if ((response[0] != 0x01 && response[0] != 0xF8) || response[1] != 0x04 || response[2] != 0x12) {
            handleError();
            return false;
        }

        // Verify CRC16
        uint16_t expectedCrc = calculateCRC(response, 21);
        uint16_t receivedCrc = response[21] | (response[22] << 8);
        if (expectedCrc != receivedCrc) {
            handleError();
            return false;
        }

        // Parse 9 Registers (18 payload bytes, Big-Endian)
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
        _telemetry.energy = (float)energyRaw * 0.001f; // Wh -> kWh

        // Reg 7: Frequency (0.1Hz)
        _telemetry.frequency = (float)((response[17] << 8) | response[18]) * 0.1f;

        // Reg 8: Power Factor (0.01)
        _telemetry.powerFactor = (float)((response[19] << 8) | response[20]) * 0.01f;

        _telemetry.isOnline = true;
        _telemetry.lastReadMillis = millis();
        _consecutiveErrors = 0;
        _okPolls++;
        return true;
    }

    const PzemTelemetry& getTelemetry() const {
        return _telemetry;
    }

    bool isOnline() const {
        return _telemetry.isOnline;
    }

    String diagJson() const {
        String hex;
        for (uint8_t i = 0; i < _lastRxLen && i < 12; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%02X", _lastRx[i]);
            if (i) hex += " ";
            hex += b;
        }

        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"rxPin\":%d,\"txPin\":%d,\"queriedAddr\":\"0x%02X\","
            "\"polls\":%lu,\"okPolls\":%lu,\"lastRxLen\":%u,\"lastRxHex\":\"%s\",\"totalRxBytes\":%lu}",
            PIN_PZEM_RX, PIN_PZEM_TX, _targetAddr,
            (unsigned long)_polls, (unsigned long)_okPolls,
            _lastRxLen, hex.c_str(), (unsigned long)_totalRxBytes);
        return String(buf);
    }

private:
    PzemTelemetry _telemetry;
    uint32_t      _lastPollMs;
    uint8_t       _consecutiveErrors;
    uint32_t      _polls;
    uint32_t      _okPolls;
    uint32_t      _totalRxBytes;
    uint8_t       _lastRxLen;
    uint8_t       _lastRx[25];
    uint8_t       _targetAddr;

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
