#!/usr/bin/env bash
# Launch the Chinese Emerald3DS build in Azahar (local virtual SD).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build/upstream/3ds_port"
DEST="${HOME}/Library/Application Support/Azahar/sdmc/3ds/emerald3ds"
THREE_DSX="${DEST}/Emerald3DS.3dsx"
APP="${AZAHAR_APP:-/Applications/Azahar.app}"

mkdir -p "$DEST"
: > "${DEST}/debug.txt"

if [[ -f "${BUILD}/dist/Emerald3DS.3dsx" && -f "${BUILD}/build/emerald3ds.pak" ]]; then
  cp -f "${BUILD}/dist/Emerald3DS.3dsx" "${THREE_DSX}"
  cp -f "${BUILD}/dist/Emerald3DS.smdh" "${DEST}/Emerald3DS.smdh"
  cp -f "${BUILD}/build/emerald3ds.pak" "${DEST}/emerald3ds.pak"
  echo "deployed dist/Emerald3DS.3dsx + pak (PORT_LANG=zh_cn)"
elif [[ -f "${BUILD}/emerald3ds.3dsx" ]]; then
  # Fat self-contained 3dsx (full romfs); no separate pak required.
  cp -f "${BUILD}/emerald3ds.3dsx" "${THREE_DSX}"
  cp -f "${BUILD}/emerald3ds.smdh" "${DEST}/Emerald3DS.smdh" 2>/dev/null || true
  echo "deployed fat emerald3ds.3dsx (no pak), PORT_LANG=zh_cn"
else
  echo "Missing zh_cn build under ${BUILD} — run:" >&2
  echo "  python tools/bootstrap.py --make --port-lang zh_cn -j8" >&2
  exit 1
fi

if [[ ! -d "$APP" ]]; then
  cat >&2 <<EOF
Azahar is not installed at ${APP}.
Install it from https://azahar.emu/ (or set AZAHAR_APP), or run the same
Emerald3DS.3dsx in RetroArch's azahar core over the virtual SD above.
EOF
  exit 1
fi
open -a "$APP" --args "$THREE_DSX"
