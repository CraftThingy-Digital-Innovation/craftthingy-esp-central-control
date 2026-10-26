#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "CentralConfig.h"

/**
 * ============================================================================
 * CraftThingy Central Control — Appliance Electrical Signature Learner
 * ============================================================================
 * Features:
 * 1. Automatic Inrush Surge Profiling: Captures peak inrush watts and duration
 *    every time a motor or inductive load starts up.
 * 2. Self-Calibrating Probing Run: Tests newly paired devices with a 3-second
 *    pulse during quiet grid conditions to discover true wattage without config hell.
 * 3. Over-Limit Countermeasure: Disqualifies appliances that exceed household limit.
 */

struct ApplianceSignature {
    char     mac[18];
    float    learnedSteadyWatts;        // Observed continuous power (W)
    float    learnedSurgeWatts;         // Observed peak inrush delta (W)
    uint32_t learnedSurgeDurationMs;    // Inrush duration (ms)
    float    learnedPowerFactor;        // Power factor (0.0 - 1.0)
    bool     isProbed;                  // Probing calibration completed
    bool     isOverLimit;               // Disqualified (exceeds breaker limit)
    uint16_t sampleCount;               // Number of runs averaged
    uint32_t lastObservedMillis;
};

enum ProbeState : uint8_t {
    PROBE_IDLE              = 0,
    PROBE_WAIT_QUIET_GRID   = 1,
    PROBE_CAPTURE_BASELINE  = 2,
    PROBE_PULSE_RUNNING     = 3,
    PROBE_EVALUATE          = 4,
    PROBE_COMPLETED         = 5,
    PROBE_FAILED_OVERLIMIT  = 6
};

class ApplianceLearner {
public:
    ApplianceLearner() : 
        _signatureCount(0), 
        _probeState(PROBE_IDLE),
        _probeStartMs(0),
        _baselineWatts(0.0f),
        _peakObservedWatts(0.0f),
        _targetMac(""),
        _activeClientMac("")
    {
        memset(_signatures, 0, sizeof(_signatures));
    }

    void begin() {
        loadSignaturesFromNvs();
    }

    // ------------------------------------------------------------------------
    // Signature Lookup & Retrieval
    // ------------------------------------------------------------------------
    ApplianceSignature* getSignature(const char* mac) {
        if (!mac || strlen(mac) == 0) return nullptr;
        for (uint8_t i = 0; i < _signatureCount; i++) {
            if (strcasecmp(_signatures[i].mac, mac) == 0) {
                return &_signatures[i];
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------
    // Startup Profiling (Triggered when an authorized device starts running)
    // ------------------------------------------------------------------------
    void onApplianceStart(const char* mac, float baselineWatts) {
        _activeClientMac = String(mac);
        _startObserveMs = millis();
        _baselineWatts = baselineWatts;
        _peakObservedWatts = baselineWatts;
        _isObservingStartup = true;
    }

    void observeStartupSample(float realTimeWatts, float powerFactor) {
        if (!_isObservingStartup) return;

        uint32_t elapsed = millis() - _startObserveMs;

        if (realTimeWatts > _peakObservedWatts) {
            _peakObservedWatts = realTimeWatts;
        }

        // After 2.5 seconds, the startup transient has settled into steady state
        if (elapsed >= 2500) {
            _isObservingStartup = false;
            float surgeDelta = _peakObservedWatts - _baselineWatts;
            float steadyDelta = realTimeWatts - _baselineWatts;

            if (surgeDelta < 0.0f) surgeDelta = 0.0f;
            if (steadyDelta < 0.0f) steadyDelta = 0.0f;

            updateSignature(_activeClientMac.c_str(), steadyDelta, surgeDelta, 350, powerFactor);
        }
    }

    // ------------------------------------------------------------------------
    // Self-Calibrating Probing Workflow (For unknown or newly paired devices)
    // ------------------------------------------------------------------------
    bool startProbing(const char* mac) {
        if (!mac || strlen(mac) == 0) return false;
        _targetMac = String(mac);
        _probeState = PROBE_WAIT_QUIET_GRID;
        _probeStartMs = millis();
        return true;
    }

    ProbeState loopProbing(float currentGridWatts, float ceilingWatts, bool& requestTurnOn, bool& requestTurnOff) {
        requestTurnOn = false;
        requestTurnOff = false;

        switch (_probeState) {
            case PROBE_WAIT_QUIET_GRID: {
                // Wait for quiet grid: at least 700W headroom available
                float headroom = ceilingWatts - currentGridWatts;
                if (headroom >= 700.0f) {
                    _probeState = PROBE_CAPTURE_BASELINE;
                    _probeStartMs = millis();
                } else if (millis() - _probeStartMs > 45000) {
                    // Timeout after 45s of grid too busy
                    _probeState = PROBE_IDLE;
                }
                break;
            }

            case PROBE_CAPTURE_BASELINE: {
                _baselineWatts = currentGridWatts;
                _peakObservedWatts = currentGridWatts;
                _probeState = PROBE_PULSE_RUNNING;
                _probeStartMs = millis();
                requestTurnOn = true; // Signal device to pulse ON
                break;
            }

            case PROBE_PULSE_RUNNING: {
                if (currentGridWatts > _peakObservedWatts) {
                    _peakObservedWatts = currentGridWatts;
                }

                float activeDelta = currentGridWatts - _baselineWatts;

                // Immediate emergency trip countermeasure:
                // If device alone exceeds the entire household ceiling, abort instantly!
                if (activeDelta >= (ceilingWatts - 50.0f) || currentGridWatts >= (ceilingWatts + 50.0f)) {
                    requestTurnOff = true;
                    _probeState = PROBE_FAILED_OVERLIMIT;
                    markOverLimit(_targetMac.c_str(), activeDelta);
                    return _probeState;
                }

                // Normal 3-second test pulse complete
                if (millis() - _probeStartMs >= 3000) {
                    requestTurnOff = true;
                    _probeState = PROBE_EVALUATE;
                }
                break;
            }

            case PROBE_EVALUATE: {
                float surgeDelta = _peakObservedWatts - _baselineWatts;
                float steadyDelta = currentGridWatts - _baselineWatts;
                if (surgeDelta < 0.0f) surgeDelta = 0.0f;
                if (steadyDelta < 0.0f) steadyDelta = 0.0f;

                updateSignature(_targetMac.c_str(), steadyDelta, surgeDelta, 350, 0.85f, true);
                _probeState = PROBE_COMPLETED;
                break;
            }

            case PROBE_COMPLETED:
            case PROBE_FAILED_OVERLIMIT:
            case PROBE_IDLE:
            default:
                break;
        }

        return _probeState;
    }

    ProbeState getProbeState() const {
        return _probeState;
    }

    void resetProbeState() {
        _probeState = PROBE_IDLE;
    }

private:
    uint8_t            _signatureCount;
    ApplianceSignature _signatures[MAX_REGISTERED_DEVICES];

    // Startup observer state
    bool               _isObservingStartup;
    uint32_t           _startObserveMs;
    float              _baselineWatts;
    float              _peakObservedWatts;
    String             _activeClientMac;

    // Probing state
    ProbeState         _probeState;
    uint32_t           _probeStartMs;
    String             _targetMac;

    void updateSignature(const char* mac, float steadyWatts, float surgeWatts, uint32_t surgeDurationMs, float pf, bool isProbed = false) {
        if (!mac || strlen(mac) == 0) return;

        ApplianceSignature* sig = getSignature(mac);
        if (!sig) {
            if (_signatureCount >= MAX_REGISTERED_DEVICES) return;
            sig = &_signatures[_signatureCount++];
            strncpy(sig->mac, mac, sizeof(sig->mac) - 1);
            sig->learnedSteadyWatts = steadyWatts;
            sig->learnedSurgeWatts = surgeWatts;
            sig->learnedSurgeDurationMs = surgeDurationMs;
            sig->learnedPowerFactor = pf;
            sig->sampleCount = 1;
        } else {
            // Refine with exponential moving average
            sig->learnedSteadyWatts = 0.75f * sig->learnedSteadyWatts + 0.25f * steadyWatts;
            if (surgeWatts > sig->learnedSurgeWatts * 0.5f) {
                sig->learnedSurgeWatts = 0.70f * sig->learnedSurgeWatts + 0.30f * surgeWatts;
            }
            sig->learnedPowerFactor = 0.80f * sig->learnedPowerFactor + 0.20f * pf;
            sig->sampleCount++;
        }

        if (isProbed) sig->isProbed = true;
        sig->lastObservedMillis = millis();
        saveSignaturesToNvs();
    }

    void markOverLimit(const char* mac, float measuredWatts) {
        ApplianceSignature* sig = getSignature(mac);
        if (!sig) {
            if (_signatureCount >= MAX_REGISTERED_DEVICES) return;
            sig = &_signatures[_signatureCount++];
            strncpy(sig->mac, mac, sizeof(sig->mac) - 1);
        }
        sig->learnedSteadyWatts = measuredWatts;
        sig->isOverLimit = true;
        sig->isProbed = true;
        saveSignaturesToNvs();
    }

    void loadSignaturesFromNvs() {
        Preferences prefs;
        if (prefs.begin("app_learn", true)) {
            _signatureCount = prefs.getUChar("count", 0);
            if (_signatureCount > MAX_REGISTERED_DEVICES) _signatureCount = MAX_REGISTERED_DEVICES;
            for (uint8_t i = 0; i < _signatureCount; i++) {
                String kMac = "m" + String(i);
                String kSteady = "s" + String(i);
                String kSurge = "p" + String(i);
                String kPf = "f" + String(i);
                String kProbed = "b" + String(i);
                String kOver = "o" + String(i);

                String macStr = prefs.getString(kMac.c_str(), "");
                strncpy(_signatures[i].mac, macStr.c_str(), sizeof(_signatures[i].mac) - 1);
                _signatures[i].learnedSteadyWatts = prefs.getFloat(kSteady.c_str(), 0.0f);
                _signatures[i].learnedSurgeWatts = prefs.getFloat(kSurge.c_str(), 0.0f);
                _signatures[i].learnedPowerFactor = prefs.getFloat(kPf.c_str(), 0.85f);
                _signatures[i].isProbed = prefs.getBool(kProbed.c_str(), false);
                _signatures[i].isOverLimit = prefs.getBool(kOver.c_str(), false);
            }
            prefs.end();
        }
    }

    void saveSignaturesToNvs() {
        Preferences prefs;
        if (prefs.begin("app_learn", false)) {
            prefs.putUChar("count", _signatureCount);
            for (uint8_t i = 0; i < _signatureCount; i++) {
                String kMac = "m" + String(i);
                String kSteady = "s" + String(i);
                String kSurge = "p" + String(i);
                String kPf = "f" + String(i);
                String kProbed = "b" + String(i);
                String kOver = "o" + String(i);

                prefs.putString(kMac.c_str(), _signatures[i].mac);
                prefs.putFloat(kSteady.c_str(), _signatures[i].learnedSteadyWatts);
                prefs.putFloat(kSurge.c_str(), _signatures[i].learnedSurgeWatts);
                prefs.putFloat(kPf.c_str(), _signatures[i].learnedPowerFactor);
                prefs.putBool(kProbed.c_str(), _signatures[i].isProbed);
                prefs.putBool(kOver.c_str(), _signatures[i].isOverLimit);
            }
            prefs.end();
        }
    }
};
