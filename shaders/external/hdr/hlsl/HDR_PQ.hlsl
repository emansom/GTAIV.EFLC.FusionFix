// Final HDR output transform for GTA IV.
//
// Runs once per frame as a full-screen blit at onEndScene, i.e. AFTER the postfx
// composite and after the HUD, video and every other 2D element have been drawn.
// That ordering is the whole point of doing it here:
//
//   * The game keeps writing display-referred gamma-2.2 colour, exactly as it
//     always has, so all 2D content blends against the scene in gamma space -
//     which is what its artists assumed. An earlier design PQ-encoded inside each
//     shader, which left GTA IV's fixed-function blender combining PQ codes; that
//     is meaningless and drove HUD elements over bright scenery to ~6000 nits.
//   * Overbright survives because the back buffer is fp16 (upgradeBackBufferTo),
//     so values above 1.0 reach us intact.
//   * Anything drawn in 2D is handled automatically. No per-shader patching, and
//     nothing to miss - Bink video, loading screens and the pause map all just work.
//
// hdrParams (register c50):
//   x  mode: 0 = HDR (scene-referred, roll off to the panel peak)
//            1 = SDR (the game's ordinary look, scaled to paper white)
//   y  paper white nits / 10000
//   z  peak nits        / 10000
//   w  roll-off shoulder nits / 10000
//
// consoleGamma (register c51):
//   x  0 = none, 1 = Xenon (360) gamma, 2 = Cell (PS3) gamma - fused here so the
//      console look survives into HDR. See the note in PSMain for the 2.2 decode.

float4 globalScreenSize : register(c44);
float4 hdrParams        : register(c50);
float4 consoleGamma     : register(c51);

float3 SRGBDecode(float3 color)
{
    float3 linearSection = color / 12.92f;
    float3 powerSection = pow((max(color, 0.0f) + 0.055f) / 1.055f, 2.4f);
    return (color >= 0.04045f) ? powerSection : linearSection;
}

float3 Rec709Encode(float3 color)
{
    float3 linearSection = color * 4.5f;
    float3 powerSection = 1.099f * pow(max(color, 0.0f), 0.45f) - 0.099f;
    return (color >= 0.018f) ? powerSection : linearSection;
}

sampler2D FrameBufferSampler : register(s0);

struct VS_INPUT
{
    float4 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : POSITION0;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT VSMain(VS_INPUT In)
{
    VS_OUTPUT Out;
    Out.Position = In.Position;
    Out.TexCoord = In.TexCoord;
    return Out;
}

// BT.709 -> BT.2020. A container change, not gamut expansion: GTA IV's assets are
// BT.709 throughout, so this re-expresses the same colours in the wider encoding.
static const float3x3 BT709_TO_BT2020 =
{
    0.6274039f, 0.3292830f, 0.0433131f,
    0.0690973f, 0.9195406f, 0.0113623f,
    0.0163914f, 0.0880133f, 0.8955953f
};

// SMPTE ST 2084. Input is normalised so 1.0 == 10000 nits.
float3 LinearToPQ(float3 L)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;

    float3 Lm = pow(max(L, 0.0f), m1);
    return pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2);
}

// Exponential range compression above a shoulder, applied to luminance so
// chromaticity is preserved. Chosen over BT.2390 because it is the only common
// curve that genuinely bounds its output at the peak - BT.2390's Hermite segment
// extrapolates past InputLuminanceMax rather than clamping.
float3 RollOffHighlights(float3 color, float peak, float shoulder)
{
    float Y = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    if (Y <= shoulder)
        return color;

    // Guard the divisor: at peak == shoulder this goes infinite, the recombination
    // collapses and the whole frame is multiplied by zero.
    float range = max(peak - shoulder, 1e-4f);
    float excess = (Y - shoulder) / range;
    float rolled = shoulder + range * (1.0f - exp(-excess));

    return color * (rolled / max(Y, 1e-4f));
}

// https://blog.demofox.org/2022/02/01/two-low-discrepancy-grids-plus-shaped-sampling-ldg-and-r2-ldg/
float R2LDG(float2 pos)
{
    return frac(dot(pos, float2(0.754877666247f, 0.569840290998f)));
}

float4 PSMain(VS_OUTPUT In) : COLOR0
{
    float4 color = tex2D(FrameBufferSampler, In.TexCoord);

    // Console gamma fusion: apply FusionFix's Xenon/Cell curve here, then the
    // pow(2.2) decode below - matching the "input is gamma 2.2" the stock
    // console-gamma shaders assume, so HDR reproduces exactly what the SDR path
    // shows. NOT 2.4: the authentic 360 look is a 4-segment PWL sRGB approximation
    // (Xenos HW) that this curve already approximates; a 2.4 TV-reference exponent
    // would be a third, invented look. Refs: MJP "Correcting XNA's Gamma
    // Correction"; FusionFix #1433. Zero = untouched.
    if (consoleGamma.x > 1.5f)          // Cell (PS3)
        color.rgb = pow(max(color.rgb, 0.0f), 1.2f);
    else if (consoleGamma.x > 0.5f)     // Xenon (360)
        color.rgb = Rec709Encode(SRGBDecode(color.rgb));

    // The frame is display-referred gamma 2.2. Decode to scene-linear; overbright
    // above 1.0 expands rather than clipping.
    color.rgb = pow(max(color.rgb, 0.0f), 2.2f);

    // Scene 1.0 == SDR white == paper white; rescale into PQ's 0..10000 nit domain.
    // This is the same operation Windows performs on SDR content while the desktop
    // is in HDR mode - multiply by (SDR white level / 80) in linear light.
    color.rgb *= hdrParams.y;

    // Both modes roll off rather than clip - the fp16 targets carry real
    // overbright in SDR too, and a hard saturate there blows out every light.
    // The ASI sets the targets per mode: panel peak in HDR, the SDR white
    // point itself in SDR-in-PQ, so SDR highlights compress into the top of
    // the SDR range exactly like a display-side tone map would.
    color.rgb = RollOffHighlights(color.rgb, hdrParams.z, hdrParams.w);

    color.rgb = mul(BT709_TO_BT2020, color.rgb);
    color.rgb = LinearToPQ(max(color.rgb, 0.0f));

    // Dither: PQ at 10 bits bands visibly in near-blacks, where its code words are
    // most widely spaced in luminance.
    float noise = R2LDG(globalScreenSize.xy * In.TexCoord) - 0.5f;
    color.rgb += noise * (1.0f / 1023.0f);

    return float4(saturate(color.rgb), color.a);
}
