#!/usr/bin/env bash
# Linux equivalent of buildshaders.bat — rebuilds RAGE .fxc shaders from .asm/.xml
# source using RageShaderEditor.exe under Wine, then stages them into data/update.
#
# Verified: reproduces FusionFix v5.0.1's rage_postfx.fxc byte-for-byte
# (md5 34ece2ed530e9692e274e668ee3e7e21).
#
# Usage:  ./build-shaders.sh [shader-name ...]
#         ./build-shaders.sh              # all shaders
#         ./build-shaders.sh rage_postfx  # just one (fast iteration loop)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHADERS="$ROOT/shaders"
WIN32_30="$ROOT/data/update/common/shaders/win32_30"
TOOL="$ROOT/tools/RageShaderEditor"

export WINEPREFIX="${WINEPREFIX:-${XDG_RUNTIME_DIR:-/tmp}/fusionfix-shaderbuild-wine}"
export WINEDEBUG="${WINEDEBUG:--all}"
# d3dx9_40=n is REQUIRED: the repo ships the real Microsoft D3DX9_40.dll next to
# the exe, and only its D3DXAssembleShaderFromFileA accepts these .asm files.
# Wine's builtin d3dx9 assembler rejects them ("syntax error from bison").
# mscoree/mshtml are disabled purely to suppress the wine-mono/gecko install prompts.
export WINEDLLOVERRIDES="mscoree,mshtml=;d3dx9_40=n"

[[ -d "$WINEPREFIX" ]] || { mkdir -p "$WINEPREFIX"; wineboot -i >/dev/null 2>&1 || true; }

mapfile -t xmls < <(
  if (( $# )); then
    for name; do find "$SHADERS" -name "${name}.fxc.xml" -print; done
  else
    find "$SHADERS" -name '*.fxc.xml' -print | sort
  fi
)
(( ${#xmls[@]} )) || { echo "no matching .fxc.xml found" >&2; exit 1; }

cd "$TOOL"
fail=0
for xml in "${xmls[@]}"; do
  name="$(basename "$xml" .fxc.xml)"
  printf '  %-40s ' "$name"
  if out=$(timeout 300 wine RageShaderEditor.exe "Z:${xml}" </dev/null 2>&1) \
     && [[ -f "${xml%.xml}" ]]; then
    printf 'ok (%s bytes)\n' "$(stat -c%s "${xml%.xml}")"
  else
    printf 'FAILED\n'; grep -v 'radv is not' <<<"$out" | sed 's/^/      /' | head -5
    fail=1
  fi
done

mkdir -p "$WIN32_30"
find "$SHADERS" -name '*.fxc' -exec cp -f {} "$WIN32_30/" \;
if [[ -d "$SHADERS/GTAIV.EFLC.FusionShaders/resources" ]]; then
  cp -rf "$SHADERS/GTAIV.EFLC.FusionShaders/resources/." "$ROOT/data/update/"
fi
rm -f "$ROOT/data/update/common/shaders/preload.list"

echo "staged into $WIN32_30"
exit $fail
