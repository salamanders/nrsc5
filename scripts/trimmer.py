#!/usr/bin/env python3
"""
trimmer.py - Jukebox Builder for NRSC5 Broadcast Recordings

Enforces the strict "2-Airing Rule" to build a curated, sample-accurate Jukebox:
1. Scans raw broadcast recordings in RECORDINGS_DIR (captured with 15s pre/post padding).
2. For any song not yet in JUKEBOX_DIR:
   - If < 2 takes exist: Waits for a 2nd airing (no guessing with silence detection).
   - If 2+ takes exist: Aligns candidate pairs using FFT cross-correlation to find the exact
     sample-accurate divergence cliff (where ads/DJ banter end and studio music begins/ends).
   - If correlation confidence >= threshold (default 0.85): Losslessly extracts (-c copy)
     the pristine track into JUKEBOX_DIR/<Artist>/<Title>.m4a with companion .txt metadata.
3. Completely non-destructive: Never modifies or deletes raw captures.
4. Skips tracks already verified in JUKEBOX_DIR.
"""

import os
import sys
import argparse
import subprocess
import re
import time
import numpy as np
from scipy import signal

RECORDINGS_DIR = "/home/benjamin/nrsc5/recordings/raw"
JUKEBOX_DIR = "/home/benjamin/nrsc5/jukebox"
SAMPLE_RATE = 11025  # Low-res PCM mono for fast, lightweight FFT correlation
WINDOW_MS = 60       # Window size for correlation sliding search
STEP_MS = 20         # Step size for sliding search
CONFIDENCE_THRESHOLD = 0.85


def run_cmd(cmd):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr


def get_audio_duration(file_path):
    cmd = [
        "ffprobe", "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        file_path
    ]
    code, out, _ = run_cmd(cmd)
    try:
        return float(out.strip())
    except (ValueError, TypeError):
        return 0.0


def get_file_comment(file_path):
    cmd = [
        "ffprobe", "-v", "error",
        "-show_entries", "format_tags=comment",
        "-of", "default=noprint_wrappers=1:nokey=1",
        file_path
    ]
    code, out, _ = run_cmd(cmd)
    return out.strip() if code == 0 else ""


def parse_companion_txt(txt_path):
    data = {}
    if not os.path.isfile(txt_path):
        return data
    try:
        with open(txt_path, "r", encoding="utf-8") as f:
            for line in f:
                if ":" in line:
                    k, v = line.split(":", 1)
                    k = k.strip()
                    v = v.strip()
                    try:
                        if "." in v:
                            data[k] = float(v)
                        else:
                            data[k] = int(v)
                    except ValueError:
                        data[k] = v
    except Exception:
        pass
    return data


def extract_pcm(file_path, sample_rate=SAMPLE_RATE, max_duration=None):
    cmd = [
        "ffmpeg", "-v", "error",
        "-i", file_path,
        "-vn",
        "-f", "s16le",
        "-ac", "1",
        "-ar", str(sample_rate)
    ]
    if max_duration:
        cmd.extend(["-t", str(max_duration)])
    cmd.append("-")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    raw_data, _ = proc.communicate()
    if not raw_data:
        return np.array([], dtype=np.float32)

    samples = np.frombuffer(raw_data, dtype=np.int16).astype(np.float32)
    max_val = np.max(np.abs(samples))
    if max_val > 0:
        samples /= max_val
    return samples


def compute_energy_envelope(pcm, sr=SAMPLE_RATE, fps=100):
    """Computes a smoothed root-mean-square (RMS) energy envelope at `fps` frames per second."""
    hop = int(sr / fps)
    win = int(sr * 0.05)  # 50ms smoothing window
    if len(pcm) < win:
        return np.array([], dtype=np.float32), np.array([], dtype=np.float32)

    sq = pcm ** 2
    kernel = np.ones(win, dtype=np.float32) / win
    smoothed = np.convolve(sq, kernel, mode="same")
    rms = np.sqrt(np.maximum(0.0, smoothed))[::hop]
    rms_norm = rms - np.mean(rms)
    norm = np.linalg.norm(rms_norm)
    if norm > 0:
        rms_norm = rms_norm / norm
    return rms, rms_norm


def multi_capture_alignment(file1, file2, verbose=False):
    pcm1 = extract_pcm(file1)
    pcm2 = extract_pcm(file2)

    dur1 = len(pcm1) / SAMPLE_RATE
    dur2 = len(pcm2) / SAMPLE_RATE

    if dur1 < 45.0 or dur2 < 45.0:
        if verbose:
            print(f"    [ALIGN DEBUG] Durations too short ({dur1:.1f}s, {dur2:.1f}s)")
        return None

    fps = 100  # 10ms resolution
    raw_env1, env1 = compute_energy_envelope(pcm1, SAMPLE_RATE, fps=fps)
    raw_env2, env2 = compute_energy_envelope(pcm2, SAMPLE_RATE, fps=fps)

    if len(env1) < fps * 40 or len(env2) < fps * 40:
        return None

    # Step 1: Global Time Alignment using middle 30s anchor of envelope
    anchor_start = int(30.0 * fps)
    anchor_len = int(30.0 * fps)
    anchor = env1[anchor_start : anchor_start + anchor_len]
    a_norm = np.linalg.norm(anchor)
    if a_norm == 0:
        return None
    anchor = anchor / a_norm

    # Cross-correlate anchor across the entire env2
    corr = signal.fftconvolve(env2, anchor[::-1], mode="valid")
    peak_idx = int(np.argmax(corr))
    peak_val = corr[peak_idx]

    matched_seg = env2[peak_idx : peak_idx + len(anchor)]
    seg_norm = np.linalg.norm(matched_seg)
    norm_corr = float(peak_val / seg_norm) if seg_norm > 0 else 0.0

    offset_frames = peak_idx - anchor_start
    offset_sec = offset_frames / float(fps)

    def local_correlation(slice1, slice2):
        s1 = slice1 - np.mean(slice1)
        s2 = slice2 - np.mean(slice2)
        n1 = np.linalg.norm(s1)
        n2 = np.linalg.norm(s2)
        if n1 > 1e-4 and n2 > 1e-4:
            return float(np.dot(s1, s2) / (n1 * n2))
        return 1.0 if abs(np.mean(slice1) - np.mean(slice2)) < 0.05 else 0.0

    eval_win = int(0.6 * fps)  # 600ms window
    step = 4                   # 40ms step

    if verbose:
        print(f"    [ALIGN DEBUG] Envelope peak_val={peak_val:.2f}, norm_corr={norm_corr:.4f}, offset={offset_sec:.3f}s")

    if norm_corr < 0.65:
        if verbose:
            print(f"    [ALIGN DEBUG] norm_corr {norm_corr:.4f} < 0.65, insufficient correlation")
        return None

    # Step 2: Search for intro boundary in [0s, 25s] window
    intro_scan_end = min(int(25.0 * fps), int(dur1 * 0.3 * fps))
    trim_start = 0.0
    for f in range(eval_win, intro_scan_end, step):
        f2 = f + offset_frames
        if f2 >= 0 and (f2 + eval_win) <= len(raw_env2):
            r = local_correlation(raw_env1[f : f + eval_win], raw_env2[f2 : f2 + eval_win])
            if r > 0.70:
                trim_start = max(0.0, (f - step) / float(fps))
                break

    # Step 3: Search for outro boundary in [dur - 35s, dur] window
    outro_scan_start = max(int((dur1 - 35.0) * fps), int(dur1 * 0.7 * fps))
    trim_end = dur1
    for f in range(outro_scan_start, len(raw_env1) - eval_win, step):
        f2 = f + offset_frames
        if f2 >= 0 and (f2 + eval_win) <= len(raw_env2):
            r = local_correlation(raw_env1[f : f + eval_win], raw_env2[f2 : f2 + eval_win])
            if r < 0.40:
                f_next = f + eval_win
                f2_next = f_next + offset_frames
                if f2_next + eval_win <= len(raw_env2):
                    r_next = local_correlation(raw_env1[f_next : f_next + eval_win], raw_env2[f2_next : f2_next + eval_win])
                    if r_next < 0.40:
                        trim_end = f / float(fps)
                        break

    confidence = min(0.99, norm_corr)
    trimmed_len = trim_end - trim_start
    if trimmed_len < 40.0:
        confidence = 0.40

    if verbose:
        print(f"    [ALIGN DEBUG] Divergence points: start={trim_start:.2f}s, end={trim_end:.2f}s, conf={confidence:.2f}")

    return {
        "method": "cross_correlation_divergence",
        "primary_file": file1,
        "compared_file": file2,
        "trim_start": trim_start,
        "trim_end": trim_end,
        "confidence": confidence,
        "offset_sec": offset_sec,
        "norm_corr": norm_corr,
        "duration": dur1
    }


def find_raw_song_groups(base_dir):
    """Finds all raw takes in base_dir grouped by (artist, clean_title)."""
    groups = {}
    if not os.path.isdir(base_dir):
        return groups

    for root, dirs, files in os.walk(base_dir):
        for f in sorted(files):
            if f.lower().endswith(".m4a") and not f.startswith("."):
                full_path = os.path.join(root, f)
                try:
                    if os.path.getsize(full_path) <= 1024:
                        continue
                except OSError:
                    continue

                rel_path = os.path.relpath(full_path, base_dir)
                parts = rel_path.split(os.sep)
                artist = parts[0] if len(parts) >= 2 else "Unknown"

                raw_name = os.path.splitext(parts[-1])[0]
                clean_title = re.sub(r"_\d{3}$", "", raw_name)
                key = (artist, clean_title)
                if key not in groups:
                    groups[key] = []
                groups[key].append(full_path)

    for key in groups:
        groups[key].sort()

    return groups


def execute_lossless_trim(input_path, trim_start, trim_end, out_path, trim_comment=None):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    tmp_out = out_path + ".trimmed.tmp.m4a"
    cmd = [
        "ffmpeg", "-y", "-v", "error",
        "-ss", f"{trim_start:.3f}",
        "-to", f"{trim_end:.3f}",
        "-i", input_path,
        "-c", "copy",
        "-map", "0",
    ]
    if trim_comment:
        cmd.extend(["-metadata", f"comment={trim_comment}"])
    cmd.append(tmp_out)

    code, _, _ = run_cmd(cmd)
    if code != 0 or not os.path.isfile(tmp_out) or os.path.getsize(tmp_out) <= 1024:
        if os.path.isfile(tmp_out):
            try:
                os.unlink(tmp_out)
            except OSError:
                pass
        return False

    os.replace(tmp_out, out_path)
    return True


def write_jukebox_companion_txt(txt_path, trim_info, artist, title):
    lines = [
        f"title: {title}",
        f"artist: {artist}",
        "trim_status: trimmed",
        f"trim_start_sec: {trim_info['trim_start']:.3f}",
        f"trim_end_sec: {trim_info['trim_end']:.3f}",
        f"trimmed_duration_sec: {(trim_info['trim_end'] - trim_info['trim_start']):.3f}",
        f"trim_method: {trim_info['method']}",
        f"trim_confidence: {trim_info['confidence']:.2f}",
        f"source_take: {os.path.basename(trim_info['primary_file'])}",
        f"compared_take: {os.path.basename(trim_info['compared_file'])}",
        f"norm_corr: {trim_info.get('norm_corr', 0.0):.4f}"
    ]
    with open(txt_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def analyze_song(artist, title, raw_paths, jukebox_dir, threshold=CONFIDENCE_THRESHOLD, verbose=False):
    """
    Evaluates a song group according to the 2-Airing Rule.
    Returns decision dict.
    """
    canonical_m4a = os.path.join(jukebox_dir, artist, f"{title}.m4a")
    canonical_txt = os.path.join(jukebox_dir, artist, f"{title}.txt")

    # 1. Already verified in Jukebox?
    if os.path.isfile(canonical_m4a):
        txt_data = parse_companion_txt(canonical_txt)
        dur = txt_data.get("trimmed_duration_sec", get_audio_duration(canonical_m4a))
        conf = txt_data.get("trim_confidence", 1.0)
        method = txt_data.get("trim_method", "cross_correlation_divergence")
        return {
            "status": "already_in_jukebox",
            "canonical_m4a": canonical_m4a,
            "canonical_txt": canonical_txt,
            "duration": dur,
            "confidence": conf,
            "method": method,
            "raw_takes_count": len(raw_paths)
        }

    # 2. Strict 2-Airing Rule: < 2 takes cannot be verified
    if len(raw_paths) < 2:
        dur = get_audio_duration(raw_paths[0]) if raw_paths else 0.0
        return {
            "status": "pending_second_airing",
            "duration": dur,
            "confidence": 0.0,
            "raw_takes_count": len(raw_paths),
            "primary_file": raw_paths[0] if raw_paths else None
        }

    # 3. 2+ takes exist: Pairwise cross-correlation divergence
    best_align = None
    for i in range(len(raw_paths)):
        for j in range(len(raw_paths)):
            if i == j:
                continue
            if verbose:
                print(f"    [ANALYZE] Correlating: {os.path.basename(raw_paths[i])} vs {os.path.basename(raw_paths[j])}")
            align_res = multi_capture_alignment(raw_paths[i], raw_paths[j], verbose=verbose)
            if align_res and align_res["confidence"] >= threshold:
                if best_align is None or align_res["confidence"] > best_align["confidence"]:
                    best_align = align_res
                    if best_align["confidence"] >= 0.95:
                        break
        if best_align and best_align["confidence"] >= 0.95:
            break

    if best_align and best_align["confidence"] >= threshold:
        best_align["status"] = "ready_to_mint"
        best_align["canonical_m4a"] = canonical_m4a
        best_align["canonical_txt"] = canonical_txt
        best_align["raw_takes_count"] = len(raw_paths)
        return best_align

    # 4. Takes exist but correlation failed to meet quality threshold
    return {
        "status": "correlation_ambiguous",
        "duration": get_audio_duration(raw_paths[0]),
        "confidence": best_align["confidence"] if best_align else 0.0,
        "raw_takes_count": len(raw_paths),
        "primary_file": raw_paths[0]
    }


def process_library(recordings_dir, jukebox_dir, is_apply=False, threshold=CONFIDENCE_THRESHOLD,
                    song_filter=None, artist_filter=None, title_filter=None, verbose=False, quiet=False):
    groups = find_raw_song_groups(recordings_dir)
    total_groups = len(groups)

    if not quiet:
        print(f"[JUKEBOX BUILDER] Scanning {total_groups} unique songs in {recordings_dir}...")
        print(f"[JUKEBOX BUILDER] Output jukebox directory: {jukebox_dir}")
        print(f"[JUKEBOX BUILDER] Quality mode: Strict 2-Airing Cross-Correlation (threshold >= {threshold:.2f})\n")

    already_jukebox_count = 0
    mintable_count = 0
    minted_count = 0
    waiting_count = 0
    ambiguous_count = 0

    for (artist, title), paths in sorted(groups.items()):
        if artist_filter and artist_filter.lower() != artist.lower():
            continue
        if title_filter and title_filter.lower() != title.lower():
            continue
        if song_filter and (song_filter.lower() not in title.lower() and song_filter.lower() not in artist.lower()):
            continue

        res = analyze_song(
            artist, title, paths, jukebox_dir,
            threshold=threshold, verbose=verbose
        )
        status = res["status"]

        if status == "already_in_jukebox":
            already_jukebox_count += 1
            if not quiet:
                dur = res.get("duration", 0.0)
                print(f"🎵 [JUKEBOX]   \"{title}\" by {artist}: verified in jukebox ({dur:.1f}s, {res['raw_takes_count']} raw takes archived)")

        elif status == "ready_to_mint":
            mintable_count += 1
            t_start = res["trim_start"]
            t_end = res["trim_end"]
            orig_dur = res["duration"]
            new_dur = max(0.0, t_end - t_start)
            conf = res["confidence"]
            src_take = os.path.basename(res["primary_file"])
            ref_take = os.path.basename(res["compared_file"])

            if not quiet or is_apply:
                print(f"✨ [MINTABLE]  \"{title}\" by {artist} ({res['raw_takes_count']} takes): "
                      f"trim [{t_start:.2f}s -> {t_end:.2f}s] (dur: {new_dur:.1f}s / raw: {orig_dur:.1f}s, "
                      f"conf: {conf:.2f}, source: {src_take} vs {ref_take})")

            if is_apply:
                comment_tag = f"trimmed=true;start={t_start:.3f};end={t_end:.3f};method=cross_correlation_divergence;conf={conf:.2f}"
                ok = execute_lossless_trim(
                    res["primary_file"], t_start, t_end,
                    out_path=res["canonical_m4a"], trim_comment=comment_tag
                )
                if ok:
                    write_jukebox_companion_txt(res["canonical_txt"], res, artist, title)
                    minted_count += 1
                    print(f"    -> Successfully minted to {res['canonical_m4a']}")
                else:
                    print(f"    -> ERROR: ffmpeg remux failed for {res['canonical_m4a']}")

        elif status == "pending_second_airing":
            waiting_count += 1
            if not quiet:
                print(f"⏳ [WAITING]   \"{title}\" by {artist}: 1 take recorded, waiting for 2nd airing")

        elif status == "correlation_ambiguous":
            ambiguous_count += 1
            if not quiet:
                conf = res.get("confidence", 0.0)
                print(f"⚠️  [AMBIGUOUS] \"{title}\" by {artist}: {res['raw_takes_count']} takes, but correlation did not meet threshold ({conf:.2f} < {threshold:.2f})")

    if not quiet:
        print("\n" + "=" * 65)
        print(f"Jukebox Builder Summary ({total_groups} unique songs inspected):")
        print(f"  Already Verified in Jukebox:   {already_jukebox_count}")
        if is_apply:
            print(f"  Successfully Minted Today:     {minted_count}")
        else:
            print(f"  Eligible / Ready to Mint:      {mintable_count} (run with --apply to mint)")
        print(f"  Waiting for 2nd Airing:        {waiting_count}")
        print(f"  Ambiguous (Need 3rd Airing):   {ambiguous_count}")
        print("=" * 65)

    return minted_count


def main():
    parser = argparse.ArgumentParser(description="NRSC5 Jukebox Builder (2-Airing Rule)")
    parser.add_argument("--recordings-dir", "--dir", default=RECORDINGS_DIR, help="Path to raw recordings directory")
    parser.add_argument("--jukebox-dir", default=JUKEBOX_DIR, help="Path to curated jukebox directory")
    parser.add_argument("--apply", action="store_true", help="Mint eligible high-confidence songs into the jukebox directory")
    parser.add_argument("--dry-run", action="store_true", help="Analyze and print boundary decisions without modifying disk")
    parser.add_argument("--watch", type=int, nargs="?", const=30, default=None, metavar="SECONDS",
                        help="Run continuously in background, polling for new airings every SECONDS (default: 30s)")
    parser.add_argument("--threshold", type=float, default=CONFIDENCE_THRESHOLD, help="Confidence threshold for correlation divergence (default: 0.85)")
    parser.add_argument("--artist", help="Target a specific artist name")
    parser.add_argument("--title", help="Target a specific song title")
    parser.add_argument("--song", help="Filter analysis to a specific song title or artist substring")
    parser.add_argument("--verbose", action="store_true", help="Print verbose correlation debug information")
    args = parser.parse_args()

    if args.watch is not None:
        interval = max(5, args.watch)
        print(f"[JUKEBOX BUILDER] Watch mode active: polling {args.recordings_dir} every {interval}s.")
        print("Press Ctrl+C to stop.\n")
        try:
            while True:
                process_library(
                    args.recordings_dir, args.jukebox_dir, is_apply=True,
                    threshold=args.threshold, song_filter=args.song,
                    artist_filter=args.artist, title_filter=args.title,
                    verbose=args.verbose, quiet=True
                )
                time.sleep(interval)
        except KeyboardInterrupt:
            print("\n[JUKEBOX BUILDER] Watch mode stopped.")
        return

    # Default one-shot run
    is_apply = args.apply
    process_library(
        args.recordings_dir, args.jukebox_dir, is_apply=is_apply,
        threshold=args.threshold, song_filter=args.song,
        artist_filter=args.artist, title_filter=args.title,
        verbose=args.verbose, quiet=False
    )


if __name__ == "__main__":
    main()
