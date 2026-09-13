#!/usr/bin/env python3
"""
Generates the simplified 2-step printable badge for the Pirate Hat:
Step 1 QR: Wi-Fi Credentials (WIFI:S:PirateHat;T:WPA;P:treasure;;)
           Network (SSID) and Password clearly printed right beneath it.
Step 2 QR: Target Browser URL (http://192.168.4.1/)
           Direct URL/IP clearly printed right beneath it.

Generates 100% offline, self-contained base64 PNG images embedded in the HTML.
"""

import os
import sys
import io
import base64
import qrcode

def generate_base64_qr(data_text):
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=2,
    )
    qr.add_data(data_text)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    buffer = io.BytesIO()
    img.save(buffer, format="PNG")
    b64 = base64.b64encode(buffer.getvalue()).decode("ascii")
    return f"data:image/png;base64,{b64}"

def generate_html_card(ssid="PirateHat", password="treasure", ip="192.168.4.1", output_html="pirate_badge.html"):
    wifi_str = f"WIFI:S:{ssid};T:WPA;P:{password};;"
    url_str = f"http://{ip}/"

    # Load pirate hat vector art for self-contained branding
    hat_svg_path = os.path.join(os.path.dirname(__file__), "static", "pirate_hat.svg")
    hat_svg = ""
    if os.path.isfile(hat_svg_path):
        with open(hat_svg_path, "r", encoding="utf-8") as f:
            hat_svg = f.read().strip()

    # Generate offline base64 data URIs
    wifi_qr_data = generate_base64_qr(wifi_str)
    url_qr_data = generate_base64_qr(url_str)

    html_content = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Pirate Vessel Badge</title>
  <style>
    * {{
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }}
    body {{
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Georgia, serif;
      text-align: center;
      padding: 24px 12px;
      background: #e8e2d5;
      color: #2b1d14;
      -webkit-print-color-adjust: exact;
      print-color-adjust: exact;
    }}
    .badge {{
      max-width: 620px;
      margin: 0 auto;
      background: #fdfaf3;
      border: 3px solid #382417;
      border-radius: 12px;
      padding: 24px 20px;
      box-shadow: 0 6px 20px rgba(0,0,0,0.18);
      -webkit-print-color-adjust: exact;
      print-color-adjust: exact;
    }}
    .hat-wrapper {{
      width: 90px;
      height: 68px;
      margin: 0 auto 6px auto;
    }}
    .hat-wrapper svg {{
      width: 100%;
      height: 100%;
      display: block;
    }}
    h1 {{
      color: #8b1e1e;
      margin: 0 0 4px 0;
      font-size: 1.8rem;
      text-transform: uppercase;
      letter-spacing: 1.5px;
    }}
    p.lead {{
      color: #5c4331;
      font-size: 1rem;
      margin: 0 0 20px 0;
      font-style: italic;
    }}
    .qr-grid {{
      display: flex;
      justify-content: center;
      gap: 24px;
      flex-wrap: wrap;
    }}
    .qr-card {{
      flex: 1;
      min-width: 240px;
      max-width: 270px;
      background: #f5eedf;
      border: 2px solid #8c6f56;
      border-radius: 10px;
      padding: 16px 14px;
      box-sizing: border-box;
      display: flex;
      flex-direction: column;
      align-items: center;
    }}
    .step-number {{
      display: inline-block;
      background: #8b1e1e;
      color: #fff;
      font-size: 0.8rem;
      font-weight: 800;
      padding: 3px 12px;
      border-radius: 12px;
      text-transform: uppercase;
      letter-spacing: 0.8px;
      margin-bottom: 6px;
    }}
    .qr-card h2 {{
      font-size: 1.15rem;
      color: #2b1d14;
      margin: 0 0 10px 0;
      font-weight: 700;
    }}
    .qr-container {{
      background: #ffffff;
      border: 1px solid #c4b5a2;
      border-radius: 6px;
      padding: 6px;
      display: inline-block;
      margin-bottom: 12px;
      box-shadow: 0 2px 6px rgba(0,0,0,0.06);
    }}
    .qr-container img {{
      display: block;
      width: 190px;
      height: 190px;
    }}
    .qr-details {{
      width: 100%;
      background: rgba(74, 53, 37, 0.08);
      border: 1px solid #c4b5a2;
      border-radius: 6px;
      padding: 10px 12px;
      box-sizing: border-box;
      font-size: 0.92rem;
      line-height: 1.6;
      text-align: center;
    }}
    .qr-details .detail-row {{
      margin: 2px 0;
    }}
    .qr-details .lbl {{
      color: #5c4331;
      font-weight: 500;
    }}
    .qr-details .val {{
      color: #2b1d14;
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 1rem;
      font-weight: 700;
    }}
    .qr-details .url-val {{
      font-size: 0.95rem;
      word-break: break-all;
    }}
    .qr-details .subtext {{
      color: #7a6352;
      font-size: 0.8rem;
      margin-top: 3px;
    }}
    @media print {{
      body {{ background: #fff; padding: 0; }}
      .badge {{ box-shadow: none; border-color: #000; max-width: 100%; page-break-inside: avoid; break-inside: avoid; }}
      .qr-grid {{ flex-wrap: nowrap; gap: 16px; }}
      .qr-card {{ max-width: 48%; page-break-inside: avoid; break-inside: avoid; }}
    }}
  </style>
</head>
<body>
  <div class="badge">
    <div class="hat-wrapper">
      {hat_svg}
    </div>
    <h1>PIRATE VESSEL RADIO</h1>
    <p class="lead">Air-recorded broadcast tracks &bull; Plunder one song per pirate</p>

    <div class="qr-grid">
      <div class="qr-card">
        <span class="step-number">Step 1</span>
        <h2>Connect Wi-Fi</h2>
        <div class="qr-container">
          <img src="{wifi_qr_data}" alt="Wi-Fi QR Code">
        </div>
        <div class="qr-details">
          <div class="detail-row"><span class="lbl">Network:</span> <strong class="val">{ssid}</strong></div>
          <div class="detail-row"><span class="lbl">Password:</span> <strong class="val">{password}</strong></div>
        </div>
      </div>

      <div class="qr-card">
        <span class="step-number">Step 2</span>
        <h2>Open Chest</h2>
        <div class="qr-container">
          <img src="{url_qr_data}" alt="URL QR Code">
        </div>
        <div class="qr-details">
          <div class="detail-row"><span class="lbl">URL:</span> <strong class="val url-val">{url_str}</strong></div>
          <div class="subtext">Safari &bull; Chrome &bull; Browser</div>
        </div>
      </div>
    </div>
  </div>
</body>
</html>
"""
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(html_content)
    print(f"[PIRATE] Generated offline dual-QR badge template: {output_html}")

if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "pirate_badge.html")
    generate_html_card(output_html=out)
