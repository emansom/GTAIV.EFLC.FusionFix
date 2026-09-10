#!/usr/bin/env python3
"""Insert the native HDR10 output path into GTA IV's shaders.

Two sites need it, because they write to the backbuffer at different times:

  postfx  rage_postfx's composite passes produce the 3D scene. Six variants share a
          byte-identical tail, so one patch covers whichever the engine selects.
  ui      gta_im / rage_im draw the HUD, map and pause menu AFTER postfx, straight
          into the backbuffer. Untouched they write SDR white as 1.0, and in a PQ
          buffer code 1.0 is 10000 nits by definition - which is exactly what
          Lilium's analysis reported.

Only the 2D UI variants are patched. The ones that write oDepth are used in-world
(drawn before postfx, into the HDR buffer) and must NOT be encoded twice.

Constants (chosen to be free in rage_postfx, gta_im AND rage_im simultaneously -
note gta_im binds c22..c27 and c208, so those are unusable)
---------------------------------------------------------------------------------
c207.x  HDR enable (0 = off). D3D9 float constants default to 0, so without an ASI
        driving it the shaders stay behaviourally identical to stock.
c207.y  paperWhiteNits / 10000
c207.z  peakNits       / 10000
c207.w  shoulderStart  / 10000
c46..48 BT.709 -> BT.2020 matrix rows
c49     (log2(e), epsilon, 0, 0)

--force-hdr bakes a `def c207` so the path can be exercised without an ASI build
(nothing calls SetPixelShaderConstantF(207), so the def stands).

Usage:
    ./tools/apply-hdr-shader-patch.py [--force-hdr] [--revert] [--only postfx|ui]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "shaders/GTAIV.EFLC.FusionShaders/win32_30_nv8"

BEGIN = "    // ---- BEGIN FusionFix HDR10 ----"
END = "    // ---- END FusionFix HDR10 ----"

DEFS = """\
    def c46, 0.6274039, 0.3292830, 0.0433131, 0
    def c47, 0.0690973, 0.9195406, 0.0113623, 0
    def c48, 0.0163914, 0.0880133, 0.8955953, 0
    def c49, 1.442695041, 0.0001, 0, 0
"""
FORCE_DEF = "    def c207, 1, 0.0203, 0.04, 0.02 // --force-hdr: on, 203/400/200 nits\n"

# Shared: linear BT.709 (1.0 == 10000 nits) -> BT.2020 PQ. {r} is the working register.
# PQ constants match the ones the SDR tone-mapping path already defines
# (c20.y=m1, c20.z=c2, c20.w=c1, c21.x=c3, c21.y=1, c21.z=m2).
ENCODE = """\
      dp3 r{t}.x, r{r}, c46
      dp3 r{t}.y, r{r}, c47
      dp3 r{t}.z, r{r}, c48
      max r{r}.xyz, r{t}, c1.x

      log r{t}.x, r{r}_abs.x
      log r{t}.y, r{r}_abs.y
      log r{t}.z, r{r}_abs.z
      mul r{t}.xyz, r{t}, c20.y
      exp r{t}.x, r{t}.x
      exp r{t}.y, r{t}.y
      exp r{t}.z, r{t}.z
      mad r{u}.xyz, r{t}, c20.z, c20.w
      mad r{t}.xyz, r{t}, c21.x, c21.y
      rcp r{t}.x, r{t}.x
      rcp r{t}.y, r{t}.y
      rcp r{t}.z, r{t}.z
      mul r{t}.xyz, r{t}, r{u}
      log r{t}.x, r{t}_abs.x
      log r{t}.y, r{t}_abs.y
      log r{t}.z, r{t}_abs.z
      mul r{t}.xyz, r{t}, c21.z
      exp r{r}.x, r{t}.x
      exp r{r}.y, r{t}.y
      exp r{r}.z, r{t}.z
      mov_sat r{r}.xyz, r{r}
"""

GAMMA_DECODE = """\
      log r{r}.x, r{r}_abs.x
      log r{r}.y, r{r}_abs.y
      log r{r}.z, r{r}_abs.z
      mul r{r}.xyz, r{r}, c13.y
      exp r{r}.x, r{r}.x
      exp r{r}.y, r{r}.y
      exp r{r}.z, r{r}.z
"""

# Exponential range compression above a shoulder, on luminance so chromaticity is
# preserved. Chosen over BT.2390 because it is the only common curve that genuinely
# bounds its output at the peak; BT.2390's Hermite segment extrapolates above
# InputLuminanceMax rather than clamping.
#
# The `max r{t}.y, r{t}.y, c49.y` guard on (peak - shoulder) is load-bearing, not
# defensive padding: at peak == shoulder the reciprocal below goes infinite, the
# recombination collapses to zero and the whole scene is multiplied by 0 - a black
# image with a perfectly intact HUD, because the UI path has no roll-off.
ROLLOFF = """\
      dp3 r{t}.x, r{r}, c1.yzww
      add r{t}.y, c207.z, -c207.w
      max r{t}.y, r{t}.y, c49.y
      add r{t}.z, r{t}.x, -c207.w
      rcp r{t}.w, r{t}.y
      mul r{t}.z, r{t}.z, r{t}.w
      mul r{t}.z, -r{t}.z, c49.x
      exp r{t}.z, r{t}.z
      add r{t}.z, -r{t}.z, c2.y
      mad r{t}.z, r{t}.z, r{t}.y, c207.w
      max r{u}.y, r{t}.x, c49.y
      rcp r{t}.w, r{u}.y
      mul r{t}.w, r{t}.w, r{t}.z
      add r{u}.x, r{t}.x, -c207.w
      cmp r{t}.w, r{u}.x, r{t}.w, c2.y
      mul r{r}.xyz, r{r}, r{t}.w
"""

POSTFX_BODY = (
    BEGIN + """
    // Output encode for the 3D scene. The swapchain is always BT.2020 PQ (DXVK
    // refuses to upgrade the format unless a colour space is also configured, so
    // the container cannot be switched at runtime), and this block decides what
    // gets written into it:
    //
    //   c50.x == 0  HDR  - scene-referred, highlights rolled off to the display peak
    //   c50.x != 0  SDR  - the game's ordinary look, scaled to paper white
    //
    // The SDR branch is what Windows itself does for SDR content while the desktop
    // is in HDR mode: decode to linear, multiply by (SDR white level / 80), encode
    // for the HDR container. See "Use DirectX with Advanced Color", which spells out
    // the nonlinear case - "consider converting into linear gamma to apply the
    // adjustment".
    //
    // Mutually exclusive with the SDR "Tone mapping" block below: the ASI must clear
    // c217.z (PREF_TONEMAPPING) whenever c207.x is set, else this PQ-encodes and that
    // block then reads PQ as if it were gamma 2.2.
    if_ne -c207_abs.x, c207_abs.x

      // SDR: clamp to the displayable range first, so the result is the game's
      // normal image rather than raw overbright from the fp16 targets.
      if_ne -c50_abs.x, c50_abs.x
        mov_sat r0.xyz, r0
      endif

      // r0 is display-referred gamma 2.2 here - the same assumption the SDR block
      // makes. Decode to scene-linear; overbright above 1.0 (preserved because the
      // render targets are fp16) expands rather than clipping.
"""
    + GAMMA_DECODE.format(r=0)
    + """
      // Scene 1.0 == SDR white == paper white; rescale so 1.0 == 10000 nits, the
      // domain PQ is defined on.
      mul r0.xyz, r0, c207.y

      // HDR only: roll highlights off to the panel peak. Skipped for SDR, where
      // nothing exceeds paper white and a roll-off would only darken the image.
      if_eq -c50_abs.x, c50_abs.x
"""
    + ROLLOFF.format(r=0, t=1, u=2)
    + """      endif

      // BT.709 -> BT.2020 and PQ encode. Container change only: GTA IV's assets are
      // BT.709, so this re-expresses the same colours, it does not invent gamut.
"""
    + ENCODE.format(r=0, t=1, u=2)
    + "    endif\n"
    + END
    + "\n"
)

UI_BODY = (
    BEGIN + """
    // The HUD, map and pause menu are drawn after postfx straight into the
    // backbuffer. Encode them at paper white so SDR white lands there instead of
    // at PQ code 1.0 (== 10000 nits). No roll-off: UI is bounded at paper white.
    if_ne -c207_abs.x, c207_abs.x
      mov_sat r10.xyz, r10
"""
    + GAMMA_DECODE.format(r=10)
    + "      mul r10.xyz, r10, c207.y\n\n"
    + ENCODE.format(r=10, t=11, u=12)
    + "    endif\n    mov oC0, r10\n"
    + END
    + "\n"
)


# Colour writes come in two shapes: a single `mul oC0, r0, v1`, or a split pair
# `mul oC0.xyz, r0, c39.y` / `mov oC0.w, r0.w`. Matching only the first shape missed
# gta_imPS1 and PS2 - the big sprite shaders that draw the HUD.
OC0_WRITE = re.compile(r"^([ \t]*)(\S+) oC0(\.\w+)?,", re.M)


def postfx_targets():
    d = ROOT / "rage_postfx"
    return sorted(p for p in d.glob("rage_postfxPS*.asm") if "// Tone mapping" in p.read_text())


def ui_targets():
    """Every gta_im / rage_im pixel shader that writes a colour.

    An earlier version filtered these to the ones that do not write oDepth, on the
    theory that depth-writing variants are in-world and must not be encoded. That was
    wrong twice over: those `// LogDepth Write` blocks are a FusionFix addition, so
    they mark which shaders FusionFix touched rather than whether a draw is HUD or
    world - and the filter excluded the shaders actually drawing the HUD, which is
    why the map and pause menu stayed pegged at 10000 nits.

    The filter is also unnecessary. Encoding is gated at runtime on c207.x, which the
    plugin only raises once the postfx composite is under way; in-world sprites are
    drawn before that with the flag clear and pass through untouched. The frame phase
    decides, not the shader.
    """
    out = []
    for name in ("rage_im", "gta_im"):
        for p in sorted((ROOT / name).glob(f"{name}PS*.asm")):
            if OC0_WRITE.search(p.read_text()):
                out.append(p)
    return out


def strip(text):
    text = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n", "", text, flags=re.S)
    text = re.sub(r"^ *def c4[6-9],.*\n", "", text, flags=re.M)
    text = re.sub(r"^ *def c207,.*\n", "", text, flags=re.M)
    # restore UI output writes that were redirected to r10 (both the plain and the
    # split .xyz/.w forms)
    text = re.sub(r"^([ \t]*\S+) r10(\.\w+)?,", r"\1 oC0\2,", text, flags=re.M)
    return text


def add_defs(text, name, force):
    defs = DEFS + (FORCE_DEF if force else "")
    # PQ constants live in c20/c21; postfx defines them already, the im shaders do not.
    if "def c20," not in text:
        defs = ("    def c20, 0.01, 0.159301757813, 18.8515625, 0.8359375\n"
                "    def c21, 18.6875, 1, 78.84375, 0\n") + defs
    if "def c13," not in text:
        defs = "    def c13, 0, 2.2, 0, 0\n" + defs
    if "def c1," not in text:
        defs = "    def c1, 0, 0.212500006, 0.715399981, 0.0720999986\n" + defs
    if "def c2," not in text:
        defs = "    def c2, 0.25, 1, 0.5, 0\n" + defs
    anchor = re.search(r"^ *ps_3_0\s*\n(?: *def .*\n)*", text, flags=re.M)
    if not anchor:
        sys.exit(f"{name}: no ps_3_0 / def anchor")
    return text[:anchor.end()] + defs + text[anchor.end():]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force-hdr", action="store_true")
    ap.add_argument("--revert", action="store_true")
    ap.add_argument("--only", choices=("postfx", "ui"))
    args = ap.parse_args()

    groups = {"postfx": postfx_targets(), "ui": ui_targets()}
    if args.only:
        groups = {args.only: groups[args.only]}

    for kind, paths in groups.items():
        if not paths:
            sys.exit(f"no {kind} targets found under {ROOT}")
        for path in paths:
            text = strip(path.read_text())
            if not args.revert:
                text = add_defs(text, path.name, args.force_hdr)
                if kind == "postfx":
                    m = re.search(r"^ *// Tone mapping\n", text, flags=re.M)
                    if not m:
                        sys.exit(f"{path.name}: no '// Tone mapping' anchor")
                    text = text[:m.start()] + POSTFX_BODY + "\n" + text[m.start():]
                else:
                    if not OC0_WRITE.search(text):
                        sys.exit(f"{path.name}: no oC0 write")
                    # Redirect every colour write to r10, then encode once after the
                    # last of them - a split .xyz/.w pair must both land in r10
                    # before the encode runs, or alpha is lost.
                    text = OC0_WRITE.sub(r"\1\2 r10\3,", text)
                    last = None
                    for last in re.finditer(r"^[ \t]*\S+ r10(\.\w+)?,.*$", text, flags=re.M):
                        pass
                    text = text[:last.end() + 1] + UI_BODY + text[last.end() + 1:]
            path.write_text(text)
            print(f"  {'reverted' if args.revert else 'patched '} {kind:6s} {path.name}")


if __name__ == "__main__":
    main()
