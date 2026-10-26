#pragma once

#include <Arduino.h>
#include <DeviceController.h>
#include "CentralConfig.h"
#include "HardwarePins.h"
#include "PzemReader.h"
#include "Ina226Reader.h"
#include "SdDatalogger.h"
#include "PowerArbiter.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Core Hardware Controller Implementation
 * ============================================================================
 * Integrates PZEM-004T (3.3V), INA226 (I2C), MicroSD (VSPI), and PowerArbiter
 * directly into the CraftThingyCore framework.
 */
class CentralController : public IDeviceController {
public:
    CentralController() : 
        _pairingWindowActive(false), 
        _pairingOpenedMs(0), 
        _buzzerMuted(false),
        _lastBuzzerToggleMs(0) {}

    void setup() override {
        // Configure GPIO pins
        pinMode(PIN_BUTTON_PAIR, INPUT_PULLUP);
        pinMode(PIN_LED_STATUS, OUTPUT);
        pinMode(PIN_BUZZER_ALARM, OUTPUT);
        digitalWrite(PIN_LED_STATUS, LOW);
        digitalWrite(PIN_BUZZER_ALARM, LOW);

        // Initialize subsystems
        _pzem.begin();
        _ina226.begin(INA226_I2C_ADDR, 0.01f);
        _sd.begin();
        _arbiter.begin();
    }

    void loop() override {
        // Poll sensors (1 Hz)
        _pzem.loop(1000);
        _ina226.loop(1000);

        // Run power arbiter supervisory logic
        _arbiter.loop(_pzem.getTelemetry());

        // Periodic datalogging to 8GB MicroSD
        const PzemTelemetry& ac = _pzem.getTelemetry();
        const DcTelemetry& dc = _ina226.getTelemetry();
        _sd.logSample(ac, dc, _arbiter.getActivePermitCount(), _arbiter.getProfile().effectiveCeilingWatts);

        // Physical 60-Second Pairing Button Monitor
        if (digitalRead(PIN_BUTTON_PAIR) == LOW) {
            delay(50); // Debounce
            if (digitalRead(PIN_BUTTON_PAIR) == LOW && !_pairingWindowActive) {
                openPairingWindow(60);
            }
        }

        // Expire Pairing Window after timeout
        if (_pairingWindowActive && (millis() - _pairingOpenedMs > 60000)) {
            _pairingWindowActive = false;
            digitalWrite(PIN_LED_STATUS, LOW);
        }

        // Alarm Buzzer & Status LED logic
        handleSurgeAlarm();
    }

    const char* getRoleName() const override {
        return "central_control";
    }

    const char* getAppVersion() const override {
        return APP_VERSION;
    }

    bool handleCommand(const String& cmd, const String& payload, String& response) override {
        if (cmd == "set_profile") {
            int preset = payload.toInt();
            _arbiter.setProfilePreset((PowerProfilePreset)preset);
            response = "{\"status\":\"success\",\"message\":\"Profile preset updated\"}";
            return true;
        }

        if (cmd == "set_custom_watts") {
            float customW = payload.toFloat();
            _arbiter.setProfilePreset(PRESET_CUSTOM, customW);
            response = "{\"status\":\"success\",\"message\":\"Custom wattage updated\"}";
            return true;
        }

        if (cmd == "set_thermal_buffer") {
            int buf = payload.toInt();
            _arbiter.setThermalBufferPct((uint8_t)buf);
            response = "{\"status\":\"success\",\"message\":\"Thermal buffer updated\"}";
            return true;
        }

        if (cmd == "open_pairing") {
            openPairingWindow(60);
            response = "{\"status\":\"success\",\"message\":\"Pairing window opened for 60 seconds\"}";
            return true;
        }

        if (cmd == "request_permit") {
            // Payload format: {"mac":"XX:XX:XX","watts":500,"duration":300}
            // For simple string parsing:
            String mac = parseField(payload, "mac");
            float watts = parseField(payload, "watts").toFloat();
            uint32_t dur = (uint32_t)parseField(payload, "duration").toInt();
            if (watts <= 0.0f) watts = 500.0f;
            if (dur == 0) dur = 300;

            PowerPermitToken token = _arbiter.requestPermit(mac.c_str(), watts, dur, _pzem.getTelemetry());

            char resBuf[256];
            snprintf(resBuf, sizeof(resBuf),
                "{\"status\":\"success\",\"tokenId\":%lu,\"granted\":%s,\"permitStatus\":%u,\"grantedWatts\":%.1f}",
                token.tokenId, (token.status == TOKEN_GRANTED) ? "true" : "false",
                (unsigned int)token.status, token.grantedWatts
            );
            response = String(resBuf);
            return true;
        }

        if (cmd == "release_permit") {
            uint32_t tokenId = (uint32_t)payload.toInt();
            bool released = _arbiter.releasePermit(tokenId);
            response = released ? "{\"status\":\"success\"}" : "{\"status\":\"error\",\"message\":\"Token not found\"}";
            return true;
        }

        if (cmd == "register_device") {
            String mac = parseField(payload, "mac");
            String name = parseField(payload, "name");
            String role = parseField(payload, "role");
            int priority = parseField(payload, "priority").toInt();
            float watts = parseField(payload, "watts").toFloat();
            bool pausable = (parseField(payload, "pausable") == "true");

            if (priority <= 0 || priority > 4) priority = (int)PRIORITY_NORMAL;
            _arbiter.registerOrUpdateAppliance(mac.c_str(), name.c_str(), role.c_str(), 
                                               (AppliancePriority)priority, watts, pausable);
            response = "{\"status\":\"success\",\"message\":\"Appliance registered\"}";
            return true;
        }

        if (cmd == "probe_device") {
            String mac = parseField(payload, "mac");
            if (mac.length() > 0 && _arbiter.getLearner().startProbing(mac.c_str())) {
                response = "{\"status\":\"success\",\"message\":\"Probing run started for device\"}";
                return true;
            } else {
                response = "{\"status\":\"error\",\"message\":\"Failed to start probing\"}";
                return false;
            }
        }

        response = "{\"status\":\"error\",\"message\":\"Unknown command\"}";
        return false;
    }

    String getStatusJson() override {
        const PowerProfileConfig& prof = _arbiter.getProfile();
        const PzemTelemetry& ac = _pzem.getTelemetry();
        const DcTelemetry& dc = _ina226.getTelemetry();
        const SdStatus& sd = _sd.getStatus();
        const VirtualMcbModel& mcb = _arbiter.getMcb();

        uint8_t devCount = 0;
        const RegisteredAppliance* devs = _arbiter.getDevices(devCount);

        String json = "{";
        
        // Power Profile & Arbiter Status
        json += "\"profile\":{";
        json += "\"preset\":" + String((int)prof.preset) + ",";
        json += "\"name\":\"" + String(prof.profileName) + "\",";
        json += "\"maxWatts\":" + String(prof.maxWatts, 1) + ",";
        json += "\"thermalBufferPct\":" + String((int)prof.thermalBufferPct) + ",";
        json += "\"effectiveCeilingWatts\":" + String(prof.effectiveCeilingWatts, 1) + ",";
        json += "\"activePermits\":" + String((int)_arbiter.getActivePermitCount()) + ",";
        json += "\"surgeAlert\":" + String(_arbiter.isSurgeAlertActive() ? "true" : "false");
        json += "},";

        // Virtual MCB Thermal Model
        json += "\"mcb\":{";
        json += "\"thermalHeatPct\":" + String((int)mcb.getThermalHeatPct()) + ",";
        json += "\"isBreakerHot\":" + String(mcb.isBreakerHot() ? "true" : "false") + ",";
        json += "\"timeToTripSec\":" + String((unsigned long)mcb.estimateTimeToTripSec(ac.power));
        json += "},";

        // PZEM-004T Grid AC Telemetry
        json += "\"ac\":{";
        json += "\"online\":" + String(ac.isOnline ? "true" : "false") + ",";
        json += "\"voltage\":" + String(ac.voltage, 1) + ",";
        json += "\"current\":" + String(ac.current, 3) + ",";
        json += "\"power\":" + String(ac.power, 1) + ",";
        json += "\"energy\":" + String(ac.energy, 3) + ",";
        json += "\"frequency\":" + String(ac.frequency, 1) + ",";
        json += "\"powerFactor\":" + String(ac.powerFactor, 2);
        json += "},";

        // INA226 DC Battery Telemetry
        json += "\"battery\":{";
        json += "\"online\":" + String(dc.isOnline ? "true" : "false") + ",";
        json += "\"voltage\":" + String(dc.busVoltageV, 2) + ",";
        json += "\"currentMa\":" + String(dc.shuntCurrentMa, 1) + ",";
        json += "\"powerMw\":" + String(dc.powerMw, 1) + ",";
        json += "\"socPct\":" + String((int)dc.batterySocPct);
        json += "},";

        // MicroSD Status
        json += "\"sd\":{";
        json += "\"mounted\":" + String(sd.isMounted ? "true" : "false") + ",";
        json += "\"totalMb\":" + String((uint32_t)(sd.totalBytes / (1024 * 1024))) + ",";
        json += "\"usedMb\":" + String((uint32_t)(sd.usedBytes / (1024 * 1024))) + ",";
        json += "\"recordsToday\":" + String(sd.recordsWrittenToday) + ",";
        json += "\"errors\":" + String(sd.writeErrors) + ",";
        json += "\"logFile\":\"" + String(sd.currentLogFile) + "\"";
        json += "},";

        // Registered Appliances with Learned Signatures
        json += "\"devices\":[";
        for (uint8_t i = 0; i < devCount; i++) {
            if (i > 0) json += ",";
            ApplianceSignature* sig = _arbiter.getLearner().getSignature(devs[i].mac);

            json += "{";
            json += "\"mac\":\"" + String(devs[i].mac) + "\",";
            json += "\"name\":\"" + String(devs[i].name) + "\",";
            json += "\"role\":\"" + String(devs[i].role) + "\",";
            json += "\"priority\":" + String((int)devs[i].priority) + ",";
            json += "\"ratedWatts\":" + String(devs[i].ratedWatts, 1) + ",";
            json += "\"learnedSteadyWatts\":" + String(sig ? sig->learnedSteadyWatts : 0.0f, 1) + ",";
            json += "\"learnedSurgeWatts\":" + String(sig ? sig->learnedSurgeWatts : 0.0f, 1) + ",";
            json += "\"isProbed\":" + String((sig && sig->isProbed) ? "true" : "false") + ",";
            json += "\"isOverLimit\":" + String((sig && sig->isOverLimit) ? "true" : "false") + ",";
            json += "\"pausable\":" + String(devs[i].isPausable ? "true" : "false") + ",";
            json += "\"online\":" + String(devs[i].isOnline ? "true" : "false") + ",";
            json += "\"activePermit\":" + String(devs[i].hasActivePermit ? "true" : "false");
            json += "}";
        }
        json += "],";

        json += "\"pairingActive\":" + String(_pairingWindowActive ? "true" : "false");
        json += "}";
        return json;
    }

    bool isSafeToUpdate() const override {
        // Safe to update if no emergency surge is underway
        return !_arbiter.isSurgeAlertActive();
    }

    void prepareForUpdate() override {
        // Flush all pending logs to SD prior to reboot
        _sd.flushBuffer();
    }

    void openPairingWindow(uint32_t seconds = 60) {
        _pairingWindowActive = true;
        _pairingOpenedMs = millis();
        digitalWrite(PIN_LED_STATUS, HIGH); // Light up LED to indicate pairing mode
    }

    PzemReader&   getPzem()    { return _pzem; }
    Ina226Reader& getIna226()  { return _ina226; }
    SdDatalogger& getSd()      { return _sd; }
    PowerArbiter& getArbiter() { return _arbiter; }

private:
    PzemReader   _pzem;
    Ina226Reader _ina226;
    SdDatalogger _sd;
    PowerArbiter _arbiter;

    bool     _pairingWindowActive;
    uint32_t _pairingOpenedMs;
    bool     _buzzerMuted;
    uint32_t _lastBuzzerToggleMs;

    void handleSurgeAlarm() {
        if (_arbiter.isSurgeAlertActive()) {
            // Pulse buzzer every 500ms when breaker is in danger
            if (millis() - _lastBuzzerToggleMs > 500) {
                _lastBuzzerToggleMs = millis();
                digitalWrite(PIN_BUZZER_ALARM, !digitalRead(PIN_BUZZER_ALARM));
                digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
            }
        } else if (!_pairingWindowActive) {
            digitalWrite(PIN_BUZZER_ALARM, LOW);
            digitalWrite(PIN_LED_STATUS, LOW);
        }
    }

    String parseField(const String& json, const String& field) {
        String key = "\"" + field + "\":";
        int start = json.indexOf(key);
        if (start < 0) return "";
        start += key.length();

        while (start < json.length() && (json[start] == ' ' || json[start] == '\"')) {
            start++;
        }

        int end = start;
        while (end < json.length() && json[end] != '\"' && json[end] != ',' && json[end] != '}') {
            end++;
        }
        return json.substring(start, end);
    }
};
