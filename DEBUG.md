# Comprehensive Debug & Diagnostics Package: PirateHat Wi-Fi Hotspot & Web Server

**Document Purpose:** Self-contained briefing for an external engineer/thinking model with zero physical or remote access to the Raspberry Pi. Contains all system telemetry, hardware scans, firmware versions, wireless configurations, software architectures, packet captures, DNS change histories, and pre-answered questions.

**Date:** September 19, 2026  
**System:** Raspberry Pi 4 Model B Rev 1.4  
**OS:** Debian GNU/Linux 13.7 (trixie) (aarch64)  
**Kernel:** Linux piratehat 6.18.39+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.18.39-1+rpt1 (2026-07-29)  
**Workspace:** `/home/benjamin/nrsc5`  
**Host & AP SSID:** `PirateHat` on `wlan0`  
**Primary Test Client:** Google Pixel (`Benjamin-s-Pixel-Corp`, MAC `42:7c:40:5e:08:63`)  
**Secondary Test Client:** Laptop via Ethernet (`eth0`, IP `192.168.86.142`)

---

## 1. Executive Summary & The Problem

We are deploying a standalone, offline "Pirate Radio" web server on a Raspberry Pi for Maker Faire. The Pi broadcasts a local Wi-Fi hotspot (`PirateHat`). Visitors scan a printed 2-step badge (or type an address) to preview and download ("plunder") over-the-air radio recordings (.m4a files) captured live by an onboard RTL-SDR receiver.

### The Working Baseline:
* The web server (`pirate-server`) runs as a systemd service executing Python 3 `http.server.ThreadingHTTPServer`.
* **Laptop over Ethernet (`eth0`):** Navigating to `http://192.168.86.139/index.html` loads **instantly with HTTP 200 OK**. All CSS (`/static/style.css`), background images (`/static/map_bg.jpg`), and song lists render completely without issue.

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
	addr e4:5f:01:0a:23:d0
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
| **Python Capabilities** | `cap_net_bind_service=ep` | Allowed to bind to privileged ports 80 and 443 as user `benjamin` |
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
* **Privilege Separation:** Runs under non-root user `benjamin`. Privilege to bind to low ports (< 1024) is granted via filesystem capability:
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
1. **No Wildcard Poisoning:** Legitimate internet queries for outside domains pass through upstream nameserver `192.168.86.1#53` via `eth0` when plugged in, or return NXDOMAIN/timeout cleanly when offline.
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
User=benjamin
WorkingDirectory=/home/benjamin/nrsc5
Environment=PIRATE_PORT=80
Environment=PIRATE_HOST=10.42.0.1
ExecStart=/usr/bin/python3 /home/benjamin/nrsc5/pirate_server/server.py
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
* **Evidence:** A laptop connected to `eth0` requested `http://192.168.86.139/index.html`. The server rendered all 608 radio tracks, served `/static/style.css` and `/static/map_bg.jpg` with `HTTP/1.0 200 OK` in 110ms.

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
   * The client hostname is `Benjamin-s-Pixel-Corp`. Could an MDM, Enterprise Work Profile, or Google Corporate policy be enforcing a local VPN or proxy that drops unroutable intranet traffic?
2. **Does the synthetic `204 No Content` response cause Android to enter a broken validation state?**
   * Modern Android NetworkMonitor checks both HTTP `generate_204` and HTTPS `generate_204`. If HTTP returns 204 but HTTPS fails, Android treats this as a portal anomaly. Would allowing Android's standard CaptivePortalLogin flow to trigger actually open up socket access?
3. **Hardware / Power impact:**
   * Does the active under-voltage state (`throttled=0x50005`) and `Power save: on` state on `wlan0` cause 802.11 frame drops that specifically break the heavier TCP handshake exchanges of mobile browsers while lighter system probes (`generate_204`) slip through?
