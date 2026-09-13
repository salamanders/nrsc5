#!/usr/bin/env python3
"""
Generates the single-QR printable badge for the Pirate Hat:
Wi-Fi Credentials: WIFI:S:PirateHat;T:WPA;P:treasure;;
Directs visitors to open: http://192.168.4.1
"""

import sys
import os
import urllib.parse

def generate_html_card(ssid="PirateHat", password="treasure", ip="192.168.4.1", domain="pirate.box", output_html="pirate_badge.html"):
    wifi_str = f"WIFI:S:{ssid};T:WPA;P:{password};;"
    url_str = f"http://{ip}"

    # Google Charts API URL for printable QR code when connected,
    # plus local qrencode instructions for fully offline printing on Pi
    wifi_qr_url = "https://chart.googleapis.com/chart?chs=320x320&cht=qr&chl=" + urllib.parse.quote(wifi_str)

    html_content = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Pirate Vessel Badge</title>
  <style>
    body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; text-align: center; padding: 20px; background: #f0f0f0; }}
    .badge {{ width: 380px; margin: 0 auto; background: #fdfaf3; border: 3px solid #4a3525; border-radius: 10px; padding: 24px; box-shadow: 0 4px 12px rgba(0,0,0,0.15); }}
    h1 {{ color: #8b1e1e; margin-bottom: 6px; font-size: 1.5rem; text-transform: uppercase; letter-spacing: 0.5px; }}
    p.lead {{ color: #4a3525; font-size: 0.95rem; margin-top: 0; margin-bottom: 16px; line-height: 1.4; }}
    .qr-container {{ margin: 16px auto; width: 240px; text-align: center; }}
    .qr-container img {{ width: 230px; height: 230px; border: 2px solid #382417; border-radius: 8px; background: #fff; padding: 6px; }}
    .credentials {{ background: rgba(74, 53, 37, 0.08); border: 1px solid #c4b5a2; border-radius: 6px; padding: 12px; margin: 16px 0; text-align: left; font-size: 0.9rem; line-height: 1.6; color: #2b1d14; }}
    .credentials strong {{ color: #382417; }}
    .instructions {{ text-align: left; margin: 14px 0; font-size: 0.88rem; color: #4a3525; line-height: 1.5; }}
    .instructions ol {{ padding-left: 20px; margin: 6px 0; }}
    .instructions li {{ margin-bottom: 4px; }}
    .note {{ background: #fff3cd; border: 1px solid #ffeeba; padding: 10px; border-radius: 6px; font-size: 0.78rem; text-align: left; color: #533f03; margin-top: 14px; line-height: 1.4; }}
  </style>
</head>
<body>
  <div class="badge">
    <h1>PIRATE VESSEL RADIO</h1>
    <p class="lead">Scan with your camera to board the vessel and plunder one radio capture.</p>
    
    <div class="qr-container">
      <img src="{wifi_qr_url}" alt="Join Wi-Fi QR Code">
    </div>

    <div class="credentials">
      <div><strong>Wi-Fi Network:</strong> {ssid}</div>
      <div><strong>Password:</strong> {password}</div>
      <div><strong>Browser Address:</strong> {url_str} <span style="color:#666;">({domain})</span></div>
    </div>

    <div class="instructions">
      <strong>How to Board:</strong>
      <ol>
        <li>Scan the QR code with your phone camera to join the Wi-Fi.</li>
        <li>When the prompt opens, copy the link and paste into Safari (or Chrome).</li>
        <li>Browse the vault, choose your track, and plunder!</li>
      </ol>
    </div>

    <div class="note">
      <strong>Offline QR Generation (on Pi):</strong><br>
      <code>sudo apt install -y qrencode</code><br>
      <code>qrencode -s 8 -o wifi_qr.png "{wifi_str}"</code>
    </div>
  </div>
</body>
</html>
"""
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(html_content)
    print(f"[PIRATE] Generated single-QR badge template: {output_html}")

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "pirate_badge.html")
    generate_html_card(output_html=out)
