# CraftThingy Central Control — System Architecture & Specification

## 1. System Overview & The 1300W Household Problem

In residential electrical environments with a **1300VA / 1300W utility grid limit** (such as standard PLN R1 connections), heavy inductive and resistive household loads routinely exceed breaker capacity when operated concurrently:
* **Base household load**: Refrigerator, lighting, network router ($\approx 150\text{W} - 250\text{W}$).
* **Water Pump**: Inductive motor startup and run ($\approx 450\text{W} - 650\text{W}$, surge up to $1200\text{W}$).
* **Washing Machine / Iron / Kettle**: Heating & spin cycles ($\approx 500\text{W} - 1200\text{W}$).

When multiple appliances operate simultaneously, the main circuit breaker (MCB) trips.

The **CraftThingy Central Control** ("The Central Brain") runs on an **ESP32 38-Pin DevKit**, acting as the **Single Source of Truth, Energy Arbiter, and Security Master** for all appliances in the home.

```
                            ┌───────────────────────────────────┐
                            │   PLN 220V Grid (1300W Ceiling)   │
                            └─────────────────┬─────────────────┘
                                              │ (CT Clamp)
                                              ▼
                               ┌─────────────────────────────┐
                               │  Peacefair PZEM-004T (3.3V) │
                               └──────────────┬──────────────┘
                                              │ Hardware UART2 (GPIO 16/17)
                                              ▼
 ┌──────────────────┐  I2C    ┌─────────────────────────────┐   VSPI  ┌───────────────────┐
 │ INA226 DC Sensor ├────────►│                             ├────────►│ 8GB MicroSD Card  │
 └────────┬─────────┘ (21/22) │      ESP32 38-Pin "BRAIN"   │ (18-23) │ (Power & Outages) │
          │                   │                             │         └───────────────────┘
 ┌────────┴─────────┐         │   - 1300W Power Arbiter     │
 │ Li-ion / Battery │         │   - Priority Queue Engine   │
 │ + XW228DKFR4 Bar │         │   - Zero-Trust HMAC Master  │
 └──────────────────┘         └───────┬─────────────┬───────┘
                                      │             │
                    Wi-Fi / HMAC Token│             │Wi-Fi / HMAC Token
                                      ▼             ▼
                     ┌──────────────────┐         ┌───────────────────────┐
                     │  ESP-01S Node    │         │  Future Smart Node    │
                     │  (Water Pump)    │         │  (Washing Machine)    │
                     └──────────────────┘         └───────────────────────┘
```

---

## 2. Dual-Mode Operating Model

Every appliance node (beginning with the ESP-01S Water Pump) implements a dual-mode contract:

### A. Central Supervised Mode (Normal Operation)
* The appliance checks in with the Central Brain over authenticated HMAC-SHA256 packets.
* When hardware triggers demand operation (e.g. water level float drops to LOW), the appliance **does not immediately start**.
* It sends a **Power Permit Request** to the Brain: `{"action": "request_permit", "watts": 500, "estSec": 300}`.
* It only runs when the Central Brain returns an authenticated **Token Permit**.

### B. Local Autonomous Mode (Fail-Safe Fallback)
* If the Central Brain is powered down, offline for maintenance, or network connection fails for **> 60 seconds**:
  * The satellite automatically drops back to **Local Autonomous Mode**.
  * The appliance operates on local hardware interlocks (e.g. float sensor controls pump relay directly).
  * The house **never runs out of water** or essential services due to hub downtime.
* When the Central Brain comes back online, the satellite detects heartbeats and gracefully transitions back to Central Supervised Mode.

---

## 3. Power Arbiter & Priority Coordination Engine

### A. Easily Switchable Power Profiles (One-Tap Presets)
The Central Brain supports instant, zero-reboot switching between power ceilings (saved to NVS flash):
* `[ 450 W ]` — PLN 450VA / Emergency Off-grid limit
* `[ 900 W ]` — PLN 900VA limit
* `[ 1,300 W ★ ]` — PLN 1300VA standard household default
* `[ 2,200 W ]` — PLN 2200VA upgraded connection
* `[ 3,500 W ]` — PLN 3500VA high-power tier
* `[ 800 W Inverter / Solar ]` — Generator / Battery backup inverter mode
* `[ Custom Slider ]` — Set any custom continuous wattage limit

### B. The 10% Thermal Headroom Cushion
Circuit breakers trip on thermal bimetal deflection when run continuously at nominal rating. The Brain enforces a configurable **Thermal Headroom Buffer** (default 10%):
$$\text{Effective Safe Ceiling} = \text{Max Watts} \times (100\% - 10\%) = 1300\text{W} \times 0.90 = \mathbf{1170\text{W}}$$
Total active power is kept strictly below 1170W, eliminating nuisance thermal trips.

### C. Appliance Priority Hierarchy & Siphoning Table

| Priority Level | Appliance Example | Default Behavior | Pausable? |
|---|---|---|---|
| **1. Critical** | **Water Pump** (Low Tank) | Instant permit or siphons power from Priority 2-4 | No (runs to full) |
| **2. High** | **Refrigerator / Freezer** | Runs on demand; only deferred if breaker is critical | Brief pause only |
| **3. Normal** | **Washing Machine** | Can be paused for 3–8 minutes to siphon power to pump | Yes (heaters/spin pause) |
| **4. Low / Deferrable** | **Battery Charger / AC** | Paused first during any surge | Yes |

### D. Dynamic Load Shedding & Siphoning Protocol
1. **Real-time Margin Calculation**:
   $$\text{Available Margin} = \text{Effective Safe Ceiling} - \text{Real-time Measured PZEM Watts}$$
2. **Grant Decision**:
   * If $\text{Available Margin} \ge \text{Requested Watts}$: The Brain grants a token immediately.
   * If $\text{Available Margin} < \text{Requested Watts}$: The Brain checks the priority queue.
     * **Higher Priority Appliance**: The Brain signals lower-priority loads (e.g., washing machine heating element or dehumidifier) to pause for a bounded duration (e.g. 5 minutes). Power is siphoned to the critical load (Water Pump). Once completed, paused loads resume automatically.
     * **Lower Priority Appliance**: Held in an asynchronous queue with estimated wait time returned.

---

## 4. Zero-Trust Security Architecture

### A. Strict Star Topology (Hub-and-Spoke)
* Satellites **never communicate with or command other satellites**.
* Satellites only accept commands signed with the Central Brain’s master key.
* A rogue node on the network has no authority to command other appliances.

### B. Least-Privilege Reporting
* Satellites can only report local telemetry or request execution permits.
* Satellites cannot alter global power limits, cannot change priority rules, and cannot force power cuts on other rooms.

### C. True Hardware Entropy Master Key Generation
* Avoids the deterministic cold-boot flaw of raw `esp_random()`.
* At initial boot, the Central Brain pools entropy from:
  1. Active RF thermal noise from an unassociated Wi-Fi channel.
  2. Floating ADC1 analog input voltage jitter.
  3. Factory burned eFuse silicon DNA and MAC address.
  4. High-speed crystal oscillator drift ticks.
* Entropic inputs are whitened through **SHA-256** to generate a 256-bit unguessable master secret key.
* Key is committed to internal **NVS flash** (100-year retention, non-volatile).

### D. The 3 Gates of Pairing
1. **Time-Gated Window (60 Seconds)**: Hub only accepts pairing beacons during a 60-second window triggered physically (button press on ESP32) or by authenticated admin dashboard tap.
2. **RSSI Proximity Gate**: Only permits pairing if signal strength $\text{RSSI} > -50\text{ dBm}$ (device must be right next to the hub). Signals from outside house walls are dropped.
3. **1-Tap Quarantine**: Revoking a compromised device rotates the rolling network key and isolates the device immediately.

### E. "Physics Doesn't Lie" (CT Clamp Reality Check)
* The PZEM-004T CT clamp on the 220V copper wire measures actual physical power.
* If a software packet claims 2000W consumption to hoard permits, but the CT clamp measures no change $\rightarrow$ Fraud detected, device quarantined.
* If an uncoordinated load turns on and surges towards 1300W $\rightarrow$ Preemptive load shedding triggers before the physical utility breaker trips.

### F. Ghost Mode (Network Stealth)
* In Central Mode, satellites disable open fallback SoftAPs and close unauthenticated HTTP ports.
* Satellites operate strictly on encrypted, authenticated communications directed to the Central Brain.

---

## 5. Storage Hierarchy & Disaster Recovery

```
                     ┌────────────────────────────────────────┐
                     │          ESP32 Internal Flash          │
                     │          (4MB / 8MB SPI NOR)           │
                     ├────────────────────────────────────────┤
                     │  - Ground Truth: NVS Key & Registry    │
                     │  - 1300W Rules & Device Priority       │
                     │  - Core Dual-Slot OTA Engine           │
                     └──────────────────┬─────────────────────┘
                                        │ Periodic Sync
                                        ▼
                     ┌────────────────────────────────────────┐
                     │          MicroSD Card (8GB)            │
                     │          (FAT32 Removable)             │
                     ├────────────────────────────────────────┤
                     │  - Continuous 1 Hz Power CSV Logs      │
                     │  - Blackbox Breaker Trip Curves        │
                     │  - recovery_vault.bin (Encrypted)      │
                     └────────────────────────────────────────┘
```

### A. Ground Truth Lives in Internal NVS Flash
* The primary encryption key, device registry, and configuration live in **ESP32 Internal NVS Flash**.
* **If the SD card dies, is corrupted, or is removed**: The Central Brain continues running 100% uninterrupted. It reads the PZEM, coordinates the pump, and protects the 1300W breaker. Only historical CSV logging is temporarily paused.

### B. Graceful Degradation & Auto-Rebuild
* MicroSD logging runs in an isolated, non-blocking FreeRTOS task.
* When a damaged card is replaced with a fresh FAT32 card:
  1. The system auto-detects card insertion.
  2. Recreates `/logs/` and `/data/` directories.
  3. Writes a fresh `recovery_vault.bin` copy from NVS.
  4. Resumes CSV logging without requiring a reboot.

### C. Dead-Hub Disaster Recovery
1. **Tier 1 (New ESP32 + Existing SD Card)**: Insert the SD card into a replacement ESP32; it restores keys from `recovery_vault.bin` and resumes operation with zero re-pairing.
2. **Tier 2 (Cloud / Laptop Vault Export)**: The dashboard supports downloading an encrypted `craftthingy_vault.enc` anytime for offsite backup.
3. **Tier 3 (3-Power-Cycle Physical Override)**: If both the ESP32 and SD card are destroyed with no backup, cycling power to a satellite 3 times within 5 seconds wipes its dead-hub lock and re-opens pairing adoption.

---

## 6. Adaptive Predictions, Virtual MCB Thermal Model & Learning

Standard smart homes only act *reactively* after a breaker trips or when power is already exceeded. CraftThingy Central Control acts *predictively* by learning appliance characteristics and emulating physical breaker mechanics.

### A. Motor Inrush Surge Fingerprinting
Inductive motors (such as deep-well water pumps, compressors, and power tools) pull an instantaneous inrush spike 2–3x higher than their rated continuous running power, lasting 200–500ms during rotor spin-up.
1. **Waveform Observer**:
   * During the first 1.5 seconds of appliance startup, `ApplianceLearner` samples power factor and instantaneous wattage at high frequency.
   * Extracts peak surge wattage ($\Delta P_{\text{surge}}$), inrush duration ($T_{\text{surge}}$), and steady-state operating wattage ($P_{\text{steady}}$).
   * Fingerprints are saved to ESP32 internal NVS flash and updated adaptively over time.
2. **Predictive 3-Second Siphoning**:
   * If available margin can easily accommodate steady-state pump load (e.g., 550W) but would be breached by the 350ms inrush spike (e.g., 1150W), the Brain **does not reject the pump**.
   * Instead, it pauses lower-priority pausable loads (washing machine, charger) for **strictly 3 seconds** (`_inrushSiphonExpireMs`).
   * Once the motor stabilizes to steady-state running power, the paused loads automatically resume with zero user disruption.

### B. Virtual MCB Thermal Accumulation Model ($I^2t$)
Physical miniature circuit breakers (MCBs) trip via a bimetallic strip that bends as heat accumulates according to the Joule heating law:
$$\Delta Q_{\text{in}} \propto I^2 \cdot R \cdot \Delta t$$
When multiple loads run near 100% capacity, the physical breaker accumulates heat and becomes "hot", tripping much faster on subsequent small spikes.

1. **Continuous Thermal Heat Emulation**:
   * The Central Brain models breaker thermal state from 0% (ambient cold) to 100% (physical trip threshold) every second.
   * Thermal cooling follows Newton's Law of Cooling with an exponential dissipation constant ($\tau \approx 45\text{s}$).
2. **Predictive Inrush Safety Gate**:
   * Before granting any motor start permit, `isSafeForInrushSpike(currentWatts, expectedSurge, surgeDurMs)` calculates the projected thermal delta.
   * If the virtual MCB is already thermally hot ($\ge 75\%$), the motor start is deferred to queue until the breaker dissipates heat below 60%.
3. **Trip Time Estimation**:
   * Returns real-time `estimateTimeToTripSec(currentWatts)` to forecast seconds remaining before a sustained overload causes physical tripping.

### C. Self-Calibrating Probing Run for Unknown Devices
When an unregistered or uncharacterized appliance is connected:
1. **Quiet-Grid Discovery**:
   * Hub checks if grid baseline power is quiet ($< 500\text{W}$).
2. **Controlled Test Pulse (`PROBE_PULSE_RUNNING`)**:
   * Hub commands satellite to fire a 3-second diagnostic pulse.
   * Captures precise inrush peak, power factor, and steady-state wattage.
   * Commits the learned profile to NVS and MicroSD.
3. **Over-Limit Countermeasure**:
   * If a device's steady-state power exceeds the total house ceiling (e.g., 1800W continuous on a 1300W house profile):
   * Hub cuts the probe pulse immediately ($< 1\text{s}$), flags `isOverLimit = true`, disqualifies the device from future permits, and sounds the warning buzzer. Physical breaker tripping is prevented.

