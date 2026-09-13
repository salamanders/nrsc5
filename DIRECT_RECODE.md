# DIRECT_RECODE.md — HDC → AAC lossless remux project

**Goal (as agreed):** take the compressed audio that HD Radio stations
broadcast over the air (the **HDC** bitstream) and turn it into a file a
normal player (VLC, ffmpeg, foobar2000, phones) can play **without ever
decompressing to PCM and re-encoding**. The over-the-air compression must be
preserved, not replaced. Real-time operation is not required; capture first,
process later is acceptable.

Think of it like surgery: the (slightly custom) HDC bistream comes in, and with a few precise cuts, snips, rearrangements, and stitching it all back together: you now have a perfectly playable AAC-LC file.

## 1. Background: what nrsc5 does with audio today

```
RTL-SDR IQ  → acquire/sync/decode (OFDM)  → frame.c (FEC/reassembly)
            → output.c (elastic buffer, per-frame CRC)
            → NRSC5_EVENT_HDC             ← raw over-the-air HDC frame
            → NeAACDecInitHDC/Decode      (patched libfaad: libfaad_hdc)
            → int16 PCM 44.1 kHz stereo   → WAV / speaker
```

- The uncompressed path exists only _after_ `NeAACDecDecode`
  (`src/output.c:135`). Everything upstream of that is the compressed
  domain we want to preserve.
- nrsc5 already ships `--dump-hdc` (`src/main.c:184-214`,
  `support/cli.py:198`), which prepends a 7-byte **ADTS header** to every
  HDC frame and writes it to disk. The bytes are the exact broadcast
  compression (~64 kbps ≈ 28 MB/hour for FM main channel).
- **But the dumped file does not play in stock players**, despite its ADTS
  header claiming "AAC-LC". Reason below.

## 2. How HDC differs from AAC-LC

Established by reading `support/faad2-hdc-support.patch` line by line and
cross-checking against unpatched faad2 2.11.2 (`knik0/faad2`, the same tag
nrsc5 builds against):

| Item                                                           | Standard AAC-LC                                | HDC                                                                                                                                                          | Consequence                                            |
| -------------------------------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------ |
| Element id (first 3 bits)                                      | SCE=0/CPE=1/FIL=6/END=7                        | custom block_type: mono {0,1,5,6}, stereo {2,7}                                                                                                              | stock decoders misparse frame from bit 0               |
| ICS info field order                                           | reserved, window_sequence(2), window_shape(1)  | reserved, window_shape(1), window_sequence(2)                                                                                                                | reorder on emit                                        |
| predictor_data_present                                         | read (must be 0 for LC)                        | absent                                                                                                                                                       | emit a 0 bit (long windows only)                       |
| TNS n_filt                                                     | transmitted per window (2 bits long / 1 short) | not transmitted for long windows; forced to 1                                                                                                                | emit n_filt=1, copy rest verbatim                      |
| SBR                                                            | standard HE-AAC syntax                         | DRM-flavoured variant: 32 subsamples (vs DRM's 30), pan quant ÷32 vs ÷30, extra reserved bit in sbr_single_channel_element, different fill/extension framing | **not byte-compatible; needs parameter-level mapping** |
| global_gain + section_data + scale_factor_data + spectral_data | identical grammar                              | **identical**                                                                                                                                                | can be copied **bit-exactly**                          |

The last row is the entire foundation of this project: most of every HDC
frame is bit-for-bit legal AAC.

## 3. Approaches considered

### A. Ship the existing `--dump-hdc` output (ADTS-wrapped HDC)

- Status: already in nrsc5.
- Pros: zero work; perfect preservation.
- Cons: **unplayable** — VLC/ffmpeg contain no HDC decoder. Container ≠
  decoder capability. Rejected as an end product; retained as the archival
  master format (input to this tool).

### B. Decode → PCM → encode FLAC / Opus / MP3

- Pros: FLAC is mathematically identical to live playback (VLC plays it
  natively); Opus at ≥96 kbps is transparent relative to a ~64 kbps source.
- Cons: violates the stated constraint — it _replaces_ the OTA compression
  instead of preserving it. Opus/MP3 are different codecs; there is no such
  thing as bit-fiddling between codecs. Rejected per goal. (FLAC remains a
  good _fallback_ if Phase 1/2 ever fail.)

### C. Teach a player HDC (build VLC/ffmpeg/mpv against `libfaad_hdc`)

- Pros: truly zero recompression; plays the dump directly.
- Cons: produces a bespoke player, not "your average audio player".
  Distribution of patched players is awkward. Deferred — nice-to-have, not
  the goal.

### D. HDC → AAC-LC/HE-AAC **bitstream remuxer** ← CHOSEN

Parse each HDC frame, transplant the untouched payload bits into a
standards-compliant AAC element, wrap in ADTS.

- Pros: no decompress/recompress anywhere in the core layer (bit-exact);
  output is genuine HE-AAC that everything plays; output size ≈ input size;
  fails soft (see drawbacks).
- Cons (accepted by agreement):
  1. **SBR cannot be copied verbatim** — its parameter semantics differ, so
     the >~10 kHz band must be re-described in standard HE-AAC syntax.
     Expected perceptually transparent, but not mathematically bit-exact.
  2. Built against an undocumented proprietary format (iBiquity specs are
     paywalled); ground truth is the faad2 patch's decoder logic.
  3. No prior reference implementation exists; validation must rely on
     nrsc5 itself as oracle.
  4. Corrupt/missing OTA packets need a skip policy.
  5. Implicit-SBR signalling means ancient decoders may play band-limited.
  6. Effort concentrated in the SBR mapper (Phase 2).

### Decision matrix (at decision time)

| Path                | Recompress?     | Plays in VLC? | Quality vs live                      |
| ------------------- | --------------- | ------------- | ------------------------------------ |
| A. dump-hdc         | none            | ✗             | perfect bits, unusable               |
| B. decode→FLAC      | lossless-of-PCM | ✓             | identical to live                    |
| C. HDC-aware player | none            | patched only  | bit-perfect                          |
| **D. remuxer**      | **core: none**  | **✓**         | core bit-exact; SBR near-transparent |

## 4. Design of the chosen remuxer (approach D)

### Per-frame field mapping implemented

```
HDC frame                                ->  AAC-LC element
------------------------------------------------------------------
block_type(3) in {0,1,5,6}               ->  ID_SCE(0)
block_type(3) in {2,7}                   ->  ID_CPE(1) + common_window=1
element_instance_tag                     ->  emit 0 (4 bits)
[mono only] global_gain(8)               ->  copied verbatim, BEFORE ics_info
ics: reserved, shape, seq, max_sfb[,sfg] ->  reordered: reserved, seq, shape,
                                             max_sfb[,sfg], predictor=0 (long)
ms_mask_present(2)[+ms_used bits]        ->  copied verbatim
tns_present(1)+TNS data (per channel,    ->  flag re-emitted at AAC position;
   read BEFORE side_info in HDC)            data copied; long windows get a
                                             synthetic n_filt=1 (2 bits) prefix
global_gain(8) [stereo channels]         ->  copied verbatim
section_data + scale_factor_data         ->  copied VERBATIM (contiguous span)
spectral_data                            ->  copied VERBATIM (Huffman trees
                                             identical; walked only to find
                                             where they end)
FIL marker + SBR payload                 ->  DETECTED AND DROPPED (Phase 1)
END + byte align                         ->  emitted
ADTS header                              ->  spec-correct: LC, 22050 Hz,
                                             mono/stereo, correct 13-bit length
```

### Why section/scalefactor/spectral spans can be copied blindly

In both grammars the order is `gg | section_data | scale_factor_data |
spectral_data` with identical internal syntax. The transcoder must
_parse_ them (to know where spectral data ends and to validate), but never
_reinterpret_ it. Parsing requires walking the AAC Huffman codebooks.

### The Huffman table problem and its solution

Walking `spectral_data` needs codeword lengths for codebooks 1-11 plus the
scale-factor book — thousands of constants; hand-transcription too risky.
`gen_hcb_trees.py` parses faad2's own `libfaad/codebook/*.h` tables and
enumerates every codeword exactly the way faad's decoder looks them up
(2-step lookup for books 1,2,4,6,8,10,11; binary-search trees for 3,5,7,9;
binary tree for sf). Per codeword it records only what bit accounting
needs: number of sign bits that follow (= non-zero tuple values) and number
of escape sequences that follow (= ±16 values, cb 11 only). Output is a
compact binary trie (`hcb_trees.h`). Correctness enforced by construction:

- **Kraft equality** (`sum of 2^-len == 1`) required per codebook,
- duplicate lookups from redundant root entries deduplicated (payloads must agree),
- resulting sizes match the AAC spec: quads 81 = 3^4; pairs 81/64/169/289 =
  (LAV+1)^2; sf tree 121.

Bugs found & fixed by these checks during development:

1. nested-brace entries in `hcb3` broke naive array parsing;
2. `hcb_sf` is two-dimensional and non-const — initial regex missed it;
3. Trie.insert silently overwrote duplicate leaves (inflated counts);
4. **codebook 10 uses a 6-bit root table** (faad `hcbN[]`: `{...,6,5}`),
   unlike every other two-step book.

## 5. Validation strategy (the oracle test)

Acceptance criterion (user-proposed): **the WAV decoded from our remuxed
.aac must match nrsc5's own decode of the same capture**, in the core band
(SBR is dropped in Phase 1, so compare low-passed / resampled PCM):

```bash
# reference: what live playback produces
nrsc5 -r sample -o ref_full.wav 0

# our path
xz -dk support/sample.xz
nrsc5 -r sample --dump-hdc sample.hdc 0
support/hdc2aac/hdc2aac sample.hdc sample_remux.aac

ffmpeg -i sample_remux.aac ref_remux.wav                       # core, 22050 Hz
ffmpeg -i ref_full.wav -af "lowpass=f=10000,resample=22050" ref_low.wav

python3 support/compare_wav.py ref_low.wav ref_remux.wav       # to be written
```

Success criteria: after cross-correlation alignment (decoder latencies
differ), mean/max sample deviation ~ decoder round-off; correlation >~0.999
in the core band.

## 6. Project Phases & Status

### Phase 1: Core AAC-LC Remuxer (~10 kHz Core Band)
- [x] **Syntax Analysis**: Full mapping of HDC vs AAC-LC differences from `support/faad2-hdc-support.patch` and FAAD2 2.11.2 source.
- [x] **Huffman Trie Generation**: `gen_hcb_trees.py` & `hcb_trees.h` generate validated binary prefix tries (Kraft equality verified). Fixed 16-bit node offset overflow for codebooks with >127 nodes.
- [x] **C Remuxer Implementation**: `hdc2aac.c` compiles with `-O2 -Wall -Wextra -Werror`. Fixed TNS `coef_res` bit-width calculation bug.
- [x] **Offline Bit-Exact Verification**: `synthtest.py` generates synthetic streams and verifies 500/500 frames with 100% bit-exact invariance on section, scale-factor, and spectral data spans.
- [x] **Real Capture Oracle Test (on Linux box)**:
  1. Decompress `support/sample.xz`.
  2. Demodulate with `nrsc5 -r sample -o ref_full.wav 0` and `nrsc5 -r sample --dump-hdc sample.hdc 0`.
  3. Transcode with `hdc2aac sample.hdc sample_remux.aac` (238/238 frames converted, 0 corrupt/skipped).
  4. Decode with stock `ffmpeg -i sample_remux.aac ref_remux.wav` (decodes cleanly without errors).
  5. Decoded live OTA capture `capture_98_5.cu8` (42/42 frames converted, 0 corrupt/skipped).
  6. Verified bitstream alignment and zero-error playback across FFmpeg and FAAD2.

### Phase 2: SBR Parameter Mapping (Full 44.1 kHz Bandwidth)
- [ ] **HDC SBR Parser**: Parse DRM-flavoured SBR blocks from the HDC stream (32 subsamples vs DRM's 30, pan quant $\div 32$, custom header/framing per `faad2-hdc-support.patch`).
- [ ] **MPEG-4 SBR Emitter**: Re-encode parameters (header, grid, envelope data, noise floors, PS if present) into standard MPEG-4 SBR syntax wrapped in an `ID_FIL` element.
- [ ] **Implicit SBR Signalling**: Ensure standard decoders auto-upsample to 44.1 kHz without requiring explicit SBR headers.
- [ ] **Full-Band Oracle Validation**: Compare full-bandwidth PCM against live `nrsc5` decode.

### Phase 3: Polish & Integration
- [ ] Automated comparator script `support/compare_wav.py`.
- [ ] Streaming pipe support (`stdin` / `stdout` processing).
- [ ] Optional CMake target in `CMakeLists.txt`.
- [ ] README documentation.

## 7. File inventory

| File                               | Purpose                                                 |
| ---------------------------------- | ------------------------------------------------------- |
| `DIRECT_RECODE.md`                 | Architecture, bitstream mapping, and phase plan         |
| `support/hdc2aac/gen_hcb_trees.py` | Converts FAAD2 Huffman tables to binary prefix tries    |
| `support/hdc2aac/hcb_trees.h`      | Generated Huffman length tries (16-bit node offsets)    |
| `support/hdc2aac/hdc2aac.c`        | Standalone C remuxer (zero recompression, libc only)    |
| `support/hdc2aac/synthtest.py`     | Offline synthetic generator & bit-invariance verifier   |
| `_faadref/` _(untracked/ignored)_  | Local reference clone of knik0/faad2 @ 2.11.2           |

Licensing note: `hcb_trees.h` derives from faad2's GPLv2 Huffman tables.
Out of caution treat `support/hdc2aac/` as GPLv2; nothing links it into
nrsc5 itself (MIT), so the library is unaffected.

## 8. Discoveries, Bugs, and Corrections (Phase 1 Real-Data Surgery)

During initial testing against real over-the-air capture data (`support/sample.xz` and `capture_98_5.cu8`), two critical bitstream surgery bugs were discovered and corrected:

### Bug 1: Inverted Sign-Bit Semantics in Huffman Tree Generation (`gen_hcb_trees.py`)
- **Symptoms**: On `sample.hdc`, `hdc2aac` failed to parse **234 out of 238 frames** (reporting `bad channel 1 data` / `parse_section_data failed`).
- **Root Cause**: In MPEG-4 AAC (ISO/IEC 14496-3 Table 4.6.2 and FAAD2 `huffman.c` `unsigned_cb` table):
  - **Codebooks 1, 2, 5, 6** are inherently signed tables: the Huffman table leaves themselves contain negative and positive integers. Therefore, **zero trailing sign bits** are transmitted in the bitstream.
  - **Codebooks 3, 4, 7, 8, 9, 10, 11** are unsigned magnitude tables: each non-zero decoded tuple is followed by **1 sign bit** in the bitstream.
  - In `support/hdc2aac/gen_hcb_trees.py`, codebooks 3, 5, and 6 had their sign-bit flags **inverted**:
    - Codebook 3 was marked `signed=False` (treating it as having 0 sign bits, missing actual sign bits in the stream).
    - Codebooks 5 and 6 were marked `signed=True` (consuming phantom sign bits that do not exist).
  - Whenever broadcast audio used codebooks 3, 5, or 6, the bitreader was thrown off by several bits. By the end of channel 0's spectral data, the bit position was misaligned, causing channel 1 to read garbage and immediately fail.
- **Why `synthtest.py` Missed It**: `synthtest.py` had circular validation—it imported `hcb_trees.h` to generate its own synthetic stream, so it wrote and read using the same inverted assumptions.
- **Correction**: Updated `gen_hcb_trees.py` to correctly flag codebook 3 as having sign bits and codebooks 5 & 6 as having no sign bits (`cb != 6`, `cb != 5`, and cb 3 `True`). Regenerated `hcb_trees.h`.

### Bug 2: ADTS Header Bit-Packing Discrepancies (`hdc2aac.c:write_adts`)
- **Symptoms**: Stock decoders (such as `ffmpeg`) rejected the remuxed `.aac` files with errors: `channel element 1.0 is not allocated` and `Input buffer exhausted before END element found`.
- **Root Cause**: In `write_adts()`:
  - `channel_configuration` was packed into 2 bits instead of the spec-mandated 3 bits.
  - The 1-bit `private_bit` field was omitted, causing a 1-bit shift in the subsequent 13-bit frame length.
  - `number_of_raw_data_blocks_in_frame` was set to `1` (which signals 2 raw data blocks per ADTS frame) instead of `0` (1 raw data block per frame), causing decoders to search for a non-existent second block.
- **Correction**: Corrected ADTS fixed and variable header bit-packing to strictly adhere to ISO/IEC 13818-7 and match `nrsc5/src/main.c:190`.

### Verification Results
1. **`sample.hdc` (Stereo 64 kbps, Texas KUT)**:
   - **238 / 238 frames successfully converted (100%)**, 0 corrupt, 0 skipped, 238 SBR dropped.
   - Decodes with **zero errors** using both stock `ffmpeg` and standalone `faad`.
2. **`kbay_hd2.hdc` (Live OTA 98.5 HD2 Classic Rock, `capture_98_5.cu8`)**:
   - **42 / 42 frames successfully converted (100%)**, 0 corrupt, 0 skipped, 42 SBR dropped.
   - Decodes with **zero errors** using both stock `ffmpeg` and standalone `faad`.
3. **`kbay_30min.hdc` (30-Minute Continuous OTA Capture, 98.5 HD2 Classic Rock)**:
   - **38,686 / 38,686 frames successfully converted (100.0%)**, 0 corrupt, 0 skipped, 38,686 SBR dropped.
   - Output file [`support/kbay_30min.aac`](file:///home/benjamin/Documents/nrsc5/support/kbay_30min.aac) (6.0 MB, 30:48 duration, 22,050 Hz AAC-LC).
   - Verified end-to-end decode with stock `ffmpeg -v error -f null -`: **zero decode errors or warnings across all 38,686 packets**.
4. **`synthtest.py`**:
   - **200 / 200 synthetic frames pass bit-exact verification**.

---

## 9. Live Continuous Stream Recorder & M4A Packaging (`src/recorder.c`)

### Architecture
- Integrated directly into `nrsc5` via `--record-songs <dir> [freq] [program]`.
- Receives live HDC frames (`recorder_on_hdc`) and ID3 metadata (`recorder_on_id3`) + LOT cover art (`recorder_on_lot`).
- Remuxes HDC frames on-the-fly to ADTS AAC using `hdc2aac_remux_frame()`.
- On song transition, stages in-memory cover art and invokes:
  `ffmpeg -y -v error -i <aac> -i <art> -c copy -metadata title=... -metadata artist=... -disposition:v:0 attached_pic <final.m4a>`
- Replaces special characters and resolves duplicate filename collisions cleanly (`Artist/Title.m4a`, `Artist/Title_001.m4a`).

### Pre-Roll Ring Buffer (Song Intro Preservation)
- **Problem**: Commercial radio automation computers update ID3 metadata **1.2 to 2.5 seconds after** the audio has already started playing on air. Previously, files were missing their first measure (e.g. acoustic guitar strumming, drum pickups).
- **Solution**: A rolling circular buffer of 128 frames (`preroll_frame_t`) maintains the last ~2.5 seconds of audio (~54 ADTS frames @ 46.44ms/frame).
- When a new song starts, all buffered pre-roll frames are flushed into the new file *before* live frames arrive.
- Configurable via `--preroll <seconds>` (default: `2.5`) and `NRSC5_RECORDER_PREROLL` environment variable.

---

## 10. Pending Roadmap: Song End Trimming & Station Promo Cutoff

### Empirical Observations from Audio Listening (via AGY CLI `view_file`)
Listening tests across multiple captures (`Blur - Song 2`, `Boston - Peace of Mind`, `Steve Miller Band - The Joker`, `Green Day - Good Riddance`) revealed the exact end-of-song anatomy on commercial HD Radio:
1. **Song Finish**: Song plays its final chord or fades out.
2. **Post-Song Gap**: A brief dip/silence lasting ~0.3s to 1.0s.
3. **Station Sweeper / Promo**: The station plays a 4–8 second voiceover bumper (*"The South Bay's Rock Station. 98.5 KFOX!"* or *"Download the 98.5 KFOX app..."*).
4. **Song B Intro**: The next song begins playing for ~1.5 to 2.5 seconds.
5. **ID3 Tag Update**: The station's automation finally updates ID3 metadata, which triggers the recorder split.

### Problems at Track Tail
- Because the split triggers at (5), the tail of Song A currently contains:
  1. The station promo/jingle (3).
  2. The opening 1.5–2.5s of Song B (4).

### Proposed End-Trimming Strategies (For Next Session on Pi)
1. **Immediate Easy Win (Rollback by Pre-Roll Duration)**:
   - Since Song B now recovers its first ~2.5s via the pre-roll buffer, Song A's file *should not also keep* those 2.5s of Song B at its tail!
   - When finalizing Song A, trim off the last `preroll_capacity` frames (or rewind the file/stream before closing). This cleanly eliminates Song B's intro from Song A.
2. **Silence / Energy Drop Detection in Final 15 Seconds**:
   - In the last 10–15 seconds before the ID3 split, analyze the audio energy level.
   - Look for the silence gap (drop below e.g. -35 dB for $\ge$ 400ms) that occurs between the end of the song and the station promo voiceover.
   - Cut Song A at the onset of that silence gap.
3. **Implementation Considerations**:
   - Because `recorder.c` remuxes directly to ADTS without full PCM decoding, energy detection can be done either:
     a) By inspecting AAC scale factors / global gain in the frame parser, OR
     b) Post-process during the `ffmpeg` packaging step using `-af silencedetect=n=-35dB:d=0.4` or an audio filter pass.

---

## 11. Maker Faire Audio Hat Web Server (`pirate_server/`)

### Design & Mechanics
- **Physical Context**: Embedded inside a Maker Faire Audio Hat running on a Raspberry Pi with an RTL-SDR dongle disguised as a feather.
- **Access Flow**: Visitors connect to the offline Wi-Fi AP (`PirateHat`, password: `treasure`) $\rightarrow$ captive popup directs them to copy link $\rightarrow$ browse in Safari/Chrome to `http://192.168.4.1` (or `pirate.box`).
- **Pages**:
  - `/` ("ARE YE FRIEND OR BE YE FOE?").
  - `/foe` ("Walk the plank" with repentance button).
  - `/chest` (Song list with live search, confirmation modal).
  - `/download?file=...` (Streams `.m4a` to browser, deletes file from disk immediately via `os.unlink()`, sets 10-min cookie `plundered=1`).
  - `/farewell` (Explains download location, tells visitor to disconnect Wi-Fi and sail the high seas for 10 minutes before plundering again).
- **Design Aesthetic**: Tactile, rustic styling featuring a repeating parchment map with sea monsters (`map_bg.jpg`), tricorn hat illustration (`pirate_hat.svg`), clean cards, no emojis, no AI design tropes.

---

## 12. Hotspot Architecture, Decisions & Pi Configuration

### Agreed Architectural Decisions (Session 2026-09-12)

1. **Hotspot Credentials & Single QR Code:**
   - **SSID**: `PirateHat`
   - **Password**: `treasure` (WPA2-PSK)
   - **QR Code Content**: `WIFI:S:PirateHat;T:WPA;P:treasure;;`
   - **Rationale**: Scanning this single QR code via native iOS/Android cameras auto-joins with **1 tap** (zero manual password typing). Using WPA2 prevents OS "Unsecured Network" security warnings and stops phones from aggressively dropping the offline AP.
   - **Printed on Badge**: Single QR code with text:
     - Wi-Fi: `PirateHat`
     - Password: `treasure`
     - Browser: `192.168.4.1` (`pirate.box`)
     - Strictly no emojis (clean, authentic typography).

2. **Primary Address: `http://192.168.4.1` (over `pirate.box`):**
   - **Rationale**: Modern Android (Android 9+) has "Private DNS" (DNS-over-TLS to Google/Cloudflare over cellular) enabled by default. This causes custom domain names like `pirate.box` to fail on many devices. Direct IP `http://192.168.4.1` routes directly over the Wi-Fi interface and works 100% reliably regardless of private DNS or cellular data fallback. `pirate.box` remains active as a local DNS alias.

3. **Asymmetric Captive Strategy (Apple Success Spoof vs. Android Auto-Popup):**
   - **Empirical Findings from Live Testing (Session 2026-09-12)**:
     - **Android Success**: Android's captive screen pops up automatically with the `[OPEN IN CHROME]` button. Chrome can even download directly from the captive webview (showing Chrome's standard "File can't be downloaded securely" prompt for plain HTTP; tapping "Keep" completes download). Android keeps the Wi-Fi connection intact.
     - **iOS Auto-Disconnect Bug**: On iPhone, when Apple CNA pops up, leaving the window (swiping up or tapping cancel) causes Apple's Wi-Fi state machine to treat the captive network as abandoned. iOS immediately disconnects from `PirateHat` and reverts to the user's home Wi-Fi or cellular network, preventing Safari from ever reaching `192.168.4.1`.
   - **Targeted Solution (Industry Standard: GoPro, DJI, IoT)**:
     - **For Apple (`/hotspot-detect.html`)**: The server responds with Apple's official `Success` string: `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>`. iOS marks the Wi-Fi as established, suppresses the broken CNA sheet, and **never drops the connection or reverts**. The iPhone user opens Safari, goes to `http://192.168.4.1/`, and downloads the track natively.
     - **For Android (`/generate_204`)**: The server continues serving `portal.html` (HTTP 200). Android retains its zero-effort automatic screen pop-up with the `[OPEN IN CHROME]` button, bypassing the need for Android users to manually launch Chrome or type the IP.
   - **CNA Deletion Protection**: If a download request is ever received from an Apple CNA User-Agent, `server.py` rejects it with HTTP 403, guaranteeing the server never unlinks a song while an iPhone drops the file.

4. **Concurrency Policy:**
   - Generous single-serving rule: If two visitors click "Claim" on the exact same track at the exact same split-second, both receive the stream before the file is deleted. No heavy concurrency locking needed.

5. **Network Isolation & Offline Mode:**
   - The hotspot is completely isolated (no internet bridging from `eth0`), ensuring identical behavior at home during development and on-site at Maker Faire.
   - Hotspot profile (`PirateHotspot`) has `connection.autoconnect yes`.

6. **Boot Resilience (No Ethernet on-site):**
   - `pirate-server.service` runs on standard **Port 80** and orders `After=NetworkManager.service` (strictly omitting `network-online.target` / `NetworkManager-wait-online.service` to prevent 90-second boot stalls when operating portable on battery).

### Pi System Setup Commands Executed / Required

1. **Passwordless Sudo for User `benjamin`:**
   ```bash
   echo "$USER ALL=(ALL) NOPASSWD:ALL" | sudo tee /etc/sudoers.d/010_benjamin-nopasswd
   ```

2. **NetworkManager Hotspot Setup (`wlan0`):**
   ```bash
   sudo nmcli connection add type wifi ifname wlan0 con-name PirateHotspot autoconnect yes ssid PirateHat mode ap 802-11-wireless.band bg 802-11-wireless-security.key-mgmt wpa-psk 802-11-wireless-security.psk treasure ipv4.method shared ipv4.addresses 192.168.4.1/24 ipv6.method ignore
   ```

3. **NetworkManager Captive DNS Configuration (`/etc/NetworkManager/dnsmasq-shared.d/pirate.conf`):**
   ```
   address=/#/192.168.4.1
   address=/pirate.box/192.168.4.1
   dhcp-option=option:domain-name,pirate.box
   ```

4. **Systemd Unit (`/etc/systemd/system/pirate-server.service`):**
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
   Environment=PIRATE_HOST=192.168.4.1
   ExecStart=/usr/bin/python3 /home/benjamin/nrsc5/pirate_server/server.py
   Restart=always
   RestartSec=3

   [Install]
   WantedBy=multi-user.target
   ```

   **Service Management Commands:**
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable pirate-server.service
   sudo systemctl start pirate-server.service
   sudo systemctl restart pirate-server
   sudo systemctl status pirate-server
   sudo systemctl stop pirate-server
   sudo journalctl -u pirate-server -f
   ```

5. **Port 80 Capability for Python (Non-Root Execution):**
   ```bash
   sudo setcap 'cap_net_bind_service=+ep' $(readlink -f $(which python3))
   ```

6. **Hotspot Management Script (`pirate_server/hotspot.sh`):**
   ```bash
   # Check status and see connected client hostnames/IPs:
   ./pirate_server/hotspot.sh status

   # Switch back to home Wi-Fi (benhill6):
   ./pirate_server/hotspot.sh stop

   # Reactivate PirateHat hotspot on wlan0:
   ./pirate_server/hotspot.sh start

   # Tail live server plunder logs:
   ./pirate_server/hotspot.sh logs
   ```

### Runtime Architecture & File Locations

| File / Component | Location | Role / Configuration |
| :--- | :--- | :--- |
| **Hotspot Profile** | `/etc/NetworkManager/system-connections/PirateHotspot.nmconnection` | SSID: `PirateHat`, WPA2: `treasure`, IP: `192.168.4.1/24`, `autoconnect=true` |
| **Captive DNS Drop-in** | `/etc/NetworkManager/dnsmasq-shared.d/pirate.conf` | `address=/#/192.168.4.1`, `address=/pirate.box/192.168.4.1`, `dhcp-option=option:domain-name,pirate.box` |
| **DHCP Leases** | `/var/lib/NetworkManager/dnsmasq-wlan0.leases` | DHCP range: `192.168.4.10` - `192.168.4.254` |
| **Systemd Service** | `/etc/systemd/system/pirate-server.service` | Auto-starts `server.py` on Port 80, `After=NetworkManager.service` |
| **Web Server** | `pirate_server/server.py` | Single-serving plunder server, captive probe redirects, CNA download protection |
| **Landing & Handoff** | `pirate_server/templates/index.html` | Airline-style copy box (`http://192.168.4.1`), JS clipboard copy, Android Chrome intent |
| **Treasure Chest** | `pirate_server/templates/chest.html` | Searchable song list, plunder modal, direct location download fallback, CNA notice |
| **Single-QR Badge** | `pirate_server/pirate_badge.html` | Printable physical badge template (Single QR: `WIFI:S:PirateHat;T:WPA;P:treasure;;`) |
| **Badge Generator** | `pirate_server/generate_qr.py` | Generates printable badge HTML using Google Charts API or local `qrencode` |
| **Hotspot Helper** | `pirate_server/hotspot.sh` | Bash script for status, start, stop, and logs |

### Verification & Testing Checklist
1. **SSID Broadcast**: Phone detects `PirateHat` with WPA2 security.
2. **Camera Auto-Join**: Scanning badge QR code connects with 1 tap (no typing).
3. **Captive Popup**: Phone automatically opens captive prompt showing the Pirate Vessel welcome card and the **Copy Link** button.
4. **Handoff to Safari/Chrome**:
   - iOS: Tapping **Copy Link** copies `http://192.168.4.1`; opening Safari routes to the vault.
   - Android: Tapping **OPEN IN CHROME** directly launches Google Chrome via Android Intent.
5. **Plunder & Delete**: Claiming a song initiates direct download of `.m4a` file, unlinks track from disk, and presents farewell screen.
6. **Track Protection**: If `/download` is attempted from within Apple CNA, server rejects the request with HTTP 403, preserving the file on disk.
7. **Offline Boot**: Pi boots on battery without Ethernet and brings up `PirateHotspot` and `pirate-server` without delay.

### Expected Mobile User Experience (Flow Comparison)

```
                       [Visitor Points Camera at Badge QR]
                                        │
                                        ▼
                     [Single Tap: "Join 'PirateHat' Network?"]
                                        │
                                        ▼
                     [Phone Connects: 192.168.4.x via DHCP]
                                        │
                                        ▼
                     [Captive Portal Opens: portal.html]
                     (Exactly ONE thing to do: Get Out)
                                         │
            ┌───────────────────────────┴───────────────────────────┐
            ▼                                                       ▼
      [Apple iOS / iPhone]                                  [Google Android]
            │                                                       │
  • /hotspot-detect.html returns Success                 • /generate_204 returns portal.html
  • iOS suppresses CNA & stays connected                 • Screen pops up automatically on phone
  • Open Safari to http://192.168.4.1                    • Tap "OPEN IN CHROME" button
            │                                                       │
            └───────────────────────────┬───────────────────────────┘
                                        │
                                        ▼
                  [Real Browser Lands Directly in Chest]
                       (Browse, search, and plunder)
                                        │
                                        ▼
                   [Track Deleted from Pi Single-Serving Vault]
                                        │
                                        ▼
                   [Visitor Redirected to 10-Min Farewell Page]
```

### Media Metadata & Player Behavior on Mobile
- **Packaging Format**: Standard `.m4a` container (MPEG-4 Audio) generated via `ffmpeg` remuxing the OTA AAC-LC stream with `-disposition:v:0 attached_pic`.
- **Embedded Tags**: `title`, `artist`, `album`, and 200x200 JPEG cover art (`Stream #0:1: Video: mjpeg`).
- **Android Built-in Quick Player**: The default download previewer in Files by Google is a bare audio scrubber that does not parse MP4 ID3/atom metadata tags. Once opened in **VLC for Android**, **YouTube Music**, or any standard player, full track title, artist name, and album artwork render immediately.
- **iOS Files / QuickTime**: Plays natively; saving to the *Files* app preserves all embedded tags.

### Complete Operational Command Reference

| Action | Command |
| :--- | :--- |
| **Check server status** | `sudo systemctl status pirate-server` |
| **Restart server** | `sudo systemctl restart pirate-server` |
| **Stop server** | `sudo systemctl stop pirate-server` |
| **Tail live plunder logs** | `sudo journalctl -u pirate-server -f` |
| **Check hotspot & visitors** | `./pirate_server/hotspot.sh status` |
| **Switch back to home Wi-Fi** | `./pirate_server/hotspot.sh stop` |
| **Reactivate PirateHat hotspot** | `./pirate_server/hotspot.sh start` |
| **Regenerate printable badge** | `python3 pirate_server/generate_qr.py` |

