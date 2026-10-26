# CraftThingy Over-The-Air (OTA) Rescue & Offline Upgrade Guide (Core v1.1.2 / App v1.1.7)

## Why is a Rescue Tool Needed?
1. **Legacy ESP8266 v1.0.0 Firmware Buffer Limitations**:
   ESP-01S (1MB) devices running early firmware `v1.0.0` were compiled with a 4,096-byte (4KB) TLS input buffer and legacy RSA-CBC ciphers. When downloading directly from GitHub / AWS CloudFront (`release-assets.githubusercontent.com`), AWS transmits 16KB TLS records and enforces modern ECDHE ciphers. This causes BearSSL to abort with `BR_ERR_TOO_LARGE` or SSL handshake connection drops.
2. **Decoupled DNS Redirection**:
   The rescue bridge utilizes the decoupled `/api/v1/network/ip` endpoint introduced in Core `v1.1.2`, pointing the ESP's DNS directly to the host PC over authenticated HMAC requests without resetting Wi-Fi association or requiring Wi-Fi passwords.
3. **Zero-Internet / Isolated Environments**:
   When upgrading devices deployed in the field without internet access, the rescue bridge acts as a local simulated GitHub releases endpoint.
4. **100% Wireless Recovery**:
   No USB cable, disassembly, or UART programmer is needed. The tool streams firmware in safe **1,024-byte chunks with 5ms pacing** directly into the ESP's flash.

---

## Scenario A: ESP Connected to Local Wi-Fi (Recommended)
If your ESP is connected to your local network (e.g. `192.168.2.228`):

1. Open PowerShell or Terminal with Administrator privileges (needed to bind port 53/443).
2. Run the rescue bridge pointing to the ESP:
   ```powershell
   # In craftthingy-esp-core-ota (recovers Core v1.1.2 by default):
   python tools/ota_rescue_bridge.py --esp 192.168.2.228

   # In craftthingy-esp-water-pump (recovers Water Pump v1.1.7 by default):
   python tools/ota_rescue_bridge.py --esp 192.168.2.228 --target water_pump
   ```
3. The tool will:
   - Query live ESP telemetry and installed firmware versions.
   - Point the ESP's DNS to the host PC via `/api/v1/network/ip`.
   - Generate temporary local TLS certificates matching GitHub hostnames.
   - Serve simulated GitHub release metadata (`v1.1.2` for core, `v1.1.7` for water_pump).
   - Stream the firmware binary in 1,024-byte paced TLS records.
   - Monitor the ESP's flash write and verify reboot into the target firmware.

---

## Scenario B: ESP in Hotspot Mode / Field Recovery (Zero Internet Needed!)
If the ESP is in fallback AP mode (`CraftThingy-5371` or `CraftThingy-Water-Pump-AP` at `192.168.4.1`):

### Method 1: Using your Laptop's Mobile Hotspot
1. Turn on **Mobile Hotspot** on your laptop (e.g. SSID: `RescueAP`, Password: `password123`).
2. Run the rescue tool providing hotspot credentials:
   ```powershell
   python tools/ota_rescue_bridge.py --esp 192.168.4.1 --wifi-ssid "RescueAP" --wifi-pass "password123"
   ```
3. The tool provisions the ESP to join your laptop's hotspot and streams the firmware locally.

### Method 2: Browser Web Manager (`tools/esp_manager.html`)
1. Connect your laptop or phone Wi-Fi directly to the ESP's hotspot (`CraftThingy-5371`).
2. Open [esp_manager.html](file:///tools/esp_manager.html) in your browser.
3. Under **Layer 2: Wi-Fi Credentials**, enter your Wi-Fi SSID and password and click **Save Wi-Fi Credentials**.
4. Once connected to your local Wi-Fi, run:
   ```powershell
   python tools/ota_rescue_bridge.py --esp <Assigned-ESP-IP>
   ```

---

## Command Options

| Argument | Description | Default |
|---|---|---|
| `--esp` | Target ESP IP address | `192.168.2.228` |
| `--key` | Master 64-hex Secret Key | Default developer key |
| `--bin` | Path to custom firmware `.bin` | Auto-detected from local build or release |
| `--tag` | Release tag to simulate / target | `v1.1.2` (core) / `v1.1.7` (water_pump) |
| `--target` | Role target (`core` or `water_pump`) | `core` (in core repo) / `water_pump` (in app repo) |
| `--bridge-ip` | Host IP address for rescue bridge | Auto-detected |
| `--gateway` | Local Network Gateway IP | `192.168.2.1` |
| `--wifi-ssid` | Provision SSID (for AP mode rescue) | `None` |
| `--wifi-pass` | Provision Password (for AP mode rescue) | `None` |
| `--version` | Display rescue tool version | — |
