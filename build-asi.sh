#!/usr/bin/env bash
# Build GTAIV.EFLC.FusionFix.asi on Linux using the genuine MSVC toolchain and
# MSBuild under Wine (github.com/mstorsjo/msvc-wine) - same tools and the same
# Platform=Win32 target as the GitHub Actions CI, so there is no dialect drift.
#
# One-time setup (see tools/README-linux-build.md):
#   sudo pacman -S --needed msitools wine-mono
#   git clone https://github.com/mstorsjo/msvc-wine && cd msvc-wine
#   python3 vsdownload.py --accept-license --dest /tmp/msvc --architecture x86 --with-msbuild
#   ./install.sh /tmp/msvc
#
# Usage:  ./build-asi.sh [msbuild args...]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Prefer a persisted toolchain (survives reboot) over the ephemeral /tmp copy.
# Persist once with:  cp -a /tmp/msvc ~/.local/opt/msvc
if [[ -z "${MSVC_BIN:-}" ]]; then
  for _c in "$HOME/.local/opt/msvc/bin/x86" /tmp/msvc/bin/x86; do
    [[ -x "$_c/msbuild" ]] && MSVC_BIN="$_c" && break
  done
fi
MSVC_BIN="${MSVC_BIN:-/tmp/msvc/bin/x86}"
export WINEPREFIX="${WINEPREFIX:-${XDG_RUNTIME_DIR:-/tmp}/fusionfix-msbuild-wine}"
export WINEDEBUG="${WINEDEBUG:--all}"

[[ -x "$MSVC_BIN/msbuild" ]] || { echo "msvc-wine not found at $MSVC_BIN (set MSVC_BIN)" >&2; exit 1; }

# A fresh prefix has no wine-mono registration, and MSBuild then exits silently
# with no diagnostic at all. Boot it once before doing anything else.
if [[ ! -f "$WINEPREFIX/system.reg" ]]; then
  echo "initialising wine prefix $WINEPREFIX ..."
  WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1 || true
fi

# MSBuild is a .NET Framework app; wine-mono provides that. It additionally needs
# an assembly that ships in the download but outside its probing path - without
# this it dies with `TypeLoadException ... Microsoft.VisualStudio.Telemetry`.
# amd64, because bin/x86/msbuild sets PreferredToolArchitecture=x64 (a 64-bit
# MSBuild driving a 32-bit build is correct - host arch != target arch).
_msvc_root="$(cd "$MSVC_BIN/../.." && pwd)"
for d in "$_msvc_root"/MSBuild/Current/Bin "$_msvc_root"/MSBuild/Current/Bin/amd64; do
  [[ -d "$d" && ! -f "$d/Microsoft.VisualStudio.Telemetry.dll" ]] && \
    cp -n "$_msvc_root/Common7/Tools/Microsoft.VisualStudio.Telemetry.dll" "$d/" 2>/dev/null || true
done

# The prebuild steps invoke the vendored Microsoft fxc.exe, which loads
# D3DCompiler_43.dll from beside itself - but only if Wine is told to prefer the
# native DLL. Wine's builtin (vkd3d) rejects SMAA.hlsl outright:
#   E5005: No compatible 3 parameter declaration for "SMAALumaEdgeDetectionPS"
# Microsoft's fxc reports X#### codes, vkd3d reports E#### - handy for telling
# which one actually ran.
#
# NOTE: msvc-wine's msvcenv.sh assigns WINEDLLOVERRIDES rather than appending, so
# it must be patched to preserve ours:
#   export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}vcruntime140=n;..."
# Do NOT disable mscoree here: it is the .NET runtime loader, and disabling it makes
# MSBuild exit silently with no diagnostic whatsoever. Only mshtml (gecko) is muted.
export WINEDLLOVERRIDES="mshtml=;d3dcompiler_43,d3dx9_43=n"
if ! grep -q 'WINEDLLOVERRIDES:+' "$MSVC_BIN/msvcenv.sh"; then
  echo "warning: $MSVC_BIN/msvcenv.sh overwrites WINEDLLOVERRIDES; the fxc prebuild will fail." >&2
  echo "         patch line 42 to append instead of assign." >&2
fi

cd "$ROOT"
VERSION="${VERSION:-99.0.0.0}"
[[ -f build/GTAIV.EFLC.FusionFix.vcxproj ]] || \
  wine premake5.exe vs2026 --with-version="$VERSION" 2>&1 | grep -v "radv is not" || true

"$MSVC_BIN/msbuild" -m build/GTAIV.EFLC.FusionFix.vcxproj \
  /property:Configuration=Release /property:Platform=Win32 "$@"

echo
find "$ROOT/bin" -name '*.asi' -printf '  built %p (%s bytes)\n' 2>/dev/null || true
