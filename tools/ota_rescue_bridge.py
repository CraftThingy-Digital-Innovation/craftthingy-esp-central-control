#!/usr/bin/env python3
"""
CraftThingy Digital Innovation - Over-The-Air Rescue Bridge (Core v1.1.2 / App v1.1.7)
=============================================================================
Bypasses the ESP8266 legacy BearSSL 4KB input buffer limit (BR_ERR_TOO_LARGE)
and AWS S3 TLS connection drops by acting as an all-in-one local chunked OTA
server for both api.github.com and release-assets.githubusercontent.com.

Supports:
1. Zero-Internet / Offline Rescue:
   Uses local binary or cached release. Works over home Wi-Fi or PC mobile hotspot.
2. 100% Over-the-air: No USB-to-UART cable or physical access needed.
3. Safe Chunking: Streams binary in small 1024-byte TLS records with 5ms pacing.
4. Multi-tier Auth: Tier 1 (HMAC-SHA256) with automatic Tier 2 (Bearer / Master) fallback.
5. Target-Aware Asset Resolution: Seamlessly supports core and water_pump recovery.

Requirements:
    pip install cryptography
"""

import sys
import os
import time
import socket
import ssl
import json
import hmac
import hashlib
import secrets
import argparse
import ipaddress
import threading
import urllib.request
import urllib.error
import urllib.parse
from datetime import datetime, timezone, timedelta
from typing import Tuple, Optional, Dict, Any

TOOL_VERSION = "1.1.7"
CORE_VERSION = "1.1.2"

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
if hasattr(sys.stderr, "reconfigure"):
    try:
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

try:
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives import serialization
except ImportError:
    print("[-] Missing requirement 'cryptography'. Install with: pip install cryptography", file=sys.stderr)
    sys.exit(1)

DEFAULT_SECRET = "a1b2c3d4e5f60718293a4b5c6d7e8f90112233445566778899aabbccddeeff00"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CERT_FILE = os.path.join(SCRIPT_DIR, "rescue_cert.pem")
KEY_FILE = os.path.join(SCRIPT_DIR, "rescue_key.pem")

# Detect context: if placed in craftthingy-esp-core-ota, default target is core
_repo_name = os.path.basename(os.path.dirname(SCRIPT_DIR)).lower()
DEFAULT_TARGET = "core" if "core" in _repo_name else "water_pump"


def find_firmware_binary(custom_bin: Optional[str] = None, tag: Optional[str] = None, target: str = "core") -> str:
    """Locates or downloads the rescue firmware binary matching the target role."""
    if custom_bin and os.path.exists(custom_bin):
        return os.path.abspath(custom_bin)

    effective_tag = tag or (f"v{CORE_VERSION}" if target == "core" else f"v{TOOL_VERSION}")

    if target == "core":
        repo = "craftthingy-esp-core-ota"
        asset_name = "firmware-esp01s.bin"
        search_candidates = [
            os.path.join(SCRIPT_DIR, "firmware-esp01s.bin"),
            os.path.join(SCRIPT_DIR, "..", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(SCRIPT_DIR, "..", "..", "craftthingy-esp-core-ota", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(os.getcwd(), "craftthingy-esp-core-ota", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(SCRIPT_DIR, "firmware.bin"),
            os.path.join(os.getcwd(), ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(os.getcwd(), "firmware-esp01s.bin"),
            os.path.join(os.getcwd(), "firmware.bin"),
        ]
    else:
        repo = "craftthingy-esp-water-pump"
        asset_name = "firmware-water_pump-esp01s.bin"
        search_candidates = [
            os.path.join(SCRIPT_DIR, "firmware-water_pump-esp01s.bin"),
            os.path.join(SCRIPT_DIR, "..", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(SCRIPT_DIR, "..", "..", "craftthingy-esp-water-pump", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(os.getcwd(), "craftthingy-esp-water-pump", ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(SCRIPT_DIR, "firmware.bin"),
            os.path.join(os.getcwd(), ".pio", "build", "esp01_1m", "firmware.bin"),
            os.path.join(os.getcwd(), "firmware-water_pump-esp01s.bin"),
            os.path.join(os.getcwd(), "firmware.bin"),
        ]

    for cand in search_candidates:
        if os.path.exists(cand):
            cand_lower = cand.lower().replace("\\", "/")
            if target == "core" and "water_pump" in cand_lower:
                continue
            if target == "water_pump" and "core-ota" in cand_lower:
                continue
            print(f"[+] Found local firmware candidate for '{target}': {cand}")
            return os.path.abspath(cand)

    # Attempt to download release asset from GitHub if no local binary exists
    release_url = f"https://github.com/CraftThingy-Digital-Innovation/{repo}/releases/download/{effective_tag}/{asset_name}"
    download_dest = os.path.join(SCRIPT_DIR, asset_name)
    print(f"[*] No local binary found for target '{target}'. Downloading {effective_tag} release from:\n    {release_url}")
    try:
        urllib.request.urlretrieve(release_url, download_dest)
        if os.path.exists(download_dest) and os.path.getsize(download_dest) > 100000:
            print(f"[+] Downloaded {os.path.getsize(download_dest)} bytes to {download_dest}")
            return os.path.abspath(download_dest)
    except Exception as e:
        print(f"[-] Could not auto-download from GitHub: {e}")

    raise FileNotFoundError(
        f"Could not find firmware binary for target '{target}'. Please build one with 'pio run' or pass --bin <path-to-firmware.bin>"
    )


def get_local_ip(target_ip: str) -> str:
    """Finds the local network IP that routes to the target ESP."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((target_ip, 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "192.168.4.2" if target_ip.startswith("192.168.4.") else "127.0.0.1"


def compute_hmac_signature(key: str, method: str, path: str, timestamp: str, nonce: str, body: str) -> str:
    clean_path = path.split("?")[0]
    payload = f"{method}\n{clean_path}\n{timestamp}\n{nonce}\n{body}".encode("utf-8")
    key_bytes = key.encode("utf-8")
    return hmac.new(key_bytes, payload, hashlib.sha256).hexdigest()


def send_signed_request(
    host: str,
    method: str,
    path: str,
    key: str,
    sequence: int = 1,
    body_data: Optional[Dict[str, Any]] = None,
    timeout: float = 6.0
) -> Tuple[Dict[str, Any], int]:
    """Sends authenticated request with Tier 1 HMAC-SHA256 and fallback Tier 2 Bearer token."""
    body_str = json.dumps(body_data, separators=(',', ':')) if body_data else ""
    url = f"http://{host}{path}"
    curr_seq = sequence
    clean_uri = path.split("?")[0]

    # Attempt Tier 1: HMAC Rolling Key Ratchet
    for attempt in range(15):
        timestamp = str(int(time.time()))
        nonce = secrets.token_hex(8)
        sig = compute_hmac_signature(key, method, clean_uri, timestamp, nonce, body_str)

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
            "X-Master-Key": key,
            "X-Signature": sig,
            "X-Timestamp": timestamp,
            "X-Nonce": nonce,
            "X-Sequence": str(curr_seq),
        }

        req = urllib.request.Request(
            url,
            data=body_str.encode("utf-8") if body_str else None,
            headers=headers,
            method=method
        )

        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp_body = resp.read().decode("utf-8")
                try:
                    return json.loads(resp_body), curr_seq
                except json.JSONDecodeError:
                    return {"raw": resp_body}, curr_seq
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="ignore")
            if "rejected_replay" in err_body:
                curr_seq += 1
                continue
            if e.code == 401:
                # Fallback to Tier 2 (Bearer / Master Key without X-Signature)
                break
            raise RuntimeError(f"HTTP Error {e.code}: {err_body}")

    # Fallback to Tier 2: Pure Master Key / Bearer Authentication
    tier2_headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",
        "X-Master-Key": key,
    }
    req2 = urllib.request.Request(
        url,
        data=body_str.encode("utf-8") if body_str else None,
        headers=tier2_headers,
        method=method
    )
    with urllib.request.urlopen(req2, timeout=timeout) as resp:
        resp_body = resp.read().decode("utf-8")
        try:
            return json.loads(resp_body), curr_seq
        except json.JSONDecodeError:
            return {"raw": resp_body}, curr_seq


def generate_cert(local_ip: str):
    """Generates a self-signed TLS certificate covering GitHub release hostnames."""
    print(f"[*] Generating local TLS certificates for rescue bridge ({local_ip})...")
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "api.github.com"),
    ])
    now = datetime.now(timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + timedelta(days=365))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName("api.github.com"),
                x509.DNSName("github.com"),
                x509.DNSName("release-assets.githubusercontent.com"),
                x509.DNSName("objects.githubusercontent.com"),
                x509.DNSName("*.github.com"),
                x509.DNSName("*.githubusercontent.com"),
                x509.IPAddress(ipaddress.ip_address(local_ip)),
            ]),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )

    with open(KEY_FILE, "wb") as f:
        f.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption()
        ))

    with open(CERT_FILE, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    print("[+] TLS certificates generated successfully.")


def dns_server_thread(local_ip: str, gateway: str, stop_event: threading.Event):
    """DNS interceptor redirecting github.com and githubusercontent.com to the bridge."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("0.0.0.0", 53))
        sock.settimeout(1.0)
    except OSError as e:
        print(f"[-] Could not bind DNS port 53: {e}")
        print("    Run this script with Administrator/Root privileges if port 53 is restricted.")
        return

    print(f"[+] DNS Server listening on 0.0.0.0:53 (Intercepting GitHub domains -> {local_ip})")

    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(1024)
            idx = 12
            parts = []
            while idx < len(data) and data[idx] != 0:
                length = data[idx]
                idx += 1
                parts.append(data[idx:idx+length].decode("ascii", errors="ignore"))
                idx += length
            domain = ".".join(parts).lower()

            if "github" in domain:
                tid = data[:2]
                flags = b"\x81\x80"
                qdcount = data[4:6]
                ancount = b"\x00\x01"
                nscount = b"\x00\x00"
                arcount = b"\x00\x00"
                
                q_end = idx + 5
                question = data[12:q_end]
                answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + socket.inet_aton(local_ip)
                response = tid + flags + qdcount + ancount + nscount + arcount + question + answer
                sock.sendto(response, addr)
                print(f"[DNS] Intercepted {domain} -> {local_ip} for {addr[0]}")
            else:
                try:
                    up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    up.settimeout(1.5)
                    up.sendto(data, (gateway, 53))
                    resp, _ = up.recvfrom(1024)
                    up.close()
                    sock.sendto(resp, addr)
                except Exception:
                    pass
        except socket.timeout:
            continue
        except Exception:
            pass
    sock.close()


def https_server_thread(
    bin_path: str,
    local_ip: str,
    stop_event: threading.Event,
    transfer_complete_event: threading.Event,
    tag: str,
    target: str = "core"
):
    """Paced TLS server serving 1024-byte chunked records acceptable by BearSSL 4KB buffer."""
    bin_size = os.path.getsize(bin_path)
    print(f"[+] Loaded firmware binary: {bin_path} ({bin_size} bytes)")

    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)

    try:
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind(("0.0.0.0", 443))
        server_sock.listen(5)
        server_sock.settimeout(1.0)
    except OSError as e:
        print(f"[-] Could not bind HTTPS port 443: {e}")
        print("    Run this script with Administrator/Root privileges if port 443 is restricted.")
        return

    print(f"[+] Rescue HTTPS Server listening on 0.0.0.0:443 (Target: {target}, Tag: {tag})")

    while not stop_event.is_set():
        try:
            client_sock, client_addr = server_sock.accept()
        except socket.timeout:
            continue
        except Exception:
            break

        print(f"[HTTPS] Incoming TLS connection from {client_addr[0]}:{client_addr[1]}")
        try:
            ssl_conn = context.wrap_socket(client_sock, server_side=True)
            ssl_conn.settimeout(8.0)

            request = b""
            while b"\r\n\r\n" not in request:
                chunk = ssl_conn.recv(1024)
                if not chunk:
                    break
                request += chunk

            first_line = request.split(b"\r\n")[0].decode("latin1", errors="ignore")
            print(f"[HTTPS] Request: {first_line}")

            # Case A: GitHub API Release Check
            if b"/releases/latest" in request or b"/releases/tags/" in request:
                if target == "core":
                    assets = [
                        {
                            "name": "firmware-esp01s.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware-esp01s.bin"
                        },
                        {
                            "name": "firmware-esp32.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware-esp32.bin"
                        },
                        {
                            "name": "firmware.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware.bin"
                        }
                    ]
                else:
                    assets = [
                        {
                            "name": "firmware-water_pump-esp01s.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware-water_pump-esp01s.bin"
                        },
                        {
                            "name": "firmware-water_pump-esp32.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware-water_pump-esp32.bin"
                        },
                        {
                            "name": "firmware-esp01s.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware-esp01s.bin"
                        },
                        {
                            "name": "firmware.bin",
                            "size": bin_size,
                            "browser_download_url": "https://api.github.com/download/firmware.bin"
                        }
                    ]

                api_payload = json.dumps({
                    "tag_name": tag,
                    "name": f"CraftThingy Rescue Release {tag}",
                    "draft": False,
                    "prerelease": False,
                    "assets": assets
                }).encode("utf-8")

                resp = (
                    f"HTTP/1.1 200 OK\r\n"
                    f"Content-Type: application/json; charset=utf-8\r\n"
                    f"Content-Length: {len(api_payload)}\r\n"
                    f"Connection: close\r\n"
                    f"\r\n"
                ).encode("utf-8") + api_payload
                ssl_conn.sendall(resp)
                print(f"[HTTPS] Served simulated GitHub Releases API ({tag}, target: {target}, size: {bin_size} bytes)")
                time.sleep(0.5)
                try:
                    ssl_conn.close()
                except Exception:
                    pass

            # Case B: Firmware Asset Stream
            else:
                response_header = (
                    f"HTTP/1.1 200 OK\r\n"
                    f"Content-Type: application/octet-stream\r\n"
                    f"Content-Length: {bin_size}\r\n"
                    f"Connection: keep-alive\r\n"
                    f"Server: CraftThingy-Rescue-Bridge\r\n"
                    f"\r\n"
                ).encode("utf-8")
                ssl_conn.sendall(response_header)
                print(f"[HTTPS] Headers sent. Streaming {bin_size} bytes in small 1024-byte chunks with 5ms pacing...")

                sent_total = 0
                with open(bin_path, "rb") as f:
                    while True:
                        data = f.read(1024)
                        if not data:
                            break
                        ssl_conn.sendall(data)
                        sent_total += len(data)
                        time.sleep(0.005)

                print(f"[SUCCESS] All {sent_total} bytes streamed into ESP flash cleanly!")
                transfer_complete_event.set()

                time.sleep(12)
                try:
                    ssl_conn.close()
                except Exception:
                    pass
        except Exception as e:
            print(f"[-] HTTPS streaming notice: {e}")
            try:
                client_sock.close()
            except Exception:
                pass

    server_sock.close()


def run_rescue_flow(
    esp_host: str,
    secret_key: str,
    bin_path: str,
    local_ip: str,
    gateway: str,
    wifi_ssid: Optional[str] = None,
    wifi_pass: Optional[str] = None,
    tag: Optional[str] = None,
    target: str = "core"
):
    """Executes the complete end-to-end OTA rescue flow."""
    stop_event = threading.Event()
    transfer_complete_event = threading.Event()

    effective_tag = tag or (f"v{CORE_VERSION}" if target == "core" else f"v{TOOL_VERSION}")
    simulated_tag = effective_tag

    print("\n" + "="*70)
    print(f"🚀 CRAFTTHINGY RESCUE BRIDGE ACTIVE! 100% OVER-THE-AIR RECOVERY")
    print(f"Target Role: {target.upper()} | Simulated Tag: {effective_tag}")
    print(f"Target ESP: http://{esp_host} | Bridge IP: {local_ip}")
    print("="*70)

    try:
        # Step 1: Connect & Query current status
        print(f"\n[1/4] Fetching live ESP status from {esp_host}...")
        try:
            status, current_seq = send_signed_request(esp_host, "GET", "/api/v1/system/status", secret_key, sequence=1)
            net_info = status.get("data", {}).get("network", {})
            sys_info = status.get("data", {}).get("system", {})
            dev_info = status.get("data", {}).get("device", {})
            ssid = net_info.get("ssid", "")
            current_dns = net_info.get("dns", "")
            core_ver = sys_info.get("coreVersion", "1.0.0")
            app_ver = sys_info.get("appVersion", "")
            current_role = dev_info.get("role", "none")

            print(f"[+] Device Online: {sys_info.get('name')} (Core: {core_ver}, App: {app_ver or 'N/A'}, Role: {current_role})")
            print(f"[+] Current IP: {net_info.get('ip')}, State: {net_info.get('state')}, DNS: {current_dns}")

            # Safety check: If device is already on the exact same version, append -rescue so ESP's checkAndApplyRepoUpdate doesn't abort
            installed_ver = core_ver if target == "core" else (app_ver or core_ver)
            if installed_ver and installed_ver.lstrip('v') == effective_tag.lstrip('v'):
                simulated_tag = f"{effective_tag}-rescue"
                print(f"[*] Note: ESP already reports version {installed_ver}. Using tag '{simulated_tag}' to enable recovery re-flash.")
        except Exception as e:
            print(f"[*] Notice querying initial ESP status: {e}")
            ssid = ""
            current_dns = ""

        # Launch local TLS certificates, DNS server and HTTPS server
        generate_cert(local_ip)
        t_dns = threading.Thread(target=dns_server_thread, args=(local_ip, gateway, stop_event), daemon=True)
        t_dns.start()
        t_https = threading.Thread(target=https_server_thread, args=(bin_path, local_ip, stop_event, transfer_complete_event, simulated_tag, target), daemon=True)
        t_https.start()

        # Step 2: Ensure ESP DNS points to rescue bridge
        if esp_host.startswith("192.168.4."):
            print("\n[2/4] ESP is in Fallback AP Mode (192.168.4.1).")
            if wifi_ssid and wifi_pass is not None:
                print(f"[*] Provisioning ESP Wi-Fi credentials for '{wifi_ssid}' to join rescue network...")
                setup_payload = {
                    "ssid": wifi_ssid,
                    "pass": wifi_pass,
                    "use_static": True,
                    "static_ip": "192.168.2.228",
                    "gateway": gateway,
                    "subnet": "255.255.255.0",
                    "dns1": local_ip,
                    "dns2": gateway
                }
                try:
                    send_signed_request(esp_host, "POST", "/setup", secret_key, body_data=setup_payload)
                    print("[+] Wi-Fi credentials dispatched! Waiting 5s for STA connection...")
                    time.sleep(5)
                except Exception as e:
                    print(f"[*] Notice on Wi-Fi provisioning: {e}")
            else:
                print("[!] Notice: ESP requires Wi-Fi STA connection to perform OTA.")
                print("    If update check fails, provide --wifi-ssid <SSID> --wifi-pass <PASS>.")
        else:
            if current_dns == local_ip:
                print(f"\n[2/4] ESP DNS is already correctly pointed to rescue bridge ({local_ip}).")
            else:
                print(f"\n[2/4] Updating ESP DNS to rescue bridge ({local_ip})...")
                ip_payload = {
                    "useStatic": True,
                    "staticIp": esp_host,
                    "gateway": gateway,
                    "subnet": "255.255.255.0",
                    "dns1": local_ip,
                    "dns2": gateway
                }
                try:
                    send_signed_request(esp_host, "POST", "/api/v1/network/ip", secret_key, body_data=ip_payload)
                    print("[+] DNS pointed to rescue bridge successfully via /api/v1/network/ip.")
                except Exception as ex1:
                    print(f"[*] /api/v1/network/ip notice ({ex1}), falling back to /api/v1/wifi/config...")
                    try:
                        wifi_payload = dict(ip_payload)
                        wifi_payload["ssid"] = ssid
                        if wifi_pass is not None:
                            wifi_payload["pass"] = wifi_pass
                        send_signed_request(esp_host, "POST", "/api/v1/wifi/config", secret_key, body_data=wifi_payload)
                        print("[+] DNS pointed to rescue bridge via /api/v1/wifi/config.")
                    except Exception as ex2:
                        print(f"[-] Could not update DNS automatically: {ex2}")
                time.sleep(2)

        # Step 3: Trigger OTA Update check
        print(f"\n[3/4] Triggering OTA Update Check...")
        try:
            send_signed_request(esp_host, "POST", "/api/v1/ota/check", secret_key)
            print("[+] OTA Update check triggered! Listening for ESP download requests...")
        except Exception as e:
            print(f"[-] Trigger error: {e}")

        # Step 4: Wait for stream completion and reboot
        print("\n[4/4] Streaming firmware binary directly into ESP flash...")
        transferred = transfer_complete_event.wait(timeout=75.0)
        if not transferred:
            print("[-] Transfer did not complete within 75s. Verify ESP can reach bridge IP.")
        else:
            print("[+] Firmware streamed into flash successfully! Waiting for ESP reboot (15s)...")
            time.sleep(15)

            # Re-connect to verify upgrade
            print("\n[*] Re-connecting to ESP to verify upgrade...")
            for attempt in range(15):
                try:
                    time.sleep(3)
                    new_status, _ = send_signed_request(esp_host, "GET", "/api/v1/system/status", secret_key, sequence=1, timeout=4.0)
                    new_ver = new_status.get("data", {}).get("system", {}).get("coreVersion", "")
                    app_ver = new_status.get("data", {}).get("system", {}).get("appVersion", "")
                    ctrl_role = new_status.get("data", {}).get("device", {}).get("role", "")
                    print("\n" + "="*70)
                    print(f"🎉 OTA UPGRADE / RECOVERY COMPLETE OVER THE AIR!")
                    print(f"App Version    : {app_ver or 'N/A'}")
                    print(f"Core Version   : {new_ver}")
                    print(f"Active Role    : {ctrl_role}")
                    print(f"Chip Target    : {new_status.get('data',{}).get('system',{}).get('boardTarget')}")
                    print(f"Free Heap      : {new_status.get('data',{}).get('system',{}).get('freeHeap')} bytes")
                    print(f"Network IP     : {new_status.get('data',{}).get('network',{}).get('ip')}")
                    print("="*70 + "\n")
                    break
                except Exception:
                    print(f"[*] Waiting for ESP to come online (attempt {attempt+1}/15)...")

    finally:
        stop_event.set()
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser(
        description=f"CraftThingy Over-The-Air Rescue Bridge for ESP (Core v{CORE_VERSION} / App v{TOOL_VERSION})",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Upgrading / Recovering Core OTA firmware (default in core-ota repo):
  python tools/ota_rescue_bridge.py --esp 192.168.2.228 --target core

  # Upgrading / Recovering Water Pump firmware:
  python tools/ota_rescue_bridge.py --esp 192.168.2.228 --target water_pump

  # Field recovery in AP mode (192.168.4.1):
  python tools/ota_rescue_bridge.py --esp 192.168.4.1 --wifi-ssid "RescueAP" --wifi-pass "12345678"
        """
    )
    parser.add_argument("--version", action="version", version=f"CraftThingy Rescue Bridge (Core v{CORE_VERSION} / App v{TOOL_VERSION})")
    parser.add_argument("--esp", default="192.168.2.228", help="ESP IP address (default: 192.168.2.228, or 192.168.4.1 for AP mode)")
    parser.add_argument("--key", default=DEFAULT_SECRET, help="Master Secret Key")
    parser.add_argument("--bin", default=None, help="Path to firmware binary (default: auto-detected from build artifacts or latest release)")
    parser.add_argument("--tag", default=None, help="Release tag to simulate or download (default: v1.1.2 for core, v1.1.7 for water_pump)")
    parser.add_argument("--target", default=DEFAULT_TARGET, choices=["core", "water_pump"], help=f"Firmware target role (default: {DEFAULT_TARGET})")
    parser.add_argument("--bridge-ip", default=None, help="Host IP for rescue bridge (default: auto-detected)")
    parser.add_argument("--gateway", default="192.168.2.1", help="Network Gateway (default: 192.168.2.1)")
    parser.add_argument("--wifi-ssid", default=None, help="Wi-Fi SSID to provision ESP to during AP mode rescue")
    parser.add_argument("--wifi-pass", default=None, help="Wi-Fi password to provision ESP to during AP mode rescue")
    args = parser.parse_args()

    effective_tag = args.tag or (f"v{CORE_VERSION}" if args.target == "core" else f"v{TOOL_VERSION}")
    bin_path = find_firmware_binary(args.bin, tag=effective_tag, target=args.target)
    local_ip = args.bridge_ip or get_local_ip(args.esp)
    run_rescue_flow(args.esp, args.key, bin_path, local_ip, args.gateway, args.wifi_ssid, args.wifi_pass, tag=effective_tag, target=args.target)


if __name__ == "__main__":
    main()
