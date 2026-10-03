# Engineering & Stability Audit (`BUGS.md`)

This document details the engineering design decisions, bug fixes, failure modes, and architectural improvements across the continuous HD Radio audio stream recorder and automated Jukebox Builder.

---

## 1. Process Stability & Runtime Reliability

### 1.1 RTL-SDR Transient USB Drops & Systemd Auto-Restart
- **Problem**: RTL-SDR dongles frequently experience USB transient drops, thermal resets, or power dips. When `nrsc5` terminated, recording halted permanently unless manually restarted.
- **Solution**:
  - Implemented `nrsc5-recorder.service` systemd unit configured with `Restart=always` and `RestartSec=30`.
  - The 30-second backoff allows the RTL-SDR dongle to cool down and prevents log flooding if temporarily disconnected.
  - Standardized quiet execution (`-q`) with logs routed directly into `systemd-journald`.

### 1.2 Unbounded Log Growth & SD Card Storage Protection
- **Problem**: Emitting per-second telemetry (MER, BER, bit rate) generated several gigabytes of unmanaged log files, risking SD card wear and exhaustion.
- **Solution**:
  - Activated quiet mode (`-q`) in continuous operation. High-value song transition and minting events remain cleanly logged via `log_info()`.
  - Configured journald for volatile storage (`Storage=volatile`), keeping runtime logs in RAM (`/run/log/journal`) to protect SD card endurance.

---

## 2. Audio Pipeline & Direct Recode (`src/recorder.c`)

### 2.1 Asynchronous Packaging via Detached Background Worker
- **Problem**: `finalize_current_song()` originally ran synchronous `waitpid()` on `ffmpeg`, blocking the main `nrsc5` event thread for 200–800ms. This stalled the event loop, causing RTL-SDR USB sample buffers to drop frames and lose OFDM sync.
- **Solution**:
  - Staged temporary recording files cleanly on song transition: `base_dir/.staging_<pid>_<time>_<seq>.aac`.
  - Spawned detached background worker threads (`pthread_create(&tid, &attr, remux_worker_thread, job)` with `PTHREAD_CREATE_DETACHED`).
  - The main SDR capture loop returns to processing HD Radio OFDM frames in < 1 millisecond. USB buffers never starve or overflow during song finalization.

### 2.2 Corrupted 0-Byte Audio File Prevention
- **Problem**: If `ffmpeg` failed or received truncated streams, empty 0-byte `.m4a` files were left on disk.
- **Solution**:
  - Verified exit codes and performed `stat()` validation (`st.st_size > 1024`).
  - If remuxing fails or produces an empty stub, the file is unlinked immediately and logged as an error.

### 2.3 Collision-Free Staging & Startup Orphan Sweeper
- **Problem**: Static temporary file paths (`.tmp_recording.aac`) risked collisions upon abrupt reboots or overlapping song transitions.
- **Solution**:
  - Dynamically generated PID- and monotonic timestamp-indexed staging filenames: `.staging_<pid>_<time>_<seq>.aac` and `.tmp_art_<pid>_<time>_<seq>`.
  - Added an orphan cleaner to `recorder_create()` to sweep away any stale temporary artifacts left from unexpected power loss.

---

## 3. Automated Song Boundary Trimming & Jukebox Minting

### 3.1 The 2-Airing Rule vs. Silence Guessing
- **Problem**: Single-take silence detection (`silencedetect`) cuts off intentional artistic audio (spoken dialogue, acoustic guitar intros, dramatic pauses in concept albums like Pink Floyd) and allows DJ chatter into recordings.
- **Solution**:
  - Implemented the strict **2-Airing Rule** in `scripts/trimmer.py`.
  - Raw captures are recorded with 15-second pre-roll and post-roll overlap buffers and archived permanently in `recordings/raw/<Artist>/`.
  - Songs are only trimmed and minted into `jukebox/<Artist>/<Title>.m4a` when 2 or more independent airings exist and cross-correlate with high confidence ($\ge 0.85$).

### 3.2 Automated Minting on Capture Completion
- **Problem**: Trimming previously required manual batch post-processing passes.
- **Solution**:
  - As soon as `remux_worker_thread()` in `src/recorder.c` completes writing a new raw capture, it automatically spawns `trimmer.py --apply --artist "<clean_artist>" --title "<clean_title>"`.
  - If this new take provides the second airing needed for high-confidence boundary resolution, the clean master is minted into the Jukebox immediately.
  - Raw source takes are always kept intact.
