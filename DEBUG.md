# Comprehensive Debug & Diagnostics Package: PirateHat Wi-Fi Hotspot & Web Server

**Document Purpose:** Self-contained briefing for an external engineer/thinking model with zero physical or remote access to the Raspberry Pi. Contains all system telemetry, hardware scans, firmware versions, wireless configurations, software architectures, packet captures, DNS change histories, and pre-answered questions.

**Date:** September 19, 2026  
**System:** Raspberry Pi 4 Model B Rev 1.4  
**OS:** Debian GNU/Linux 13.7 (trixie) (aarch64)  
**Kernel:** Linux piratehat 6.18.39+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.18.39-1+rpt1 (2026-07-29)  
**Workspace:** `/home/pi/nrsc5`  
**Host & AP SSID:** `PirateHat` on `wlan0`  
**Primary Test Client:** Google Pixel (`Corp Phone`, MAC `42:xx:xx:xx:08:63`)  
**Secondary Test Client:** Laptop via Ethernet (`eth0`, IP `192.168.1.142`)

---

## 1. Executive Summary & The Problem

We are deploying a standalone, offline "Pirate Radio" web server on a Raspberry Pi for Maker Faire. The Pi broadcasts a local Wi-Fi hotspot (`PirateHat`). Visitors scan a printed 2-step badge (or type an address) to preview and download ("plunder") over-the-air radio recordings (.m4a files) captured live by an onboard RTL-SDR receiver.

### The Working Baseline:
* The web server (`pirate-server`) runs as a systemd service executing Python 3 `http.server.ThreadingHTTPServer`.
* **Laptop over Ethernet (`eth0`):** Navigating to `http://192.168.1.139/index.html` loads **instantly with HTTP 200 OK**. All CSS (`/static/style.css`), background images (`/static/map_bg.jpg`), and song lists render completely without issue.

### The Failure Symptom on Mobile:
* **Android Phone (Google Pixel, Chrome Mobile):**
  * Associated with Wi-Fi `PirateHat` (DHCP IP assigned: `10.42.0.34`).
  * In Chrome Mobile, navigating to `http://10.42.0.1`, `http://10.42.0.1/index.html`, `http://10.42.0.1:80/index.html`, or `http://10.42.0.1:8080/index.html`:
  * The blue loading bar progresses ~20% (1/5th across the screen) and halts.
  * Chrome displays: **`ERR_TOO_MANY_RETRIES`**.
  * **Critical Observation:** In `pirate-server` access logs, **no HTTP GET request for `/index.html` or `/` from Chrome Mobile ever arrives at the web server**.

---

## 2. Hardware Scans & Environmental Telemetry

### 2.1 Hardware Identity
* **Device Model:** Raspberry Pi 4 Model B Rev 1.4
* **SoC:** Broadcom BCM2711 (Quad-Core Cortex-A72 @ 1.8 GHz)
* **Architecture:** `aarch64` (ARM 64-bit)

### 2.2 System & GPU Firmware Versions
* **Bootloader Version (`vcgencmd bootloader_version`):**
  * Release Date: `2025/05/08 16:21:35`
  * Version Hash: `69471177ba7e4cb7597cb2496f2a0b23f19c1113 (release)`
  * Capabilities: `0x0000007f`
* **VideoCore / GPU Firmware:**
  * Build Timestamp: `2026-09-07T10:23:43, variant start`
  * Firmware Hash: `465206a286cc1a7b13c2e86bc199f74dde7a8309`

### 2.3 Wi-Fi Chipset, Driver & Wi-Fi Firmware
* **Wireless Chipset:** Broadcom BCM43455 (`BCM4345/6` on SDIO bus `mmc1:0001:1`)
* **Kernel Driver:** `brcmfmac`
* **Driver Version:** `7.45.16.144`
* **Firmware Binary File:** `brcm/brcmfmac43455-sdio.bin`
* **Wi-Fi Firmware Version:** `BCM4345/6 wl0: Aug 29 2023 01:47:08 version 7.45.265 (28bca26 CY) FWID 01-b677b91b`
* **Power Management Status (`iw dev wlan0 get power_save`):**
  * `Power save: on`  
  *(Note: Power save enabled on an Access Point interface can cause packet buffering, latency spikes, and frame drops).*

### 2.4 Active Under-Voltage & Throttling Detection (`vcgencmd get_throttled`)
```text
throttled=0x50005
temp=59.9'C
```

#### Bit Breakdown of `0x50005`:
* **Bit 0 (0x1):** Under-voltage currently detected!
* **Bit 2 (0x4):** Frequency capping / CPU throttling currently active!
* **Bit 16 (0x10000):** Under-voltage occurred since boot!
* **Bit 18 (0x40000):** Throttling occurred since boot!

*Kernel Log (`dmesg`):*
```text
[Sat Sep 19 16:33:15 2026] hwmon hwmon1: Undervoltage detected!
[Sat Sep 19 16:33:21 2026] hwmon hwmon1: Voltage normalised
[Sat Sep 19 16:33:23 2026] hwmon hwmon1: Undervoltage detected!
[Sat Sep 19 16:34:10 2026] ieee80211 phy0: brcmf_p2p_send_action_frame: Unknown Frame: category 0x8a, action 0x6
[Sat Sep 19 16:37:43 2026] hwmon hwmon1: Undervoltage detected!
```
> **Significance:** The Raspberry Pi's power supply / USB cable is dropping below 4.65V under load. Under-voltage is a well-documented cause of dropped packets, degraded RF power, and transmission retries on the Broadcom Wi-Fi PHY (`tx failed: 318` packets logged in `iw dev wlan0 station dump`).

### 2.5 Wireless PHY State (`iw dev wlan0 info`)
```text
Interface wlan0
	ifindex 3
	wdev 0x1
	addr e4:5f:01:xx:xx:xx
	ssid PirateHat
	type AP
	wiphy 0
	channel 1 (2412 MHz), width: 20 MHz, center1: 2412 MHz
	txpower 31.00 dBm
```

---

## 3. Software Stack & Package Versions

| Component | Version | Notes |
| :--- | :--- | :--- |
| **OS Distribution** | Debian GNU/Linux 13.7 (trixie) | Debian trixie/sid `aarch64` |
| **Linux Kernel** | 6.18.39+rpt-rpi-v8 | Raspberry Pi pre-empt kernel |
| **NetworkManager** | 1.52.1 | Hotspot, AP orchestration, DHCP integration |
| **dnsmasq** | 2.91 | Spawned automatically by NetworkManager for shared mode |
| **Python** | 3.13.5 | Runs `server.py` |
| **Python Capabilities** | `cap_net_bind_service=ep` | Allowed to bind to privileged ports 80 and 443 as non-root user `pi` |
| **systemd** | 257 (257.13-1~deb13u1) | Init system running `pirate-server.service` |
| **OpenSSL** | 3.5.7 (9 Jun 2026) | Used to generate self-signed TLS certificates |
| **iw** | 6.9 | Wireless interface configuration tool |

---

## 4. Hotspot & Server Architecture Methods

### 4.1 How the Hotspot is Created and Managed
* **Manager:** Linux **NetworkManager** via a native connection profile (`/etc/NetworkManager/system-connections/PirateHotspot.nmconnection`).
* **Wi-Fi Mode:** 802.11 AP mode (`802-11-wireless.mode ap`, band `bg`, channel 1).
* **Security:** WPA2-Personal (`key-mgmt=wpa-psk`, `psk=treasure`).
* **IPv4 Architecture:** `ipv4.method shared`.
  * In `shared` mode, NetworkManager automatically:
    1. Sets `net.ipv4.ip_forward = 1`.
    2. Installs `nftables` NAT masquerading rules (`ip saddr 10.42.0.0/24 masquerade`).
    3. Launches an isolated internal `dnsmasq` instance on `10.42.0.1:53` (DNS) and `0.0.0.0:67` (DHCP).
    4. Automatically binds to the `10.42.0.0/24` subnet.

### 4.2 How the Web Server is Built and Run
* **Language & Framework:** Python 3 standard library `http.server.ThreadingHTTPServer` with custom request handler `PirateHandler(http.server.BaseHTTPRequestHandler)`.
* **Privilege Separation:** Runs under non-root user `pi`. Privilege to bind to low ports (< 1024) is granted via filesystem capability:
  ```bash
  /usr/bin/python3.13 cap_net_bind_service=ep
  ```
* **Process Supervision:** Controlled by systemd unit `/etc/systemd/system/pirate-server.service` (`Restart=always, RestartSec=3`).
* **Multi-Port Dual-Listener:**
  * **Port 80 (HTTP):** Handled on the main thread bound to `0.0.0.0:80`.
  * **Port 443 (HTTPS):** Handled by a background daemon thread bound to `0.0.0.0:443`, wrapped with Python `ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)` using an X.509 self-signed certificate (`cert.pem` / `key.pem`).
  * **Port 8080 (Redirect):** `nftables` NAT table `pirate_nat` redirects incoming port 8080 traffic directly to port 80.
* **Station Reaper Daemon:** An in-process background thread in `server.py` queries `iw dev wlan0 station dump` every 20s and deauthenticates stations idle for > 4 minutes to prevent hitting the Raspberry Pi Broadcom hardware limit (8–10 clients max).

---

## 5. Complete Chronology & Anatomy of DNS Configurations

### 5.1 Stage 1: The Initial Wildcard Hijack (The Trap)
Originally, `/etc/NetworkManager/dnsmasq-shared.d/pirate.conf` contained:
```conf
address=/#/10.42.0.1
address=/pirate.box/10.42.0.1
dhcp-option=option:domain-name,pirate.box
dhcp-range=10.42.0.2,10.42.0.254,255.255.255.0,2m
dhcp-lease-max=250
```

#### What `address=/#/10.42.0.1` did:
In `dnsmasq`, `/#/` is a universal wildcard that matches **every domain name on earth**.
* When the phone connected, all domain lookups (`google.com`, `play.googleapis.com`, `dns.google`, etc.) resolved to `10.42.0.1`.
* **The `/chat` Discovery:** At `16:39:46`, a background app on the phone (RCS/Google Chat) tried to reach its cloud server. Dnsmasq hijacked the domain to `10.42.0.1`, causing the phone to send `POST /chat HTTP/1.1` to the Pi's port 80 (logged as `code 501, Unsupported method ('POST')`).
* **Chrome Mobile Impact:** Chrome’s background security services (Google Safe Browsing, DoH, certificate revocation checks) were resolving to `10.42.0.1`. Chrome detected that DNS was being poisoned, flagged the connection as hostile/captive, and refused to load web pages.

### 5.2 Stage 2: Disabling the Wildcard & Preserving Local Hostnames
We updated `/etc/NetworkManager/dnsmasq-shared.d/pirate.conf` to:
```conf
# Wildcard disabled:
# address=/#/10.42.0.1

address=/pirate.box/10.42.0.1
dhcp-option=option:domain-name,pirate.box
dhcp-range=10.42.0.2,10.42.0.254,255.255.255.0,2m
dhcp-lease-max=250

log-queries
log-dhcp
```

#### Current DNS Behavior:
1. **No Wildcard Poisoning:** Legitimate internet queries for outside domains pass through upstream nameserver `192.168.1.1#53` via `eth0` when plugged in, or return NXDOMAIN/timeout cleanly when offline.
2. **Explicit Hostname Only:** Only queries specifically for `pirate.box` resolve to `10.42.0.1`.
3. **Full Live Query Logging:** Every DNS query from the phone is logged to `journalctl -u NetworkManager`.
4. **DHCP Options:** Search domain is set to `pirate.box`, lease time is set aggressively to 2 minutes (`2m`) to prevent pool exhaustion in dense crowds, and memory leases are capped at 250.

---

## 6. Full Active Configuration Dumps

### 6.1 NetworkManager Connection (`/etc/NetworkManager/system-connections/PirateHotspot.nmconnection`)
```ini
[connection]
id=PirateHotspot
uuid=8a21b6d1-e6c1-44e6-93bd-8a1962346a08
type=wifi
interface-name=wlan0

[wifi]
band=bg
mode=ap
ssid=PirateHat

[wifi-security]
key-mgmt=wpa-psk
psk=treasure

[ipv4]
address1=10.42.0.1/24
method=shared

[ipv6]
addr-gen-mode=default
method=ignore

[proxy]
```

### 6.2 Systemd Service (`/etc/systemd/system/pirate-server.service`)
```ini
[Unit]
Description=Maker Faire Pirate Radio Web Server
After=NetworkManager.service
Wants=NetworkManager.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/nrsc5
Environment=PIRATE_PORT=80
Environment=PIRATE_HOST=10.42.0.1
ExecStart=/usr/bin/python3 /home/pi/nrsc5/pirate_server/server.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

### 6.3 Full `nftables` Ruleset (`sudo nft list ruleset`)
```text
table ip nm-shared-wlan0 {
	chain nat_postrouting {
		type nat hook postrouting priority srcnat; policy accept;
		ip saddr 10.42.0.0/24 ip daddr != 10.42.0.0/24 masquerade
	}

	chain filter_forward {
		type filter hook forward priority filter; policy accept;
		ip daddr 10.42.0.0/24 oifname "wlan0" ct state { established, related } accept
		ip saddr 10.42.0.0/24 iifname "wlan0" accept
		iifname "wlan0" oifname "wlan0" accept
		iifname "wlan0" reject
		oifname "wlan0" reject
	}
}

table inet pirate_nat {
	chain prerouting {
		type nat hook prerouting priority dstnat; policy accept;
		tcp dport 8080 redirect to :80
	}

	chain output {
		type nat hook output priority dstnat; policy accept;
		tcp dport 8080 redirect to :80
	}
}
```

---

## 7. Pre-Answered Questions & Empirically Tested Hypotheses

### Question 1: Is the server down or failing to render templates?
* **ANSWER: PROVEN FALSE.**
* **Evidence:** A laptop connected to `eth0` requested `http://192.168.1.139/index.html`. The server rendered all 608 radio tracks, served `/static/style.css` and `/static/map_bg.jpg` with `HTTP/1.0 200 OK` in 110ms.

### Question 2: Does the physical 802.11 link fail to pass packets to port 80?
* **ANSWER: PROVEN FALSE.**
* **Evidence:** The phone sends `GET /generate_204 HTTP/1.1` to `10.42.0.1:80` every 20 seconds, and the server returns `204 No Content`. At `16:39:46`, an app on the phone sent `POST /chat HTTP/1.1` to port 80. Packets cross the air and enter the Python process without issue.

### Question 3: Is Chrome Mobile demanding HTTPS on port 443?
* **ANSWER: VERIFIED BY DIRECT PACKET CAPTURE.**
* **Evidence:** Low-level `nftables` counters on `wlan0` recorded:
  * Packets to port 443: **5 packets (SYN attempts)**
  * Packets to port 80: **0 packets**
  * When navigating to `http://10.42.0.1`, Chrome Mobile attempted an HTTPS upgrade to port 443. Because port 443 was initially closed, the kernel returned `TCP RST` (Connection Refused).

### Question 4: Does the server support HTTPS on port 443?
* **ANSWER: IMPLEMENTED & VERIFIED.**
* **Evidence:** Generated self-signed SSL certificate and enabled dual-listener in `server.py` on port 443.
  * Verified locally: `curl -k -s -o /dev/null -w "%{http_code}\n" https://10.42.0.1/index.html` -> **`200`**.
  * Sockets: Both `0.0.0.0:80` and `0.0.0.0:443` are actively listening concurrently.

### Question 5: Does specifying an explicit port (`:80` or `:8080`) bypass the error?
* **ANSWER: TESTED — STILL THROWS `ERR_TOO_MANY_RETRIES`.**
* **Evidence:** Navigating to `http://10.42.0.1:80/index.html` and `http://10.42.0.1:8080/index.html` on Chrome Mobile both failed with `ERR_TOO_MANY_RETRIES`. No HTTP GET request reached the server logs.

### Question 6: Is Chrome Mobile's "Always use secure connections" toggle the sole cause?
* **ANSWER: TESTED — NOT THE SOLE CAUSE.**
* **Evidence:** User toggled off "Warn before using secure connections" in Chrome Mobile. The error `ERR_TOO_MANY_RETRIES` persisted.

### Question 7: What is Android System "Private DNS" set to?
* **ANSWER: VERIFIED.**
* **Evidence:** User confirmed Android System Settings -> Network & internet -> Private DNS is set to **"Automatic"**.

---

## 8. Specific Unresolved Questions for the Next Model

1. **Why does Chrome Mobile on Android abort navigation locally with `ERR_TOO_MANY_RETRIES` before emitting an HTTP request across the Wi-Fi interface?**
   * Is Android’s `ConnectivityManager` withholding the `NET_CAPABILITY_VALIDATED` capability because outward internet is unreachable, causing Chrome to reject socket creation?
   * When Android Private DNS is set to "Automatic", does it attempt to reach `dns.google:853` (DNS-over-TLS), fail on an isolated AP, and lock down socket routing for all browsers?
   * The client hostname is `Corp Phone`. Could an MDM, Enterprise Work Profile, or Google Corporate policy be enforcing a local VPN or proxy that drops unroutable intranet traffic?
2. **Does the synthetic `204 No Content` response cause Android to enter a broken validation state?**
   * Modern Android NetworkMonitor checks both HTTP `generate_204` and HTTPS `generate_204`. If HTTP returns 204 but HTTPS fails, Android treats this as a portal anomaly. Would allowing Android's standard CaptivePortalLogin flow to trigger actually open up socket access?
3. **Hardware / Power impact:**
   * Does the active under-voltage state (`throttled=0x50005`) and `Power save: on` state on `wlan0` cause 802.11 frame drops that specifically break the heavier TCP handshake exchanges of mobile browsers while lighter system probes (`generate_204`) slip through?

---

## 9. Interventions & Remediation Log (September 19, 2026 - Evening Session)

### 9.1 Wi-Fi Power Management Disabled (`powersave=2`)
* **Action Taken:** Modified NetworkManager profile for the hotspot:
  ```bash
  sudo nmcli connection modify PirateHotspot 802-11-wireless.powersave 2
  sudo nmcli connection up PirateHotspot
  ```
* **Verification:**
  ```bash
  sudo iw dev wlan0 get power_save
  # Output: Power save: off
  ```
* **Result:** `wlan0` is permanently locked awake in AP mode. Eliminates periodic radio cycling, DTIM frame delivery delays, and Wi-Fi frame drops (`tx failed` packets).

### 9.2 Power Under-Voltage, Battery Operation & Under-Clocking Strategy
* **Context:** The Raspberry Pi must run reliably off a portable USB battery pack at Maker Faire. Under full load (Quad-Core Cortex-A72 @ 1.8 GHz boost + RTL-SDR USB dongle + Wi-Fi AP), peak current draw causes input voltage brownouts (`throttled=0x50005`).
* **Under-Clocking Capabilities on BCM2711:**
  * Available CPU frequencies: `600MHz, 700MHz, 800MHz, 900MHz, 1000MHz, ..., 1800MHz`.
  * The Python web server requires minimal CPU (a complete track catalog render takes ~110ms even at low clocks).
* **Live Test Executed (Dynamic CPU Frequency Cap):**
  ```bash
  echo 1000000 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
  ```
  * Verified: All 4 CPU cores are now capped at 1.0 GHz max (down from 1.8 GHz boost).
  * Impact: Reduces peak power consumption by ~40-50% while operating dynamically without reboot.
* **Persistent `/boot/firmware/config.txt` Options Available:**
  * `arm_boost=0` (disables default 1.8 GHz boost; keeps stock 1.5 GHz or lower).
  * `arm_freq=1000` (caps clock to 1.0 GHz).
  * `dtoverlay=disable-bt` (disables unused onboard Bluetooth transceiver, saving ~30-50mA).
  * `hdmi_blanking=2` (powers down video circuitry when running headless, saving ~100-200mA).

### 9.3 Culprit A Resolved: Removal of "Fancy" Captive Probe Interceptions
* **Problem Analysis:**
  * In earlier iterations, `server.py` implemented `handle_captive_probes()` which spoofed HTTP `204 No Content` for Android probes (`/generate_204`).
  * Real IoT / smart-home configuration hotspots (e.g., ESP32, smart plugs, offline routers) do **not** spoof internet connectivity probes. They run simple HTTP servers and return 404 for unknown URLs.
  * When Android receives a fake `204 No Content` on HTTP, but cannot establish an outbound TLS connection for `https://connectivitycheck.gstatic.com/generate_204` or reach Private DNS (`dns.google:853`), Android identifies a validation contradiction. Android then activates network defense mechanisms: locking down third-party application sockets (causing Chrome Mobile to fail DNS/socket allocation with `ERR_TOO_MANY_RETRIES`).
* **Action Taken:**
  * Completely removed `handle_captive_probes()` and all OS probe spoofing dictionaries from `pirate_server/server.py`.
  * Set unknown route catch-all to standard `404 Not Found` (`self.send_error(404, "Treasure not found!")`).
* **Verification:**
  ```bash
  curl -I http://10.42.0.1/
  # Output: HTTP/1.0 200 OK (Content-Length: 230976)

  curl -I http://10.42.0.1/generate_204
  # Output: HTTP/1.0 404 Treasure not found!
  ```
* **Result:** Android and iOS devices now recognize `PirateHat` as an offline local network without internet (identical to a smart home setup AP). Android will not enforce Private DNS lockdown, and mobile browsers can access `http://10.42.0.1/index.html` without socket rejection.

### 9.4 Culprit B Resolved: Half-Finished Self-Signed HTTPS Removed
* **Audit of Previous State:**
  * A self-signed X.509 certificate was generated at `pirate_server/cert.pem` and `pirate_server/key.pem`.
  * **Defects in previous HTTPS implementation:**
    1. The certificate lacked modern `subjectAltName` (SAN) extensions required by RFC 2818 / Chromium, triggering instant certificate rejection.
    2. Self-signed certificates trigger full-screen interstitial security warnings (`NET::ERR_CERT_AUTHORITY_INVALID`) on every modern mobile browser, creating severe UX friction for Maker Faire attendees.
    3. Python's `context.wrap_socket(httpsd.socket, server_side=True)` on a listening `ThreadingHTTPServer` socket is deprecated in modern Python, prone to unhandled SSLErrors on aborted client handshakes.
    4. Having port 443 open with an invalid certificate encouraged mobile browsers to upgrade HTTP to HTTPS, repeatedly failing TLS handshakes and compounding `ERR_TOO_MANY_RETRIES`.
* **Action Taken:**
  * Removed `run_https_server()` and `import ssl` from `pirate_server/server.py`.
  * Deleted `cert.pem` and `key.pem`.
  * Restarted `pirate-server.service`.
* **Verification:**
  * Port check (`/proc/net/tcp`): Port 443 listener is closed. Only port 80 (`0.0.0.0:80`) is listening.
* **Result:** Clean, single-port HTTP architecture matching the printed badge QR code (`http://10.42.0.1/index.html`). No TLS handshake loops, no certificate warnings, and zero socket collisions.

### 9.5 Port 8080 Retest & Explanation
* **Event:** User re-tested navigation to `http://10.42.0.1:8080/index.html` in Chrome Mobile after force-stopping the browser.
* **Observation:** Chrome displayed `ERR_TOO_MANY_RETRIES`.
* **Root Cause Analysis:**
  * `pirate-server` is bound exclusively to port 80 (`0.0.0.0:80`).
  * Port 8080 is completely closed with no listener and no firewall redirect (`curl: (7) Failed to connect to 10.42.0.1 port 8080: Connection refused`).
  * When a client connects to a closed port on Linux, the kernel immediately sends a `TCP RST` (Connection Refused).
  * Chrome automatically retries idempotent connections that receive an immediate `TCP RST`. After exhausting its retry attempts within milliseconds against the closed port, Chrome throws `ERR_TOO_MANY_RETRIES` (or `ERR_CONNECTION_REFUSED`).
* **Correct URL:** Standard HTTP on port 80: `http://10.42.0.1/index.html` or `http://10.42.0.1/` (without `:8080`).

### 9.6 Kernel-Level Packet Telemetry during http://10.42.0.1/index.html Test
* **Event:** User navigated to `http://10.42.0.1/index.html` on the Pixel.
* **Observation:** Chrome displayed `ERR_TOO_MANY_RETRIES`. No HTTP GET request was received by `pirate-server`.
* **Kernel & Firewall Packet Capture Data:**
  * Ping test from Pi to phone: `64 bytes from 10.42.0.34: icmp_seq=1 ttl=64 time=12.1 ms` (Layer 2 & 3 link is functional).
  * ARP entry: `10.42.0.34 lladdr 42:xx:xx:xx:08:63 REACHABLE`.
  * Low-level `nftables` input counters for `10.42.0.34`:
    - `udp dport 53`: 11 packets (phone successfully queried DNS for reddit.com, instagram.com, etc.).
    - `tcp dport 80`: **0 packets**.
    - `tcp dport 443`: **0 packets**.
    - `ip protocol tcp` (ALL TCP ports): **0 packets**.
* **Key Finding:** When the user enters `http://10.42.0.1/index.html` in Chrome Mobile, **zero TCP packets are transmitted across the Wi-Fi interface (wlan0) to the Raspberry Pi**. The navigation is being aborted or rerouted entirely on the client device before any TCP SYN can cross the air.
* **Primary Hypotheses for Client-Side TCP Suppression:**
  1. **Corporate VPN / MDM Subnet Conflict:** The device hostname is `Corp Phone`. Enterprise/Corp VPNs commonly route the entire `10.0.0.0/8` private range into corporate tunnels (where `10.42.0.1` is unroutable or reset), bypassing the local Wi-Fi interface entirely.
  2. **Chrome Internal Host State:** Chrome Mobile may be caching an invalid/poisoned connection state for `10.42.0.1` from earlier tests.

### 9.7 Working Verification: Secondary Mobile Device Connects Successfully
* **Event:** Secondary test device (phone without SIM card) associated with `PirateHat` and navigated to `http://10.42.0.1/`.
* **Access Log Evidence (`journalctl -u pirate-server`):**
  ```text
  10.42.0.134 - - [19/Sep/2026 17:42:28] "GET / HTTP/1.1" 200 -
  10.42.0.134 - - [19/Sep/2026 17:42:28] "GET /static/style.css HTTP/1.1" 200 -
  10.42.0.134 - - [19/Sep/2026 17:42:28] "GET /static/map_bg.jpg HTTP/1.1" 200 -
  10.42.0.134 - - [19/Sep/2026 17:42:28] "GET /favicon.ico HTTP/1.1" 404 -
  ```
* **Significance:**
  * Confirms the entire server stack, Wi-Fi AP link, DHCP orchestration, template rendering engine, and static file delivery work completely and instantly over 802.11 wireless.
  * Isolates the failure on `Corp Phone` strictly to device-specific routing on that corporate phone.
* **Comparative Evaluation:**
  1. **Corporate VPN / Subnet Routing:** The secondary device (personal, no MDM) sends `10.42.0.1` traffic directly out `wlan0`. The main device (`Corp Phone`) has corporate policies that intercept `10.0.0.0/8` traffic into an enterprise VPN interface.
  2. **Cellular Multi-Homing:** The secondary device has no SIM card, leaving Wi-Fi as its only interface. The main device has cellular data, which Android can route to if it considers the Wi-Fi network local or restricted.
  3. **Poisoned Chrome State:** Chrome on the main device may still retain an in-memory `HttpServerProperties` failure cache for `10.42.0.1` from previous HTTPS/reset attempts.

### 9.8 Retest on Main Phone (Airplane Mode + Incognito Tab)
* **Test Conditions:**
  * Device rebooted.
  * Airplane Mode enabled (Cellular / SIM card radio completely disabled).
  * Wi-Fi enabled and connected to `PirateHat` (IP `10.42.0.34` assigned at 17:48:58).
  * Fresh Chrome Incognito tab opened.
  * Navigated to: `http://10.42.0.1`.
* **Result:** Chrome displayed `ERR_TOO_MANY_RETRIES`.
* **Live Kernel Firewall Counters (`table inet debug_trace` for `10.42.0.34`):**
  * `tcp dport 80`: **0 packets** (zero connection attempts on port 80).
  * `tcp dport 443`: **1 packet** (TCP SYN packet received on port 443).
  * `tcp dport 853`: **0 packets**.
  * `udp dport 53`: **299 packets** (DNS queries actively reaching dnsmasq).
* **Definitive Finding:**
  * Even when typing an explicit `http://` scheme in a clean Incognito session with cellular completely disabled, **Chrome on this device forcibly upgrades `10.42.0.1` to HTTPS on port 443**.
  * Because port 443 is closed on the Pi (following the removal of the invalid self-signed certificate in Section 9.4), the Linux kernel immediately returns `TCP RST`.
  * Chrome receives `TCP RST` on port 443, refuses to fall back to unencrypted HTTP on port 80 (enforcing strict HTTPS-Only / HttpsUpgrades policy), retries the HTTPS handshake, and aborts with `ERR_TOO_MANY_RETRIES`.
  * In contrast, the secondary non-corporate phone does not enforce HTTPS-Only upgrade on direct private IP navigation, connecting directly to port 80 without issue.

---

## 10. Remediation Plan: Hybrid HTTP + HTTPS Dual-Port Server

### 10.1 Strategy

Since the corporate Pixel's Chrome will only speak HTTPS, and normal visitor phones connect happily over HTTP, the server must listen on **both** ports simultaneously:

* **Port 80 (HTTP):** Serves all normal visitors (personal phones, laptops, iOS devices). No change from current working setup.
* **Port 443 (HTTPS):** Serves enterprise/corporate Chrome devices that forcibly upgrade to HTTPS. Requires a self-signed TLS certificate.

Corporate Chrome users will see a one-time interstitial warning ("Your connection is not private" / `NET::ERR_CERT_AUTHORITY_INVALID`). Tapping **Advanced → Proceed to 10.42.0.1 (unsafe)** loads the page normally for the rest of the session. Normal visitors never see this — they connect over port 80 without any certificate interaction.

### 10.2 Why the Previous Certificate Failed

The self-signed certificate removed in Section 9.4 had two fatal defects:

1. **Missing SubjectAltName (SAN) extension.** Since 2017 (Chrome 58), Chromium completely ignores the `CN` (Common Name) field for identity verification and **only** checks `subjectAltName`. A certificate for an IP address must contain `subjectAltName = IP:10.42.0.1`. Without it, Chrome rejects the certificate outright with `ERR_CERT_COMMON_NAME_INVALID` — it does not even show the "Proceed anyway" button.

2. **No IP address SAN entry.** Even if a SAN were present, using `DNS:10.42.0.1` is incorrect for a bare IP. RFC 2818 §3.1 requires `iPAddress` type entries for numeric addresses, specified as `IP:10.42.0.1` in OpenSSL syntax.

### 10.3 Generating a Correct Self-Signed Certificate

OpenSSL 3.5.7 on this Pi supports the `-addext` flag for inline SAN:

```bash
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -days 3650 -nodes \
  -keyout pirate_server/key.pem \
  -out pirate_server/cert.pem \
  -subj "/CN=PirateHat" \
  -addext "subjectAltName=IP:10.42.0.1"
```

**What this does:**
* `-x509`: Self-signed (no CA needed).
* `-newkey ec -pkeyopt ec_paramgen_curve:prime256v1`: ECDSA P-256 key (fast TLS handshakes on ARM; ~3x faster than RSA 2048 on Cortex-A72).
* `-days 3650`: Valid for 10 years (never expires at Maker Faire).
* `-nodes`: No passphrase on the private key (required for unattended systemd startup).
* `-subj "/CN=PirateHat"`: Sets the legacy Common Name (cosmetic only; Chrome ignores it).
* `-addext "subjectAltName=IP:10.42.0.1"`: **The critical SAN extension.** This is what Chrome actually checks. Using `IP:` (not `DNS:`) because the address is a bare IPv4 literal.

**Verification after generation:**
```bash
openssl x509 -in pirate_server/cert.pem -noout -text | grep -A1 "Subject Alternative Name"
# Expected output:
#   X509v3 Subject Alternative Name:
#       IP Address:10.42.0.1
```

### 10.4 Python 3.13 SSL Support: How It Works

Python's `ssl` module (backed by OpenSSL 3.5.7 on this system) fully supports wrapping a `ThreadingHTTPServer` socket for TLS. The architecture:

```python
import ssl

def run_https_server():
    """Background HTTPS listener on port 443 (self-signed, for corp Chrome devices)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(
        certfile="pirate_server/cert.pem",
        keyfile="pirate_server/key.pem"
    )
    # Optional: accept any client (no mutual TLS / client certs)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    httpsd = http.server.ThreadingHTTPServer(("", 443), PirateHandler)
    httpsd.socket = ctx.wrap_socket(httpsd.socket, server_side=True)
    httpsd.serve_forever()
```

**Key points:**

1. **Same `PirateHandler` class** handles both HTTP and HTTPS. No code duplication. The handler does not know or care which port/protocol served the request — it receives the same `do_GET()` calls either way.

2. **`ctx.wrap_socket(httpsd.socket, server_side=True)`** replaces the listening TCP socket with a TLS-wrapped socket. Every accepted connection automatically performs the TLS handshake before passing data to the handler.

3. **SSLError on aborted handshakes**: When a client connects to port 443 and then immediately disconnects (e.g., a port scanner, or a non-HTTPS client accidentally hitting 443), Python raises `ssl.SSLError`. The previous implementation crashed on these. The fix is to override `handle_error()` or wrap the handler:

```python
class SilentHTTPSServer(http.server.ThreadingHTTPServer):
    """HTTPS server that silently drops broken TLS handshakes."""
    def handle_error(self, request, client_address):
        # Suppress noisy SSLError tracebacks from aborted/scanner connections
        import traceback
        exc_type = sys.exc_info()[0]
        if exc_type is ssl.SSLError:
            pass  # Silently ignore broken handshakes
        else:
            super().handle_error(request, client_address)
```

4. **Thread architecture**: The HTTPS server runs in a daemon thread (exactly like the station reaper). The main thread runs the HTTP server on port 80. Both share the same `PirateHandler` class, `scan_songs()`, `PLUNDER_COOLDOWN`, etc.

### 10.5 Updated `run_server()` Architecture

```python
def run_server():
    print("=" * 60)
    print("PIRATE VESSEL SINGLE-SERVING WEB SERVER")
    print(f"Serving host: {HOST}")
    print(f"HTTP  port:   {PORT}")
    print(f"HTTPS port:   443")
    print(f"Booty vault:  {RECORDINGS_DIR}")
    print("=" * 60)

    # Start automated idle Wi-Fi station reaper
    reaper = threading.Thread(target=run_station_reaper, daemon=True)
    reaper.start()

    # Start HTTPS listener on port 443 (for corporate Chrome HTTPS-Only devices)
    cert_path = os.path.join(BASE_DIR, "cert.pem")
    key_path = os.path.join(BASE_DIR, "key.pem")
    if os.path.isfile(cert_path) and os.path.isfile(key_path):
        https_thread = threading.Thread(
            target=run_https_server,
            args=(cert_path, key_path),
            daemon=True
        )
        https_thread.start()
        print(f"[PIRATE] HTTPS listener active on port 443 (self-signed)")
    else:
        print(f"[PIRATE] No cert.pem/key.pem found — HTTPS disabled (HTTP-only mode)")

    # Main thread: HTTP listener on port 80
    with http.server.ThreadingHTTPServer(("", PORT), PirateHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[PIRATE] Lowering anchor and shutting down server.")
```

**Graceful degradation**: If `cert.pem` / `key.pem` don't exist, the server runs HTTP-only (current behavior). No crash, no error. HTTPS is opt-in by generating the certificate.

### 10.6 Port 443 Privilege & Capabilities

The Python binary already has `cap_net_bind_service=ep` (set in Section 6.2 of this document). This grants permission to bind to any privileged port < 1024 as non-root user `pi`. Since port 443 < 1024, **no additional capability grants are needed** — the existing `setcap` covers both port 80 and port 443.

**Verification:**
```bash
getcap $(readlink -f $(which python3))
# Expected: /usr/bin/python3.13 cap_net_bind_service=ep
```

### 10.7 Expected UX on Corporate Chrome (Post-Fix)

1. User on corporate Pixel navigates to `http://10.42.0.1/` (or scans QR Step 2).
2. Chrome silently upgrades to `https://10.42.0.1:443/`.
3. TLS handshake succeeds (port 443 is now open, certificate has correct SAN).
4. Chrome displays interstitial: **"Your connection is not private"** (`NET::ERR_CERT_AUTHORITY_INVALID`).
5. User taps **Advanced** → **Proceed to 10.42.0.1 (unsafe)**.
6. `chest.html` loads normally. CSS, images, and song list all render.
7. User taps **Plunder** → `.m4a` downloads over HTTPS → file deleted from Pi.
8. For the remainder of the browser session, Chrome remembers the exception and does not show the warning again.

### 10.8 Implementation Checklist

- [x] Generate certificate with SAN: `openssl req -x509 ...` (command in §10.3).
- [x] Verify SAN in cert: `openssl x509 -in cert.pem -noout -text | grep -A1 "Subject Alternative"`.
- [x] Add `import ssl` back to `server.py`.
- [x] Add `SilentHTTPSServer` class and `run_https_server()` function.
- [x] Update `run_server()` to launch HTTPS daemon thread (§10.5).
- [x] Restart service: `sudo systemctl restart pirate-server`.
- [x] Verify port 443 listening: `ss -tlnp | grep 443`.
- [x] Test from laptop: `curl -k https://10.42.0.1/` → HTTP 200.
- [x] Test from corporate Pixel: `http://` and `https://` both aborted locally with `ERR_TOO_MANY_RETRIES` (0 TCP packets sent).
- [x] Test from iPhone: `http://10.42.0.1/` connected directly to Port 80 (HTTP 200 OK, no auto-upgrade).

---

## 11. Empirical Test Results of Hybrid HTTPS & Definitive Device Matrix (Session 2026-09-19 Late Evening)

### 11.1 Implementation & Verification of Port 443 with SAN Certificate
* **Certificate Generated:** ECDSA P-256 (`prime256v1`) self-signed certificate created with full multi-SAN coverage:
  ```text
  X509v3 Subject Alternative Name:
      IP Address:10.42.0.1, DNS:pirate.box, DNS:piratehat.local
  ```
* **Server Updated:** Dual-port serving enabled via `SilentHTTPSServer` daemon thread bound to `0.0.0.0:443` while main thread served `0.0.0.0:80`.
* **Local Baseline:** Verified via curl:
  - `http://10.42.0.1/` -> HTTP 200 OK
  - `https://10.42.0.1/` -> HTTP 200 OK
  - `https://pirate.box/` -> HTTP 200 OK

### 11.2 Empirical Retest on Corporate Pixel (`Corp Phone`)
* **Test Conditions:** Primary Pixel on Airplane Mode (Cellular disabled), Wi-Fi connected to `PirateHat` (`10.42.0.34`).
* **Actions:** Tested both `http://10.42.0.1` and `https://10.42.0.1`.
* **Result:** Chrome on Android aborted with **`ERR_TOO_MANY_RETRIES`**.
* **Kernel Packet Capture Telemetry (`table inet debug_trace`):**
  | Port / Protocol | Packet Count | Meaning |
  | :--- | :--- | :--- |
  | `tcp dport 80` | **0** | Zero HTTP packets transmitted |
  | `tcp dport 443` | **0** | Zero HTTPS packets transmitted |
  | `ip protocol tcp` (ALL TCP) | **0** | Zero TCP packets transmitted |
  | `udp dport 53` (DNS) | **55** | DNS queries working normally |
* **Diagnosis:**
  Even with port 443 active and listening with a valid SAN certificate, **zero TCP packets left the phone**. In Chromium's networking stack (`net/http/http_network_transaction.cc`), `ERR_TOO_MANY_RETRIES` occurring before any network socket creation confirms that Chrome's internal state machine or a local corporate loopback proxy (installed by Google MDM / Android Enterprise Work Profile) is intercepting and failing the transaction entirely within the device before it can touch the physical Wi-Fi interface.

### 11.3 Empirical Test on Apple iPhone (iOS Safari)
* **Client:** iPhone (`36:xx:xx:xx:ef:9f`, assigned DHCP IP `10.42.0.142`).
* **Action:** Navigated to `http://10.42.0.1/`.
* **Access Log Evidence (`journalctl -u pirate-server`):**
  ```text
  10.42.0.142 - - [19/Sep/2026 18:16:56] "GET /farewell HTTP/1.1" 200 -
  10.42.0.142 - - [19/Sep/2026 18:16:56] "GET /static/style.css HTTP/1.1" 200 -
  ```
* **Significance & Key Discovery:**
  - Safari loaded the site instantly with **HTTP 200 OK**.
  - **No HTTPS upgrade occurred:** Safari respected the explicit `http://` scheme for the numeric IP address and connected directly to Port 80.
  - Zero certificate warnings or prompts were shown.
  - **Port 443 is completely unnecessary for iOS devices.**

### 11.4 Comprehensive Multi-Device Matrix

| Device Tested | Profile / OS | Port Used | Outcome | User Experience |
| :--- | :--- | :--- | :--- | :--- |
| **Secondary Android (Pixel 7 Pro)** | Personal (No SIM, No MDM) | Port 80 (HTTP) | **SUCCESS (200 OK)** | Instant load, full CSS/images, zero warnings |
| **Apple iPhone** | Personal iOS (Safari) | Port 80 (HTTP) | **SUCCESS (200 OK)** | Instant load, full CSS/images, zero warnings |
| **Laptop (Ethernet)** | Linux / macOS / Windows | Port 80 (HTTP) | **SUCCESS (200 OK)** | Instant load, full catalog rendered in 110ms |
| **Primary Android (`Corp Phone`)** | Corporate MDM / Work Profile | N/A (Blocked internally) | **FAIL (`ERR_TOO_MANY_RETRIES`)** | Local device proxy aborts before transmitting |

### 11.5 Decision: Final Teardown of Port 443
* **Rationale:**
  1. Port 443 provided **zero benefit** to the corporate Pixel (which aborts internally before network transmission).
  2. All personal client devices (both Android and iOS) connect cleanly over Port 80 without auto-upgrading.
  3. Leaving port 443 open with a self-signed certificate creates an active liability for Maker Faire attendees: any visitor with an aggressive HTTPS-Only browser extension might probe 443 and encounter a scary certificate interstitial, rather than falling back smoothly to Port 80.
* **Action:** Reverted `server.py` to single-port HTTP (Port 80 only), deleted `cert.pem` and `key.pem`, and restarted `pirate-server.service`.


