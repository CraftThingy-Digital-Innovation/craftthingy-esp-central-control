#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "CentralConfig.h"
#include "HardwarePins.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Lightweight Zero-Dependency INA226 DC Driver
 * ============================================================================
 * Reads Battery / Bus Voltage (1.25 mV/LSB) & Shunt Current (2.5 uV/LSB)
 * over Hardware I2C (GPIO 21 SDA / GPIO 22 SCL).
 */
class Ina226Reader {
public:
    Ina226Reader() : _addr(INA226_I2C_ADDR), _shuntOhms(0.01f), _lastPollMs(0), _consecutiveErrors(0) {
        memset(&_telemetry, 0, sizeof(DcTelemetry));
    }

    bool begin(uint8_t addr = INA226_I2C_ADDR, float shuntOhms = 0.01f) {
        _addr = addr;
        _shuntOhms = shuntOhms;
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.setClock(100000); // Standard 100 kHz I2C speed

        // Diagnostic I2C bus scan
        Serial.printf("[I2C] Scanning I2C bus on SDA:GPIO%d, SCL:GPIO%d...\n", PIN_I2C_SDA, PIN_I2C_SCL);
        uint8_t foundCount = 0;
        uint8_t detectedAddr = 0;
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            uint8_t err = Wire.endTransmission(true);
            if (err == 0) {
                Serial.printf("[I2C] -> Device ACK at address 0x%02X!\n", a);
                foundCount++;
                if (a >= 0x40 && a <= 0x4F) {
                    detectedAddr = a;
                }
            }
        }

        if (foundCount == 0) {
            Serial.println(F("[I2C] -> WARNING: 0 I2C devices responded! Check SDA (GPIO 21) & SCL (GPIO 22) wiring."));
            _telemetry.isOnline = false;
            return false;
        }

        if (detectedAddr != 0) {
            _addr = detectedAddr;
            Serial.printf("[I2C] -> Using INA226 address 0x%02X\n", _addr);
        }

        // Verify communication by reading Manufacturer ID (0xFE) and Die ID (0xFF)
        uint16_t mfgId = readRegister(0xFE);
        uint16_t dieId = readRegister(0xFF);
        Serial.printf("[I2C] -> Read Reg 0xFE (Mfg ID): 0x%04X, Reg 0xFF (Die ID): 0x%04X\n", mfgId, dieId);

        // TI INA226 Mfg ID is 0x5449 ("TI"), Die ID is 0x2260
        if (mfgId != 0x5449 && dieId != 0x2260 && mfgId == 0xFFFF) {
            Serial.println(F("[I2C] -> INA226 ID mismatch / unreadable register"));
            _telemetry.isOnline = false;
            return false;
        }

        // Configure INA226 (Reg 0x00):
        // Averaging: 16 samples (0b010)
        // VBUS Conv Time: 1.1ms (0b100)
        // VSHUNT Conv Time: 1.1ms (0b100)
        // Mode: Shunt and Bus Continuous (0b111)
        // Value: 0b0100_0101_0010_0111 = 0x4527
        writeRegister(0x00, 0x4527);

        _telemetry.isOnline = true;
        _lastPollMs = 0;
        _consecutiveErrors = 0;
        return true;
    }

    bool loop(uint32_t pollIntervalMs = 1000) {
        if (millis() - _lastPollMs < pollIntervalMs) {
            return false;
        }
        _lastPollMs = millis();

        // Reg 0x02: Bus Voltage (1.25 mV per LSB, full range up to 36V)
        int16_t rawBus = (int16_t)readRegister(0x02);
        // Reg 0x01: Shunt Voltage (2.5 uV per LSB)
        int16_t rawShunt = (int16_t)readRegister(0x01);

        if (rawBus == -1 && rawShunt == -1) {
            _consecutiveErrors++;
            if (_consecutiveErrors > 3) {
                _telemetry.isOnline = false;
            }
            return false;
        }

        _telemetry.busVoltageV = (float)rawBus * 0.00125f;

        // Current = Vshunt (uV) / Rshunt (ohms) -> mA
        float shuntUv = (float)rawShunt * 2.5f;
        _telemetry.shuntCurrentMa = (shuntUv / _shuntOhms) * 0.001f;

        // Power in mW
        _telemetry.powerMw = _telemetry.busVoltageV * _telemetry.shuntCurrentMa;

        // Estimate battery state of charge (Li-ion 1S cell mapping: 3.2V = 0%, 4.2V = 100%)
        // If 12V lead-acid / 3S / 4S, normalizes accordingly:
        _telemetry.batterySocPct = estimateSoc(_telemetry.busVoltageV);

        _telemetry.isOnline = true;
        _telemetry.lastReadMillis = millis();
        _consecutiveErrors = 0;
        return true;
    }

    const DcTelemetry& getTelemetry() const {
        return _telemetry;
    }

    bool isOnline() const {
        return _telemetry.isOnline;
    }

private:
    uint8_t     _addr;
    float       _shuntOhms;
    uint32_t    _lastPollMs;
    uint8_t     _consecutiveErrors;
    DcTelemetry _telemetry;

    uint8_t estimateSoc(float voltage) {
        // Auto-detect single cell vs 12V pack
        float vCell = voltage;
        if (voltage > 10.0f) {
            vCell = voltage / 3.0f; // 3S Li-ion or ~12V pack equivalent
        }

        if (vCell <= 3.20f) return 0;
        if (vCell >= 4.18f) return 100;
        // Linear approximation across 3.20V -> 4.20V (1.0V span)
        float pct = ((vCell - 3.20f) / 0.98f) * 100.0f;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return (uint8_t)pct;
    }

    uint16_t readRegister(uint8_t reg) {
        Wire.beginTransmission(_addr);
        Wire.write(reg);
        uint8_t err = Wire.endTransmission(false);
        if (err != 0) {
            // Retry with STOP bit if repeated start was NACKed
            Wire.beginTransmission(_addr);
            Wire.write(reg);
            err = Wire.endTransmission(true);
            if (err != 0) {
                return 0xFFFF;
            }
        }

        if (Wire.requestFrom(_addr, (uint8_t)2) != 2) {
            return 0xFFFF;
        }

        uint8_t msb = Wire.read();
        uint8_t lsb = Wire.read();
        return (uint16_t)((msb << 8) | lsb);
    }

    void writeRegister(uint8_t reg, uint16_t value) {
        Wire.beginTransmission(_addr);
        Wire.write(reg);
        Wire.write((uint8_t)((value >> 8) & 0xFF));
        Wire.write((uint8_t)(value & 0xFF));
        Wire.endTransmission();
    }
};
