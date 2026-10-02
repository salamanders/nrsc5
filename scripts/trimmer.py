#!/usr/bin/env python3
"""
trimmer.py - Intelligent Song Boundary Trimmer for NRSC5 Broadcast Recordings

Detects exact song boundaries and losslessly trims recordings using:
1. Multi-Capture Cross-Correlation Divergence (Tier 1 - High Confidence >= 0.85)
   Aligns 2+ airings of the same song with sample accuracy via FFT cross-correlation,
   then detects the exact divergence cliff where non-song audio begins/ends.
2. Targeted Silence & Transient Onset Detection (Tier 2 - Single-Capture Fallback)
   Scans the known 15s pre/post-roll windows for inter-track silence gaps.

Trims losslessly (-c copy) strictly when confidence >= 0.85. Low-confidence tracks
are preserved with raw overlap intact so future airings can resolve boundaries.
"""

import os
import sys
import argparse
import subprocess
import re
import shutil
import numpy as np
from scipy import signal

RECORDINGS_DIR = "/home/benjamin/nrsc5/recordings"
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


def find_silence_boundaries(file_path, offset_front=15.0, offset_end=15.0, duration=None):
    if duration is None:
        duration = get_audio_duration(file_path)
    if duration <= 0:
        return None

    cmd = [
        "ffmpeg", "-hide_banner",
        "-i", file_path,
        "-af", "silencedetect=noise=-36dB:d=0.2",
        "-f", "null", "-"
    ]
    code, _, stderr = run_cmd(cmd)

    silences = []
    # Parse lines like:
    # [silencedetect @ ...] silence_start: 13.318
    # [silencedetect @ ...] silence_end: 13.853 | silence_duration: 0.535
    cur_start = None
    for line in stderr.splitlines():
        start_match = re.search(r"silence_start:\s*([\d\.]+)", line)
        if start_match:
            cur_start = float(start_match.group(1))
        end_match = re.search(r"silence_end:\s*([\d\.]+)\s*\|\s*silence_duration:\s*([\d\.]+)", line)
        if end_match and cur_start is not None:
            silences.append({
                "start": cur_start,
                "end": float(end_match.group(1)),
                "duration": float(end_match.group(2))
            })
            cur_start = None

    intro_window_max = offset_front + 4.0
    best_intro = None
    for s in silences:
        if s["end"] <= intro_window_max and s["end"] >= 0.5:
            if best_intro is None or s["duration"] > best_intro["duration"]:
                best_intro = s

    outro_window_min = max(0.0, duration - offset_end - 6.0)
    best_outro = None
    for s in silences:
        if s["start"] >= outro_window_min:
            if best_outro is None or s["duration"] > best_outro["duration"]:
                best_outro = s

    trim_start = best_intro["end"] if best_intro else offset_front
    trim_end = best_outro["start"] if best_outro else (duration - offset_end)

    confidence = 0.50
    if best_intro and best_outro:
        confidence = 0.88 if (best_intro["duration"] >= 0.2 and best_outro["duration"] >= 0.2) else 0.78
    elif best_intro or best_outro:
        confidence = 0.70

    return {
        "method": "silence_detection",
        "trim_start": trim_start,
        "trim_end": trim_end,
        "confidence": confidence,
        "intro_silence": best_intro,
        "outro_silence": best_outro,
        "duration": duration
    }


def compute_energy_envelope(pcm, sr=SAMPLE_RATE, fps=100):
    """Computes a smoothed root-mean-square (RMS) energy envelope at `fps` frames per second."""
    hop = int(sr / fps)
    win = int(sr * 0.05)  # 50ms smoothing window
    if len(pcm) < win:
        return np.array([], dtype=np.float32)

    # Squared magnitude
    sq = pcm ** 2
    # Moving average
    kernel = np.ones(win, dtype=np.float32) / win
    smoothed = np.convolve(sq, kernel, mode="same")
    rms = np.sqrt(np.maximum(0.0, smoothed))[::hop]
    # Subtract mean and normalize
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

    # Offset in seconds: time in file2 minus time in file1
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
        print("    [ALIGN DEBUG] Timeline probe of correlation across song:")
        probe_pts = []
        for s in range(5, int(dur1) - 5, 15):
            f = int(s * fps)
            f2 = f + offset_frames
            if f2 >= 0 and f2 + eval_win <= len(raw_env2) and f + eval_win <= len(raw_env1):
                rc = local_correlation(raw_env1[f:f+eval_win], raw_env2[f2:f2+eval_win])
                probe_pts.append(f"{s}s:{rc:.2f}")
        print("      " + " ".join(probe_pts))

    if norm_corr < 0.65:
        if verbose:
            print(f"    [ALIGN DEBUG] norm_corr {norm_corr:.4f} < 0.65, falling back")
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
                # Confirm with next window
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
        print(f"    [ALIGN DEBUG] Targeted divergence: start={trim_start:.2f}s, end={trim_end:.2f}s, conf={confidence:.2f}")

    return {
        "method": "cross_correlation_divergence",
        "trim_start": trim_start,
        "trim_end": trim_end,
        "confidence": confidence,
        "offset_sec": offset_sec,
        "norm_corr": norm_corr,
        "duration": dur1
    }


def find_song_groups(base_dir):
    """Finds all artists and groups songs by title."""
    groups = {}
    if not os.path.isdir(base_dir):
        return groups

    for root, dirs, files in os.walk(base_dir):
        for f in files:
            if f.lower().endswith(".m4a") and not f.startswith("."):
                full_path = os.path.join(root, f)
                rel_path = os.path.relpath(full_path, base_dir)
                parts = rel_path.split(os.sep)
                artist = parts[0] if len(parts) >= 2 else "Unknown"

                # Normalize title by stripping trailing sequence suffixes like _001, _002
                raw_name = os.path.splitext(f)[0]
                norm_title = re.sub(r"_\d{3}$", "", raw_name)
                key = (artist, norm_title)
                if key not in groups:
                    groups[key] = []
                groups[key].append(full_path)

    # Sort each group so takes with companion .txt files and canonical names come first
    for key in groups:
        groups[key].sort(key=lambda p: (not os.path.isfile(os.path.splitext(p)[0] + ".txt"), len(os.path.basename(p)), p))

    return groups


def migrate_raw_files(base_dir, dry_run=False):
    """
    Renames any legacy raw broadcast captures that lack a _### suffix
    to have a suffix starting at _100.m4a (and _100.txt).
    Preserves any file that is already verified trimmed.
    """
    if not os.path.isdir(base_dir):
        print(f"[MIGRATE] Directory not found: {base_dir}")
        return 0

    migrated_count = 0
    preserved_count = 0

    for root, dirs, files in os.walk(base_dir):
        for f in sorted(files):
            if not f.lower().endswith(".m4a") or f.startswith("."):
                continue

            full_path = os.path.join(root, f)
            base_name, ext = os.path.splitext(f)

            # If it already has a 3-digit suffix (e.g. _001, _002), it's already identified as raw
            if re.search(r"_\d{3}$", base_name):
                continue

            # It lacks a suffix. Check if it's already trimmed!
            txt_path = os.path.join(root, base_name + ".txt")
            txt_data = parse_companion_txt(txt_path)
            comment = get_file_comment(full_path)

            if txt_data.get("trim_status") in ("trimmed", "already_trimmed") or "trimmed=true" in comment:
                print(f"[MIGRATE] Preserving verified trimmed master: {os.path.relpath(full_path, base_dir)}")
                preserved_count += 1
                continue

            # This is an existing raw file lacking a suffix! Find available _100+ slot
            target_idx = 100
            new_m4a = ""
            while True:
                candidate = os.path.join(root, f"{base_name}_{target_idx:03d}.m4a")
                if not os.path.exists(candidate):
                    new_m4a = candidate
                    break
                target_idx += 1

            new_txt = os.path.join(root, f"{base_name}_{target_idx:03d}.txt")
            rel_old = os.path.relpath(full_path, base_dir)
            rel_new = os.path.relpath(new_m4a, base_dir)

            if dry_run:
                print(f"[MIGRATE DRY-RUN] Would rename: {rel_old} -> {os.path.basename(new_m4a)}")
            else:
                os.rename(full_path, new_m4a)
                if os.path.exists(txt_path):
                    os.rename(txt_path, new_txt)
                print(f"[MIGRATE] Renamed: {rel_old} -> {os.path.basename(new_m4a)}")

            migrated_count += 1

    print(f"\n[MIGRATE] Complete. Renamed {migrated_count} raw files to _100+ format. Preserved {preserved_count} trimmed masters.")
    return migrated_count


def execute_lossless_trim(input_path, trim_start, trim_end, out_path=None, trim_comment=None):
    if out_path is None:
        out_path = input_path

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

    code, _, err = run_cmd(cmd)
    if code != 0 or not os.path.isfile(tmp_out) or os.path.getsize(tmp_out) <= 1024:
        if os.path.isfile(tmp_out):
            os.unlink(tmp_out)
        return False

    os.replace(tmp_out, out_path)
    return True


def update_companion_txt(txt_path, trim_info):
    lines = []
    if os.path.isfile(txt_path):
        with open(txt_path, "r", encoding="utf-8") as f:
            for line in f:
                if not any(line.startswith(prefix) for prefix in [
                    "trim_status:", "trim_start_sec:", "trim_end_sec:",
                    "trimmed_duration_sec:", "trim_method:", "trim_confidence:"
                ]):
                    lines.append(line.rstrip())

    lines.append(f"trim_status: {trim_info['status']}")
    lines.append(f"trim_start_sec: {trim_info['trim_start']:.3f}")
    lines.append(f"trim_end_sec: {trim_info['trim_end']:.3f}")
    lines.append(f"trimmed_duration_sec: {(trim_info['trim_end'] - trim_info['trim_start']):.3f}")
    lines.append(f"trim_method: {trim_info['method']}")
    lines.append(f"trim_confidence: {trim_info['confidence']:.2f}")

    with open(txt_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def analyze_song(song_paths, verbose=False):
    """
    Analyzes one song group (single capture or multiple takes).
    Returns dict with decision and best trim points.
    """
    primary = song_paths[0]
    txt_path = os.path.splitext(primary)[0] + ".txt"
    txt_data = parse_companion_txt(txt_path)
    offset_front = txt_data.get("offset_front_sec", 15.0)
    offset_end = txt_data.get("offset_end_sec", 15.0)
    duration = txt_data.get("total_duration_sec", get_audio_duration(primary))

    # Check if already trimmed
    comment = get_file_comment(primary)
    if txt_data.get("trim_status") in ("trimmed", "already_trimmed") or "trimmed=true" in comment:
        return {
            "method": "already_trimmed",
            "trim_start": 0.0,
            "trim_end": duration,
            "confidence": 1.0,
            "duration": duration,
            "primary_file": primary,
            "all_files": song_paths,
            "status": "already_trimmed"
        }

    result = None

    # Tier 1: If 2+ takes exist, try multi-capture divergence across candidate pairs
    if len(song_paths) >= 2:
        best_align = None
        for i in range(len(song_paths)):
            for j in range(i + 1, len(song_paths)):
                if verbose:
                    print(f"    [ANALYZE] Comparing take {i+1} ({os.path.basename(song_paths[i])}) vs take {j+1} ({os.path.basename(song_paths[j])})")
                align_res = multi_capture_alignment(song_paths[i], song_paths[j], verbose=verbose)
                if align_res and align_res["confidence"] >= CONFIDENCE_THRESHOLD:
                    best_align = align_res
                    primary = song_paths[i]
                    txt_path = os.path.splitext(primary)[0] + ".txt"
                    txt_data = parse_companion_txt(txt_path)
                    break
            if best_align:
                break
        if best_align:
            result = best_align

    # Tier 2: Single-capture silence detection fallback
    if result is None:
        silence_res = find_silence_boundaries(primary, offset_front, offset_end, duration)
        if silence_res:
            result = silence_res

    if result is None:
        result = {
            "method": "none",
            "trim_start": 0.0,
            "trim_end": duration,
            "confidence": 0.0,
            "duration": duration
        }

    result["primary_file"] = primary
    result["all_files"] = song_paths
    result["status"] = "trimmed" if result["confidence"] >= CONFIDENCE_THRESHOLD else "pending_confirmation"
    return result


def main():
    parser = argparse.ArgumentParser(description="NRSC5 Song Boundary Trimmer")
    parser.add_argument("--dir", default=RECORDINGS_DIR, help="Path to recordings directory")
    parser.add_argument("--dry-run", action="store_true", help="Analyze and print boundary decisions without modifying files")
    parser.add_argument("--apply", action="store_true", help="Apply destructive lossless trimming on high-confidence songs")
    parser.add_argument("--dedup", action="store_true", help="Delete duplicate takes after successful high-confidence trimming")
    parser.add_argument("--migrate", action="store_true", help="Rename legacy untrimmed raw files to _100+ format")
    parser.add_argument("--analyze", help="Analyze a specific song file or pattern")
    args = parser.parse_args()

    if not args.dry_run and not args.apply and not args.analyze and not args.migrate:
        parser.print_help()
        sys.exit(0)

    if args.migrate:
        migrate_raw_files(args.dir, dry_run=args.dry_run)
        if not args.apply and not args.analyze:
            sys.exit(0)

    groups = find_song_groups(args.dir)
    total_groups = len(groups)
    print(f"[TRIMMER] Scanning {total_groups} unique songs in {args.dir}...")

    already_trimmed_count = 0
    high_conf_count = 0
    low_conf_count = 0
    trimmed_count = 0

    for (artist, title), paths in sorted(groups.items()):
        if args.analyze and not any(args.analyze.lower() in p.lower() for p in paths):
            continue

        res = analyze_song(paths, verbose=(args.analyze is not None))
        conf = res["confidence"]
        status = res["status"]
        method = res["method"]
        t_start = res["trim_start"]
        t_end = res["trim_end"]
        orig_dur = res.get("duration", 0.0)
        new_dur = max(0.0, t_end - t_start)

        if status == "already_trimmed":
            already_trimmed_count += 1
            print(f"⏭️  [TRIMMED]   \"{title}\" by {artist}: already trimmed ({orig_dur:.1f}s)")
            if args.apply and args.dedup and len(paths) > 1:
                for dup in paths:
                    if dup == res["primary_file"]:
                        continue
                    try:
                        if os.path.exists(dup):
                            os.unlink(dup)
                        dup_txt = os.path.splitext(dup)[0] + ".txt"
                        if os.path.isfile(dup_txt):
                            os.unlink(dup_txt)
                        print(f"    -> Cleaned post-trim duplicate take: {os.path.basename(dup)}")
                    except OSError:
                        pass
            continue

        is_high = conf >= CONFIDENCE_THRESHOLD
        if is_high:
            high_conf_count += 1
        else:
            low_conf_count += 1

        prefix = "✅ [HIGH CONF]" if is_high else "⏳ [PENDING]  "
        print(f"{prefix} \"{title}\" by {artist} ({len(paths)} take{'s' if len(paths)>1 else ''}): "
              f"trim [{t_start:.2f}s -> {t_end:.2f}s] (dur: {new_dur:.1f}s / orig: {orig_dur:.1f}s, "
              f"conf: {conf:.2f}, via {method})")

        if args.apply:
            if is_high:
                canonical_m4a = os.path.join(os.path.dirname(res["primary_file"]), f"{title}.m4a")
                canonical_txt = os.path.join(os.path.dirname(res["primary_file"]), f"{title}.txt")
                comment_tag = f"trimmed=true;start={t_start:.3f};end={t_end:.3f};method={method};conf={conf:.2f}"
                ok = execute_lossless_trim(res["primary_file"], t_start, t_end, out_path=canonical_m4a, trim_comment=comment_tag)
                if ok:
                    trimmed_count += 1
                    update_companion_txt(canonical_txt, res)
                    print(f"    -> Losslessly trimmed to {os.path.basename(canonical_m4a)}")

                    if args.dedup:
                        for dup in paths:
                            if dup == canonical_m4a:
                                continue
                            try:
                                if os.path.exists(dup):
                                    os.unlink(dup)
                                dup_txt = os.path.splitext(dup)[0] + ".txt"
                                if os.path.exists(dup_txt):
                                    os.unlink(dup_txt)
                                print(f"    -> Cleaned raw take: {os.path.basename(dup)}")
                            except OSError:
                                pass
            else:
                txt_path = os.path.splitext(res["primary_file"])[0] + ".txt"
                update_companion_txt(txt_path, res)

    print("\n" + "=" * 60)
    print(f"Summary: {total_groups} songs inspected.")
    print(f"  Already Trimmed:                      {already_trimmed_count}")
    print(f"  High Confidence (Eligible for trim):  {high_conf_count}")
    print(f"  Pending / Low Confidence (Preserved): {low_conf_count}")
    if args.apply:
        print(f"  Successfully Trimmed In-Place:        {trimmed_count}")
    print("=" * 60)


if __name__ == "__main__":
    main()
