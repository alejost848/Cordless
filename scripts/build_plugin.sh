#!/usr/bin/env bash
# Build the background voice plugins (cordless_mem.skprx, cordless_voice_poc.suprx,
# cordless_voice_core.suprx, cordless_hud.suprx).
# Pass --stub to build the minimal 8K PoC core stub instead of the full voice core.
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

FULL_OPT="ON"
if [[ "${1:-}" == "--stub" ]]; then
  FULL_OPT="OFF"
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$HOME/.cache/cordless_plugin_build"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo ">> Mirroring repo to $WORK"
mkdir -p "$WORK"
rsync -a --delete \
  --exclude build/ --exclude .git/ \
  --exclude '*.vpk' --exclude '*.suprx' --exclude '*.skprx' \
  "$ROOT_DIR"/ "$WORK"/

if [[ -f "$WORK/build/CMakeCache.txt" ]] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$WORK/plugin" "$WORK/build/CMakeCache.txt"; then
  rm -rf "$WORK/build"
fi

echo ">> Configuring plugin (BUILD_VOICE_FULL=$FULL_OPT)"
cmake -S "$WORK/plugin" -B "$WORK/build" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DBUILD_VOICE_FULL="$FULL_OPT" \
  >/dev/null

echo ">> Building (-j$JOBS)"
cmake --build "$WORK/build" -j"$JOBS"

POC="$WORK/build/cordless_voice_poc.suprx"
MEM="$WORK/build/cordless_mem.skprx"
HUD="$WORK/build/cordless_hud.suprx"
if [[ "$FULL_OPT" == "ON" ]]; then
  CORE="$WORK/build/cordless_voice_full.suprx"
else
  CORE="$WORK/build/cordless_voice_core.suprx"
fi

if [[ -f "$POC" && -f "$CORE" && -f "$MEM" && -f "$HUD" ]]; then
  cp "$POC"  "$ROOT_DIR/cordless_voice_poc.suprx"
  cp "$CORE" "$ROOT_DIR/cordless_voice_core.suprx"
  cp "$MEM"  "$ROOT_DIR/cordless_mem.skprx"
  cp "$HUD"  "$ROOT_DIR/cordless_hud.suprx"
  echo ">> OK: poc=$(du -h "$POC" | cut -f1), core=$(du -h "$CORE" | cut -f1), mem=$(du -h "$MEM" | cut -f1), hud=$(du -h "$HUD" | cut -f1)"
else
  echo ">> BUILD FAILED: missing plugin output" >&2
  exit 1
fi
