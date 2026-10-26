#pragma once

#include <Arduino.h>

/**
 * ============================================================================
 * CraftThingy Central Control — System Configuration, Power Profiles & Models
 * ============================================================================
 */

// ----------------------------------------------------------------------------
// Power Profile Presets (Easily Switchable via UI / REST API)
// ----------------------------------------------------------------------------
enum PowerProfilePreset : uint8_t {
    PRESET_450W         = 0,    // 450 VA (PLN R1-450 / Small Off-grid)
    PRESET_900W         = 1,    // 900 VA (PLN R1-900)
    PRESET_1300W        = 2,    // 1300 VA (Standard PLN R1-1300 Default)
    PRESET_2200W        = 3,    // 2200 VA (PLN R1-2200)
    PRESET_3500W        = 4,    // 3500 VA (PLN R2-3500)
    PRESET_INVERTER_UPS = 5,    // 800 W Battery Inverter / Generator Fallback
    PRESET_CUSTOM       = 6     // User-defined continuous wattage ceiling
};

struct PowerProfileConfig {
    PowerProfilePreset preset;
    float              maxWatts;            // Nominal breaker rating (e.g. 1300.0f)
    uint8_t            thermalBufferPct;    // Safety cushion against breaker thermal trip (e.g. 10%)
    float              effectiveCeilingWatts;// Calculated safe ceiling: maxWatts * (1 - buffer/100)
    char               profileName[32];     // e.g. "PLN 1300VA (Normal)"

    void calculateEffectiveCeiling() {
        if (thermalBufferPct > 30) thermalBufferPct = 10;
        effectiveCeilingWatts = maxWatts * (1.0f - (static_cast<float>(thermalBufferPct) / 100.0f));
    }
};

// ----------------------------------------------------------------------------
// Appliance Priority Levels
// ----------------------------------------------------------------------------
enum AppliancePriority : uint8_t {
    PRIORITY_CRITICAL   = 1,    // Water Pump (essential household water pressure)
    PRIORITY_HIGH       = 2,    // Refrigerator / Cooling
    PRIORITY_NORMAL     = 3,    // Washing Machine (deferrable/pausable cycle)
    PRIORITY_LOW        = 4     // Battery Charger / Dehumidifier / Non-essential
};

// ----------------------------------------------------------------------------
// Registered Appliance Device Model
// ----------------------------------------------------------------------------
#define MAX_REGISTERED_DEVICES 16

struct RegisteredAppliance {
    char              mac[18];          // MAC address (Primary ID)
    char              name[32];         // Human readable nickname e.g. "Submersible Pump"
    char              role[24];         // Role identifier e.g. "water_pump", "washing_machine"
    AppliancePriority priority;         // 1 = Critical, 2 = High, 3 = Normal, 4 = Low
    float             ratedWatts;       // Expected power draw (e.g. 500W)
    bool              isPausable;       // Can the Central Brain pause this to siphon power?
    bool              isOnline;         // Currently sending heartbeats
    bool              hasActivePermit;  // Currently holding a valid power permit
    uint32_t          lastSeenMillis;
    float             cumulativeKwh;
};

// ----------------------------------------------------------------------------
// Token Request / Grant Status
// ----------------------------------------------------------------------------
enum TokenStatus : uint8_t {
    TOKEN_REJECTED_OVERLOAD = 0,
    TOKEN_GRANTED           = 1,
    TOKEN_QUEUED            = 2,
    TOKEN_PAUSED_BY_SHED    = 3
};

struct PowerPermitToken {
    uint32_t    tokenId;
    char        clientMac[18];
    char        applianceRole[24];
    float       grantedWatts;
    uint32_t    startMillis;
    uint32_t    maxDurationSec;
    TokenStatus status;
};

// ----------------------------------------------------------------------------
// Peacefair PZEM-004T AC Telemetry Data Model
// ----------------------------------------------------------------------------
struct PzemTelemetry {
    float    voltage;       // Volts AC (RMS)
    float    current;       // Amperes (RMS)
    float    power;         // Active Power (Watts)
    float    energy;        // Cumulative Energy (kWh)
    float    frequency;     // Grid Frequency (Hz)
    float    powerFactor;   // Power Factor (0.00 - 1.00)
    bool     isOnline;      // Sensor communicating
    uint32_t lastReadMillis;
};

// ----------------------------------------------------------------------------
// INA226 DC Battery & Bus Telemetry Model
// ----------------------------------------------------------------------------
struct DcTelemetry {
    float    busVoltageV;   // Battery / Bus Voltage (Volts)
    float    shuntCurrentMa;// Charge (>0) or Discharge (<0) Current (mA)
    float    powerMw;       // Active DC Power (milliwatts)
    uint8_t  batterySocPct; // Approximate State-of-Charge (0 - 100%)
    bool     isOnline;      // Sensor communicating
    uint32_t lastReadMillis;
};

// ----------------------------------------------------------------------------
// MicroSD Datalogger Status Model
// ----------------------------------------------------------------------------
struct SdStatus {
    bool     isMounted;
    uint64_t totalBytes;
    uint64_t usedBytes;
    uint32_t recordsWrittenToday;
    uint32_t writeErrors;
    char     currentLogFile[64];
};
