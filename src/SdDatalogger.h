#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "CentralConfig.h"
#include "HardwarePins.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Resilient 8GB MicroSD Datalogger & Vault
 * ============================================================================
 * Non-blocking datalogger running over VSPI (GPIO 18, 19, 23, CS 5).
 * Built with Graceful Degradation: System never blocks if SD card dies or is ejected.
 */
class SdDatalogger {
public:
    SdDatalogger() : _lastFlushMs(0), _bufferIndex(0) {
        memset(&_status, 0, sizeof(SdStatus));
        _currentDateStr[0] = '\0';
    }

    bool begin() {
        pinMode(PIN_SD_CS, OUTPUT);
        digitalWrite(PIN_SD_CS, HIGH);

        SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

        // Try mounting at standard 4 MHz (reliable over jumper wires)
        bool mounted = SD.begin(PIN_SD_CS, SPI, 4000000);
        if (!mounted) {
            // Safe fallback at 1 MHz
            mounted = SD.begin(PIN_SD_CS, SPI, 1000000);
        }

        if (!mounted) {
            _status.isMounted = false;
            _status.writeErrors++;
            return false;
        }

        uint8_t cardType = SD.cardType();
        if (cardType == CARD_NONE) {
            _status.isMounted = false;
            return false;
        }

        _status.isMounted = true;
        _status.totalBytes = SD.totalBytes();
        _status.usedBytes = SD.usedBytes();

        // Ensure directory structure exists
        if (!SD.exists("/logs")) SD.mkdir("/logs");
        if (!SD.exists("/data")) SD.mkdir("/data");

        return true;
    }

    bool retryMount() {
        if (_status.isMounted) return true;
        return begin();
    }

    bool isMounted() const {
        return _status.isMounted;
    }

    /**
     * @brief Appends a telemetry sample to the write buffer and flushes to SD periodically.
     */
    void logSample(const PzemTelemetry& ac, const DcTelemetry& dc, uint8_t activePermits, float ceilingWatts, const char* dateStr = nullptr) {
        if (!_status.isMounted) {
            // Attempt auto-reconnect every 30 seconds if card was re-inserted
            if (millis() - _lastFlushMs > 30000) {
                _lastFlushMs = millis();
                begin();
            }
            return;
        }

        // Check if date changed for file rotation
        if (dateStr && strcmp(dateStr, _currentDateStr) != 0) {
            flushBuffer();
            strncpy(_currentDateStr, dateStr, sizeof(_currentDateStr) - 1);
            snprintf(_status.currentLogFile, sizeof(_status.currentLogFile), "/logs/power_%s.csv", _currentDateStr);
            ensureHeaderExists(_status.currentLogFile);
        } else if (_status.currentLogFile[0] == '\0') {
            snprintf(_status.currentLogFile, sizeof(_status.currentLogFile), "/logs/power_live.csv");
            ensureHeaderExists(_status.currentLogFile);
        }

        // Format CSV row
        char row[160];
        int len = snprintf(row, sizeof(row),
            "%lu,%.1f,%.3f,%.1f,%.3f,%.1f,%.2f,%.2f,%.1f,%.1f,%u,%u,%.1f\n",
            millis(),
            ac.voltage, ac.current, ac.power, ac.energy, ac.frequency, ac.powerFactor,
            dc.busVoltageV, dc.shuntCurrentMa, dc.powerMw, dc.batterySocPct,
            activePermits, ceilingWatts
        );

        if (len > 0 && (_bufferIndex + len < sizeof(_writeBuffer))) {
            memcpy(_writeBuffer + _bufferIndex, row, len);
            _bufferIndex += len;
        }

        // Flush buffer every 5 seconds or when nearly full (reduces SD flash wear!)
        if (millis() - _lastFlushMs >= 5000 || _bufferIndex > sizeof(_writeBuffer) - 200) {
            flushBuffer();
        }
    }

    void flushBuffer() {
        if (!_status.isMounted || _bufferIndex == 0) return;

        File f = SD.open(_status.currentLogFile, FILE_APPEND);
        if (f) {
            size_t written = f.write((const uint8_t*)_writeBuffer, _bufferIndex);
            f.flush();
            f.close();
            if (written == _bufferIndex) {
                _status.recordsWrittenToday++;
            } else {
                _status.writeErrors++;
            }
        } else {
            _status.writeErrors++;
            if (_status.writeErrors > 5) {
                _status.isMounted = false; // Mark unmounted to retry later
            }
        }

        _bufferIndex = 0;
        _lastFlushMs = millis();
    }

    /**
     * @brief Saves encrypted pairing vault to /data/recovery_vault.bin
     */
    bool saveVaultBackup(const uint8_t* vaultData, size_t len) {
        if (!_status.isMounted) return false;
        File f = SD.open("/data/recovery_vault.bin", FILE_WRITE);
        if (!f) return false;
        size_t written = f.write(vaultData, len);
        f.flush();
        f.close();
        return written == len;
    }

    /**
     * @brief Loads recovery vault from SD card for new hub disaster recovery.
     */
    bool loadVaultBackup(uint8_t* outData, size_t maxLen, size_t& outLen) {
        outLen = 0;
        if (!_status.isMounted || !SD.exists("/data/recovery_vault.bin")) return false;
        File f = SD.open("/data/recovery_vault.bin", FILE_READ);
        if (!f) return false;
        outLen = f.read(outData, maxLen);
        f.close();
        return outLen > 0;
    }

    const SdStatus& getStatus() const {
        return _status;
    }

private:
    SdStatus _status;
    uint32_t _lastFlushMs;
    char     _currentDateStr[16];
    char     _writeBuffer[1024]; // 1KB RAM ring buffer
    size_t   _bufferIndex;

    void ensureHeaderExists(const char* filePath) {
        if (!SD.exists(filePath)) {
            File f = SD.open(filePath, FILE_WRITE);
            if (f) {
                f.print("uptime_ms,ac_v,ac_a,ac_w,ac_kwh,ac_hz,ac_pf,dc_v,dc_ma,dc_mw,batt_soc,permits,ceiling_w\n");
                f.flush();
                f.close();
            }
        }
    }
};
