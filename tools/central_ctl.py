#!/usr/bin/env python3
"""
CraftThingy Central Control — Energy Arbiter & Grid Monitor CLI Tool
=====================================================================
Direct interactive terminal utility to monitor power telemetry, 
virtual MCB thermal health, battery status, appliance permits, 
and configure power profiles across the household.
"""

import sys
import argparse
import json
import urllib.request
import urllib.error
import time

BANNER = r"""
  ╔═══════════════════════════════════════════════════════════════╗
  ║    ⚡ CraftThingy Central Control & 1300W Energy Arbiter ⚡    ║
  ║         Single Source of Truth & Zero-Trust Grid Brain        ║
  ╚═══════════════════════════════════════════════════════════════╝
"""

def send_request(host, endpoint, method="GET", data=None, token=None, timeout=6.0):
    url = f"http://{host}{endpoint}"
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
        headers["X-Master-Key"] = token

    req_data = None
    if data is not None:
        if isinstance(data, dict):
            req_data = json.dumps(data).encode("utf-8")
        elif isinstance(data, str):
            req_data = data.encode("utf-8")

    req = urllib.request.Request(url, data=req_data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return {"raw": body}
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        return {"error": f"HTTP {e.code}: {e.reason}", "details": err_body}
    except Exception as e:
        return {"error": f"Connection failed: {str(e)}"}

def cmd_status(args):
    print(BANNER)
    print(f"Connecting to Central Control Brain at: http://{args.host} ...\n")
    res = send_request(args.host, "/api/v1/device/status", method="GET", token=args.token)

    if "error" in res:
        print(f"[!] Error: {res['error']}")
        return 1

    # Extract controller data payload
    data = res.get("data", res)
    prof = data.get("profile", {})
    mcb = data.get("mcb", {})
    ac = data.get("ac", {})
    bat = data.get("battery", {})
    sd = data.get("sd", {})
    devs = data.get("devices", [])

    print(f"  ⚡ POWER PROFILE & CEILING")
    print(f"  ─────────────────────────────────────────────────────────────")
    print(f"  Profile Name       : {prof.get('name', 'N/A')}")
    print(f"  Nominal Limit      : {prof.get('maxWatts', 0):.1f} W")
    print(f"  Thermal Cushion    : {prof.get('thermalBufferPct', 10)}% (Effective: {prof.get('effectiveCeilingWatts', 0):.1f} W)")
    print(f"  Active Permits     : {prof.get('activePermits', 0)}")
    print(f"  Surge Defense Alert: {'🚨 ACTIVE (SHEDDING LOADS)' if prof.get('surgeAlert') else '🟢 Normal'}\n")

    print(f"  🔥 VIRTUAL MCB THERMAL EMULATION (I²t Bimetallic Model)")
    print(f"  ─────────────────────────────────────────────────────────────")
    heat = mcb.get("thermalHeatPct", 0)
    bar_len = 25
    filled = int(bar_len * (heat / 100.0))
    bar = "█" * filled + "░" * (bar_len - filled)
    print(f"  Breaker Heat Accum : [{bar}] {heat}%")
    print(f"  Breaker State      : {'⚠️ THERMALLY HOT (MOTOR DELAYS)' if mcb.get('isBreakerHot') else '❄️ Cold / Safe'}")
    trip_sec = mcb.get("timeToTripSec", 0)
    if trip_sec > 0:
        print(f"  EST. TIME TO TRIP  : ⏳ {trip_sec} seconds at current load!")
    print()

    print(f"  🔌 220V AC GRID TELEMETRY (Peacefair PZEM-004T v3/v4)")
    print(f"  ─────────────────────────────────────────────────────────────")
    if ac.get("online"):
        print(f"  Voltage            : {ac.get('voltage', 0):.1f} V")
        print(f"  Current            : {ac.get('current', 0):.3f} A")
        print(f"  Active Power       : {ac.get('power', 0):.1f} W")
        print(f"  Energy Consumed    : {ac.get('energy', 0):.3f} kWh")
        print(f"  Frequency          : {ac.get('frequency', 0):.1f} Hz")
        print(f"  Power Factor       : {ac.get('powerFactor', 0):.2f}")
    else:
        print("  PZEM Sensor Status : ❌ OFFLINE (Check UART GPIO 16/17)")
    print()

    print(f"  🔋 DC BATTERY & BACKUP TELEMETRY (INA226 I2C)")
    print(f"  ─────────────────────────────────────────────────────────────")
    if bat.get("online"):
        soc = bat.get("socPct", 0)
        soc_bar = "█" * int(20 * (soc / 100.0)) + "░" * (20 - int(20 * (soc / 100.0)))
        print(f"  Battery Voltage    : {bat.get('voltage', 0):.2f} V")
        print(f"  Current Draw       : {bat.get('currentMa', 0):.1f} mA")
        print(f"  Power Consumption  : {bat.get('powerMw', 0):.1f} mW")
        print(f"  Battery Fuel Gauge : [{soc_bar}] {soc}%")
    else:
        print("  INA226 Status      : ❌ OFFLINE (Check I2C GPIO 21/22)")
    print()

    print(f"  💾 MICROSD RESILIENT DATALOGGER (8GB FAT32)")
    print(f"  ─────────────────────────────────────────────────────────────")
    if sd.get("mounted"):
        print(f"  Card Capacity      : {sd.get('usedMb', 0)} MB used / {sd.get('totalMb', 0)} MB total")
        print(f"  Samples Today      : {sd.get('recordsToday', 0)} records")
        print(f"  Current Log File   : {sd.get('logFile', 'N/A')}")
        print(f"  Write Errors       : {sd.get('errors', 0)}")
    else:
        print("  MicroSD Card Status: ⚠️ UNMOUNTED (Graceful Degradation Active)")
    print()

    print(f"  🏷️ REGISTERED APPLIANCE HIERARCHY & LEARNED SURGE SIGNATURES")
    print(f"  ─────────────────────────────────────────────────────────────")
    if devs:
        fmt = "  {:<18} {:<16} {:<10} {:<8} {:<10} {:<10} {:<8}"
        print(fmt.format("MAC Address", "Name", "Priority", "Pausable", "Steady W", "Surge W", "Status"))
        print(f"  {'─'*18} {'─'*16} {'─'*10} {'─'*8} {'─'*10} {'─'*10} {'─'*8}")
        p_names = {1: "Critical", 2: "High", 3: "Normal", 4: "Low"}
        for d in devs:
            pr = p_names.get(d.get("priority", 3), str(d.get("priority", 3)))
            paus = "Yes" if d.get("pausable") else "No"
            st_w = f"{d.get('learnedSteadyWatts', d.get('ratedWatts', 0)):.0f}W"
            su_w = f"{d.get('learnedSurgeWatts', 0):.0f}W"
            stat = "🟢 ACTIVE" if d.get("activePermit") else ("🟡 Online" if d.get("online") else "⚪ Off")
            print(fmt.format(d.get("mac", ""), d.get("name", "")[:16], pr, paus, st_w, su_w, stat))
    else:
        print("  No appliances registered yet. Pair an ESP node or register manually.")
    print()
    return 0

def cmd_set_profile(args):
    print(BANNER)
    presets = {
        "450": 0, "900": 1, "1300": 2, "2200": 3, "3500": 4, "inverter": 5, "custom": 6
    }
    choice = str(args.preset).lower()
    if choice not in presets and not choice.isdigit():
        print(f"[!] Invalid preset. Choices: 450, 900, 1300, 2200, 3500, inverter, or custom")
        return 1

    payload = json.dumps({
        "action": "set_profile",
        "payload": str(presets.get(choice, 2))
    })
    res = send_request(args.host, "/api/v1/device/command", method="POST", data=payload, token=args.token)
    print("Response:", res)
    return 0

def cmd_probe(args):
    print(BANNER)
    print(f"Initiating quiet-grid self-calibrating test pulse for MAC: {args.mac} ...")
    payload = json.dumps({
        "action": "probe_device",
        "payload": json.dumps({"mac": args.mac})
    })
    res = send_request(args.host, "/api/v1/device/command", method="POST", data=payload, token=args.token)
    print("Response:", res)
    return 0

def cmd_pairing(args):
    print(BANNER)
    print(f"Opening 60-Second Physical/Software Pairing Gate at {args.host} ...")
    payload = json.dumps({
        "action": "open_pairing",
        "payload": "60"
    })
    res = send_request(args.host, "/api/v1/device/command", method="POST", data=payload, token=args.token)
    print("Response:", res)
    return 0

def main():
    parser = argparse.ArgumentParser(description="CraftThingy Central Control & Energy Arbiter CLI")
    parser.add_argument("--host", default="192.168.4.1", help="Target ESP32 Central Control IP (default: 192.168.4.1)")
    parser.add_argument("--token", default=None, help="Bearer / Master Key Authentication Token")

    sub = parser.add_subparsers(dest="command", required=True)

    # status
    p_status = sub.add_parser("status", help="Display full real-time telemetry, MCB status, and appliance table")
    p_status.set_defaults(func=cmd_status)

    # profile
    p_prof = sub.add_parser("set-profile", help="Switch continuous power profile preset")
    p_prof.add_argument("preset", help="Preset: 450, 900, 1300, 2200, 3500, inverter, custom")
    p_prof.set_defaults(func=cmd_set_profile)

    # probe
    p_probe = sub.add_parser("probe", help="Perform 3-second self-calibrating probing run on an appliance")
    p_probe.add_argument("mac", help="Appliance MAC address")
    p_probe.set_defaults(func=cmd_probe)

    # pairing
    p_pair = sub.add_parser("pairing", help="Open the 60-second pairing adoption gate")
    p_pair.set_defaults(func=cmd_pairing)

    args = parser.parse_args()
    return args.func(args)

if __name__ == "__main__":
    sys.exit(main() or 0)
