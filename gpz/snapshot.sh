#!/bin/sh
# One-command snapshot of a Minecraft server folder.
#   sh snapshot.sh /path/to/server [output.gpza]
# Builds gpz if needed, packs the folder, then verifies every file's checksum.
# For a consistent world, run `save-off` and `save-all flush` in the server console first,
# and `save-on` afterwards.
set -e
cd "$(dirname "$0")"
[ -n "$1" ] || { echo "usage: sh snapshot.sh /path/to/server [output.gpza]"; exit 2; }
command -v cc >/dev/null 2>&1 || { echo "No C compiler. Install one (Debian/Ubuntu: sudo apt install -y build-essential)"; exit 1; }
make -s
echo "--- devices (GPU is used automatically if listed):"; ./gpz list || true
SRC="$1"
OUT="${2:-$HOME/$(basename "$(cd "$SRC" && pwd)")-$(date -u +%Y%m%d-%H%M%S).gpza}"
./gpz pack -v "$SRC" "$OUT"
echo "--- snapshot ready:"; ls -lh "$OUT"; du -sh "$SRC"
echo "Restore later with:  ./gpz unpack \"$OUT\" /path/to/restore/dir"
