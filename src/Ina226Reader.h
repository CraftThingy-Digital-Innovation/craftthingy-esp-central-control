#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "esp32-hal-i2c.h"
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
    Ina226Reader() : _addr(INA226_I2C_ADDR), _shuntOhms(INA226_SHUNT_OHMS), _lastPollMs(0), _consecutiveErrors(0) {
        memset(&_telemetry, 0, sizeof(DcTelemetry));
    }

    bool begin(uint8_t addr = INA226_I2C_ADDR, float shuntOhms = INA226_SHUNT_OHMS) {
        _addr = addr;
        _shuntOhms = shuntOhms;
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.setClock(100000);
        Wire.setTimeOut(25); // Avoid ESP32 FIFO lockup
        delay(50);

        // Scan for active INA226 address
        for (uint8_t a = 0x40; a <= 0x4F; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission(true) == 0) {
                _addr = a;
                break;
            }
        }

        // Verify communication by reading Manufacturer ID (0xFE) and Die ID (0xFF)
        uint16_t mfgId = readRegister(0xFE);
        uint16_t dieId = readRegister(0xFF);
        _mfgId = mfgId;
        _dieId = dieId;

        _telemetry.isOnline = (mfgId == 0x5449 && dieId == 0x2260);
        _lastPollMs = 0;
        _consecutiveErrors = 0;
        return _telemetry.isOnline;
    }

    bool loop(uint32_t pollIntervalMs = 1000) {
        if (millis() - _lastPollMs < pollIntervalMs) {
            return false;
        }
        _lastPollMs = millis();
        _polls++;

        // Sweep 0x40..0x4F to find the address returning TI Mfg ID 0x5449
        uint8_t activeAddr = 0;
        for (uint8_t a = 0x40; a <= 0x4F; a++) {
            uint8_t regAddr = 0xFE;
            uint8_t rxBuf[2] = {0};
            size_t count = 0;
            esp_err_t err = i2cWriteReadNonStop(0, a, &regAddr, 1, rxBuf, 2, 20, &count);
            if (err == ESP_OK && count == 2) {
                uint16_t id = (rxBuf[0] << 8) | rxBuf[1];
                if (id == 0x5449) {
                    activeAddr = a;
                    _addr = a;
                    _mfgId = id;
                    break;
                }
            }
        }

        if (activeAddr == 0) {
            _readErrors++;
            _consecutiveErrors++;
            if (_consecutiveErrors > 3) _telemetry.isOnline = false;
            return false;
        }

        // Ensure Configuration Register (0x00) is 0x4127 once at init or recovery
        if (_rawConfig != 0x4127) {
            uint8_t cfgCmd[3] = { 0x00, 0x41, 0x27 };
            i2cWrite(0, activeAddr, cfgCmd, 3, 20);
            _rawConfig = 0x4127;
            delay(5); // Allow initial conversion to complete (1.1ms)
        }

        // Read Bus Voltage (0x02) and Shunt Voltage (0x01) on the verified active address
        uint8_t regBus = 0x02;
        uint8_t bufBus[2] = {0xFF, 0xFF};
        size_t countBus = 0;
        esp_err_t errBus = i2cWriteReadNonStop(0, activeAddr, &regBus, 1, bufBus, 2, 20, &countBus);
        uint16_t rawBus = (bufBus[0] << 8) | bufBus[1];
        _errBus = (uint8_t)errBus;
        _countBus = countBus;
        _busMsb = bufBus[0];
        _busLsb = bufBus[1];

        uint8_t regShunt = 0x01;
        uint8_t bufShunt[2] = {0};
        size_t countShunt = 0;
        i2cWriteReadNonStop(0, activeAddr, &regShunt, 1, bufShunt, 2, 20, &countShunt);
        int16_t rawShunt = (int16_t)((bufShunt[0] << 8) | bufShunt[1]);

        _rawBus = rawBus;
        _rawShunt = (uint16_t)rawShunt;
        _telemetry.busVoltageV = (float)rawBus * 0.00125f;
        _telemetry.shuntCurrentMa = ((float)rawShunt * 2.5f / _shuntOhms) * 0.001f;
        _telemetry.powerMw = _telemetry.busVoltageV * _telemetry.shuntCurrentMa;
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

    // Raw register dump for remote hardware diagnosis
    String diagJson() const {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"i2cScan\":\"%s\",\"addr\":\"0x%02X\",\"mfgId\":\"0x%04X\",\"dieId\":\"0x%04X\","
            "\"config\":\"0x%04X\",\"rawBus\":\"0x%04X\",\"rawShunt\":\"0x%04X\",\"shuntUv\":%.1f,"
            "\"polls\":%lu,\"readErrors\":%lu,\"probeErr\":%u,\"probeErrAfterRecover\":%u,"
            "\"lastTxErr\":%u,\"lastRxCount\":%u,\"msb\":\"0x%02X\",\"lsb\":\"0x%02X\","
            "\"sdaLevel\":%d,\"sclLevel\":%d}",
            _scanList.c_str(), _addr, _mfgId, _dieId, _rawConfig, _rawBus, _rawShunt,
            (float)(int16_t)_rawShunt * 2.5f, (unsigned long)_polls, (unsigned long)_readErrors,
            _probeErr, _probeErr2, _lastTxErr, _lastRxCount, _lastMsb, _lastLsb,
            digitalRead(PIN_I2C_SDA), digitalRead(PIN_I2C_SCL));
        return String(buf);
    }

private:
    uint8_t     _errBus = 0, _busMsb = 0, _busLsb = 0;
    size_t      _countBus = 0;
    uint8_t     _lastMsb = 0, _lastLsb = 0;
    uint8_t     _probeErr = 0, _probeErr2 = 0, _lastTxErr = 0, _lastRxCount = 0;
    uint32_t    _recoveries = 0;
    int         _sdaLevel = -1, _sclLevel = -1;

    // Release a stuck I2C bus: 9 SCL clocks + STOP, then re-init the driver
    void recoverBus() {
        _recoveries++;
        Wire.end();
        pinMode(PIN_I2C_SDA, INPUT_PULLUP);
        pinMode(PIN_I2C_SCL, INPUT_PULLUP);
        delayMicroseconds(10);
        _sdaLevel = digitalRead(PIN_I2C_SDA);
        _sclLevel = digitalRead(PIN_I2C_SCL);
        pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
        for (int i = 0; i < 9; i++) {
            digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(5);
            digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
        }
        pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
        digitalWrite(PIN_I2C_SDA, LOW);  delayMicroseconds(5);
        digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
        Wire.setClock(100000);
    }

private:
    String      _scanList;
    uint16_t    _mfgId = 0, _dieId = 0, _rawConfig = 0, _rawBus = 0, _rawShunt = 0;
    uint32_t    _polls = 0, _readErrors = 0;
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
        for (uint8_t attempt = 0; attempt < 3; attempt++) {
            uint16_t v = readRegisterOnce(reg);
            if (v != 0xFFFF) return v;
            delay(1);
        }
        return 0xFFFF;
    }

    uint16_t readRegisterOnce(uint8_t reg) {
        uint8_t regAddr = reg;
        uint8_t rxBuf[2] = {0, 0};
        size_t count = 0;
        esp_err_t err = i2cWriteReadNonStop(0, _addr, &regAddr, 1, rxBuf, 2, 50, &count);
        _lastTxErr = (uint8_t)err;
        _lastRxCount = count;
        if (err != ESP_OK || count != 2) {
            return 0xFFFF;
        }

        _lastMsb = rxBuf[0];
        _lastLsb = rxBuf[1];
        return (uint16_t)((rxBuf[0] << 8) | rxBuf[1]);
    }

    void writeRegister(uint8_t reg, uint16_t value) {
        Wire.beginTransmission(_addr);
        Wire.write(reg);
        Wire.write((uint8_t)((value >> 8) & 0xFF));
        Wire.write((uint8_t)(value & 0xFF));
        Wire.endTransmission(true);
    }
};
