#!/usr/bin/env bash
# Adb-push every variant skel .so listed in a variant_builder manifest into
# /data/local/tmp/ on the device. The APK loads them via the per-cfg URI
# constructed by TunerService.nativeRunGemv from cfg_id.
#
# Usage:
#   scripts/push_skels.sh scripts/database/variants/manifest.json
#
# Env:
#   ANDROID_DEVICE / ANDROID_SERIAL  adb serial. Defaults to OnePlus 15.

set -euo pipefail

MANIFEST="${1:-}"
DEVICE="${ANDROID_DEVICE:-${ANDROID_SERIAL:-3C15AU002CL00000}}"

if [[ -z "$MANIFEST" || ! -f "$MANIFEST" ]]; then
    echo "Usage: $0 <manifest.json>" >&2
    echo "  e.g. $0 scripts/database/variants/manifest.json" >&2
    exit 1
fi

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# Each manifest entry has {"cfg_id": ..., "so": "..."}. Extract the SO paths.
SO_PATHS=$(python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
for entry in m:
    print(entry["so"])
' "$MANIFEST")

count=0
while IFS= read -r so_rel; do
    [[ -z "$so_rel" ]] && continue
    so_abs="$REPO_ROOT/$so_rel"
    [[ -f "$so_abs" ]] || { echo "[push_skels] missing: $so_abs" >&2; exit 1; }
    echo "[push_skels] adb -s $DEVICE push $so_rel -> /data/local/tmp/"
    adb -s "$DEVICE" push "$so_abs" /data/local/tmp/ > /dev/null
    count=$((count + 1))
done <<< "$SO_PATHS"

echo "[push_skels] pushed $count variants to $DEVICE:/data/local/tmp/"
