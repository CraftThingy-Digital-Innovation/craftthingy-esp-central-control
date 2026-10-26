#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "CentralConfig.h"
#include "HardwarePins.h"
#include "PzemReader.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Power Arbiter & Priority Coordination Engine
 * ============================================================================
 * Enforces the household power ceiling (1300W default) with a 10% thermal trip
 * cushion. Arbitrates permits, queues requests, and coordinates load siphoning.
 */
class PowerArbiter {
public:
    PowerArbiter() : _tokenCounter(100), _deviceCount(0), _activeTokenCount(0), _surgeAlertActive(false) {
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
                                   AppliancePriority priority, float ratedWatts, bool isPausable) {
        if (!mac || strlen(mac) == 0) return false;

        int idx = findDeviceIndex(mac);
        if (idx < 0) {
            if (_deviceCount >= MAX_REGISTERED_DEVICES) return false;
            idx = _deviceCount++;
            strncpy(_devices[idx].mac, mac, sizeof(_devices[idx].mac) - 1);
        }

        if (name && strlen(name) > 0) strncpy(_devices[idx].name, name, sizeof(_devices[idx].name) - 1);
        if (role && strlen(role) > 0) strncpy(_devices[idx].role, role, sizeof(_devices[idx].role) - 1);
        _devices[idx].priority = priority;
        _devices[idx].ratedWatts = ratedWatts;
        _devices[idx].isPausable = isPausable;
        _devices[idx].isOnline = true;
        _devices[idx].lastSeenMillis = millis();
        return true;
    }

    void updateHeartbeat(const char* mac) {
        int idx = findDeviceIndex(mac);
        if (idx >= 0) {
            _devices[idx].isOnline = true;
            _devices[idx].lastSeenMillis = millis();
        }
    }

    const RegisteredAppliance* getDevices(uint8_t& count) const {
        count = _deviceCount;
        return _devices;
    }

    // ------------------------------------------------------------------------
    // Power Permit Token Protocol (Arbiter Engine)
    // ------------------------------------------------------------------------
    PowerPermitToken requestPermit(const char* clientMac, float requestedWatts, uint32_t durationSec, const PzemTelemetry& pzem) {
        PowerPermitToken token;
        memset(&token, 0, sizeof(PowerPermitToken));
        token.tokenId = ++_tokenCounter;
        strncpy(token.clientMac, clientMac ? clientMac : "unknown", sizeof(token.clientMac) - 1);
        token.grantedWatts = requestedWatts;
        token.startMillis = millis();
        token.maxDurationSec = (durationSec > 0 && durationSec <= 3600) ? durationSec : 300;

        int devIdx = findDeviceIndex(clientMac);
        AppliancePriority clientPriority = (devIdx >= 0) ? _devices[devIdx].priority : PRIORITY_NORMAL;
        if (devIdx >= 0) {
            strncpy(token.applianceRole, _devices[devIdx].role, sizeof(token.applianceRole) - 1);
        }

        float currentGridWatts = pzem.isOnline ? pzem.power : calculateAllocatedWatts();
        float availableMargin = _profile.effectiveCeilingWatts - currentGridWatts;

        // Case A: Margin is available immediately
        if (availableMargin >= requestedWatts) {
            token.status = TOKEN_GRANTED;
            addActiveToken(token);
            if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
            return token;
        }

        // Case B: Power is tight -> Check if high priority can siphon power by pausing lower priority
        if (clientPriority == PRIORITY_CRITICAL) {
            // Find an active pausable token with lower priority
            for (uint8_t i = 0; i < _activeTokenCount; i++) {
                int activeDevIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (activeDevIdx >= 0 && _devices[activeDevIdx].isPausable && _devices[activeDevIdx].priority > PRIORITY_CRITICAL) {
                    // Pause the lower priority appliance
                    _activeTokens[i].status = TOKEN_PAUSED_BY_SHED;
                    _devices[activeDevIdx].hasActivePermit = false;

                    // Siphon power to critical client
                    token.status = TOKEN_GRANTED;
                    addActiveToken(token);
                    if (devIdx >= 0) _devices[devIdx].hasActivePermit = true;
                    return token;
                }
            }
        }

        // Case C: Insufficient power and cannot siphon -> Queue request
        token.status = TOKEN_QUEUED;
        return token;
    }

    bool releasePermit(uint32_t tokenId) {
        for (uint8_t i = 0; i < _activeTokenCount; i++) {
            if (_activeTokens[i].tokenId == tokenId) {
                int devIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (devIdx >= 0) _devices[devIdx].hasActivePermit = false;

                // Remove token by sliding array
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
    // Periodic Supervisory Loop (Heartbeat timeouts & Surge check)
    // ------------------------------------------------------------------------
    void loop(const PzemTelemetry& pzem) {
        uint32_t now = millis();

        // Expire expired tokens
        for (int i = (int)_activeTokenCount - 1; i >= 0; i--) {
            uint32_t elapsedSec = (now - _activeTokens[i].startMillis) / 1000;
            if (elapsedSec > _activeTokens[i].maxDurationSec) {
                releasePermit(_activeTokens[i].tokenId);
            }
        }

        // Check device heartbeat timeouts (60 seconds)
        for (uint8_t i = 0; i < _deviceCount; i++) {
            if (_devices[i].isOnline && (now - _devices[i].lastSeenMillis > 60000)) {
                _devices[i].isOnline = false;
            }
        }

        // Physics check: If physical PZEM measures surge nearing ceiling, enforce emergency load-shed
        if (pzem.isOnline && pzem.power >= (_profile.maxWatts - 50.0f)) {
            _surgeAlertActive = true;
            // Emergency: Pause all pausable appliances immediately!
            for (uint8_t i = 0; i < _activeTokenCount; i++) {
                int devIdx = findDeviceIndex(_activeTokens[i].clientMac);
                if (devIdx >= 0 && _devices[devIdx].isPausable && _activeTokens[i].status == TOKEN_GRANTED) {
                    _activeTokens[i].status = TOKEN_PAUSED_BY_SHED;
                    _devices[devIdx].hasActivePermit = false;
                }
            }
        } else if (_surgeAlertActive && pzem.power < (_profile.effectiveCeilingWatts - 200.0f)) {
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

private:
    PowerProfileConfig  _profile;
    uint32_t            _tokenCounter;
    uint8_t             _deviceCount;
    RegisteredAppliance _devices[MAX_REGISTERED_DEVICES];
    uint8_t             _activeTokenCount;
    PowerPermitToken    _activeTokens[MAX_REGISTERED_DEVICES];
    bool                _surgeAlertActive;

    int findDeviceIndex(const char* mac) {
        if (!mac) return -1;
        for (uint8_t i = 0; i < _deviceCount; i++) {
            if (strcasecmp(_devices[i].mac, mac) == 0) return i;
        }
        return -1;
    }

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
};
