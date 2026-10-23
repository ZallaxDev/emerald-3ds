#!/usr/bin/env bash
# Launch the Japanese Emerald3DS build in Azahar (local virtual SD).
set -euo pipefail
APP="/Applications/Azahar.app"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build/upstream-ja/3ds_port"
DEST="${HOME}/Library/Application Support/Azahar/sdmc/3ds/emerald3ds"
THREE_DSX="${DEST}/Emerald3DS.3dsx"

mkdir -p "$DEST"
: > "${DEST}/debug.txt"

if [[ -f "${BUILD}/dist/Emerald3DS.3dsx" && -f "${BUILD}/build/emerald3ds.pak" ]]; then
  cp -f "${BUILD}/dist/Emerald3DS.3dsx" "${DEST}/Emerald3DS.3dsx"
  cp -f "${BUILD}/dist/Emerald3DS.smdh" "${DEST}/Emerald3DS.smdh"
  cp -f "${BUILD}/build/emerald3ds.pak" "${DEST}/emerald3ds.pak"
  echo "Deployed dist/Emerald3DS.3dsx + pak from build/upstream-ja"
elif [[ -f "${BUILD}/emerald3ds.3dsx" ]]; then
  # Fat self-contained 3dsx (full romfs); no separate pak required.
  cp -f "${BUILD}/emerald3ds.3dsx" "${DEST}/Emerald3DS.3dsx"
  cp -f "${BUILD}/emerald3ds.smdh" "${DEST}/Emerald3DS.smdh" 2>/dev/null || true
  echo "Deployed fat emerald3ds.3dsx (no pak)"
else
  echo "Missing JA build under ${BUILD} — run bootstrap/make release pak first." >&2
  exit 1
fi

open -a "$APP" --args "$THREE_DSX"
