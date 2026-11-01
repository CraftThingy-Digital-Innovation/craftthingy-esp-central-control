#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
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
        // Configure optional UI pins (safely skipped if set to -1)
        if (PIN_BUTTON_PAIR >= 0) pinMode(PIN_BUTTON_PAIR, INPUT_PULLUP);
        if (PIN_LED_STATUS >= 0) {
            pinMode(PIN_LED_STATUS, OUTPUT);
            digitalWrite(PIN_LED_STATUS, LOW);
        }
        if (PIN_BUZZER_ALARM >= 0) {
            pinMode(PIN_BUZZER_ALARM, OUTPUT);
            digitalWrite(PIN_BUZZER_ALARM, LOW);
        }

        Serial.println(F("[INIT] Initializing Central Control subsystems..."));

        // Initialize subsystems
        _pzem.begin();
        Serial.printf("[INIT] PZEM-004T AC Sensor (RX:%d, TX:%d): Modbus-RTU 9600 baud initialized\n", PIN_PZEM_RX, PIN_PZEM_TX);

        bool inaOk = _ina226.begin(INA226_I2C_ADDR, INA226_SHUNT_OHMS);
        Serial.printf("[INIT] INA226 DC Sensor (I2C 0x%02X, SDA:%d, SCL:%d): %s\n", 
                      INA226_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL, 
                      inaOk ? "ONLINE (Ready)" : "NOT DETECTED (Offline)");

        bool sdOk = _sd.begin();
        Serial.printf("[INIT] 8GB MicroSD Logger (CS:%d, MOSI:%d, MISO:%d, SCK:%d): %s\n", 
                      PIN_SD_CS, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_SCK, 
                      sdOk ? "MOUNTED (FAT32)" : "NO CARD / UNMOUNTED");

        _arbiter.begin();
        Serial.printf("[INIT] 1300W Energy Arbiter: ACTIVE (Ceiling: %.0fW, Nominal Breaker: %.0fW, Cushion: %d%%)\n",
                      _arbiter.getProfile().effectiveCeilingWatts,
                      _arbiter.getProfile().maxWatts,
                      _arbiter.getProfile().thermalBufferPct);

        if (MDNS.begin("craftthingy-central")) {
            MDNS.addService("craftthingy", "tcp", 80);
            MDNS.addService("http", "tcp", 80);
            Serial.println(F("[INIT] mDNS responder started: craftthingy-central.local"));
        }

        Serial.println(F("[INIT] System ready. Commencing 1Hz energy monitoring & load arbitration."));
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

        // Periodic Serial telemetry heartbeat (every 5 seconds)
        static uint32_t lastSerialHeartbeatMs = 0;
        if (millis() - lastSerialHeartbeatMs >= 5000) {
            lastSerialHeartbeatMs = millis();

            // Periodic SD mount retry if card was inserted after boot
            if (!_sd.isMounted()) {
                if (_sd.retryMount()) {
                    Serial.println(F("[SD] -> MicroSD Card detected & MOUNTED successfully (FAT32)!"));
                }
            }

            String wifiModeStr = "OFF";
            String ipStr = "0.0.0.0";
            if (WiFi.status() == WL_CONNECTED) {
                wifiModeStr = "STA (" + WiFi.SSID() + ")";
                ipStr = WiFi.localIP().toString();
            } else if (WiFi.getMode() & WIFI_AP) {
                wifiModeStr = "AP (" + WiFiManager::getApSSID() + ")";
                ipStr = WiFi.softAPIP().toString();
            } else {
                wifiModeStr = "CONNECTING";
            }

            Serial.printf("[STATUS] AC: %.1fV %.2fA %.1fW (pzem:%s) | DC: %.2fV %.2fA %.1fW (ina:%s) | SD:%s | WiFi:%s [%s] | Heap:%u B\n",
                          ac.voltage, ac.current, ac.power, ac.isOnline ? "ONLINE" : "WAITING_220V",
                          dc.busVoltageV, dc.shuntCurrentMa / 1000.0f, dc.powerMw / 1000.0f, dc.isOnline ? "ONLINE" : "OFFLINE",
                          _sd.isMounted() ? "MOUNTED" : "NO_CARD",
                          wifiModeStr.c_str(), ipStr.c_str(),
                          ESP.getFreeHeap());
        }

        // Optional Physical 60-Second Pairing Button Monitor (if wired)
        if (PIN_BUTTON_PAIR >= 0 && digitalRead(PIN_BUTTON_PAIR) == LOW) {
            delay(50); // Debounce
            if (digitalRead(PIN_BUTTON_PAIR) == LOW && !_pairingWindowActive) {
                openPairingWindow(60);
            }
        }

        // Expire Pairing Window after timeout
        if (_pairingWindowActive && (millis() - _pairingOpenedMs > 60000)) {
            _pairingWindowActive = false;
            if (PIN_LED_STATUS >= 0) digitalWrite(PIN_LED_STATUS, LOW);
        }

        // Periodic Satellite Heartbeat & Smart Dry-Run Supervisor (every 10 seconds)
        static uint32_t lastSatelliteProbeMs = 0;
        if (WiFi.status() == WL_CONNECTED && (millis() - lastSatelliteProbeMs >= 10000)) {
            lastSatelliteProbeMs = millis();
            uint8_t count = 0;
            const RegisteredAppliance* devs = _arbiter.getDevices(count);
            for (uint8_t i = 0; i < count; i++) {
                // Only poll online devices to prevent TCP connect stall on offline IPs
                if (devs[i].isOnline && strlen(devs[i].ip) > 0 && strcmp(devs[i].ip, "--") != 0 && strcmp(devs[i].ip, "0.0.0.0") != 0) {
                    WiFiClient client;
                    client.setTimeout(1);
                    HTTPClient http;
                    http.setConnectTimeout(350);
                    http.begin(client, "http://" + String(devs[i].ip) + "/api/v1/system/status");
                    http.setTimeout(450);
                    int code = http.GET();
                    if (code == 200) {
                        _arbiter.updateHeartbeat(devs[i].mac, devs[i].ip);
                        String body = http.getString();
                        superviseWaterPumpNode(devs[i], body);
                    }
                    http.end();
                }
            }
        }

        // Background Auto-Discovery Loop (only when pairing gate is open)
        static uint32_t lastAutoDiscoverMs = 0;
        if (_pairingWindowActive && WiFi.status() == WL_CONNECTED && (millis() - lastAutoDiscoverMs >= 10000)) {
            lastAutoDiscoverMs = millis();
            scanNetworkNodes();
        }

        // Alarm Buzzer & Status LED logic (safely skipped if pins are -1)
        handleSurgeAlarm();

        // Process Serial USB provisioning commands (e.g. WIFI:SSID,PASSWORD)
        handleSerialCommands();
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
            // Payload format: {"mac":"XX:XX:XX","watts":500,"duration":300,"ip":"...","rssi":-45}
            String mac = parseField(payload, "mac");
            String ip = parseField(payload, "ip");
            int rssi = parseField(payload, "rssi").toInt();
            float watts = parseField(payload, "watts").toFloat();
            uint32_t dur = (uint32_t)parseField(payload, "duration").toInt();
            if (watts <= 0.0f) watts = 500.0f;
            if (dur == 0) dur = 300;

            if (mac.length() > 0) {
                _arbiter.updateHeartbeat(mac.c_str(), ip.length() > 0 ? ip.c_str() : nullptr, (int8_t)rssi);
            }

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

        if (cmd == "heartbeat") {
            String mac = parseField(payload, "mac");
            String ip = parseField(payload, "ip");
            _arbiter.updateHeartbeat(mac.c_str(), ip.length() > 0 ? ip.c_str() : nullptr);
            response = "{\"status\":\"success\",\"message\":\"Heartbeat acknowledged\"}";
            return true;
        }

        if (cmd == "register_device") {
            String mac = parseField(payload, "mac");
            String name = parseField(payload, "name");
            String role = parseField(payload, "role");
            String ip = parseField(payload, "ip");
            int priority = parseField(payload, "priority").toInt();
            float watts = parseField(payload, "watts").toFloat();
            bool pausable = (parseField(payload, "pausable") == "true");

            if (priority <= 0 || priority > 4) priority = (int)PRIORITY_NORMAL;
            _arbiter.registerOrUpdateAppliance(mac.c_str(), name.c_str(), role.c_str(), 
                                               (AppliancePriority)priority, watts, pausable,
                                               ip.length() > 0 ? ip.c_str() : nullptr);
            response = "{\"status\":\"success\",\"message\":\"Appliance registered\"}";
            return true;
        }

        if (cmd == "update_priority") {
            String mac = parseField(payload, "mac");
            int priority = parseField(payload, "priority").toInt();
            float watts = parseField(payload, "watts").toFloat();
            bool pausable = (parseField(payload, "pausable") == "true");

            if (priority <= 0 || priority > 4) priority = (int)PRIORITY_NORMAL;
            bool ok = _arbiter.updateDevicePriority(mac.c_str(), (AppliancePriority)priority, watts, pausable);
            response = ok ? "{\"status\":\"success\",\"message\":\"Priority updated\"}" : "{\"status\":\"error\",\"message\":\"Device not found\"}";
            return ok;
        }

        if (cmd == "remove_device" || cmd == "quarantine_device") {
            String mac = parseField(payload, "mac");
            bool ok = _arbiter.removeDevice(mac.c_str());
            response = ok ? "{\"status\":\"success\",\"message\":\"Device removed / quarantined\"}" : "{\"status\":\"error\",\"message\":\"Device not found\"}";
            return ok;
        }

        if (cmd == "override_device") {
            String mac = parseField(payload, "mac");
            String targetIp = parseField(payload, "ip");
            String action = parseField(payload, "action"); // "start", "stop", "auto"
            if (action.length() == 0) action = parseField(payload, "state");

            // Look up IP from registered devices if not passed
            if (targetIp.length() == 0 && mac.length() > 0) {
                uint8_t count = 0;
                const RegisteredAppliance* devs = _arbiter.getDevices(count);
                for (uint8_t i = 0; i < count; i++) {
                    if (strcmp(devs[i].mac, mac.c_str()) == 0) {
                        targetIp = devs[i].ip;
                        break;
                    }
                }
            }

            if (targetIp.length() == 0) {
                response = "{\"status\":\"error\",\"message\":\"No target IP found for device\"}";
                return false;
            }

            bool isOverridden = (action == "start" || action == "on" || action == "stop" || action == "off");
            _arbiter.setDeviceOverride(mac.c_str(), isOverridden);

            // Dispatch command to satellite node
            HTTPClient http;
            http.begin("http://" + targetIp + "/api/v1/device/command");
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(1500);

            String body = "{\"cmd\":\"" + action + "\"}";
            int code = http.POST(body);
            String payloadRes = (code > 0) ? http.getString() : "Timeout";
            http.end();

            if (code <= 0 || code >= 400) {
                // Fallback to /api/v1/pump/command if device was a pump
                http.begin("http://" + targetIp + "/api/v1/pump/command");
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(1500);
                code = http.POST(body);
                payloadRes = (code > 0) ? http.getString() : "Timeout";
                http.end();
            }

            response = "{\"status\":\"success\",\"code\":" + String(code) + ",\"remoteRes\":\"" + payloadRes + "\"}";
            return true;
        }

        if (cmd == "discover_nodes") {
            response = scanNetworkNodes();
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
            json += "\"ip\":\"" + String(devs[i].ip) + "\",";
            json += "\"rssi\":" + String((int)devs[i].rssi) + ",";
            json += "\"priority\":" + String((int)devs[i].priority) + ",";
            json += "\"ratedWatts\":" + String(devs[i].ratedWatts, 1) + ",";
            json += "\"learnedSteadyWatts\":" + String(sig ? sig->learnedSteadyWatts : 0.0f, 1) + ",";
            json += "\"learnedSurgeWatts\":" + String(sig ? sig->learnedSurgeWatts : 0.0f, 1) + ",";
            json += "\"isProbed\":" + String((sig && sig->isProbed) ? "true" : "false") + ",";
            json += "\"isOverLimit\":" + String((sig && sig->isOverLimit) ? "true" : "false") + ",";
            json += "\"pausable\":" + String(devs[i].isPausable ? "true" : "false") + ",";
            json += "\"online\":" + String(devs[i].isOnline ? "true" : "false") + ",";
            json += "\"isOverridden\":" + String(devs[i].isOverridden ? "true" : "false") + ",";
            json += "\"activePermit\":" + String(devs[i].hasActivePermit ? "true" : "false");
            json += "}";
        }
        json += "],";

        json += "\"pairingActive\":" + String(_pairingWindowActive ? "true" : "false") + ",";
        json += "\"diag\":{\"ina226\":" + _ina226.diagJson() + ",\"pzem\":" + _pzem.diagJson() + "}";
        json += "}";
        return json;
    }

    bool isSafeToUpdate() const override {
        // Safe to update as long as no active power permit is in flight
        return _arbiter.getActivePermitCount() == 0;
    }

    void prepareForUpdate() override {
        // Flush all pending logs to SD prior to reboot
        _sd.flushBuffer();
    }

    void openPairingWindow(uint32_t seconds = 60) {
        _pairingWindowActive = true;
        _pairingOpenedMs = millis();
        if (PIN_LED_STATUS >= 0) digitalWrite(PIN_LED_STATUS, HIGH);
    }

    bool handleHttpRequest(ApiServerType& server, const String& uri, const String& method) override {
        if (!_sd.isMounted()) return false;

        // 1. SD File Upload / Write endpoint (Supports chunking & append)
        if (uri == "/api/v1/sd/write" && method == "POST") {
            String path = server.hasArg("path") ? server.arg("path") : "";
            if (path.length() == 0) {
                server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing path parameter\"}");
                return true;
            }
            if (!path.startsWith("/")) path = "/" + path;
            if (path.indexOf("..") >= 0) {
                server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid path traversal\"}");
                return true;
            }

            bool isAppend = server.hasArg("append") && (server.arg("append") == "1" || server.arg("append") == "true");
            String content = server.hasArg("content") ? server.arg("content") : (server.hasArg("plain") ? server.arg("plain") : "");
            ensureParentDir(path);

            File f = SD.open(path, isAppend ? FILE_APPEND : FILE_WRITE);
            if (!f) {
                server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to open file for writing\"}");
                return true;
            }

            size_t written = 0;
            if (content.length() > 0) {
                written = f.print(content);
            } else {
                WiFiClient client = server.client();
                uint8_t sBuf[512];
                while (client.available()) {
                    int n = client.read(sBuf, sizeof(sBuf));
                    if (n > 0) {
                        written += f.write(sBuf, n);
                    }
                }
            }
            f.flush();
            f.close();

            char res[180];
            snprintf(res, sizeof(res), "{\"status\":\"success\",\"path\":\"%s\",\"bytes\":%u,\"append\":%s}", 
                     path.c_str(), (unsigned int)written, isAppend ? "true" : "false");
            server.send(200, "application/json", res);
            return true;
        }

        // 2. SD File List endpoint
        if (uri == "/api/v1/sd/list" && method == "GET") {
            String dirPath = server.hasArg("dir") ? server.arg("dir") : "/www";
            if (!dirPath.startsWith("/")) dirPath = "/" + dirPath;
            File root = SD.open(dirPath);
            if (!root || !root.isDirectory()) {
                if (root) root.close();
                server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Directory not found\"}");
                return true;
            }

            String json = "{\"status\":\"success\",\"dir\":\"" + dirPath + "\",\"files\":[";
            File file = root.openNextFile();
            bool first = true;
            while (file) {
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + String(file.name()) + "\",\"isDir\":" + (file.isDirectory() ? "true" : "false") + ",\"size\":" + String(file.size()) + "}";
                file = root.openNextFile();
            }
            root.close();
            json += "]}";
            server.send(200, "application/json", json);
            return true;
        }

        // 3. SD File Download endpoint
        if (uri == "/api/v1/sd/download" && method == "GET") {
            String path = server.hasArg("path") ? server.arg("path") : "";
            if (path.length() > 0 && serveSdFile(server, path, true)) {
                return true;
            }
            server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"File not found\"}");
            return true;
        }

        // 4. Root portal (/ or /index.html) -> Checks /www/index.html first, then /index.html
        if (uri == "/" || uri == "/index.html") {
            if (serveSdFile(server, "/www/index.html") || serveSdFile(server, "/index.html")) {
                return true;
            }
            return false;
        }

        // 5. Static file request from SD (/www/* or root-relative)
        if (method == "GET") {
            if (serveSdFile(server, uri)) {
                return true;
            }
        }

        return false;
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

    bool postSatelliteCommand(const String& targetIp, const String& cmd, const String& payload = "") {
        if (targetIp.length() == 0 || targetIp == "--" || targetIp == "0.0.0.0") return false;
        HTTPClient http;
        http.begin("http://" + targetIp + "/api/v1/device/command");
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(1200);
        String body = "{\"cmd\":\"" + cmd + "\",\"payload\":\"" + payload + "\"}";
        int code = http.POST(body);
        http.end();
        return (code == 200);
    }

    void superviseWaterPumpNode(const RegisteredAppliance& dev, const String& statusJson) {
        if (strcmp(dev.role, "water_pump") != 0 && statusJson.indexOf("\"role\":\"water_pump\"") < 0) {
            return;
        }

        bool pumpRunning = (statusJson.indexOf("\"pumpRunning\":true") >= 0);
        bool inPrimeProbe = (statusJson.indexOf("\"inPrimeProbe\":true") >= 0);

        // Parse run duration
        uint32_t runDurationSec = 0;
        int idxRun = statusJson.indexOf("\"runDurationSec\":");
        if (idxRun >= 0) {
            runDurationSec = statusJson.substring(idxRun + 17).toInt();
        }

        // Parse satellite reported power (if PZEM present on satellite)
        float satellitePower = 0.0f;
        int idxPwr = statusJson.indexOf("\"power\":");
        if (idxPwr >= 0) {
            satellitePower = statusJson.substring(idxPwr + 8).toFloat();
        }

        // Total Central PZEM telemetry
        float centralPower = _pzem.getTelemetry().power;

        // Resolve device dynamic electrical signature
        ApplianceSignature* sig = _arbiter.getLearner().getSignature(dev.mac);
        float expectedWatts = (sig && sig->learnedSteadyWatts > 30.0f) ? sig->learnedSteadyWatts : dev.ratedWatts;
        if (expectedWatts < 40.0f) expectedWatts = 280.0f; // safety floor

        // Dynamic thresholds derived proportionally from learned/rated power curve:
        // - Cavitation cutoff: < 55% of continuous operating load (free rotor air spin)
        // - Loaded threshold: >= 70% of continuous operating load (viscous fluid displacement)
        float cavitationCutoffWatts = expectedWatts * 0.55f;
        float loadedThresholdWatts = expectedWatts * 0.70f;

        // -------------------------------------------------------------
        // Pump is actively running: evaluate power health & lease
        // -------------------------------------------------------------
        if (pumpRunning) {
            // Give 5 seconds for inrush spike (800W) to settle into plateau (~200W)
            if (runDurationSec >= 5 || inPrimeProbe) {
                bool hasSatPzem = (statusJson.indexOf("\"hasPzem\":true") >= 0 && statusJson.indexOf("\"pzemOnline\":true") >= 0);
                bool isCavitating = false;
                bool isLoaded = false;

                if (hasSatPzem) {
                    if (satellitePower > 5.0f && satellitePower < cavitationCutoffWatts) {
                        isCavitating = true;
                    } else if (satellitePower >= loadedThresholdWatts) {
                        isLoaded = true;
                    }
                } else {
                    // Central PZEM assessment against expected load
                    if (centralPower >= loadedThresholdWatts) {
                        isLoaded = true;
                    }
                }

                if (isCavitating) {
                    Serial.printf("[SUPERVISOR] Cavitation / dry run detected on %s: %.1fW is below dynamic threshold (%.1fW)! Tripping...\n",
                                  dev.name, satellitePower, cavitationCutoffWatts);
                    postSatelliteCommand(dev.ip, "trip_dry_run", "cavitation_detected");
                } else if (isLoaded || !hasSatPzem) {
                    // Healthy pumping! Extend lease so the pump never falsely trips 15m timeout while filling
                    static uint32_t lastLeaseGrantMs = 0;
                    if (millis() - lastLeaseGrantMs >= 25000) { // grant lease renewal every 25-30s
                        lastLeaseGrantMs = millis();
                        Serial.printf("[SUPERVISOR] %s power healthy (%.1fW >= %.1fW). Extending runtime lease by 180s...\n",
                                      dev.name, hasSatPzem ? satellitePower : centralPower, loadedThresholdWatts);
                        postSatelliteCommand(dev.ip, "extend_lease", "180");
                    }
                }
            }
        }
    }

    void handleSurgeAlarm() {
        if (_arbiter.isSurgeAlertActive()) {
            // Pulse buzzer and LED every 500ms when breaker is in danger (if connected)
            if (millis() - _lastBuzzerToggleMs > 500) {
                _lastBuzzerToggleMs = millis();
                if (PIN_BUZZER_ALARM >= 0) digitalWrite(PIN_BUZZER_ALARM, !digitalRead(PIN_BUZZER_ALARM));
                if (PIN_LED_STATUS >= 0) digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
            }
        } else if (!_pairingWindowActive) {
            if (PIN_BUZZER_ALARM >= 0) digitalWrite(PIN_BUZZER_ALARM, LOW);
            if (PIN_LED_STATUS >= 0) digitalWrite(PIN_LED_STATUS, LOW);
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

    static String getMimeType(const String& path) {
        if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html; charset=utf-8";
        if (path.endsWith(".css")) return "text/css; charset=utf-8";
        if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
        if (path.endsWith(".json")) return "application/json";
        if (path.endsWith(".png")) return "image/png";
        if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
        if (path.endsWith(".gif")) return "image/gif";
        if (path.endsWith(".ico")) return "image/x-icon";
        if (path.endsWith(".svg")) return "image/svg+xml";
        if (path.endsWith(".csv")) return "text/csv; charset=utf-8";
        if (path.endsWith(".txt")) return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    bool serveSdFile(ApiServerType& server, const String& path, bool asAttachment = false) {
        if (!_sd.isMounted()) return false;
        if (path.indexOf("..") >= 0) return false;

        String fullPath = path;
        if (!SD.exists(fullPath)) {
            if (!fullPath.startsWith("/www")) {
                fullPath = "/www" + (fullPath.startsWith("/") ? fullPath : ("/" + fullPath));
            }
        }

        if (!SD.exists(fullPath)) return false;

        File f = SD.open(fullPath, FILE_READ);
        if (!f || f.isDirectory()) {
            if (f) f.close();
            return false;
        }

        server.sendHeader("Access-Control-Allow-Origin", "*");
        if (asAttachment) {
            int lastSlash = fullPath.lastIndexOf('/');
            String filename = (lastSlash >= 0) ? fullPath.substring(lastSlash + 1) : fullPath;
            server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        } else {
            server.sendHeader("Cache-Control", "no-cache");
        }

        String mime = getMimeType(fullPath);
        server.streamFile(f, mime);
        f.close();
        return true;
    }

    bool probeNode(const IPAddress& ip, String& outName, String& outMac, String& outRole) {
        if (ip == WiFi.localIP() || ip == IPAddress(0, 0, 0, 0)) return false;

        HTTPClient http;
        http.begin("http://" + ip.toString() + "/api/v1/system/status");
        http.setTimeout(400);
        int code = http.GET();
        if (code != 200) {
            http.end();
            return false;
        }

        String res = http.getString();
        http.end();

        // Check CraftThingy signature
        if (res.indexOf("craftthingy") < 0 && res.indexOf("CraftThingy") < 0 && res.indexOf("coreVersion") < 0) {
            return false;
        }

        outMac = parseField(res, "mac");
        outName = parseField(res, "name");
        outRole = parseField(res, "role");
        if (outRole.length() == 0) outRole = "appliance";
        if (outName.length() == 0) outName = (outRole == "water_pump") ? "Water Pump" : "Smart Appliance";

        return (outMac.length() > 0);
    }

    String scanNetworkNodes() {
        String json = "{\"status\":\"success\",\"discovered\":[";
        bool first = true;

        uint8_t count = 0;
        const RegisteredAppliance* devs = _arbiter.getDevices(count);

        auto checkPaired = [&](const String& mac, const String& ip) -> bool {
            for (uint8_t d = 0; d < count; d++) {
                if ((mac.length() > 0 && strcasecmp(devs[d].mac, mac.c_str()) == 0) ||
                    (ip.length() > 0 && String(devs[d].ip) == ip)) {
                    return true;
                }
            }
            return false;
        };

        // 1. Direct fast mDNS Host Lookup for water pump (300ms)
        IPAddress pumpIp = MDNS.queryHost("craftthingy-water-pump", 300);
        if (pumpIp != IPAddress(0, 0, 0, 0) && pumpIp != WiFi.localIP()) {
            String name, mac, role;
            if (probeNode(pumpIp, name, mac, role)) {
                bool isPaired = checkPaired(mac, pumpIp.toString());
                if (!first) json += ",";
                first = false;
                json += "{";
                json += "\"host\":\"craftthingy-water-pump.local\",";
                json += "\"ip\":\"" + pumpIp.toString() + "\",";
                json += "\"port\":80,";
                json += "\"isPaired\":" + String(isPaired ? "true" : "false") + ",";
                json += "\"role\":\"" + role + "\",";
                json += "\"name\":\"" + name + "\",";
                json += "\"mac\":\"" + mac + "\"";
                json += "}";
            }
        }

        // 2. Query mDNS PTR services
        int n = MDNS.queryService("http", "tcp");
        for (int i = 0; i < n; ++i) {
            String host = MDNS.hostname(i);
            IPAddress ip = MDNS.IP(i);
            uint16_t port = MDNS.port(i);
            if (ip == WiFi.localIP() || ip == IPAddress(0, 0, 0, 0) || ip == pumpIp) continue;

            String name, mac, role;
            if (probeNode(ip, name, mac, role)) {
                bool isPaired = checkPaired(mac, ip.toString());
                if (!first) json += ",";
                first = false;
                json += "{";
                json += "\"host\":\"" + host + "\",";
                json += "\"ip\":\"" + ip.toString() + "\",";
                json += "\"port\":" + String(port) + ",";
                json += "\"isPaired\":" + String(isPaired ? "true" : "false") + ",";
                json += "\"role\":\"" + role + "\",";
                json += "\"name\":\"" + name + "\",";
                json += "\"mac\":\"" + mac + "\"";
                json += "}";
            }
        }

        json += "]}";
        return json;
    }

    void ensureParentDir(const String& path) {
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0) {
            String dir = path.substring(0, lastSlash);
            if (!SD.exists(dir)) {
                SD.mkdir(dir);
            }
        }
    }

    void handleSerialCommands() {
        if (!Serial.available()) return;
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return;

        if (line.startsWith("WIFI:") || line.startsWith("wifi:")) {
            String payload = line.substring(5);
            int commaIdx = payload.indexOf(',');
            if (commaIdx < 0) commaIdx = payload.indexOf(':');

            if (commaIdx > 0) {
                String ssid = payload.substring(0, commaIdx);
                String pass = payload.substring(commaIdx + 1);
                ssid.trim();
                pass.trim();

                Serial.printf("\n[PROVISION] Storing Wi-Fi credentials for SSID: '%s'...\n", ssid.c_str());
                ConfigStore::setWiFiConfig(ssid, pass);
                Serial.println(F("[PROVISION] Saved to NVS! Connecting to Wi-Fi now..."));
                WiFiManager::connectSTA(ssid.c_str(), pass.c_str(), 10);
            } else {
                Serial.println(F("\n[PROVISION] Format error! Use: WIFI:SSID,PASSWORD"));
            }
        } else if (line.equalsIgnoreCase("SCAN")) {
            Serial.println(F("\n[WIFI] Scanning nearby 2.4GHz Wi-Fi networks..."));
            wifi_mode_t origMode = WiFi.getMode();
            if (!(origMode & WIFI_MODE_STA)) {
                WiFi.mode(WIFI_MODE_APSTA);
            }
            int n = WiFi.scanNetworks(false, false, false, 250);
            if (n <= 0) {
                Serial.println(F("[WIFI] No networks found or scan failed."));
            } else {
                for (int i = 0; i < n; i++) {
                    Serial.printf("  [%d] %-24s (RSSI: %3d dBm, Ch: %2d, %s)\n",
                                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURED");
                }
                Serial.printf("[WIFI] Scan complete: %d networks found.\n", n);
            }
            WiFi.scanDelete();
            if (!(origMode & WIFI_MODE_STA) && WiFi.status() != WL_CONNECTED) {
                WiFi.mode(origMode);
            }
        } else if (line.equalsIgnoreCase("STATUS") || line.equalsIgnoreCase("IP")) {
            Serial.printf("\n[WIFI] Mode: %s | SSID: %s | IP: %s\n",
                          WiFi.status() == WL_CONNECTED ? "STA" : "AP",
                          WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : WiFiManager::getApSSID().c_str(),
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
        } else if (line.equalsIgnoreCase("HELP")) {
            Serial.println(F("\n--- Central Control Serial Commands ---"));
            Serial.println(F("  WIFI:<SSID>,<PASSWORD>  - Save credentials & connect to home Wi-Fi"));
            Serial.println(F("  SCAN                    - Scan and list nearby Wi-Fi networks"));
            Serial.println(F("  STATUS                  - Display IP address & current connection"));
            Serial.println(F("--------------------------------------"));
        }
    }
};
