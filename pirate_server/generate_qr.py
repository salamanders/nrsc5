#!/usr/bin/env python3
"""
Generates printable QR codes for the Pirate Hat badge:
1. Wi-Fi Auto-Login: WIFI:S:PirateHat;T:nopass;;
2. Web Server URL:   http://192.168.4.1
"""

import sys
import os
import urllib.parse

def generate_html_card(ssid="PirateHat", ip="192.168.4.1", output_html="pirate_badge.html"):
    wifi_str = f"WIFI:S:{ssid};T:nopass;;"
    url_str = f"http://{ip}"

    # Google Charts API URL for printable QR codes if connected to internet,
    # and instructions on how to use qrencode locally on Linux/Raspberry Pi
    wifi_qr_url = "https://chart.googleapis.com/chart?chs=300x300&cht=qr&chl=" + urllib.parse.quote(wifi_str)
    web_qr_url = "https://chart.googleapis.com/chart?chs=300x300&cht=qr&chl=" + urllib.parse.quote(url_str)

    html_content = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Pirate Hat Badge</title>
  <style>
    body {{ font-family: sans-serif; text-align: center; padding: 20px; background: #f0f0f0; }}
    .badge {{ width: 450px; margin: 0 auto; background: #fff; border: 4px solid #8b5a2b; border-radius: 12px; padding: 20px; box-shadow: 0 4px 12px rgba(0,0,0,0.2); }}
    h1 {{ color: #8b0000; margin-bottom: 4px; }}
    .row {{ display: flex; justify-content: space-around; margin: 20px 0; }}
    .qr-box {{ width: 190px; text-align: center; }}
    .qr-box img {{ width: 180px; height: 180px; border: 2px solid #333; border-radius: 8px; }}
    .label {{ font-weight: bold; margin-top: 8px; font-size: 0.95rem; }}
    .desc {{ font-size: 0.8rem; color: #555; margin-top: 4px; }}
    .note {{ background: #fff3cd; border: 1px solid #ffeeba; padding: 10px; border-radius: 6px; font-size: 0.85rem; }}
  </style>
</head>
<body>
  <div class="badge">
    <h1>PIRATE VESSEL RADIO</h1>
    <p>Scan to board the vessel and plunder one radio capture!</p>
    
    <div class="row">
      <div class="qr-box">
        <img src="{wifi_qr_url}" alt="Wi-Fi QR">
        <div class="label">1. JOIN WI-FI</div>
        <div class="desc">SSID: <strong>{ssid}</strong><br>(No password)</div>
      </div>
      <div class="qr-box">
        <img src="{web_qr_url}" alt="Web QR">
        <div class="label">2. OPEN IN BROWSER</div>
        <div class="desc">Sail to:<br><strong>http://{ip}</strong></div>
      </div>
    </div>

    <div class="note">
      <strong>Local Terminal Instructions (Offline generation on Pi):</strong><br>
      <code>sudo apt install -y qrencode</code><br>
      <code>qrencode -s 6 -o wifi_qr.png "{wifi_str}"</code><br>
      <code>qrencode -s 6 -o web_qr.png "{url_str}"</code>
    </div>
  </div>
</body>
</html>
"""
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(html_content)
    print(f"[PIRATE] Generated badge template: {output_html}")

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "pirate_badge.html")
    generate_html_card(output_html=out)
