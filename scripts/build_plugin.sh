#!/usr/bin/env bash
# Build the background voice PoC plugin (cordless_voice_poc.suprx).
set -euo pipefail

if [[ -z "${VITASDK:-}" ]]; then
  if [[ -d /usr/local/vitasdk ]]; then
    VITASDK=/usr/local/vitasdk
  else
    VITASDK="$HOME/vitasdk"
  fi
fi
export VITASDK
export PATH="$VITASDK/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"

if [[ ! -f "$VITASDK/share/vita.toolchain.cmake" ]]; then
  echo "error: VitaSDK not found at $VITASDK." >&2
  exit 1
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/plugin"
OUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$HOME/.cache/cordless_plugin_build"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo ">> Mirroring plugin sources to $WORK"
mkdir -p "$WORK"
rsync -a --delete --exclude build/ "$SRC_DIR"/ "$WORK"/

echo ">> Configuring plugin"
cmake -S "$WORK" -B "$WORK/build" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  >/dev/null

echo ">> Building (-j$JOBS)"
cmake --build "$WORK/build" -j"$JOBS"

SUPRX="$WORK/build/cordless_voice_poc.suprx"
CORE_SUPRX="$WORK/build/cordless_voice_core.suprx"
MEM_SKPRX="$WORK/build/cordless_mem.skprx"
if [[ -f "$SUPRX" && -f "$CORE_SUPRX" && -f "$MEM_SKPRX" ]]; then
  cp "$SUPRX" "$OUT_DIR/cordless_voice_poc.suprx"
  cp "$CORE_SUPRX" "$OUT_DIR/cordless_voice_core.suprx"
  cp "$MEM_SKPRX" "$OUT_DIR/cordless_mem.skprx"
  echo ">> OK: poc=$(du -h "$SUPRX" | cut -f1), core=$(du -h "$CORE_SUPRX" | cut -f1), mem=$(du -h "$MEM_SKPRX" | cut -f1)"
else
  echo ">> BUILD FAILED: missing plugin output" >&2
  exit 1
fi
