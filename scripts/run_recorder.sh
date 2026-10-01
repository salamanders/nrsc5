#!/bin/bash
set -u

NRSC5_BIN="/home/benjamin/nrsc5/build/src/nrsc5"
RECORDINGS_DIR="${RECORDINGS_DIR:-/home/benjamin/nrsc5/recordings}"
CONF_FILE="${CONF_FILE:-/home/benjamin/nrsc5/stations.conf}"
PREROLL="${NRSC5_PREROLL:-2.5}"
STATUS_FILE="${RECORDINGS_DIR}/.current_station"
PROBE_TIMEOUT_SEC=6
PROBE_AUDIO_MIN_BYTES=1024

echo "[recorder-launcher] Starting radio probe sequence..."

if [ ! -x "$NRSC5_BIN" ]; then
    echo "[recorder-launcher] ERROR: nrsc5 binary not found or not executable at $NRSC5_BIN" >&2
    exit 1
fi

if [ ! -f "$CONF_FILE" ]; then
    echo "[recorder-launcher] ERROR: Configuration file not found at $CONF_FILE" >&2
    exit 1
fi

mkdir -p "$RECORDINGS_DIR"

TMP_PROBE="/tmp/.nrsc5_probe_$$_${RANDOM}.hdc"
cleanup_probe() {
    if [ -n "${PROBE_PID:-}" ]; then
        kill "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
    rm -f "$TMP_PROBE"
}
trap cleanup_probe EXIT INT TERM

# Read configuration file, line by line
selected_freq=""
selected_prog=""
selected_desc=""

while IFS= read -r line || [ -n "$line" ]; do
    # Trim leading/trailing whitespace
    line="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"

    # Skip comments and empty lines
    [[ -z "$line" || "$line" =~ ^# ]] && continue

    # Parse frequency, program, and optional description
    # Format: <freq> <program> [# <desc>]
    freq="$(echo "$line" | awk '{print $1}')"
    prog="$(echo "$line" | awk '{print $2}')"
    desc="$(echo "$line" | sed -n 's/^[^#]*#[[:space:]]*//p')"
    [ -z "$desc" ] && desc="Station ${freq} HD$((prog + 1))"

    echo "[recorder-launcher] Probing station: $freq HD$((prog + 1)) ($desc)..."
    rm -f "$TMP_PROBE"

    # Start probe in background
    "$NRSC5_BIN" -q --dump-hdc "$TMP_PROBE" "$freq" "$prog" >/dev/null 2>&1 &
    PROBE_PID=$!

    # Poll for audio arrival up to PROBE_TIMEOUT_SEC
    # Checking every 0.2 seconds (5 checks per sec)
    max_checks=$(( PROBE_TIMEOUT_SEC * 5 ))
    audio_found=0

    for (( i=1; i<=max_checks; i++ )); do
        sleep 0.2

        # Check if process died early
        if ! kill -0 "$PROBE_PID" 2>/dev/null; then
            break
        fi

        if [ -f "$TMP_PROBE" ]; then
            fsize=$(stat -c %s "$TMP_PROBE" 2>/dev/null || echo 0)
            if [ "$fsize" -ge "$PROBE_AUDIO_MIN_BYTES" ]; then
                elapsed=$(awk "BEGIN { printf \"%.1f\", $i * 0.2 }")
                echo "[recorder-launcher] -> LOCKED! Audio packets received in ${elapsed}s ($fsize bytes). Station accepted."
                audio_found=1
                break
            fi
        fi
    done

    # Stop probe process
    kill "$PROBE_PID" 2>/dev/null || true
    wait "$PROBE_PID" 2>/dev/null || true
    unset PROBE_PID
    rm -f "$TMP_PROBE"

    # Small settle pause to let RTL-SDR USB endpoints release cleanly
    sleep 0.5

    if [ "$audio_found" -eq 1 ]; then
        selected_freq="$freq"
        selected_prog="$prog"
        selected_desc="$desc"
        break
    else
        echo "[recorder-launcher] -> No signal / audio lock on $freq HD$((prog + 1)) within ${PROBE_TIMEOUT_SEC}s. Advancing..."
    fi
done < "$CONF_FILE"

if [ -z "$selected_freq" ]; then
    echo "[recorder-launcher] All candidate stations failed signal check." >&2
    echo "[recorder-launcher] Exiting with error so systemd can apply backoff and retry." >&2
    exit 1
fi

# Record selected station in status file
echo "$selected_freq $selected_prog $selected_desc" > "$STATUS_FILE"
echo "[recorder-launcher] Locked onto: $selected_desc ($selected_freq HD$((selected_prog + 1)))"
echo "[recorder-launcher] Launching continuous song recorder..."

# Exec replacing shell so systemd directly manages the nrsc5 process
exec "$NRSC5_BIN" -q --record-songs "$RECORDINGS_DIR" --preroll "$PREROLL" "$selected_freq" "$selected_prog"
