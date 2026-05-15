#!/usr/bin/env bash
# Upload media to the ESP32-S3 badge.
#
# Single file via WiFi (fast):
#   ./upload_media.sh --wifi <file> [badge_ip]
#   badge_ip defaults to 192.168.4.1
#   Requires: badge WiFi AP active (swipe down → Config → WiFi ON)
#
# Bulk flash via USB (slow, replaces entire filesystem):
#   ./upload_media.sh [media_dir] [port]
#   media_dir defaults to ./media, port defaults to /dev/ttyUSB0
#   Requires: mklittlefs (bundled with arduino-esp32), esptool.py

set -euo pipefail

# ── WiFi single-file upload ───────────────────────────────────────────────────
if [[ "${1:-}" == "--wifi" ]]; then
    FILE="${2:?Usage: $0 --wifi <file> [badge_ip]}"
    BADGE_IP="${3:-192.168.4.1}"
    [[ -f "$FILE" ]] || { echo "File not found: $FILE" >&2; exit 1; }
    echo "Uploading $(basename "$FILE") → http://$BADGE_IP/upload …"
    curl --fail --progress-bar \
         -X POST "http://$BADGE_IP/upload" \
         -F "file=@$FILE"
    echo
    echo "Done."
    exit 0
fi

MEDIA_DIR="${1:-./media}"
PORT="${2:-/dev/ttyUSB0}"
IMAGE="/tmp/badge_littlefs.bin"

# ── Partition layout — must match arduino/badge/partitions.csv ─────────────────
FS_OFFSET=0x310000   # badge/partitions.csv — immediately after app0
FS_SIZE=0xCE0000     # ~13 MB

BLOCK_SIZE=4096
PAGE_SIZE=256

# ── Locate mklittlefs ──────────────────────────────────────────────────────────
# arduino-esp32 ships mklittlefs under the board package tools directory.
# The version number in the path changes with package updates — find it dynamically.
MKLITTLEFS_BASE="$HOME/.arduino15/packages/esp32/tools/mklittlefs"
if [[ -d "$MKLITTLEFS_BASE" ]]; then
    MKLITTLEFS=$(find "$MKLITTLEFS_BASE" -name "mklittlefs" -type f | sort -V | tail -1)
else
    # Fall back to system PATH
    MKLITTLEFS=$(command -v mklittlefs 2>/dev/null || true)
fi

[[ -n "${MKLITTLEFS:-}" && -x "$MKLITTLEFS" ]] || {
    echo "mklittlefs not found. Install arduino-esp32 board package or put mklittlefs in PATH." >&2
    exit 1
}

# ── Locate esptool ─────────────────────────────────────────────────────────────
ESPTOOL_BASE="$HOME/.arduino15/packages/esp32/tools/esptool_py"
if [[ -d "$ESPTOOL_BASE" ]]; then
    ESPTOOL=$(find "$ESPTOOL_BASE" -name "esptool.py" -type f | sort -V | tail -1)
fi
if [[ -z "${ESPTOOL:-}" || ! -x "${ESPTOOL:-}" ]]; then
    ESPTOOL=$(command -v esptool.py 2>/dev/null || command -v esptool 2>/dev/null || true)
fi
if [[ -z "${ESPTOOL:-}" || ! -x "${ESPTOOL:-}" ]]; then
    # Check common pip install locations
    for candidate in "$HOME/.mypython/bin/esptool" "$HOME/.local/bin/esptool" "$HOME/.local/bin/esptool.py"; do
        [[ -x "$candidate" ]] && { ESPTOOL="$candidate"; break; }
    done
fi

[[ -n "${ESPTOOL:-}" && -x "$ESPTOOL" ]] || {
    echo "esptool not found. Install with: pip install esptool" >&2
    exit 1
}

[[ -d "$MEDIA_DIR" ]] || { echo "Not a directory: $MEDIA_DIR" >&2; exit 1; }

# ── Ensure all JPEGs are baseline (JPEGDEC rejects progressive) ───────────────
if command -v jpegtran &>/dev/null; then
    while IFS= read -r -d '' jpg; do
        if identify -verbose "$jpg" 2>/dev/null | grep -q "Interlace: Line"; then
            echo "Converting progressive → baseline: $jpg"
            jpegtran -optimize "$jpg" > /tmp/_jt.jpg && mv /tmp/_jt.jpg "$jpg"
        fi
    done < <(find "$MEDIA_DIR" -maxdepth 1 \( -iname "*.jpg" -o -iname "*.jpeg" \) -print0)
else
    echo "Warning: jpegtran not found — progressive JPEGs will display as black on device."
    echo "         Install with: sudo apt install libjpeg-turbo-progs"
fi

echo "Packaging $MEDIA_DIR → $IMAGE"
"$MKLITTLEFS" \
    -c "$MEDIA_DIR" \
    -b $BLOCK_SIZE \
    -p $PAGE_SIZE \
    -s $FS_SIZE \
    "$IMAGE"

echo "Flashing to $PORT at offset $FS_OFFSET …"
"$ESPTOOL" \
    --chip esp32s3 \
    --port "$PORT" \
    --baud 921600 \
    --before default_reset \
    --after hard_reset \
    write-flash \
    --flash-mode dio \
    --flash-freq 80m \
    $FS_OFFSET "$IMAGE"

echo "Done. Media uploaded successfully."
echo
echo "To verify the partition table matches, run:"
echo "  esptool.py --port $PORT read_flash 0x8000 0x1000 /tmp/pt.bin && gen_esp32part.py /tmp/pt.bin"
