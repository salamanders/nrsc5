import http.server
import urllib.parse
import os
import sys
import shutil
import mimetypes

PORT = int(os.environ.get("PIRATE_PORT", 80))
HOST = os.environ.get("PIRATE_HOST", "192.168.4.1")
RECORDINGS_DIR = os.environ.get("RECORDINGS_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "recordings")))

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
TEMPLATES_DIR = os.path.join(BASE_DIR, "templates")
STATIC_DIR = os.path.join(BASE_DIR, "static")

def scan_songs():
    """Scans the recordings directory and returns a sorted list of songs."""
    songs = []
    if not os.path.isdir(RECORDINGS_DIR):
        return songs

    for root, dirs, files in os.walk(RECORDINGS_DIR):
        for f in files:
            if f.lower().endswith(".m4a") and not f.startswith("."):
                full_path = os.path.join(root, f)
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

    def has_plundered(self):
        return "plundered=1" in self.headers.get("Cookie", "")

    def is_cna_client(self):
        """Detects if client is an iOS Captive Network Assistant (sandboxed popup)."""
        ua = self.headers.get("User-Agent", "")
        return "CaptiveNetworkSupport" in ua

    def send_html(self, content, status=200):
        data = content.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
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
        try:
            with open(full_path, "rb") as f:
                content = f.read()
            self.send_response(200)
            self.send_header("Content-Type", mime_type or "application/octet-stream")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        except Exception as e:
            self.send_error(500, f"Error reading file: {e}")

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
        Detects operating system captive portal probe requests and redirects them
        cleanly to the landing page so the phone opens the captive prompt.
        """
        # Apple CNA probes
        is_apple_probe = path in ("/hotspot-detect.html", "/canonical.html", "/library/test/success.html", "/success.html")
        # Android / Google probes
        is_google_probe = path == "/generate_204" or path == "/gen_204" or path.endswith("/generate_204")
        # Microsoft Windows probes
        is_ms_probe = path in ("/connecttest.txt", "/ncsi.txt")
        # Firefox probe
        is_firefox_probe = path == "/success.txt"
        # Amazon Kindle probe
        is_kindle_probe = path == "/kindle-wifi/wifiredirect.html"

        if is_apple_probe or is_google_probe or is_ms_probe or is_firefox_probe or is_kindle_probe:
            html_page = self.render_template("portal.html")
            self.send_html(html_page)
            return True

        return False

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. Handle OS Captive Portal Probes (serves single-purpose exit handoff)
        if self.handle_captive_probes(path):
            return

        # 2. Static assets
        if path.startswith("/static/"):
            self.serve_static(path[8:])
            return

        # 3. Farewell page
        if path == "/farewell":
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

        # Explicit portal test route
        if path == "/portal":
            html_page = self.render_template("portal.html")
            self.send_html(html_page)
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

            # Protection against Apple CNA file deletion bug:
            # If request is from an Apple captive popup, do NOT stream & delete!
            if self.is_cna_client():
                self.send_error(403, "Apple blocks saving songs in this preview window. Copy http://192.168.4.1 into Safari to plunder!")
                return

            filename = os.path.basename(full_path)
            file_size = os.path.getsize(full_path)

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

    with http.server.ThreadingHTTPServer(("", PORT), PirateHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[PIRATE] Lowering anchor and shutting down server.")

if __name__ == "__main__":
    run_server()
