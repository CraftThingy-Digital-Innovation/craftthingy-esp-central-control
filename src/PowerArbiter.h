#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "CentralConfig.h"
#include "HardwarePins.h"
#include "PzemReader.h"
#include "VirtualMcbModel.h"
#include "ApplianceLearner.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Predictive Power Arbiter & Priority Engine
 * ============================================================================
 * Enforces the household power ceiling (1300W default) with:
 * 1. Virtual MCB Thermal Model (I²t bimetal emulation) preventing breaker trips.
 * 2. Appliance Signature Learner for startup inrush profiling and auto-calibration.
 * 3. 2-Second Predictive Inrush Siphoning for motor spin-up without tripping.
 * 4. Priority arbitration queue and dynamic emergency load-shedding.
 */
class PowerArbiter {
public:
    PowerArbiter() : 
        _tokenCounter(100), 
        _deviceCount(0), 
        _activeTokenCount(0), 
        _surgeAlertActive(false),
        _inrushSiphonExpireMs(0)
    {
        // Default to PLN 1300VA profile with 10% thermal headroom (1170W safe limit)
        _profile.preset = PRESET_1300W;
        _profile.maxWatts = DEFAULT_MAX_HOUSE_WATTS;
        _profile.thermalBufferPct = 10;
        _profile.calculateEffectiveCeiling();
        strncpy(_profile.profileName, "PLN 1300VA (Default)", sizeof(_profile.profileName) - 1);

        memset(_devices, 0, sizeof(_devices));
        memset(_activeTokens, 0, sizeof(_activeTokens));
    }

    void begin() {
        loadProfileFromNvs();
        _mcb.begin(_profile.maxWatts);
        _learner.begin();
    }

    // ------------------------------------------------------------------------
    // Power Profile Management
    // ------------------------------------------------------------------------
    void setProfilePreset(PowerProfilePreset preset, float customWatts = 1300.0f) {
        _profile.preset = preset;
        switch (preset) {
            case PRESET_450W:
                _profile.maxWatts = 450.0f;
                strncpy(_profile.profileName, "PLN 450VA / Emergency", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_900W:
                _profile.maxWatts = 900.0f;
                strncpy(_profile.profileName, "PLN 900VA", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_1300W:
                _profile.maxWatts = 1300.0f;
                strncpy(_profile.profileName, "PLN 1300VA (Standard)", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_2200W:
                _profile.maxWatts = 2200.0f;
                strncpy(_profile.profileName, "PLN 2200VA (High)", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_3500W:
                _profile.maxWatts = 3500.0f;
                strncpy(_profile.profileName, "PLN 3500VA (Large)", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_INVERTER_UPS:
                _profile.maxWatts = 800.0f;
                strncpy(_profile.profileName, "Battery Inverter Mode", sizeof(_profile.profileName) - 1);
                break;
            case PRESET_CUSTOM:
            default:
                _profile.maxWatts = (customWatts > 100.0f) ? customWatts : 1300.0f;
                snprintf(_profile.profileName, sizeof(_profile.profileName), "Custom (%.0fW)", _profile.maxWatts);
                break;
        }

        _profile.calculateEffectiveCeiling();
        _mcb.setRatedWatts(_profile.maxWatts);
        saveProfileToNvs();
    }

    void setThermalBufferPct(uint8_t bufferPct) {
        if (bufferPct <= 30) {
            _profile.thermalBufferPct = bufferPct;
            _profile.calculateEffectiveCeiling();
            saveProfileToNvs();
        }
    }

    const PowerProfileConfig& getProfile() const {
        return _profile;
    }

    // ------------------------------------------------------------------------
    // Device Registration & Priority Management
    // ------------------------------------------------------------------------
    bool registerOrUpdateAppliance(const char* mac, const char* name, const char* role,
                                   AppliancePriority priority, float ratedWatts, bool isPausable,
                                   const char* ip = nullptr, int8_t rssi = 0) {
        if (!mac || strlen(mac) == 0) return false;

        int idx = findDeviceIndex(mac);
        if (idx < 0) {
            if (_deviceCount >= MAX_REGISTERED_DEVICES) return false;
            idx = _deviceCount++;
            memset(&_devices[idx], 0, sizeof(RegisteredAppliance));
            strncpy(_devices[idx].mac, mac, sizeof(_devices[idx].mac) - 1);
        }

        if (name && strlen(name) > 0) strncpy(_devices[idx].name, name, sizeof(_devices[idx].name) - 1);
        if (role && strlen(role) > 0) strncpy(_devices[idx].role, role, sizeof(_devices[idx].role) - 1);
        if (ip && strlen(ip) > 0) strncpy(_devices[idx].ip, ip, sizeof(_devices[idx].ip) - 1);
        if (rssi != 0) _devices[idx].rssi = rssi;
        _devices[idx].priority = priority;
        _devices[idx].ratedWatts = ratedWatts;
        _devices[idx].isPausable = isPausable;
        _devices[idx].isOnline = true;
        _devices[idx].lastSeenMillis = millis();
        saveDevicesToNvs();
        return true;
    }

    bool updateDevicePriority(const char* mac, AppliancePriority priority, float ratedWatts, bool isPausable) {
        int idx = findDeviceIndex(mac);
        if (idx < 0) return false;
        _devices[idx].priority = priority;
        if (ratedWatts > 0.0f) _devices[idx].ratedWatts = ratedWatts;
        _devices[idx].isPausable = isPausable;
        saveDevicesToNvs();
        return true;
    }

    bool removeDevice(const char* mac) {
        int idx = findDeviceIndex(mac);
        if (idx < 0) return false;
        for (uint8_t i = idx; i < _deviceCount - 1; i++) {
            _devices[i] = _devices[i + 1];
        }
        _deviceCount--;
        saveDevicesToNvs();
        return true;
    }

    bool setDeviceOverride(const char* mac, bool isOverridden) {
        int idx = findDeviceIndex(mac);
        if (idx < 0) return false;
        _devices[idx].isOverridden = isOverridden;
        return true;
    }

    void updateHeartbeat(const char* mac, const char* ip = nullptr, int8_t rssi = 0) {
        int idx = findDeviceIndex(mac);
        if (idx >= 0) {
            _devices[idx].isOnline = true;
            _devices[idx].lastSeenMillis = millis();
            if (ip && strlen(ip) > 0) strncpy(_devices[idx].ip, ip, sizeof(_devices[idx].ip) - 1);
            if (rssi != 0) _devices[idx].rssi = rssi;
        }
    }

    const RegisteredAppliance* getDevices(uint8_t& count) const {
        count = _deviceCount;
        return _devices;
    }

    // ------------------------------------------------------------------------
    // Power Permit Token Protocol (Predictive Arbiter Engine)
    // ------------------------------------------------------------------------
    PowerPermitToken requestPermit(const char* clientMac, float requestedWatts, uint32_t durationSec, const PzemTelemetry& pzem) {
        PowerPermitToken token;
        memset(&token, 0, sizeof(PowerPermitToken));
        token.tokenId = ++_tokenCounter;
        strncpy(token.clientMac, clientMac ? clientMac : "unknown", sizeof(token.clientMac) - 1);
        token.startMillis = millis();
        token.maxDurationSec = (durationSec > 0 && durationSec <= 3600) ? durationSec : 300;

        int devIdx = findDeviceIndex(clientMac);
        AppliancePriority clientPriority = (devIdx >= 0) ? _devices[devIdx].priority : PRIORITY_NORMAL;
        if (devIdx >= 0) {
            strncpy(token.applianceRole, _devices[devIdx].role, sizeof(token.applianceRole) - 1);
        }

        // Check if device was previously disqualified by probing run (e.g. 1800W draw)
        ApplianceSignature* sig = _learner.getSignature(clientMac);
        if (sig && sig->isOverLimit) {
            token.status = TOKEN_REJECTED_OVERLOAD;
            token.grantedWatts = 0.0f;
            return token;
        }

        // Use learned steady-state and inrush surge values if profiled
        float effectiveWatts = (sig && sig->learnedSteadyWatts > 20.0f) ? sig->learnedSteadyWatts : requestedWatts;
        float expectedSurge = (sig && sig->learnedSurgeWatts > 20.0f) ? sig->learnedSurgeWatts : (effectiveWatts * 1.8f);
        uint32_t surgeDurMs = (sig && sig->learnedSurgeDurationMs > 0) ? sig->learnedSurgeDurationMs : 350;
        token.grantedWatts = effectiveWatts;

        float currentGridWatts = pzem.isOnline ? pzem.power : calculateAllocatedWatts();
        float availableMargin = _profile.effectiveCeilingWatts - currentGridWatts;

        // Predictive Virtual MCB Thermal Check:
        // Even if steady power fits, can the physical breaker's bimetal strip absorb the inrush spike?
        if (!_mcb.isSafeForInrushSpike(currentGridWatts, expectedSurge, surgeDurMs)) {
            // Breaker is thermally hot! Hold off motor start to prevent thermal tripping
            token.status = TOKEN_QUEUED;
            return token;
        }

        // Case A: Margin can accommodate BOTH steady power and inrush surge immediately
        if (availableMargin >= (effectiveWatts + expectedSurge * 0.5f)) {
            token.status = TOKEN_GRANTED;
            addActiveToken(token);
            if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
            _learner.onApplianceStart(clientMac, currentGridWatts);
            return token;
        }

        // Case B: Margin accommodates steady power, BUT inrush surge is too close to ceiling
        // Action: 2-Second Predictive Siphoning (pause lower-priority load just during spin-up!)
        if (availableMargin >= effectiveWatts) {
            for (uint8_t i = 0; i < _activeTokenCount; i++) {
                int activeDevIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (activeDevIdx >= 0 && _devices[activeDevIdx].isPausable && _devices[activeDevIdx].priority > clientPriority) {
                    // Temporarily pause for 3 seconds to clear the surge window
                    _activeTokens[i].status = TOKEN_PAUSED_BY_SHED;
                    _devices[activeDevIdx].hasActivePermit = false;
                    _inrushSiphonExpireMs = millis() + 3000; // Auto-resume in 3s

                    token.status = TOKEN_GRANTED;
                    addActiveToken(token);
                    if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
                    _learner.onApplianceStart(clientMac, currentGridWatts);
                    return token;
                }
            }
        }

        // Case C: Continuous power is tight -> High-priority full siphoning
        if (clientPriority == PRIORITY_CRITICAL) {
            for (uint8_t i = 0; i < _activeTokenCount; i++) {
                int activeDevIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (activeDevIdx >= 0 && _devices[activeDevIdx].isPausable && _devices[activeDevIdx].priority > PRIORITY_CRITICAL) {
                    // Full pause of lower-priority appliance for duration of pump run
                    _activeTokens[i].status = TOKEN_PAUSED_BY_SHED;
                    _devices[activeDevIdx].hasActivePermit = false;

                    token.status = TOKEN_GRANTED;
                    addActiveToken(token);
                    if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
                    _learner.onApplianceStart(clientMac, currentGridWatts);
                    return token;
                }
            }
        }

        // Case D: Cannot accommodate or siphon -> Queue request
        token.status = TOKEN_QUEUED;
        return token;
    }

    bool releasePermit(uint32_t tokenId) {
        for (uint8_t i = 0; i < _activeTokenCount; i++) {
            if (_activeTokens[i].tokenId == tokenId) {
                int devIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (devIdx >= 0) _devices[devIdx].hasActivePermit = false;

                // Slide array
                for (uint8_t j = i; j < _activeTokenCount - 1; j++) {
                    _activeTokens[j] = _activeTokens[j + 1];
                }
                _activeTokenCount--;

                // Resume any paused appliance
                resumePausedAppliances();
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------------
    // Periodic Supervisory Loop (Thermal MCB update, Probing & Surge defense)
    // ------------------------------------------------------------------------
    void loop(const PzemTelemetry& pzem) {
        uint32_t now = millis();
        float currentPower = pzem.isOnline ? pzem.power : calculateAllocatedWatts();

        // 1. Update Virtual MCB Thermal Accumulator
        _mcb.update(currentPower);

        // 2. Feed startup inrush observer with real-time waveform
        _learner.observeStartupSample(currentPower, pzem.powerFactor);

        // 3. Handle Predictive Inrush Siphon expiration (auto-resume after 3s motor spin-up)
        if (_inrushSiphonExpireMs > 0 && now >= _inrushSiphonExpireMs) {
            _inrushSiphonExpireMs = 0;
            resumePausedAppliances();
        }

        // 4. Expire expired normal tokens
        for (int i = (int)_activeTokenCount - 1; i >= 0; i--) {
            uint32_t elapsedSec = (now - _activeTokens[i].startMillis) / 1000;
            if (elapsedSec > _activeTokens[i].maxDurationSec) {
                releasePermit(_activeTokens[i].tokenId);
            }
        }

        // 5. Device heartbeat timeouts (60 seconds)
        for (uint8_t i = 0; i < _deviceCount; i++) {
            if (_devices[i].isOnline && (now - _devices[i].lastSeenMillis > 60000)) {
                _devices[i].isOnline = false;
            }
        }

        // 6. Thermal Breaker Emergency Protection:
        // If Virtual MCB heat crosses 88% or PZEM physical watts breach ceiling:
        if (_mcb.isNearTripThreshold() || (pzem.isOnline && pzem.power >= (_profile.maxWatts - 40.0f))) {
            _surgeAlertActive = true;
            // Emergency: Immediately shed all pausable loads!
            for (uint8_t i = 0; i < _activeTokenCount; i++) {
                int devIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (devIdx >= 0 && _devices[devIdx].isPausable && _activeTokens[i].status == TOKEN_GRANTED) {
                    _activeTokens[i].status = TOKEN_PAUSED_BY_SHED;
                    _devices[devIdx].hasActivePermit = false;
                }
            }
        } else if (_surgeAlertActive && !_mcb.isBreakerHot() && pzem.power < (_profile.effectiveCeilingWatts - 150.0f)) {
            _surgeAlertActive = false;
            resumePausedAppliances();
        }
    }

    uint8_t getActivePermitCount() const {
        return _activeTokenCount;
    }

    bool isSurgeAlertActive() const {
        return _surgeAlertActive;
    }

    const VirtualMcbModel& getMcb() const {
        return _mcb;
    }

    ApplianceLearner& getLearner() {
        return _learner;
    }

    int findDeviceIndex(const char* mac) const {
        if (!mac) return -1;
        for (uint8_t i = 0; i < _deviceCount; i++) {
            if (strcasecmp(_devices[i].mac, mac) == 0) return i;
        }
        return -1;
    }

    bool isDeviceRegistered(const char* mac) const {
        return findDeviceIndex(mac) >= 0;
    }

private:
    PowerProfileConfig  _profile;
    uint32_t            _tokenCounter;
    uint8_t             _deviceCount;
    RegisteredAppliance _devices[MAX_REGISTERED_DEVICES];
    uint8_t             _activeTokenCount;
    PowerPermitToken    _activeTokens[MAX_REGISTERED_DEVICES];
    bool                _surgeAlertActive;
    uint32_t            _inrushSiphonExpireMs;

    VirtualMcbModel     _mcb;
    ApplianceLearner    _learner;

    void addActiveToken(const PowerPermitToken& token) {
        if (_activeTokenCount < MAX_REGISTERED_DEVICES) {
            _activeTokens[_activeTokenCount++] = token;
        }
    }

    float calculateAllocatedWatts() {
        float sum = 0.0f;
        for (uint8_t i = 0; i < _activeTokenCount; i++) {
            if (_activeTokens[i].status == TOKEN_GRANTED) {
                sum += _activeTokens[i].grantedWatts;
            }
        }
        return sum;
    }

    void resumePausedAppliances() {
        for (uint8_t i = 0; i < _activeTokenCount; i++) {
            if (_activeTokens[i].status == TOKEN_PAUSED_BY_SHED) {
                _activeTokens[i].status = TOKEN_GRANTED;
                int devIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
            }
        }
    }

    void loadProfileFromNvs() {
        Preferences prefs;
        if (prefs.begin("power_arb", true)) {
            uint8_t p = prefs.getUChar("preset", (uint8_t)PRESET_1300W);
            float customW = prefs.getFloat("maxWatts", 1300.0f);
            uint8_t buf = prefs.getUChar("bufferPct", 10);
            
            // Restore registered appliances across reboots / OTA (v2 format)
            uint8_t count = prefs.getUChar("dev_cnt_v2", 0);
            if (count > MAX_REGISTERED_DEVICES) count = MAX_REGISTERED_DEVICES;
            _deviceCount = count;
            if (_deviceCount > 0) {
                size_t expected = _deviceCount * sizeof(RegisteredAppliance);
                if (prefs.getBytesLength("devices_v2") == expected) {
                    prefs.getBytes("devices_v2", _devices, expected);
                } else {
                    _deviceCount = 0;
                    memset(_devices, 0, sizeof(_devices));
                }
            } else {
                memset(_devices, 0, sizeof(_devices));
            }

            // Enforce clean string termination & volatile state reset
            for (uint8_t i = 0; i < MAX_REGISTERED_DEVICES; i++) {
                _devices[i].mac[sizeof(_devices[i].mac) - 1] = '\0';
                _devices[i].name[sizeof(_devices[i].name) - 1] = '\0';
                _devices[i].role[sizeof(_devices[i].role) - 1] = '\0';
                _devices[i].ip[sizeof(_devices[i].ip) - 1] = '\0';
                _devices[i].isOnline = false;
                _devices[i].hasActivePermit = false;
                _devices[i].lastSeenMillis = 0;
            }
            prefs.end();

            setProfilePreset((PowerProfilePreset)p, customW);
            setThermalBufferPct(buf);
        }
    }

    void saveProfileToNvs() {
        Preferences prefs;
        if (prefs.begin("power_arb", false)) {
            prefs.putUChar("preset", (uint8_t)_profile.preset);
            prefs.putFloat("maxWatts", _profile.maxWatts);
            prefs.putUChar("bufferPct", _profile.thermalBufferPct);
            prefs.end();
        }
    }

    void saveDevicesToNvs() {
        Preferences prefs;
        if (prefs.begin("power_arb", false)) {
            prefs.putUChar("dev_cnt_v2", _deviceCount);
            if (_deviceCount > 0) {
                prefs.putBytes("devices_v2", _devices, _deviceCount * sizeof(RegisteredAppliance));
            }
            prefs.end();
        }
    }
};
