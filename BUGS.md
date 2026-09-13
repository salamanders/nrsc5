# BUGS & Maker Faire Readiness Audit (`BUGS.md`)

This document details all potential bugs, edge cases, failure modes, and architectural risks identified across the **Maker Faire Audio Hat** fork (`DIRECT_RECODE.md`, `src/recorder.c`, `pirate_server/server.py`, network configuration, and system services).

---

## 1. Showstoppers & Run-Killers (High Probability of Failure at Event)

### 1.1 `nrsc5` Crashes Silently with No Systemd Auto-Restart
- **Observation**: `nrsc5` was running via manual background invocation (`nohup`), not as a systemd service.
- **Evidence in `nohup.out`**:
  ```text
  cb transfer status: 1, canceling...
  rtlsdr_read_reg failed with -1
  rtlsdr_write_reg failed with -1
  Reattaching kernel driver failed!
  09:53:39 [RECORDER] Shutdown complete: 161 songs saved
  ```
- **Impact**: RTL-SDR dongles frequently experience USB transient drops, thermal resets, or power dips when running on battery packs. When `nrsc5` dies, **recording permanently stops for the entire weekend** unless manually restarted via SSH.
- **Proposed Solution**:
  1. **Create systemd unit file** at `/etc/systemd/system/nrsc5-recorder.service`:
     ```ini
     [Unit]
     Description=NRSC5 HD Radio Audio Stream Recorder
     After=network.target
     Wants=network.target

     [Service]
     Type=simple
     User=benjamin
     WorkingDirectory=/home/benjamin/nrsc5
     Environment=NRSC5_FREQ=89.5
     Environment=NRSC5_PROGRAM=1
     Environment=NRSC5_PREROLL=2.5
     # Clean quiet execution, streams directly into systemd journal
     ExecStart=/bin/sh -c 'exec /home/benjamin/nrsc5/build/src/nrsc5 -q --record-songs /home/benjamin/nrsc5/recordings --preroll "$NRSC5_PREROLL" "$NRSC5_FREQ" "$NRSC5_PROGRAM"'
     Restart=always
     # 30-second backoff gives the RTL-SDR dongle time to cool down and avoids log spam if unplugged
     RestartSec=30
     # Standardize logging directly into journald (eliminating unbounded nohup.out)
     StandardOutput=journal
     StandardError=journal

     [Install]
     WantedBy=multi-user.target
     ```
  2. **Install, enable, and launch**:
     ```bash
     sudo systemctl daemon-reload
     sudo systemctl enable nrsc5-recorder.service
     sudo systemctl start nrsc5-recorder.service
     ```
  3. **Management & Monitoring**:
     - Check status: `sudo systemctl status nrsc5-recorder`
     - Follow live audio recording logs: `sudo journalctl -u nrsc5-recorder -f`
     - On-site frequency change: edit `/etc/systemd/system/nrsc5-recorder.service` (or drop-in override) and run `sudo systemctl restart nrsc5-recorder`.
  4. **Failure Recovery & Thermal Cooldown**:
     - Program `1` specifically captures the HD2 multicast channel.
     - If the RTL-SDR disconnects, browns out (`rtlsdr_read_reg failed with -1`), or is temporarily unplugged, the 30-second restart delay prevents continuous thrashing/log spam and gives the dongle time to dissipate thermal buildup before retrying.

### 1.2 DHCP Pool Exhaustion (Network Dies After ~240 Visitors)
- **Problem**: NetworkManager `shared` mode assigns DHCP pool `192.168.4.10` to `192.168.4.254` (~244 available IP addresses).
- **Default Behavior**: Default dnsmasq lease duration in NM is **1 hour to 24 hours**.
- **Impact**: At Maker Faire, hundreds of curious attendees walk past and join the Wi-Fi. Within 2–3 hours, all 244 DHCP leases are claimed by phones sitting in pockets. Subsequent attendees will get stuck on **"Obtaining IP address..."** forever, rendering the pirate hat completely inaccessible.
- **Proposed Solution**:
  1. **Update `/etc/NetworkManager/dnsmasq-shared.d/pirate.conf`**:
     Add an explicit short lease time and widen the usable IP pool:
     ```ini
     # Hijack DNS to the pirate hat
     address=/#/192.168.4.1
     address=/pirate.box/192.168.4.1
     dhcp-option=option:domain-name,pirate.box

     # Short 2-minute lease time aggressively recycles IP addresses for high-turnover crowds
     dhcp-range=192.168.4.2,192.168.4.254,255.255.255.0,2m

     # Limit maximum concurrent leases tracked in dnsmasq memory
     dhcp-lease-max=250
     ```
  2. **Apply without dropping current connections**:
     ```bash
     sudo nmcli connection up PirateHotspot
     ```
  3. **Verification**:
     - Inspect active leases: `./pirate_server/hotspot.sh status`
     - Verify lease file is recycling: `cat /var/lib/NetworkManager/dnsmasq-wlan0.leases`
  4. **Why this works**: With a 2-minute lease (`2m`), devices that walk away or disconnect have their leases expire almost immediately. When new attendees arrive, dnsmasq hands out expired slots instantly instead of reporting "no address available".

### 1.3 Raspberry Pi Built-in Wi-Fi Hardware Station Limit (8–10 Max Clients)
- **Problem**: The onboard Cypress/Broadcom Wi-Fi chipset on Raspberry Pi (Pi 3B, 3B+, 4B, Zero 2W) enforces a hard firmware limit on maximum concurrent associated STAs—typically **8 or 10 clients**.
- **Impact**: Visitors who plunder a track often forget to "Forget This Network" despite the farewell instructions. As long as 8–10 phones stay associated in their pockets, no new visitors can associate to the SSID.
- **Proposed Solution**:
  1. **Idle Station Reaper (Automated Deauth for Inactive Phones)**:
     - Linux provides `iw dev wlan0 station dump` which reports the `inactive time` for every associated MAC address.
     - Add a background thread in `pirate_server/server.py` (or a helper loop in `hotspot.sh`) that checks station inactivity every 20 seconds.
     - If any station has `inactive time >= 45000 ms` (no data transmitted for 45 seconds), issue:
       ```bash
       sudo iw dev wlan0 station del <MAC>
       ```
     - This immediately evicts dead/pocketed phones from the Wi-Fi chip's internal 8–10 client association table.
  2. **Soft Deauth After Farewell**:
     - When a visitor visits `/farewell` (confirming they have plundered their song), look up their MAC in `/proc/net/arp` from `self.client_address[0]`.
     - Schedule a delayed disassociation (e.g. 15 seconds after `/farewell` renders):
       ```bash
       sudo iw dev wlan0 station del <MAC>
       ```
     - This seamlessly boots the user off the captive network so their phone automatically drops back to cellular data, without requiring them to manually navigate to iOS/Android Settings &rarr; Forget Network.

### 1.4 Uncapped `nohup.out` and Log Flooding Exhausting SD Card
- **Observation**: `nohup.out` had already grown to **8.1 MB** during bench testing.
- **Impact**: With verbose HD Radio framing and BER/MER metrics emitted multiple times per second, over a 48-hour continuous run, unmanaged log files can grow to several gigabytes, potentially exhausting SD card disk space or wearing out flash memory.
- **Proposed Solution**:
  1. **Run `nrsc5` with `-q` (Quiet Mode)**:
     - The per-second telemetry (`MER`, `BER`, bit rate) generated by `src/main.c` is silenced by `-q`.
     - High-value song transition logs (`[RECORDER] Now recording...`, `[RECORDER] Saved...`) use `log_info()` and remain fully visible.
  2. **Eliminate `nohup` Invocations**:
     - Never run `nohup ... &` manually on battery power.
     - The `nrsc5-recorder.service` (Bug 1.1) routes output cleanly to `systemd-journald`.
  3. **Cap Journal Size & Protect SD Card via Volatile RAM Logging**:
     - Configure `/etc/systemd/journald.conf.d/00-pirate.conf`:
       ```ini
       [Journal]
       Storage=volatile
       RuntimeMaxUse=30M
       SystemMaxUse=30M
       ```
     - `Storage=volatile` stores logs exclusively in RAM (`/run/log/journal`), completely preventing continuous SD card write wear and eliminating risk of filesystem journal corruption during battery swaps.
  4. **Clean up Existing Artifact**:
     ```bash
     rm -f /home/benjamin/nrsc5/nohup.out
     ```

---

## 2. Audio Pipeline & Recording Bugs (`src/recorder.c`)

### 2.1 Synchronous `ffmpeg` Remuxing Blocks the `nrsc5` Main Event Thread
- **Location**: `src/recorder.c:248-258`
- **Code**:
  ```c
  pid_t pid = fork();
  if (pid == 0) {
      execvp("ffmpeg", argv);
      _exit(127);
  } else if (pid > 0) {
      int status = 0;
      waitpid(pid, &status, 0); // BLOCKS!
  ```
- **Impact**: `finalize_current_song()` is invoked synchronously inside `recorder_on_id3()`, which is called directly from `nrsc5`'s main event processing thread. `ffmpeg` taking 200–800ms to remux and write an M4A file will **block the entire event loop**. This can cause RTL-SDR USB sample buffers to overflow (`cb transfer status` drops, frame sync loss).
- **Proposed Solution**:
  1. **Decouple Remuxing via a Detached Background Thread (`pthread`)**:
     - Do not block `nrsc5`'s event thread with `waitpid()`.
     - When a song finishes, rotate `rec->tmp_path` immediately to a uniquely timestamped staging file:
       `rec->base_dir/.staging_<timestamp>.aac`
     - Allocate a small job struct containing:
       - Input staging `.aac` path
       - Output `.m4a` path
       - Temporary staging `.tmp_art_*` path (if cover art present)
       - Metadata strings (title, artist, album)
     - Launch a worker thread:
       ```c
       pthread_t tid;
       pthread_attr_t attr;
       pthread_attr_init(&attr);
       pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
       pthread_create(&tid, &attr, remux_worker_thread, job);
       pthread_attr_destroy(&attr);
       ```
  2. **Worker Thread Execution**:
     - The worker runs `fork()` + `execvp("ffmpeg", argv)` + `waitpid()` in the background.
     - On completion:
       - If `ffmpeg` succeeded: logs `[RECORDER] Saved: "..." -> final_audio`.
       - If `ffmpeg` failed: calls `unlink(final_audio)` to prevent 0-byte files (fixing Bug 2.2), and logs an error.
       - Always unlinks the temporary staging `.aac` and temporary art files.
       - Frees the job struct.
  3. **Why this eliminates drops**: `nrsc5`'s event loop returns to processing HD Radio OFDM frames in < 1 millisecond. The RTL-SDR USB buffer never starves or overflows during song finalization.

### 2.2 Corrupted 0-Byte Audio Files Left on Disk on Remux Failure
- **Evidence**: Inspection of `recordings/` revealed:
  ```text
  ./Motley_Crue/Girls_Girls_Girls.m4a (0 bytes)
  ```
- **Root Cause**: `ffmpeg -y ... final_audio` creates an empty file handle on startup. If `ffmpeg` fails or exits non-zero:
  ```c
  if (!remux_ok) {
      log_error("[RECORDER] Failed to finalize \"%s\": ffmpeg remux error", rec->current_title);
  }
  ```
  `final_audio` is **never unlinked**. It remains on disk as a 0-byte orphan.
- **Impact**: Users plunder `Girls_Girls_Girls.m4a`, receive a 0-byte file, and playback fails with errors in VLC/iOS.
- **Proposed Solution**:
  1. **Recorder Level (`src/recorder.c`)**:
     - Check `remux_ok` AND verify output file size using `stat()`:
       ```c
       struct stat st;
       int is_valid = (remux_ok && stat(final_audio, &st) == 0 && st.st_size > 1024);
       if (!is_valid) {
           unlink(final_audio);
           log_error("[RECORDER] Finalize failed for \"%s\": invalid or empty file (unlinked)", rec->current_title);
       } else {
           // Success logging
       }
       ```
     - If `ffmpeg` crashes, exits non-zero, or outputs a broken 0-byte stub, `final_audio` is immediately removed.
  2. **Server Defensive Filter (`pirate_server/server.py`)**:
     - In `scan_songs()`, filter out any file smaller than 1 KB:
       ```python
       if f.lower().endswith(".m4a") and not f.startswith("."):
           full_path = os.path.join(root, f)
           if os.path.getsize(full_path) > 1024:
               ...
       ```
     - This guarantees visitors never see an empty or corrupt track even if an unlinked file somehow touches disk.
  3. **One-Time Existing Vault Cleanup**:
     ```bash
     find /home/benjamin/nrsc5/recordings -name '*.m4a' -size -1024c -delete
     ```
     Removes `./Motley_Crue/Girls_Girls_Girls.m4a` and any other empty artifacts immediately.

### 2.3 Hardcoded Temporary Staging Path Collision
- **Location**: `src/recorder.c:333`
- **Code**:
  ```c
  snprintf(rec->tmp_path, sizeof(rec->tmp_path), "%s/.tmp_recording.aac", rec->base_dir);
  ```
- **Impact**: If `nrsc5` restarts abruptly, `.tmp_recording.aac` is left in an inconsistent state. If multiple programs were ever recorded, they would overwrite the same file.
- **Proposed Solution**:
  1. **Dynamic Unique Staging Filename**:
     - In `start_new_song()`, generate unique filenames containing PID and monotonic timestamp:
       ```c
       snprintf(rec->tmp_path, sizeof(rec->tmp_path),
                "%s/.tmp_rec_%d_%lu.aac",
                rec->base_dir, (int)getpid(), (unsigned long)time(NULL));
       ```
     - This guarantees that background worker threads processing previous songs (from Bug 2.1) never collide with or read from a file being actively recorded by the next song.
  2. **Startup Orphan Sweeper (`recorder_create()`)**:
     - On initialization, scan `rec->base_dir` and delete any stale `.tmp_rec_*`, `.staging_*`, or `.tmp_art_*` files leftover from power cuts or hard resets:
       ```c
       /* Clean any interrupted staging files from prior boots */
       DIR *d = opendir(rec->base_dir);
       if (d) {
           struct dirent *de;
           while ((de = readdir(d)) != NULL) {
               if (strncmp(de->d_name, ".tmp_", 5) == 0 || strncmp(de->d_name, ".staging_", 9) == 0) {
                   char orphan[MAX_PATH_LEN * 2];
                   snprintf(orphan, sizeof(orphan), "%s/%s", rec->base_dir, de->d_name);
                   unlink(orphan);
               }
           }
           closedir(d);
       }
       ```
  3. **Guaranteed Collision-Free Pipeline**: Eliminates race conditions between simultaneous recording and background packaging passes.

### 2.4 Track Tail Bleed (Station Sweepers & Song B Intros) [DEFERRED — NOT DOING FOR MAKER FAIRE]
- **Status**: **DEFERRED**. Keep track of this for future post-Faire research; do NOT implement now.
- **Problem**: Broadcast automation systems update ID3 tags 1.5–2.5 seconds *after* Song B has started.
- **Impact**: Song A contains the station bumper/jingle plus the first ~2 seconds of Song B. Acceptable for Faire demonstration; song intros are intact.
- **Conceptual Proposal (Future Investigation Only)**:
  1. **Frame-Exact Rollback via File Truncation (Preserving Lossless DIRECT RECODE)**:
     - Because `nrsc5` preserves the OTA bitstream losslessly (`-c:a copy`), we must NOT decode to PCM or run lossy audio filters for trimming.
     - Maintain a circular array of the last `MAX_PREROLL_FRAMES` written frame byte lengths:
       `size_t frame_sizes[MAX_PREROLL_FRAMES];`
     - Whenever a frame is written to `rec->tmp_fp`, record its byte length in the ring buffer.
  2. **Truncate Song A's Tail Before Packaging**:
     - When `finalize_current_song()` is triggered by the ID3 transition:
       - Sum the byte lengths of the last `rec->preroll_count` frames currently sitting in the pre-roll buffer (which belong to Song B).
       - Flush and rewind/truncate Song A's temporary file:
         ```c
         fflush(rec->tmp_fp);
         off_t cur_pos = ftello(rec->tmp_fp);
         if (cur_pos > (off_t)tail_bytes_to_trim) {
             ftruncate(fileno(rec->tmp_fp), cur_pos - tail_bytes_to_trim);
         }
         ```
     - Song B receives those frames from the pre-roll buffer.
     - Song A has those exact frames cleanly trimmed off its end.
  3. **Result**: Zero overlap. Song A ends right before Song B begins, without any re-encoding or PCM decompression.

---

## 3. Web Server & Plunder Mechanics (`pirate_server/server.py`)

### 3.1 1.4 MB Static Map Background Loaded Synchronously into RAM with No Caching
- **Location**: `pirate_server/server.py:73-82`, `pirate_server/static/map_bg.jpg`
- **Problem**: `map_bg.jpg` is **1,399,050 bytes (~1.4 MB)**.
  ```python
  with open(full_path, "rb") as f:
      content = f.read() # Reads 1.4MB from SD card on every request
  ```
  There is **no `Cache-Control` header** on static assets.
- **Impact**: Every page refresh or navigation re-transmits 1.4 MB over 2.4 GHz Wi-Fi from the SD card. With 5–10 visitors, this will saturate the Pi's Wi-Fi bandwidth and cause severe page loading stalls.
- **Proposed Solution**:
  1. **Compress `map_bg.jpg` (~90% Bandwidth Reduction)**:
     - The background is a 450x450 repeating parchment pattern. The current 1.4 MB file is uncompressed.
     - Compress `map_bg.jpg` down to a ~90 KB high-efficiency JPEG tile (JPEG quality 80). Visually indistinguishable on mobile screens.
  2. **Selective Browser Cache (Cache Image Only — Developer Friendly)**:
     - In `pirate_server/server.py:serve_static()`, send long-lived caching **strictly for `map_bg.jpg`** (or image assets):
       ```python
       if clean_path.endswith((".jpg", ".jpeg", ".png")):
           self.send_header("Cache-Control", "public, max-age=604800")
       else:
           # Never cache CSS/JS/HTML so live styling and script changes apply immediately
           self.send_header("Cache-Control", "no-cache, must-revalidate")
       ```
     - Keeps active development completely friction-free (no fighting browser caches or hard-reloading for CSS/JS tweaks).
  3. **In-Memory Cache Restricted to Image Asset**:
     - Preload only `map_bg.jpg` into RAM memory so the Pi never reads 90 KB off SD card repeatedly.
     - CSS and HTML templates remain dynamically read from disk for hot-reloading during debugging.

### 3.2 Missing `Cache-Control` Headers on Dynamic Song List (`/` and `/chest`)
- **Problem**: Dynamic song lists served by `self.send_html()` omit `Cache-Control: no-store, no-cache, must-revalidate`.
- **Impact**: Mobile Safari and Chrome aggressively cache HTML on local offline subnets. If Visitor A plunders a song, Visitor B (or Visitor A pressing Back) sees the stale cached HTML showing the song still available. Tapping plunder then throws a 404 error ("That treasure has already been plundered!").
- **Proposed Solution**:
  1. **Add Explicit Anti-Caching Headers in `send_html()`**:
     In `pirate_server/server.py:55`:
     ```python
     def send_html(self, content, status=200):
         data = content.encode("utf-8")
         self.send_response(status)
         self.send_header("Content-Type", "text/html; charset=utf-8")
         self.send_header("Content-Length", str(len(data)))
         self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
         self.send_header("Pragma", "no-cache")
         self.end_headers()
         self.wfile.write(data)
     ```
  2. **Benefits**:
     - Eliminates "ghost tracks": Mobile browsers always fetch the real, current song list from disk rather than showing stale cached HTML.
     - When an attendee plunders a song, anyone refreshing or returning to `/chest` will see that the track has vanished immediately, preventing 404 plunder race errors.
     - Immediate dev reflection: Any changes to `templates/` or HTML render on next refresh without clearing browser history.

### 3.3 Zero-Byte Files Displayed as Plunderable Tracks
- **Location**: `pirate_server/server.py:22-26`
- **Problem**:
  ```python
  if f.lower().endswith(".m4a") and not f.startswith("."):
  ```
  There is no check for `os.path.getsize(full_path) > 0`.
- **Impact**: Corrupt or empty files (such as `Girls_Girls_Girls.m4a`) appear in the chest and deliver empty downloads.
- **Proposed Solution**:
  1. **Enforce Minimum File Size in `scan_songs()`**:
     In `pirate_server/server.py`:
     ```python
     full_path = os.path.join(root, f)
     try:
         if os.path.getsize(full_path) <= 1024:
             continue  # Exclude 0-byte or corrupted stubs
     except OSError:
         continue
     ```
  2. **Defensive Guard in `/download`**:
     In `pirate_server/server.py:do_GET()` under `/download`:
     ```python
     if not os.path.isfile(full_path) or os.path.getsize(full_path) <= 1024:
         self.send_error(404, "That treasure was damaged or plundered by another pirate!")
         return
     ```
  3. **Purge Existing Empty Files**:
     ```bash
     find /home/benjamin/nrsc5/recordings -name '*.m4a' -size -1024c -delete
     ```
     Instantly deletes `./Motley_Crue/Girls_Girls_Girls.m4a` from the vault.

### 3.4 Empty Directory Accumulation Over Time
- **Location**: `pirate_server/server.py:270`
- **Problem**: When a track is plundered, `os.unlink(full_path)` deletes the file, but `os.rmdir(os.path.dirname(full_path))` is never called.
- **Impact**: Over two days of plundering, `recordings/` will accumulate hundreds of empty artist directories. While not fatal, `os.walk` will needlessly traverse empty folders.
- **Proposed Solution**:
  1. **Safe Parent Directory Pruning in `/download`**:
     In `pirate_server/server.py:270`:
     ```python
     os.unlink(full_path)
     print(f"[PIRATE] Plundered & deleted: {filename}")

     # Clean up artist directory if that was the last song
     parent_dir = os.path.dirname(full_path)
     if parent_dir != RECORDINGS_DIR and parent_dir.startswith(RECORDINGS_DIR):
         try:
             os.rmdir(parent_dir)
             print(f"[PIRATE] Cleaned empty vault drawer: {os.path.basename(parent_dir)}")
         except OSError:
             pass  # Still contains other tracks for this artist; leave untouched
     ```
  2. **Benefits**:
     - `os.rmdir()` is atomic and strictly fails (`ENOTEMPTY`) if any other songs remain.
     - Automatically keeps the `recordings/` folder tidy and clean throughout the weekend.
     - Reduces filesystem inode and directory traversal overhead during `scan_songs()`.

### 3.5 Client-Side Cookie Cooldown Easily Bypassed
- **Location**: `pirate_server/server.py:48`, `chest.html`
- **Problem**: Cooldown relies entirely on a standard client cookie `plundered=1; Max-Age=600`.
- **Impact**: Anyone using Safari Private Browsing, Chrome Incognito, or clearing cookies can immediately plunder multiple songs in seconds.
- **Proposed Solution**:
  1. **Dual Defense (Cookie + In-Memory IP Timestamp)**:
     In `pirate_server/server.py`:
     ```python
     PLUNDER_COOLDOWN = {}  # ip -> timestamp of last plunder

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
     ```
  2. **Update Timestamp on Download**:
     In `/download` handler:
     ```python
     PLUNDER_COOLDOWN[self.client_address[0]] = time.time()
     ```
  3. **Benefits**:
     - Prevents someone in Safari Private Browsing or Chrome Incognito from rapidly draining the vault.
     - Pure in-memory (zero database overhead, zero disk writes to SD card).
     - Still allows other attendees with different IPs to plunder concurrently.

---

## 4. Hardware & Environmental Failure Modes (Maker Faire Checklist)

| Risk Factor | Root Cause | Symptom at Event | Preventive Action |
| :--- | :--- | :--- | :--- |
| **Thermal RTL-SDR Reset** | RTL-SDR dongle generates 1.5–2W of heat inside hat | USB drop, `rtlsdr_read_reg failed`, nrsc5 dies | Ensure ventilation holes; use aluminum casing RTL-SDR v4; systemd auto-restart. |
| **PPM Frequency Drift** | Temperature rise causes crystal oscillator drift | Loss of OFDM sync / audio drops after 1–2 hours | Use TCXO-equipped RTL-SDR (e.g. RTL-SDR Blog V3/V4 with 1 PPM TCXO). |
| **Power Brownout** | Pi 4 + RTL-SDR + Wi-Fi AP drawing > 2.5A from power bank | Sudden kernel reboot or USB bus reset | Use a quality 5V 3A / PD power bank; low-resistance USB-C power cable. |
| **2.4 GHz RF Jamming** | Crowded venue spectrum (Maker Faire Wi-Fi, BLE, drones) | Slow downloads, lost Wi-Fi association | Use dual-band 5 GHz AP if hardware allows; set Wi-Fi channel manually (e.g. Ch 1 or 11). |
| **Sudden Power Cut** | Battery pulled while writing audio or system journal | Corrupted ext4 filesystem on microSD | Mount root filesystem read-only or with `commit=60`; volatile journald logging. |

---

## 5. Pre-Faire Implementation Checklist

1. [x] **1.1: Install `nrsc5-recorder.service`**: Systemd unit configured with `NRSC5_FREQ=89.5`, `NRSC5_PROGRAM=1` (HD2), `RestartSec=30`, and journald logging.
2. [x] **1.2: Configure Aggressive DHCP Recycling**: Set `dhcp-range=...,2m` and `dhcp-lease-max=250` in `/etc/NetworkManager/dnsmasq-shared.d/pirate.conf`.
3. [x] **1.3: Software Station Management**: Add idle station reaper (`iw dev wlan0 station dump` > 45s deauth) and soft deauth 15s after `/farewell`.
4. [x] **1.4: Volatile In-RAM Logging**: Enable `nrsc5 -q`, configure `/etc/systemd/journald.conf.d/00-pirate.conf` with `Storage=volatile`, and delete `nohup.out`.
5. [x] **2.1: Asynchronous Packaging**: Decouple `ffmpeg` fork/waitpid into a detached background worker thread in `src/recorder.c`.
6. [x] **2.2: 0-Byte Protection & Vault Purge**: Add `stat()` check and auto-unlink on error in `recorder.c`, filter `> 1024` in `server.py`, and delete `./Motley_Crue/Girls_Girls_Girls.m4a`.
7. [x] **2.3: Collision-Free Staging**: Generate unique PID/timestamp staging files and run startup orphan cleaner in `recorder_create()`.
8. [ ] **2.4: Track Tail Bleed**: **[DEFERRED]** Do not implement for Maker Faire; keep for future research.
9. [x] **3.1: Compress Map Tile & Cache**: Compress `map_bg.jpg` to ~90 KB; send `max-age=604800` strictly for images while keeping CSS/JS live.
10. [x] **3.2: Dynamic HTML Anti-Caching**: Send `Cache-Control: no-store, no-cache, must-revalidate` in `server.py:send_html()`.
11. [x] **3.4: Safe Vault Drawer Pruning**: Call `os.rmdir()` inside `try/except OSError` on parent artist folders after unlinking plundered tracks.
12. [x] **3.5: Dual Cooldown**: Add in-memory IP timestamp check alongside `plundered=1` cookie in `server.py`.
