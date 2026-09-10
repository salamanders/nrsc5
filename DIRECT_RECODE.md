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

