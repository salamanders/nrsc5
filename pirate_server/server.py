import http.server
import urllib.parse
import os
import sys
import shutil
import mimetypes
import time
import threading
import subprocess

PORT = int(os.environ.get("PIRATE_PORT", 80))
HOST = os.environ.get("PIRATE_HOST", "192.168.4.1")
RECORDINGS_DIR = os.environ.get("RECORDINGS_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "recordings")))

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TEMPLATES_DIR = os.path.join(BASE_DIR, "templates")
STATIC_DIR = os.path.join(BASE_DIR, "static")

# In-memory dual-defense cooldown and image asset caching
PLUNDER_COOLDOWN = {}  # ip -> timestamp
STATIC_IMAGE_CACHE = {}  # rel_path -> bytes

def get_mac_for_ip(ip):
    """Looks up MAC address for given client IP from kernel ARP table."""
    try:
        with open("/proc/net/arp", "r") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 4 and parts[0] == ip:
                    mac = parts[3]
                    if mac != "00:00:00:00:00:00":
                        return mac
    except Exception:
        pass
    return None

def deauth_mac(mac):
    """Deauthenticates a Wi-Fi station to free up hardware association slots."""
    if not mac or mac == "00:00:00:00:00:00":
        return
    try:
        subprocess.run(["sudo", "iw", "dev", "wlan0", "station", "del", mac],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
    except Exception:
        pass

def schedule_farewell_deauth(ip):
    """Gives user 15s to view farewell instructions, then softly disconnects Wi-Fi."""
    def _delayed():
        time.sleep(15)
        mac = get_mac_for_ip(ip)
        if mac:
            deauth_mac(mac)
    threading.Thread(target=_delayed, daemon=True).start()

def run_station_reaper():
    """Background daemon: periodically evicts devices idle for >= 45 seconds."""
    while True:
        time.sleep(20)
        try:
            res = subprocess.run(["sudo", "iw", "dev", "wlan0", "station", "dump"],
                                 capture_output=True, text=True, timeout=5)
            if res.returncode == 0 and res.stdout:
                current_mac = None
                for line in res.stdout.splitlines():
                    line = line.strip()
                    if line.startswith("Station"):
                        current_mac = line.split()[1]
                    elif "inactive time:" in line and current_mac:
                        parts = line.split()
                        if "inactive" in parts:
                            idx = parts.index("inactive")
                            inactive_ms = int(parts[idx + 2])
                            if inactive_ms >= 45000:
                                deauth_mac(current_mac)
                                current_mac = None
        except Exception:
            pass

def scan_songs():
    """Scans the recordings directory and returns a sorted list of valid songs (> 1 KB)."""
    songs = []
    if not os.path.isdir(RECORDINGS_DIR):
        return songs

    for root, dirs, files in os.walk(RECORDINGS_DIR):
        for f in files:
            if f.lower().endswith(".m4a") and not f.startswith("."):
                full_path = os.path.join(root, f)
                try:
                    if os.path.getsize(full_path) <= 1024:
                        continue  # Filter empty or broken stubs (Bug 2.2 / 3.3)
                except OSError:
                    continue

                rel_path = os.path.relpath(full_path, RECORDINGS_DIR)
                parts = rel_path.split(os.sep)
                artist = parts[0] if len(parts) >= 2 else "Unknown"
                title = os.path.splitext(parts[-1])[0]

                songs.append({
                    "full_path": full_path,
                    "rel_path": rel_path.replace(os.sep, "/"),
                    "artist": artist,
                    "title": title,
                    "display_artist": artist.replace("_", " "),
                    "display_title": title.replace("_", " ")
                })

    songs.sort(key=lambda s: (s["display_artist"].lower(), s["display_title"].lower()))
    return songs

class PirateHandler(http.server.BaseHTTPRequestHandler):
    server_version = "PirateShip/2.0"

    def do_HEAD(self):
        self.do_GET()

    def has_plundered(self):
        # 1. Check browser cookie
        if "plundered=1" in self.headers.get("Cookie", ""):
            return True
        # 2. Check in-memory IP cooldown (10 minutes = 600s)
        client_ip = self.client_address[0]
        last_time = PLUNDER_COOLDOWN.get(client_ip, 0)
        if (time.time() - last_time) < 600:
            return True
        return False

    def is_cna_client(self):
        """Detects if client is an iOS Captive Network Assistant (sandboxed popup)."""
        ua = self.headers.get("User-Agent", "")
        return "CaptiveNetworkSupport" in ua

    def send_html(self, content, status=200):
        data = content.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        # Strictly anti-cache HTML pages so songs plundered immediately disappear on refresh
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def serve_static(self, rel_path):
        clean_path = os.path.normpath(rel_path).lstrip("/")
        full_path = os.path.join(STATIC_DIR, clean_path)

        # Protection: ensure requested file is strictly inside STATIC_DIR
        if not full_path.startswith(STATIC_DIR) or not os.path.isfile(full_path):
            self.send_error(404, "Treasure not found!")
            return

        mime_type, _ = mimetypes.guess_type(full_path)
        content_type = mime_type or "application/octet-stream"

        # Developer preference: Cache images only; never cache CSS/JS/HTML so tweaks are immediate
        is_image = clean_path.lower().endswith((".jpg", ".jpeg", ".png", ".svg"))

        if is_image and clean_path in STATIC_IMAGE_CACHE:
            content = STATIC_IMAGE_CACHE[clean_path]
        else:
            try:
                with open(full_path, "rb") as f:
                    content = f.read()
                if is_image and len(content) < 500000:
                    STATIC_IMAGE_CACHE[clean_path] = content
            except Exception as e:
                self.send_error(500, f"Error reading file: {e}")
                return

        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        if is_image:
            self.send_header("Cache-Control", "public, max-age=604800")
        else:
            self.send_header("Cache-Control", "no-cache, must-revalidate")
        self.end_headers()
        self.wfile.write(content)

    def render_template(self, name, context=None):
        context = context or {}
        tmpl_path = os.path.join(TEMPLATES_DIR, name)
        with open(tmpl_path, "r", encoding="utf-8") as f:
            content = f.read()

        # Handle simple loops like {% for song in songs %}...{% endfor %}
        if "{% for song in songs %}" in content:
            parts = content.split("{% for song in songs %}")
            before_loop = parts[0]
            loop_and_after = parts[1].split("{% endfor %}")
            loop_template = loop_and_after[0]
            after_loop = loop_and_after[1]

            rendered_loop = []
            for song in context.get("songs", []):
                item_html = loop_template
                item_html = item_html.replace("{{ song.rel_path }}", song["rel_path"])
                item_html = item_html.replace("{{ song.display_title }}", song["display_title"])
                item_html = item_html.replace("{{ song.display_artist }}", song["display_artist"])
                rendered_loop.append(item_html)

            content = before_loop + "".join(rendered_loop) + after_loop

        # Handle simple variables
        content = content.replace("{{ songs|length }}", str(len(context.get("songs", []))))
        content = content.replace("{{ host }}", HOST)
        return content

    def handle_captive_probes(self, path):
        """
        Suppresses OS captive portal popups by returning expected success responses.
        This allows devices (iOS & Android) to stay connected quietly without
        spawning restricted captive sheets. The user then navigates directly
        in Safari or Chrome to http://192.168.4.1/ via Step 2 QR code.
        """
        host = self.headers.get("Host", "").lower().split(":")[0]

        # 1. iOS / Apple captive checks: spoof Success so iOS does not launch CNA sheet
        apple_domains = {
            "captive.apple.com", "airport.us", "www.airport.us",
            "ibook.info", "www.ibook.info", "itools.info", "www.itools.info",
            "thinkdifferent.us", "www.thinkdifferent.us",
            "appleiphonecell.com", "www.appleiphonecell.com"
        }
        apple_paths = (
            "/hotspot-detect.html", "/canonical.html",
            "/library/test/success.html", "/success.html"
        )
        if path in apple_paths or host in apple_domains:
            data = b"<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            return True

        # 2. Android probes: return HTTP 204 No Content so Android does not launch CaptivePortalLogin
        android_domains = {
            "connectivitycheck.gstatic.com", "connectivitycheck.android.com",
            "clients3.google.com", "play.googleapis.com"
        }
        if path in ("/generate_204", "/gen_204") or path.endswith("/generate_204") or host in android_domains:
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return True

        # 3. Windows / Desktop probes
        if path in ("/connecttest.txt", "/ncsi.txt") or host in ("www.msftconnecttest.com", "www.msftncsi.com"):
            data = b"Microsoft Connect Test"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            return True

        # 4. Firefox / Linux probes
        if path in ("/success.txt",) or host in ("detectportal.firefox.com", "connectivity-check.ubuntu.com"):
            data = b"success\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            return True

        return False

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. Handle OS Captive Portal Probes (suppresses captive sheets)
        if self.handle_captive_probes(path):
            return

        # 2. Static assets
        if path.startswith("/static/"):
            self.serve_static(path[8:])
            return

        # 3. Farewell page
        if path == "/farewell":
            # Softly deauthenticate this station 15 seconds after reaching farewell
            schedule_farewell_deauth(self.client_address[0])
            html_page = self.render_template("farewell.html")
            self.send_html(html_page)
            return

        # 4. If already claimed booty, force to farewell screen
        if self.has_plundered():
            self.send_response(302)
            self.send_header("Location", "/farewell")
            self.end_headers()
            return

        # 5. Captive assistant detection: if an Apple CNA popup hits root, show portal handoff
        if self.is_cna_client() and path not in ["/download"]:
            html_page = self.render_template("portal.html")
            self.send_html(html_page)
            return

        # 6. Real browser in Safari or Chrome: go straight to the Treasure Chest!
        if path in ["/", "/index.html", "/chest"]:
            songs = scan_songs()
            html_page = self.render_template("chest.html", {"songs": songs})
            self.send_html(html_page)
            return

        # 7. Explicit portal test route
        if path == "/portal":
            html_page = self.render_template("portal.html")
            self.send_html(html_page)
            return

        # 8. Printable badge route
        if path == "/badge":
            badge_path = os.path.join(BASE_DIR, "pirate_badge.html")
            if os.path.isfile(badge_path):
                with open(badge_path, "r", encoding="utf-8") as f:
                    content = f.read()
                self.send_html(content)
                return
            self.send_error(404, "Badge template not found")
            return

        # 9. Download / Plunder endpoint
        if path == "/download":
            rel_path = query.get("file", [""])[0]
            if not rel_path:
                self.send_error(400, "No booty specified!")
                return

            full_path = os.path.normpath(os.path.join(RECORDINGS_DIR, rel_path))

            # Hacker protection: keep strictly within RECORDINGS_DIR
            if not full_path.startswith(RECORDINGS_DIR) or not os.path.isfile(full_path):
                self.send_error(404, "That treasure has already been plundered by another pirate!")
                return

            try:
                file_size = os.path.getsize(full_path)
                if file_size <= 1024:
                    self.send_error(404, "That treasure was damaged or lost in the depths!")
                    return
            except OSError:
                self.send_error(404, "That treasure has already been plundered by another pirate!")
                return

            # Protection against Apple CNA file deletion bug:
            # If request is from an Apple captive popup, do NOT stream & delete!
            if self.is_cna_client():
                self.send_error(403, "Apple blocks saving songs in this preview window. Copy http://192.168.4.1 into Safari to plunder!")
                return

            filename = os.path.basename(full_path)
            client_ip = self.client_address[0]
            PLUNDER_COOLDOWN[client_ip] = time.time()

            self.send_response(200)
            self.send_header("Content-Type", "audio/mp4")
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
            self.send_header("Content-Length", str(file_size))
            self.send_header("Set-Cookie", "plundered=1; Path=/; Max-Age=600")
            self.end_headers()

            # Stream the file to client, then delete from disk
            try:
                with open(full_path, "rb") as f:
                    shutil.copyfileobj(f, self.wfile)
                os.unlink(full_path)
                print(f"[PIRATE] Plundered & deleted: {filename}")

                # Clean up empty parent artist directory if all songs for this artist were plundered (Bug 3.4)
                parent_dir = os.path.dirname(full_path)
                if parent_dir != RECORDINGS_DIR and parent_dir.startswith(RECORDINGS_DIR):
                    try:
                        os.rmdir(parent_dir)
                        print(f"[PIRATE] Cleaned empty vault drawer: {os.path.basename(parent_dir)}")
                    except OSError:
                        pass
            except Exception as e:
                print(f"[PIRATE] Download error for {filename}: {e}")
            return

        # Catch-all: redirect any unknown paths to root landing page
        self.send_response(302)
        self.send_header("Location", "/")
        self.end_headers()

def run_server():
    print("=" * 60)
    print("PIRATE VESSEL SINGLE-SERVING WEB SERVER")
    print(f"Serving host: {HOST}")
    print(f"Serving port: {PORT}")
    print(f"Booty vault:  {RECORDINGS_DIR}")
    print("=" * 60)

    # Start automated idle Wi-Fi station reaper daemon thread (Bug 1.3)
    reaper = threading.Thread(target=run_station_reaper, daemon=True)
    reaper.start()

    with http.server.ThreadingHTTPServer(("", PORT), PirateHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[PIRATE] Lowering anchor and shutting down server.")

if __name__ == "__main__":
    run_server()
