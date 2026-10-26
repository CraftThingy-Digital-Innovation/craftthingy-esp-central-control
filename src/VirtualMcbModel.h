#pragma once

#include <Arduino.h>

/**
 * ============================================================================
 * CraftThingy Central Control — Virtual MCB Thermal Model (I²t Emulation)
 * ============================================================================
 * Simulates the physical bimetallic thermal deflection curve of a standard
 * residential miniature circuit breaker (e.g. Type C/B PLN MCB).
 * 
 * Allows the Central Brain to predict whether the physical breaker is "cold"
 * (able to safely absorb motor inrush spikes) or "hot" (near trip threshold).
 */
class VirtualMcbModel {
public:
    VirtualMcbModel() : 
        _thermalHeat(0.0f), 
        _ratedWatts(1300.0f), 
        _lastUpdateMs(0),
        _tauHeatSec(42.0f),     // ~42s to trip at 1.45x overload (standard Type C)
        _tauCoolSec(65.0f)      // ~65s thermal dissipation to ambient
    {}

    void begin(float ratedWatts = 1300.0f) {
        _ratedWatts = (ratedWatts > 100.0f) ? ratedWatts : 1300.0f;
        _thermalHeat = 0.0f;
        _lastUpdateMs = millis();
    }

    void setRatedWatts(float ratedWatts) {
        if (ratedWatts > 100.0f) {
            _ratedWatts = ratedWatts;
        }
    }

    /**
     * @brief Updates the thermal accumulator based on current measured active watts.
     * @param currentWatts Real-time active power measured by PZEM-004T CT clamp.
     */
    void update(float currentWatts) {
        uint32_t now = millis();
        if (_lastUpdateMs == 0) {
            _lastUpdateMs = now;
            return;
        }

        float dtSec = (float)(now - _lastUpdateMs) * 0.001f;
        _lastUpdateMs = now;
        if (dtSec <= 0.0f || dtSec > 10.0f) {
            dtSec = 1.0f; // Clamp to reasonable bounds on clock jumps
        }

        // Normalized current/power ratio: R = P / P_rated
        float r = currentWatts / _ratedWatts;

        // Bimetallic thermal characteristic:
        // Below 1.05x nominal rating, the physical bimetal strip cools toward ambient.
        // Above 1.05x nominal rating, thermal energy accumulates proportionally to (R² - 1.0).
        if (r > 1.05f) {
            float heatGen = (r * r - 1.0f) * (dtSec / _tauHeatSec);
            _thermalHeat += heatGen;
            if (_thermalHeat > 1.0f) _thermalHeat = 1.0f; // Trip point reached
        } else {
            // Dissipate heat toward ambient
            float heatDissipated = (_thermalHeat / _tauCoolSec) * dtSec;
            _thermalHeat -= heatDissipated;
            if (_thermalHeat < 0.0f) _thermalHeat = 0.0f;
        }
    }

    /**
     * @brief Evaluates whether the physical breaker can safely absorb a motor inrush spike
     *        without the bimetal strip deflecting to the trip point.
     * @param currentWatts Current steady-state household load
     * @param surgeWatts Expected inductive startup surge (e.g. 1150W)
     * @param durationMs Duration of inrush spike (e.g. 350ms)
     * @return true if safe to start motor, false if breaker is too hot and needs cooldown
     */
    bool isSafeForInrushSpike(float currentWatts, float surgeWatts, uint32_t durationMs) const {
        float peakWatts = currentWatts + surgeWatts;
        float rPeak = peakWatts / _ratedWatts;

        if (rPeak <= 1.05f) {
            return true; // Entire spike is within nominal rating
        }

        // Instantaneous magnetic trip check: standard Type C trips instantly above 5x to 10x rating
        if (rPeak >= 5.0f) {
            return false;
        }

        float dtSec = (float)durationMs * 0.001f;
        float addedThermal = (rPeak * rPeak - 1.0f) * (dtSec / _tauHeatSec);

        // Keep at least 15% safety buffer below trip point (1.0 - 0.15 = 0.85)
        return (_thermalHeat + addedThermal) < 0.85f;
    }

    /**
     * @brief Estimates time in seconds until the physical breaker will trip at current load.
     * @param currentWatts Constant active power
     * @return Estimated seconds to trip, or 9999 if load is sustainable indefinitely.
     */
    uint32_t estimateTimeToTripSec(float currentWatts) const {
        float r = currentWatts / _ratedWatts;
        if (r <= 1.05f) {
            return 9999; // Will not trip
        }

        float remainingHeat = 1.0f - _thermalHeat;
        if (remainingHeat <= 0.0f) return 0;

        float heatRatePerSec = (r * r - 1.0f) / _tauHeatSec;
        if (heatRatePerSec <= 0.0f) return 9999;

        return (uint32_t)(remainingHeat / heatRatePerSec);
    }

    float getThermalHeat() const {
        return _thermalHeat;
    }

    uint8_t getThermalHeatPct() const {
        return (uint8_t)(_thermalHeat * 100.0f);
    }

    bool isBreakerHot() const {
        return _thermalHeat >= 0.70f; // Above 70% thermal stress
    }

    bool isNearTripThreshold() const {
        return _thermalHeat >= 0.88f; // Imminent trip danger
    }

private:
    float    _thermalHeat;   // 0.0 (Cold / Ambient) to 1.0 (Trip Threshold)
    float    _ratedWatts;    // Nominal breaker limit (e.g. 1300W)
    uint32_t _lastUpdateMs;
    float    _tauHeatSec;    // Thermal heating time constant
    float    _tauCoolSec;    // Thermal dissipation cooling time constant
};
