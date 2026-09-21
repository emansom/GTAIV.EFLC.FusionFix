module;

// ===========================================================================
// shaderprecompile.ixx  —  GTA IV launch-time D3D9 shader / pipeline pre-compiler
// (Wave 2: research work items W2 + W3 + W4 + W5)
//
// GOAL: before gameplay, create EVERY D3D9 shader the game uses and drive dummy
// draws over the render-state coverage set, so there is ZERO in-gameplay shader
// / pipeline-compilation stutter — on native D3D9 AND on DXVK (backend-agnostic,
// pure D3D9 API). A fullscreen progress overlay rides the built-in DLC/episode
// loading screen while it runs.
//
// WHY IT WORKS (DXVK, this rig — see re/gtaiv-shader-precompile-research.md):
//   * The dominant stutter is DXVK's per-stage Vulkan pipeline-library ISA
//     compile, done at CreateVertex/PixelShader under VK_EXT_graphics_pipeline
//     _library (GPL, default-on RADV). Creating all 1734 unique shaders pays
//     that cost up front, off the draw thread.  [W3 create pass]
//   * The residual is the first-draw vertex-input / fragment-output library
//     build + base-pipeline link, keyed on {vs,ps · vtx-layout · topology ·
//     blend(+write-mask+MRT) · RT/depth formats · MSAA · SPIR-V spec-constants}.
//     Dummy draws over the W6 coverage set pre-build those.  [W3 draw pass]
//   * The SUBTLE part (W6 §3): DXVK folds D3D9 state into SPIR-V spec constants.
//     To reproduce a pipeline exactly, each dummy draw must bind a texture of
//     the correct TYPE (2D/CUBE/VOLUME) to every sampler the PS reads and set
//     alpha-test / fog / clip-plane render state. We recover the PS sampler
//     dimensions and the VS input signature by statically walking the SM3.0
//     token stream (ParseShaderIO), so the bound resources and vertex decl match
//     what the game produces — otherwise DXVK compiles a fresh pipeline at first
//     gameplay use.  [W3 the spec-constant/sampler-type join]
//
// COMPLETION GATE (W4): after all creates + draws are submitted we drain the GPU
// with a D3DQUERYTYPE_EVENT fence, then wait until DXVK is QUIET -- DXVK exposes no
// app-readable compiler counter, so vkcapture watches what it does instead: its
// pipeline creations from every thread, and its compiler threads' CPU time. Then
// the Vulkan replay (vkcapture) runs with the loading screen held, and the same
// wait follows it; only then does the loading screen go (see WaitUntilIdle,
// HoldForVulkanReplay). No dxvk.conf change is required or made.
//
// WHERE IT RUNS (rewritten; the old anchor is now the fail-safe). The gate is the
// RETURN of rageBoot_InitSession 0x005C1360 -- main thread, once per session
// start, AFTER the main menu and the episode/DLC menu, with the game's own
// loading screen up and one instruction before the call that would take it down.
// That is the first point in the boot cycle at which RAGE's render phases and
// targets can be read at all (08-boot-gate.md: the engine builds them 0.7 s AFTER
// it tears the loading screen down, so no loading screen ever has them -- but the
// viewport tick's own guard lets a mod ask for them early and the engine then
// skips its own build). The old anchor, the first loading-screen render, is the
// LEGALS screen at t+2.2 s, before CGame_InitSystems has finished; it is kept
// only as the fail-safe for a build whose addresses do not validate.
//
// HOW IT SHARES THE THREAD. While a loading screen is up a dedicated thread
// free-runs it at ~7 kHz through FUN_005cc760. Blocking that thread freezes the
// artwork, so the pass runs in SLICES on a FIBER of it: the engine draws its
// frame, we take a few milliseconds, we give it back. The game's own loading
// screen keeps animating, our progress bar and text are drawn INTO its frame with
// its own primitive (bootgate.h), and there is no backdrop, no blur and no
// Present of ours anywhere. Every slice is bracketed by a full device state save
// and restore, because RAGE's device wrapper filters redundant sets and would
// otherwise hand the engine's own draws our leftovers.
// ===========================================================================

#include <common.hxx>
#include <d3d9.h>
#include <d3dx9.h>
#include <d3dx9core.h>
#include <cstdarg>
#include <cstdio>
#include <intrin.h>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include "fxc_parse.h"
#include "dxvk_d3d9_interfaces.h"
#include "pipelinekeys.h"
#include "d3d9cache.h"
#include "enginewarm.h"
#include "warmmusic.h"

export module shaderprecompile;

import common;
import comvars;
import d3dx9_43;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }
#endif

// ---------------------------------------------------------------------------
// Rebase a hard 1.2.0.59 VA (image base 0x400000) to the actual load base, so a
// rebased image still hits the right code/global.
// ---------------------------------------------------------------------------
static uintptr_t RebaseVA(uintptr_t va)
{
    static uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    return va - 0x400000u + base;
}

// ===========================================================================
//  Config (FusionFix ini, section [SHADERS])
// ===========================================================================
struct PrecompileConfig
{
    bool    enabled       = true;   // PrecompileShaders
    int     breadth       = 2;      // PrecompileCoverage: 0=create-only, 1=+1 draw/pass, 2=+spec expansion (full)
    bool    overlay       = true;   // PrecompileOverlay
    bool    coverSdr      = false;  // PrecompileCoverSdr: also cover stock A8R8G8B8/A2B10G10R10 scene formats
    bool    coverPresent  = true;   // PrecompileCoverPresentModes: cover both PQ(10-bit) and scRGB(fp16) present formats
    int     flashProbe    = 0;      // PrecompileFlashProbe: see the probe note below (0 = off, N = flash every Nth loadscreen frame)
    bool    replay        = true;   // PrecompileReplayCapturedKeys: replay FusionFix.pipelinekeys.bin when present
    // OFF by default: the heuristic behind it is invalid. It times the Draw calls,
    // but DXVK does not block a draw on pipeline compilation -- it enqueues the work
    // and the cost lands later, in the drain. Measured with GPL explicitly DISABLED,
    // so compilation was definitely happening, draws still averaged 0.070 ms and the
    // exit fired, stopping after 128 of 1863 pipelines. A false positive here
    // silently disables the feature for exactly the users who need it. Needs a
    // signal that reflects compilation rather than submission before it can be
    // trusted; the code is kept for when one exists.
    bool    adaptiveExit  = false;  // PrecompileAdaptiveExit
    // No time cap by default: the whole point is to finish the job. A pipeline left
    // unwarmed is a stutter during gameplay, which is far worse than a longer load.
    // Set a positive value only if you specifically want loading bounded.
    int     budgetSeconds = 0;      // PrecompileBudgetSeconds: 0 = run until everything is warm
    int     fenceChunk    = 1;      // PrecompileFenceChunk: pipelines per fence; 1 = smoothest bar
    // Replay the spec-constant variants after the base pipelines (see ReplayBaseKey).
    // They warm DXVK's background-optimized pipelines on drivers that fast-link;
    // where a driver cannot, only the base pipelines are ever synchronous.
    bool    specVariants  = true;   // PrecompileSpecVariants
    // Authoring-only: dump the deduplicated pipeline set so it can be SHIPPED as a
    // baseline. Off for players -- it costs a file write and they have nothing to
    // contribute that their own capture does not already hold.
    bool    exportBaseline = false; // PrecompileExportBaseline
    bool    emitLog       = false;  // PrecompileEmitLog: write every emitted identity for the scorer
    int     sliceMs       = 8;      // PrecompileSliceMs: warm work per loading-screen frame
    int     gateMaxSeconds = 0;     // PrecompileGateMaxSeconds: 0 = hold until the pass is done
    // Where the pass runs. 1 = the boot gate at rageBoot_InitSession, which is
    // the only place the engine walk has its render phases; 0 = the old
    // loading-screen gate, one blocking call on the first loading screen, which
    // is also what a build the boot gate cannot be armed on falls back to.
    // Kept selectable because it is the control arm for any measurement of the
    // gate, and the 2026-09-21 comparison had to be made with two BINARIES for
    // want of it -- which is one more variable than the question needed.
    bool    bootGate      = true;   // PrecompileBootGate
    // Whether D3DRS_MULTISAMPLEANTIALIAS is part of the replay's identity.
    // DXVK's source says it should be -- BindRasterizerState maps it to
    // DxvkRsInfo::sampleCount 0 or 1, and DxvkGraphicsPipelineStateInfo is
    // compared as bytes -- but a controlled in-game A/B says otherwise (see the
    // note beside it in ReplayBaseKey). Kept as a switch so that claim can be
    // re-tested on another driver in one run instead of a rebuild.
    bool    keySampleCount = false; // PrecompileKeySampleCount
    bool    reuseStamp    = true;   // PrecompileReuseWarmCache
    int     reflectionMsaa = 0;     // EXPERIMENTAL/ReflectionMSAAQuality, read for the sample count
    // The engine walk (enginewarm.h). 0 = off, 1 = the tied coordinate, 2 = the
    // tied coordinate plus the secondary ones. Everything it enumerates comes
    // from RAGE's own live tables, so it needs no recording at all -- see the
    // ini text for what the recorded cache still covers that it cannot.
    int     engineWarm    = 0;      // PrecompileEngineWarm
    // Where the engine walk's pipelines are built. DXVK never frees a graphics
    // pipeline while its device lives (see CreateWarmDevice), and the walk
    // builds ~61,000 of them, which is ~760 MB of a 32-bit process's address
    // space kept for the whole session. 1 = build them on a D3D9 device of our
    // own and destroy it when the walk is done, so only the DRIVER's on-disk
    // cache survives; 2 = only probe whether such a device can be created here
    // and say what it cost (diagnostic); 0 = the game's device, as before.
    int     warmDevice    = 1;      // PrecompileWarmDevice
    // Debugging only: name whatever ends the process, and what the address
    // space looked like when it did. Off by default -- see ArmCrashLogging.
    bool    crashLog      = false;  // PrecompileCrashLog
    // The warm-up's music (warmmusic.h). The hold can run for tens of minutes
    // and the loading screen's own six stems loop every forty seconds, so the
    // rotation brings in the rest of what the game has resident here. 0 turns
    // it off entirely and leaves the engine's loading music exactly as shipped.
    // Each episode gets its own rotation, chosen at the gate from the engine's
    // episode index -- Vladivostok FM for IV, Liberty Rock Radio for TLAD,
    // Electro-Choc and K109 for TBoGT -- so the override list is per episode
    // too, with the all-episodes one as the fallback.
    bool        warmMusic = true;        // PrecompileWarmMusic
    std::string warmMusicTracks;         // PrecompileWarmMusicTracks, empty = the shipped rotation
    std::string warmMusicTracksIV;       // PrecompileWarmMusicTracksIV
    std::string warmMusicTracksTLAD;     // PrecompileWarmMusicTracksTLAD
    std::string warmMusicTracksTBoGT;    // PrecompileWarmMusicTracksTBoGT
    // vkcapture owns this one; it is read here only so the band can say "stage
    // 2 of 3" instead of guessing how many stages the load has.
    bool    replayVulkan  = true;   // ReplayVulkanPipelines
};

// ===========================================================================
//  SM3.0 bytecode I/O analysis — recover VS input usages + PS sampler types.
//  This is the D3D9-only static join that makes dummy draws reproduce DXVK's
//  spec-constant (SamplerType/SamplerNull) and vertex-input pipeline keys.
// ===========================================================================
struct ShaderIO
{
    // VS: declared input (usage,usageIndex) pairs, in declaration order.
    struct In { uint8_t usage; uint8_t index; };
    std::vector<In> inputs;
    // Per declared sampler slot, its texture dimension (2=2D, 3=CUBE, 4=VOLUME, 0=unknown).
    // samplerDim[slot] == 0 means "not declared / leave null". A pixel shader declares
    // s0..s15; a VERTEX shader declares s0..s3, which are D3DVERTEXTEXTURESAMPLER0..3 and
    // land in KeyRecord::samplerType[16..19].
    uint8_t samplerDim[16] = {};
    bool    usesSampler[16] = {};
    // Which b# registers the shader READS, exactly as DXVK's own analysis builds
    // D3D9ShaderConstantsInfo::boolMask (d3d9_shader_analysis.cpp:127): every
    // source operand whose register type is CONSTBOOL. The device masks its 16
    // bool constants with this before they reach the spec constant, so two draws
    // that differ only in a b# the bound shader never reads are ONE pipeline.
    // Without the mask the recorded b# would split the key space for nothing.
    uint32_t boolMask = 0;
    bool     boolMaskValid = false;   // false: the walk could not be trusted (see below)
};

static void ParseShaderIO(const uint8_t* bytecode, uint32_t size, bool isVS, ShaderIO& out)
{
    if (!bytecode || size < 8) return;
    const uint32_t* dw = reinterpret_cast<const uint32_t*>(bytecode);
    uint32_t n = size / 4;
    // The walk below reads the instruction-length field at bits 24..27, which
    // exists from SM2 on and is ZERO for every SM1 instruction. On an SM1 token
    // stream it would therefore step one dword at a time through operand data,
    // miss real declarations and invent them out of constants -- and since a
    // missing declaration now MASKS a sampler slot rather than merely being
    // ignored, a mis-parse MERGES two pipelines into one key, which is the
    // direction that under-warms and stutters. An absent map entry is safe (no
    // masking at all); a present, wrong one is not. Nothing on this install is
    // SM1, but replayBlobs carries bytecode from other PCs' shared caches and
    // from arbitrary mods, and that is exactly the population the mask is
    // applied to. So: no version, no mask.
    const uint32_t major = (dw[0] >> 8) & 0xFF;
    if (major < 2) return;
    out.boolMaskValid = true;   // the walk is trustworthy from SM2 on
    uint32_t i = 1; // skip version token
    while (i < n)
    {
        uint32_t tok = dw[i];
        uint32_t op  = tok & 0xFFFF;
        if (op == 0xFFFF) break;                 // END token
        if (op == 0xFFFE) {                      // comment: skip its payload
            uint32_t clen = (tok >> 16) & 0x7FFF;
            i += 1 + clen;
            continue;
        }
        if (op == 0x001F && i + 2 < n)           // DCL (dcl token + dest token)
        {
            uint32_t dcl = dw[i + 1];
            uint32_t dst = dw[i + 2];
            uint32_t regtype = ((dst >> 28) & 0x7) | ((dst >> 8) & 0x18);
            uint32_t regnum  = dst & 0x7FF;
            if (isVS && regtype == 1 /*INPUT*/) {
                ShaderIO::In in;
                in.usage = (uint8_t)(dcl & 0x1F);
                in.index = (uint8_t)((dcl >> 16) & 0xF);
                out.inputs.push_back(in);
            } else if (regtype == 10 /*SAMPLER*/ && regnum < 16) {
                out.usesSampler[regnum] = true;
                out.samplerDim[regnum]  = (uint8_t)((dcl >> 27) & 0xF); // D3DSTT_*
            }
            i += 3;                              // dcl is always 2 operand dwords in SM3
            continue;
        }
        // def / defi / defb carry raw float, int and bool DATA in their operand
        // dwords, not register tokens, so they are stepped over whole: scanning
        // them for register types would invent b# references out of constants.
        if (op == 0x0051 || op == 0x0052 || op == 0x0053)
        {
            i += 1 + ((tok >> 24) & 0xF);
            continue;
        }
        // Every other instruction's operands ARE register tokens, and any of them
        // naming CONSTBOOL is a b# the shader reads. Scanning all of an
        // instruction's operands rather than decoding which position is the source
        // costs a false positive at worst (the destination can never be CONSTBOOL,
        // so there are none in practice) -- and a false positive only SPLITS a key,
        // which wastes a duplicate draw, while a miss MERGES two pipelines into one
        // and stutters.
        {
            const uint32_t len = (tok >> 24) & 0xF;
            for (uint32_t a = 1; a <= len && i + a < n; a++)
            {
                const uint32_t p = dw[i + a];
                if (!(p & 0x80000000u)) continue;       // not a parameter token
                if ((((p >> 28) & 0x7u) | ((p >> 8) & 0x18u)) != 14u) continue;   // D3DSPR_CONSTBOOL
                const uint32_t reg = p & 0x7FFu;
                if (reg < 16) out.boolMask |= 1u << reg;
            }
        }
        // The instruction-length field (bits 24..27) is authoritative from SM2 on,
        // and it is legitimately 0 for the zero-operand instructions -- ELSE, ENDIF,
        // BREAK, RET, END-of-loop. The old walk added an extra dword whenever it saw
        // one, which desynchronised the rest of the shader from the first ELSE. Every
        // shader on this install is SM3 and none of them changes mask (measured: 2387
        // .fxc shaders in both directories plus the 75 the caches carry), so this is a
        // latent bug rather than an observed one -- but the walk is the thing the
        // sampler masks are built from, so it should be right.
        i += 1 + ((tok >> 24) & 0xF);
    }
}

// Map a D3DDECLUSAGE (== SM3 dcl usage code) to a reasonable vertex element type.
static D3DDECLTYPE UsageToDeclType(uint8_t usage)
{
    switch (usage)
    {
    case D3DDECLUSAGE_POSITION:      return D3DDECLTYPE_FLOAT3;
    case D3DDECLUSAGE_POSITIONT:     return D3DDECLTYPE_FLOAT4;
    case D3DDECLUSAGE_BLENDWEIGHT:   return D3DDECLTYPE_FLOAT4;
    case D3DDECLUSAGE_BLENDINDICES:  return D3DDECLTYPE_D3DCOLOR;
    case D3DDECLUSAGE_NORMAL:        return D3DDECLTYPE_FLOAT3;
    case D3DDECLUSAGE_PSIZE:         return D3DDECLTYPE_FLOAT1;
    case D3DDECLUSAGE_TEXCOORD:      return D3DDECLTYPE_FLOAT2;
    case D3DDECLUSAGE_TANGENT:       return D3DDECLTYPE_FLOAT3;
    case D3DDECLUSAGE_BINORMAL:      return D3DDECLTYPE_FLOAT3;
    case D3DDECLUSAGE_TESSFACTOR:    return D3DDECLTYPE_FLOAT1;
    case D3DDECLUSAGE_COLOR:         return D3DDECLTYPE_D3DCOLOR;
    case D3DDECLUSAGE_FOG:           return D3DDECLTYPE_FLOAT1;
    default:                         return D3DDECLTYPE_FLOAT4;
    }
}

static UINT DeclTypeSize(D3DDECLTYPE t)
{
    switch (t)
    {
    case D3DDECLTYPE_FLOAT1:   return 4;
    case D3DDECLTYPE_FLOAT2:   return 8;
    case D3DDECLTYPE_FLOAT3:   return 12;
    case D3DDECLTYPE_FLOAT4:   return 16;
    case D3DDECLTYPE_D3DCOLOR: return 4;
    default:                   return 16;
    }
}

// ===========================================================================
//  W6 coverage tables — the render-state permutations GTA IV actually uses
//  (re/shader-precompile/state-coverage/coverage.json). Only the axes that BAKE
//  into a distinct DXVK VkPipeline on this v3.1 build are represented: RT/depth
//  format sets, colour-blend, topology (+ the spec-constant drivers, handled by
//  bound-texture type and render state below). cull/z/stencil/depth-bias are
//  DYNAMIC on this DXVK and are NOT permuted.
// ===========================================================================
#ifndef FOURCC_INTZ
// 'INTZ' little-endian (avoid a MAKEFOURCC/<mmsystem.h> dependency).
#define FOURCC_INTZ ((D3DFORMAT)('I' | ('N' << 8) | ('T' << 16) | ('Z' << 24)))
#endif

struct FormatSet
{
    const char* id;
    D3DFORMAT   color[4];
    int         mrt;
    D3DFORMAT   depth;      // D3DFMT_UNKNOWN == no depth
};

// FMT01/FMT02 (R32_SINT/R32_UINT, 1 pipeline each, rare/internal) are omitted.
static const FormatSet kFormatSets[] =
{
    { "FMT00", { D3DFMT_A16B16G16R16F },                                          1, D3DFMT_UNKNOWN },
    { "FMT03", { D3DFMT_L8 },                                                     1, D3DFMT_UNKNOWN },
    { "FMT04", { D3DFMT_A8B8G8R8 },                                               1, D3DFMT_UNKNOWN },
    { "FMT05", { D3DFMT_A2B10G10R10 },                                            1, D3DFMT_UNKNOWN },
    { "FMT06", { D3DFMT_A16B16G16R16F },                                          1, D3DFMT_D24S8   },
    { "FMT07", { D3DFMT_R32F },                                                   1, D3DFMT_UNKNOWN },
    { "FMT08", { D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_R16F }, 4, D3DFMT_D24S8 },
    { "FMT09", { D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F },                    2, D3DFMT_D24S8   },
    { "FMT10", { D3DFMT_G16R16F },                                               1, D3DFMT_D24S8   },
    { "FMT11", { D3DFMT_G16R16F },                                               1, D3DFMT_UNKNOWN },
};
static constexpr int kFormatSetCount = (int)(sizeof(kFormatSets) / sizeof(kFormatSets[0]));

// Distinct colour-blend configurations (per-target masks) from coverage.json.
struct PerTargetBlend
{
    bool  abe;
    D3DBLEND src, dst;
    D3DBLENDOP op;
    bool  sepa;
    D3DBLEND srca, dsta;
    D3DBLENDOP opa;
    DWORD mask;   // COLORWRITEENABLE RGBA bitmask (bit0=R..bit3=A)
};
struct BlendConfig { const char* id; int mrt; PerTargetBlend t[4]; };

#define B_OPAQUE(m) { false, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLENDOP_ADD, false, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLENDOP_ADD, (m) }
static const BlendConfig kBlends[] =
{
    { "BL00", 1, { B_OPAQUE(15) } },
    { "BL03", 1, { { true, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, false, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, 15 } } },
    { "BL04", 1, { { true, D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_ADD, false, D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_ADD, 15 } } },
    { "BL06", 1, { { true, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, false, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, 15 } } },
    { "BL01", 1, { { true, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, true, D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, 15 } } },
    { "BL05", 1, { B_OPAQUE(0) } },
    { "BL15", 1, { B_OPAQUE(1) } },
    { "BL16", 1, { B_OPAQUE(2) } },
    { "BL17", 1, { B_OPAQUE(8) } },
    { "BL07", 4, { B_OPAQUE(15), B_OPAQUE(15), B_OPAQUE(15), B_OPAQUE(15) } },
    { "BL08", 4, { B_OPAQUE(7),  B_OPAQUE(7),  B_OPAQUE(7),  B_OPAQUE(15) } },
    { "BL09", 4, { B_OPAQUE(8),  B_OPAQUE(0),  B_OPAQUE(0),  B_OPAQUE(15) } },
    { "BL12", 2, { B_OPAQUE(15), B_OPAQUE(0) } },
    { "BL13", 2, { B_OPAQUE(15), B_OPAQUE(15) } },
};
static constexpr int kBlendCount = (int)(sizeof(kBlends) / sizeof(kBlends[0]));

// The distinct (formatSet, blend, topology) tuples GTA IV produced in-capture
// (the 53 observedBakedStateTuples, condensed; VI is covered by per-shader input
// parsing in the pass loop). Indices into kFormatSets / kBlends by id-lookup.
struct StateTuple { const char* fmt; const char* blend; D3DPRIMITIVETYPE topo; };
static const StateTuple kTuples[] =
{
    { "FMT00", "BL00", D3DPT_TRIANGLESTRIP }, { "FMT00", "BL03", D3DPT_TRIANGLELIST },
    { "FMT00", "BL00", D3DPT_TRIANGLELIST },  { "FMT03", "BL00", D3DPT_TRIANGLELIST },
    { "FMT04", "BL00", D3DPT_TRIANGLELIST },  { "FMT05", "BL00", D3DPT_TRIANGLELIST },
    { "FMT06", "BL00", D3DPT_TRIANGLELIST },  { "FMT06", "BL03", D3DPT_TRIANGLELIST },
    { "FMT06", "BL04", D3DPT_TRIANGLELIST },  { "FMT06", "BL06", D3DPT_TRIANGLELIST },
    { "FMT06", "BL05", D3DPT_TRIANGLELIST },  { "FMT06", "BL17", D3DPT_TRIANGLESTRIP },
    { "FMT06", "BL17", D3DPT_TRIANGLEFAN },   { "FMT06", "BL03", D3DPT_TRIANGLEFAN },
    { "FMT06", "BL03", D3DPT_TRIANGLESTRIP }, { "FMT07", "BL00", D3DPT_TRIANGLELIST },
    { "FMT08", "BL07", D3DPT_TRIANGLELIST },  { "FMT08", "BL08", D3DPT_TRIANGLELIST },
    { "FMT08", "BL09", D3DPT_TRIANGLELIST },  { "FMT09", "BL12", D3DPT_TRIANGLELIST },
    { "FMT09", "BL13", D3DPT_TRIANGLELIST },  { "FMT10", "BL00", D3DPT_TRIANGLELIST },
    { "FMT10", "BL15", D3DPT_TRIANGLELIST },  { "FMT10", "BL16", D3DPT_TRIANGLELIST },
    { "FMT11", "BL00", D3DPT_TRIANGLELIST },
};
static constexpr int kTupleCount = (int)(sizeof(kTuples) / sizeof(kTuples[0]));

static const FormatSet* FindFormat(const char* id)
{
    for (auto& f : kFormatSets) if (strcmp(f.id, id) == 0) return &f;
    return nullptr;
}
static const BlendConfig* FindBlend(const char* id)
{
    for (auto& b : kBlends) if (strcmp(b.id, id) == 0) return &b;
    return nullptr;
}

// Reasoned-expansion spec-constant variants (coverage.json → reasonedExpansion).
// Each is a small render-state delta layered onto the base draw of a pass. dword0
// (SamplerType) / dword2 (SamplerNull) are driven by the bound-texture type from
// ParseShaderIO, not here.
struct SpecVariant { bool alphaTest; D3DCMPFUNC alphaFunc; bool fog; bool clip; };
static const SpecVariant kSpecVariants[] =
{
    { false, D3DCMP_ALWAYS,       false, false }, // base (captured daytime/opaque)
    { true,  D3DCMP_GREATER,      false, false }, // EXP_alphatest (cutout/foliage/decal)
    { true,  D3DCMP_GREATEREQUAL, false, false },
    { true,  D3DCMP_LESS,         false, false },
    { false, D3DCMP_ALWAYS,       true,  false }, // EXP_fog (foggy/overcast/rain)
    { false, D3DCMP_ALWAYS,       false, true  }, // EXP_clipplanes (water/mirror reflection)
};
static constexpr int kSpecVariantCount = (int)(sizeof(kSpecVariants) / sizeof(kSpecVariants[0]));

// ===========================================================================
//  The precompiler
// ===========================================================================
// Not exported: nothing imports this module — its static instance's constructor
// (registered as a CRT dynamic initializer when this TU is linked into the ASI)
// is the only entry point, exactly like skipintro.ixx / the other feature modules.
class ShaderPrecompiler
{
    // ---- config / state -------------------------------------------------
    static inline PrecompileConfig cfg;
    static inline std::atomic<bool> started{ false };   // precompile pass has begun
    static inline std::atomic<bool> finished{ false };  // precompile pass is done
    static inline SafetyHookInline shLoadscreenRender{};
    static inline HMODULE hSelf = nullptr;

    // Readiness gate: don't run until the swapchain is at its FINAL, stable size
    // (so FusionFix's windowed-borderless setup + any device reset have settled)
    // and a loading screen is genuinely up. This holds off the early-legal-screen
    // prologue that fired before borderless in the first crowd-test.
    static inline UINT  gateW = 0, gateH = 0;
    static inline int   stableFrames = 0;
    static inline int   loadscreenFramesSeen = 0;
    static constexpr int kStableFramesNeeded = 24;   // ~0.4s at the 64fps loadscreen cap
    static inline HWND  gameWnd = nullptr;

    enum class Backend { Unknown, Native, DXVK };
    static inline Backend backend = Backend::Unknown;

    // ---- device + resources --------------------------------------------
    static inline IDirect3DDevice9* dev = nullptr;

    static inline std::vector<IDirect3DVertexShader9*> vsHandles;
    static inline std::vector<IDirect3DPixelShader9*>  psHandles;
    static inline std::vector<ShaderIO>                shaderIO;   // parallel to unique table

    static inline IDirect3DVertexBuffer9* dummyVB = nullptr;
    static inline IDirect3DIndexBuffer9*  dummyIB = nullptr;
    static inline IDirect3DTexture9*       tex2D  = nullptr;
    static inline IDirect3DCubeTexture9*   texCube = nullptr;
    static inline IDirect3DVolumeTexture9* texVol = nullptr;
    // Two more, for the sampler MODES (KeyRecord::samplerMode). A slot DXVK samples
    // with Fetch4 or with depth compare is a different pipeline, and neither mode
    // can be reached with an ordinary A8R8G8B8 texture: DXVK derives both from the
    // bound texture's FORMAT (DetermineFetch4Compatibility / DetermineShadowState),
    // so the replay has to bind a texture of a format that qualifies.
    //   texFetch4  R32F, one of DXVK's single-channel set; with
    //              D3DSAMP_MIPMAPLODBIAS = 'GET4' and MAGFILTER = POINT it is the
    //              whole of what UpdateActiveFetch4 asks for.
    //   texShadow  D24S8, a depth format that is NOT on the INTZ/DF16/DF24
    //              blacklist, so DXVK depth-compares it.
    static inline IDirect3DTexture9*       texFetch4 = nullptr;
    static inline IDirect3DTexture9*       texShadow = nullptr;

    // one scratch RT surface per color format we might need
    struct ScratchRT { D3DFORMAT fmt; IDirect3DSurface9* surf; };
    static inline std::vector<ScratchRT> scratchColor;
    static inline IDirect3DSurface9* scratchDepthD24S8 = nullptr;
    static inline IDirect3DTexture9*  scratchDepthINTZTex = nullptr;
    static inline IDirect3DSurface9*  scratchDepthINTZ = nullptr;
    static constexpr UINT kRTdim = 64;

    // overlay
    static inline ID3DXFont* font = nullptr;
    static inline ID3DXFont* fontBig = nullptr;
    static inline IDirect3DSurface9* backdrop = nullptr; // snapshot of the loading-screen frame
    static inline UINT bbW = 1280, bbH = 720;
    static inline D3DFORMAT bbFmt = D3DFMT_A8R8G8B8;

    // progress
    static inline std::atomic<uint32_t> workDone{ 0 };
    static inline uint32_t workTotal = 1;

    // Creating a shader object is ~100x cheaper than compiling a pass (measured:
    // 1734 creates in <1s, 1757 passes in ~110s). Counting both as one unit made the
    // bar jump to ~44% within a second and then sit there for the whole compile, which
    // reads as a hang. Weight the slow phases so the bar advances in proportion to TIME.
    static constexpr uint32_t kCreateW = 1;
    static constexpr uint32_t kPassW = 100;
    static inline std::string curLabel;
    // What the bar measures, in the title: the D3D9 pass, then the Vulkan replay.
    // curTitle replaces the whole title (no percentage, no ETA) while the bar
    // measures something that is not work done -- the waits for DXVK to go quiet.
    static inline const char* curWhat = "Compiling shaders";
    static inline std::string curTitle;
    // WHERE THE PLAYER IS IN THE WHOLE LOAD, which the bar alone cannot say: it
    // restarts at every phase, and on a cold cache there are three of them and
    // the whole thing is tens of minutes. The band shows "STAGE k OF n" and
    // scopes its time estimate to the stage it is actually measuring, because
    // an estimate that silently means "of this phase" is a lie on a 40-minute
    // load. Set by the phases themselves; counted once, at the pass's start.
    static inline int stageNo = 0;
    static inline int stageCount = 1;
    // Enter the next stage. stageCount is what the configuration says will run;
    // it grows rather than lies if a stage turns up that was not counted, and
    // HoldForVulkanReplay shrinks it when its stage turns out to have nothing
    // to do. curLabel belongs to the stage that set it, so it goes too.
    static void EnterStage(const char* what)
    {
        stageNo++;
        if (stageNo > stageCount) stageCount = stageNo;
        curWhat = what;
        curLabel.clear();
        // The bar belongs to the stage that is measuring it. Leaving the last
        // stage's counters in place would show the new one as finished before
        // it has drawn a thing, which is exactly the kind of lie a 40-minute
        // load cannot afford.
        workDone = 0;
        workTotal = 1;
        tPhase = std::chrono::steady_clock::now();
    }
    // The repeat path: the warm stamp matched and the driver still had the
    // pipelines, so the engine walk stopped after its 512-job probe. That is a
    // completely different load from the full pass and the band has to say so
    // -- in the stage it happened in, not for the rest of the load.
    static inline std::atomic<int> cheapStage{ 0 };
    static inline std::chrono::steady_clock::time_point tStart;
    static inline std::chrono::steady_clock::time_point tPhase;   // what the bar's ETA extrapolates from
    static inline std::chrono::steady_clock::time_point tLastPresent;
    static inline uint32_t overlayFrames = 0;   // overlay frames actually presented
    static inline unsigned presentsAtStart = 0;
    static inline std::chrono::steady_clock::time_point tLastLabel{};
    static inline uint32_t overlayGapMin = 0xFFFFFFFFu, overlayGapMax = 0;
    static inline uint64_t overlayGapSum = 0;
    static inline uint32_t overlayGapCount = 0;

    // -------------------------------------------------------------------
    static void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        pipelinekeys::LogLine("[ShaderPrecompile] ", buf);
    }

    // What this phase cost the 32-bit address space, and how many pipelines DXVK
    // has built by now. Both numbers on one line, at every boundary of the pass,
    // so a log alone answers "where did the address space go" without a rerun.
    // The VirtualQuery walk behind it is ~1 ms with tens of thousands of
    // regions, which is why it is at phase boundaries and not per job.
    static void LogVa(const char* where)
    {
        Log("memory %s: %s; DXVK has created %u pipelines", where,
            pipelinekeys::VaLine().c_str(), pipelinekeys::VulkanReplay().creations.load());
    }

    // ===================================================================
    //  THE AMBIENT HALF OF A DXVK PIPELINE KEY
    //
    //  Under DXVK a D3D9 pipeline is keyed on more than the recorded key
    //  carries. Everything in `D3D9SpecData` (dxvk 3.1.1 src/d3d9/d3d9_state.h)
    //  that the bound shaders declare enters `DxvkGraphicsPipelineStateInfo::sc`
    //  through DxvkContext::updateSpecConstants, so two draws with the same
    //  shaders, declaration, render targets and blend state are still two
    //  pipelines if any of it differs. Most of it the pass pins -- fog, clip
    //  planes, point sprite, the samplers it binds, the alpha compare op out of
    //  RAGE's default block. THREE things it does not, and all three are
    //  whatever the last thing to touch the device left behind:
    //
    //    * the bool constants b0..b15 per stage, masked by each shader's own
    //      boolMask (d3d9_device.cpp:7443 and 7459) -- spec IDs 3 and 6;
    //    * the sampler projection mask, which is D3DTSS_TEXTURETRANSFORMFLAGS
    //      per stage (spec ID 2, and that ID also carries psIsShaderModel3);
    //    * the fixed-function texture stage ops and args, spec IDs 8..19.
    //
    //  WHY IT IS LOGGED. Moving the pass from the loading-screen gate to the
    //  boot gate cost the DEFAULT path 51 pipelines -- 786 built before
    //  gameplay on 4c85a78 against 735 on e3f42ce, the same integer in every
    //  run of three, from provably identical work (14,543 keys, 1093 drawn
    //  pipelines, 13,450 duplicate-state skips, byte-identical state restore)
    //  -- and those 51 were worth ~46 pipeline creations and ~12 compiles per
    //  route. The 2026-09-21 control run's verdict was that the two gates hand
    //  the pass different ambient state and the same draws therefore specialise
    //  differently. This is the measurement that confirms or clears that, and
    //  it costs a dozen Get* calls at three points in a load.
    // ===================================================================
    struct AmbientSpec
    {
        uint32_t vsBools = 0, psBools = 0;          // b0..b15, one bit each
        uint8_t  ttff[8] = {};                      // D3DTSS_TEXTURETRANSFORMFLAGS
        uint8_t  colorOp[8] = {}, alphaOp[8] = {};  // spec IDs 8..19
        uint32_t fogEnable = 0, fogVertex = 0, fogTable = 0, fogRange = 0;
        uint32_t pointSprite = 0, pointScale = 0, specular = 0;
        uint32_t alphaTest = 0, alphaFunc = 0, clipPlanes = 0, srgbWrite = 0, shadeMode = 0;
        // The clip-plane COEFFICIENTS, which is what DXVK actually counts -- an
        // enabled plane that is all zero does not reach the spec constant. This is
        // the field the first ambient reading did not have, and it is where the
        // 786-vs-735 difference turned out to live: the loading screen's D3DSBT_ALL
        // block puts the engine's (zero) planes back between slices.
        uint32_t planeNonZero = 0;                  // bit per plane with a non-zero coefficient
        uint32_t planeCount = 0;                    // what DXVK would count, given clipPlanes
        bool     readable = false;                  // false: a pure device refused Get*
    };

    static AmbientSpec ReadAmbientSpec(IDirect3DDevice9* d)
    {
        AmbientSpec a;
        if (!d) return a;
        BOOL b[16] = {};
        if (SUCCEEDED(d->GetVertexShaderConstantB(0, b, 16)))
        {
            a.readable = true;
            for (int i = 0; i < 16; i++) if (b[i]) a.vsBools |= 1u << i;
        }
        memset(b, 0, sizeof(b));
        if (SUCCEEDED(d->GetPixelShaderConstantB(0, b, 16)))
        {
            a.readable = true;
            for (int i = 0; i < 16; i++) if (b[i]) a.psBools |= 1u << i;
        }
        for (DWORD s = 0; s < 8; s++)
        {
            DWORD v = 0;
            if (SUCCEEDED(d->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &v))) a.ttff[s] = (uint8_t)v;
            if (SUCCEEDED(d->GetTextureStageState(s, D3DTSS_COLOROP, &v))) a.colorOp[s] = (uint8_t)v;
            if (SUCCEEDED(d->GetTextureStageState(s, D3DTSS_ALPHAOP, &v))) a.alphaOp[s] = (uint8_t)v;
        }
        auto rs = [&](D3DRENDERSTATETYPE k) { DWORD v = 0; d->GetRenderState(k, &v); return (uint32_t)v; };
        a.fogEnable   = rs(D3DRS_FOGENABLE);
        a.fogVertex   = rs(D3DRS_FOGVERTEXMODE);
        a.fogTable    = rs(D3DRS_FOGTABLEMODE);
        a.fogRange    = rs(D3DRS_RANGEFOGENABLE);
        a.pointSprite = rs(D3DRS_POINTSPRITEENABLE);
        a.pointScale  = rs(D3DRS_POINTSCALEENABLE);
        a.specular    = rs(D3DRS_SPECULARENABLE);
        a.alphaTest   = rs(D3DRS_ALPHATESTENABLE);
        a.alphaFunc   = rs(D3DRS_ALPHAFUNC);
        a.clipPlanes  = rs(D3DRS_CLIPPLANEENABLE);
        a.srgbWrite   = rs(D3DRS_SRGBWRITEENABLE);
        a.shadeMode   = rs(D3DRS_SHADEMODE);
        for (DWORD i = 0; i < 6; i++)
        {
            float p[4] = {};
            if (FAILED(d->GetClipPlane(i, p))) continue;
            if (!(p[0] || p[1] || p[2] || p[3])) continue;
            a.planeNonZero |= 1u << i;
            if (a.clipPlanes & (1u << i)) a.planeCount++;
        }
        return a;
    }

    static void LogAmbientSpec(const char* where, IDirect3DDevice9* d)
    {
        const AmbientSpec a = ReadAmbientSpec(d);
        if (!a.readable)
        {
            Log("ambient spec %s: the device refused Get*ShaderConstantB (a pure device?) - "
                "the bool-constant half of the key cannot be read here", where);
        }
        char t[80], c[96];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%u%u%u%u%u%u%u%u",
                    a.ttff[0], a.ttff[1], a.ttff[2], a.ttff[3], a.ttff[4], a.ttff[5], a.ttff[6], a.ttff[7]);
        _snprintf_s(c, sizeof(c), _TRUNCATE, "%u/%u %u/%u %u/%u %u/%u",
                    a.colorOp[0], a.alphaOp[0], a.colorOp[1], a.alphaOp[1],
                    a.colorOp[2], a.alphaOp[2], a.colorOp[3], a.alphaOp[3]);
        Log("ambient spec %s: vsBools 0x%04X psBools 0x%04X, texture transform flags %s, "
            "stage ops (colour/alpha, 0-3) %s, fog %u (v%u t%u r%u), point sprite %u scale %u, "
            "specular %u, alpha test %u func %u, clip planes enabled 0x%X non-zero 0x%X "
            "-> DXVK would count %u, sRGB write %u, shade %u",
            where, a.vsBools, a.psBools, t, c, a.fogEnable, a.fogVertex, a.fogTable, a.fogRange,
            a.pointSprite, a.pointScale, a.specular, a.alphaTest, a.alphaFunc,
            a.clipPlanes, a.planeNonZero, a.planeCount, a.srgbWrite, a.shadeMode);
    }

    // The b# the GAME's device is carrying when the gate opens. The walk pins
    // these on whatever device it runs on, so that the arm with a device of our
    // own (brand new, every bool FALSE by the D3D9 default) and the arm on the
    // game's device build the SAME pipelines rather than two different sets
    // that only a pipeline count could tell apart.
    static inline BOOL ambientVsB[16] = {};
    static inline BOOL ambientPsB[16] = {};
    static inline bool ambientBoolsRead = false;

    // -------------------------------------------------------------------
    //  What ends the process, when something does
    //
    //  Two engine-warm runs ended with the game gone a minute or two into
    //  gameplay and nothing in the log to say why. Both still wrote vkcapture's
    //  "gameplay frames final" line, which comes from DLL_PROCESS_DETACH
    //  (dllmain.cpp) -- so the process was unwound rather than killed, i.e.
    //  somebody called ExitProcess. These two say who, and what the address
    //  space looked like at that instant.
    //
    //  The vectored handler sees an exception before any filter the game
    //  installed, and only for the codes that end a process; Wine and the game
    //  raise plenty of others all the time. It is rate limited in TIME rather
    //  than by a plain count, so a burst during startup cannot use up the
    //  budget for a fault two minutes into gameplay.
    //
    //  OFF BY DEFAULT (PrecompileCrashLog), because with these two installed
    //  the process spins instead of exiting: four launches of this build hung
    //  in the teardown after ExitProcess had been logged, on both the long pass
    //  and the 8 s one, where a build without them quit cleanly from the same
    //  gate. That is a debugging aid paying for itself only while something is
    //  being debugged, so it is opt-in and it takes itself back out of the way
    //  on the way through ExitProcess.
    // -------------------------------------------------------------------
    static inline SafetyHookInline shExitProcess{};
    static inline void* vehHandle = nullptr;
    static inline std::atomic<int64_t> lastVehUs{ 0 };
    static inline std::atomic<uint32_t> vehSeen{ 0 }, vehLogged{ 0 };

    static LONG CALLBACK ExceptionLogger(EXCEPTION_POINTERS* ep)
    {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        const bool fatal = code == EXCEPTION_ACCESS_VIOLATION
                        || code == EXCEPTION_IN_PAGE_ERROR
                        || code == EXCEPTION_STACK_OVERFLOW
                        || code == EXCEPTION_ILLEGAL_INSTRUCTION
                        || code == EXCEPTION_PRIV_INSTRUCTION
                        || code == EXCEPTION_INT_DIVIDE_BY_ZERO
                        || code == (DWORD)0xC0000017 /* STATUS_NO_MEMORY */
                        || code == (DWORD)0xC00000FD /* STATUS_STACK_OVERFLOW */;
        if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
        vehSeen++;
        const int64_t now = pipelinekeys::ClockUs();
        const int64_t last = lastVehUs.load();
        if (vehLogged.load() >= 64 || (last && now - last < 2000000)) return EXCEPTION_CONTINUE_SEARCH;
        lastVehUs = now;
        vehLogged++;
        const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
        Log("EXCEPTION 0x%08lX at %p (thread %lu, %u seen): %s %p [%u params]",
            (unsigned long)code, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),
            vehSeen.load(),
            ep->ExceptionRecord->NumberParameters >= 2
                ? (info[0] == 0 ? "reading" : info[0] == 1 ? "writing" : "executing") : "at",
            ep->ExceptionRecord->NumberParameters >= 2 ? (void*)info[1] : nullptr,
            ep->ExceptionRecord->NumberParameters);
        Log("memory at that exception: %s", pipelinekeys::VaLine().c_str());
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static void WINAPI ExitProcessDetour(UINT code)
    {
        void* from = _ReturnAddress();
        Log("ExitProcess(%u) from %p (rebased 0x%08X) - somebody is ending the process",
            code, from, (unsigned)((uintptr_t)from - (uintptr_t)GetModuleHandleW(nullptr) + 0x400000));
        Log("memory at ExitProcess: %s", pipelinekeys::VaLine().c_str());
        Log("exceptions seen by then: %u fatal-class (%u logged)", vehSeen.load(), vehLogged.load());
        // Everything past this point is teardown, and there is nothing left for
        // either of these to say about it. Out of the way first.
        if (vehHandle) { RemoveVectoredExceptionHandler(vehHandle); vehHandle = nullptr; }
        shExitProcess.reset();
        ExitProcess(code);
    }

    static void ArmCrashLogging()
    {
        if (!cfg.crashLog) return;
        vehHandle = AddVectoredExceptionHandler(0 /* last */, &ExceptionLogger);
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
            if (void* fn = (void*)GetProcAddress(k32, "ExitProcess"))
                shExitProcess = safetyhook::create_inline(fn, reinterpret_cast<void*>(&ExitProcessDetour));
        Log("crash logging armed (PrecompileCrashLog: vectored handler%s)",
            shExitProcess ? " + ExitProcess" : ", ExitProcess NOT hooked");
    }

    // Drain the message queue so Windows keeps the window "responsive" while the
    // precompile blocks the render thread. GTA IV pumps its window on the thread
    // that runs the loadscreen render (our hook thread), so a long blocking loop
    // here without this makes the window go "not responding" and starves the
    // borderless/window setup that FusionFix drives through window messages.
    // Bounded per call so a message flood can't stall the loop.
    static void PumpMessages()
    {
        MSG msg;
        for (int i = 0; i < 128 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); i++)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ---- state-neutrality verification --------------------------------
    // Pointers here are compared for IDENTITY only and never dereferenced, so the
    // references taken by the Get* calls are dropped immediately.
    struct StateSnapshot
    {
        DWORD rs[pipelinekeys::kNumRS]{};
        void* tex[pipelinekeys::kNumSamplers]{};
        void* vs{}; void* ps{}; void* decl{}; void* ib{};
        DWORD fvf{};
        struct { void* vb; UINT offset, stride, freq; } stream[4]{};
        void* rt[4]{}; void* ds{};
        D3DVIEWPORT9 vp{};
        RECT scissor{};
        // The specialisation state the replay now writes per draw. It all rides in
        // the D3DSBT_ALL block the pass takes, so it should come back untouched --
        // but "should" is what the rest of this snapshot exists to stop being.
        float clip[6][4]{};
        DWORD tss[pipelinekeys::kFFStages][10]{};
        DWORD samp[pipelinekeys::kPSSamplers][2]{};
        BOOL  vsB[16]{}, psB[16]{};
        bool valid{};
    };

    // The texture stage states the replay writes, in the order the snapshot keeps them.
    static constexpr D3DTEXTURESTAGESTATETYPE kTSSWatched[10] = {
        D3DTSS_COLOROP, D3DTSS_ALPHAOP, D3DTSS_RESULTARG,
        D3DTSS_COLORARG0, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
        D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
        D3DTSS_TEXTURETRANSFORMFLAGS,
    };
    static constexpr D3DSAMPLERSTATETYPE kSampWatched[2] = {
        D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAGFILTER,
    };

    template <typename T> static void* GrabPtr(T* p) { if (p) p->Release(); return (void*)p; }

    static void CaptureSnapshot(StateSnapshot& s)
    {
        if (!dev) return;
        for (uint32_t i = 0; i < pipelinekeys::kNumRS; i++)
            dev->GetRenderState(pipelinekeys::kTrackedRS[i].rs, &s.rs[i]);

        for (uint32_t i = 0; i < pipelinekeys::kPSSamplers; i++)
        {
            IDirect3DBaseTexture9* t = nullptr;
            dev->GetTexture(i, &t);
            s.tex[i] = GrabPtr(t);
        }
        for (uint32_t i = 0; i < pipelinekeys::kVSSamplers; i++)
        {
            IDirect3DBaseTexture9* t = nullptr;
            dev->GetTexture(D3DVERTEXTEXTURESAMPLER0 + i, &t);
            s.tex[pipelinekeys::kPSSamplers + i] = GrabPtr(t);
        }

        IDirect3DVertexShader9* vs = nullptr;      dev->GetVertexShader(&vs);        s.vs = GrabPtr(vs);
        IDirect3DPixelShader9* ps = nullptr;       dev->GetPixelShader(&ps);         s.ps = GrabPtr(ps);
        IDirect3DVertexDeclaration9* de = nullptr; dev->GetVertexDeclaration(&de);   s.decl = GrabPtr(de);
        IDirect3DIndexBuffer9* ib = nullptr;       dev->GetIndices(&ib);             s.ib = GrabPtr(ib);
        dev->GetFVF(&s.fvf);

        for (UINT i = 0; i < 4; i++)
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            dev->GetStreamSource(i, &vb, &s.stream[i].offset, &s.stream[i].stride);
            s.stream[i].vb = GrabPtr(vb);
            // The replay sets instancing now; a leaked INSTANCEDATA would make the
            // game's next draws fetch per-instance and render garbage.
            dev->GetStreamSourceFreq(i, &s.stream[i].freq);
        }
        for (UINT i = 0; i < 4; i++)
        {
            IDirect3DSurface9* rt = nullptr;
            dev->GetRenderTarget(i, &rt);
            s.rt[i] = GrabPtr(rt);
        }
        IDirect3DSurface9* ds = nullptr; dev->GetDepthStencilSurface(&ds); s.ds = GrabPtr(ds);
        dev->GetViewport(&s.vp);
        dev->GetScissorRect(&s.scissor);

        for (DWORD i = 0; i < 6; i++) dev->GetClipPlane(i, s.clip[i]);
        for (DWORD st = 0; st < pipelinekeys::kFFStages; st++)
            for (int t = 0; t < 10; t++) dev->GetTextureStageState(st, kTSSWatched[t], &s.tss[st][t]);
        for (DWORD sm = 0; sm < pipelinekeys::kPSSamplers; sm++)
            for (int t = 0; t < 2; t++) dev->GetSamplerState(sm, kSampWatched[t], &s.samp[sm][t]);
        dev->GetVertexShaderConstantB(0, s.vsB, 16);
        dev->GetPixelShaderConstantB(0, s.psB, 16);
        s.valid = true;
    }

    static void VerifyStateRestored(const StateSnapshot& before)
    {
        if (!before.valid || !dev) return;
        StateSnapshot after{};
        CaptureSnapshot(after);

        int diffs = 0;
        auto note = [&](const char* what) { if (diffs++ < 24) Log("STATE LEAK: %s", what); };

        for (uint32_t i = 0; i < pipelinekeys::kNumRS; i++)
            if (before.rs[i] != after.rs[i])
            {
                char b[128];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "render state %s: %lu -> %lu",
                            pipelinekeys::kTrackedRS[i].name,
                            (unsigned long)before.rs[i], (unsigned long)after.rs[i]);
                note(b);
            }
        for (uint32_t i = 0; i < pipelinekeys::kNumSamplers; i++)
            if (before.tex[i] != after.tex[i])
            {
                char b[128];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "texture on sampler %u: %p -> %p",
                            i, before.tex[i], after.tex[i]);
                note(b);
            }
        if (before.vs != after.vs)     note("vertex shader");
        if (before.ps != after.ps)     note("pixel shader");
        if (before.decl != after.decl) note("vertex declaration");
        if (before.ib != after.ib)     note("index buffer");
        if (before.fvf != after.fvf)   note("FVF");
        for (UINT i = 0; i < 4; i++)
            if (before.stream[i].vb != after.stream[i].vb ||
                before.stream[i].offset != after.stream[i].offset ||
                before.stream[i].stride != after.stream[i].stride)
            {
                char b[96];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "stream source %u", i);
                note(b);
            }
            else if (before.stream[i].freq != after.stream[i].freq)
            {
                char b[96];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "stream %u frequency: 0x%08x -> 0x%08x",
                            i, before.stream[i].freq, after.stream[i].freq);
                note(b);
            }
        for (UINT i = 0; i < 4; i++)
            if (before.rt[i] != after.rt[i])
            {
                char b[96];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "render target %u: %p -> %p", i, before.rt[i], after.rt[i]);
                note(b);
            }
        if (before.ds != after.ds) note("depth-stencil surface");
        if (memcmp(&before.vp, &after.vp, sizeof(D3DVIEWPORT9)) != 0) note("viewport");
        if (memcmp(&before.scissor, &after.scissor, sizeof(RECT)) != 0) note("scissor rect");

        for (int i = 0; i < 6; i++)
            if (memcmp(before.clip[i], after.clip[i], sizeof(before.clip[i])) != 0)
            {
                char b[96];
                _snprintf_s(b, sizeof(b), _TRUNCATE, "clip plane %d", i);
                note(b);
            }
        for (uint32_t st = 0; st < pipelinekeys::kFFStages; st++)
            for (int t = 0; t < 10; t++)
                if (before.tss[st][t] != after.tss[st][t])
                {
                    char b[128];
                    _snprintf_s(b, sizeof(b), _TRUNCATE, "texture stage %u state %d: %lu -> %lu",
                                st, (int)kTSSWatched[t], (unsigned long)before.tss[st][t],
                                (unsigned long)after.tss[st][t]);
                    note(b);
                }
        for (uint32_t sm = 0; sm < pipelinekeys::kPSSamplers; sm++)
            for (int t = 0; t < 2; t++)
                if (before.samp[sm][t] != after.samp[sm][t])
                {
                    char b[128];
                    _snprintf_s(b, sizeof(b), _TRUNCATE, "sampler %u state %d: %lu -> %lu",
                                sm, (int)kSampWatched[t], (unsigned long)before.samp[sm][t],
                                (unsigned long)after.samp[sm][t]);
                    note(b);
                }
        if (memcmp(before.vsB, after.vsB, sizeof(before.vsB)) != 0) note("vertex bool constants b0..b15");
        if (memcmp(before.psB, after.psB, sizeof(before.psB)) != 0) note("pixel bool constants b0..b15");

        if (diffs == 0)
            Log("state verified: device handed back byte-identical across %u render states, "
                "%u samplers, shaders, streams (incl. frequency), targets, 6 clip planes, "
                "%u texture stages and both bool constant sets",
                pipelinekeys::kNumRS, pipelinekeys::kNumSamplers, pipelinekeys::kFFStages);
        else
            Log("state NOT restored: %d differences (listed above) - THIS is the black-sky bug", diffs);
    }

    // ---- device acquisition (robust across the 1.2.0.59 device globals) --
    static IDirect3DDevice9* AcquireDevice()
    {
        IDirect3DDevice9* d = nullptr;

        // 1) the fork's robustly-resolved wrapped real device
        if (RageDirect3DDevice9::m_pRealDevice && *RageDirect3DDevice9::m_pRealDevice)
            d = *RageDirect3DDevice9::m_pRealDevice;

        // 2) the grcDevice pointer used across the HDR rig
        if (!d && rage::grcDevice::ms_pD3DDevice && *rage::grcDevice::ms_pD3DDevice)
            d = *rage::grcDevice::ms_pD3DDevice;

        // 3) Ghidra/Frida-verified real device: *(*(0x017ed8d8))+0x11ac
        if (!d)
        {
            auto ppWrapper = reinterpret_cast<uintptr_t*>(RebaseVA(0x017ed8d8));
            if (ppWrapper && *ppWrapper)
            {
                auto real = reinterpret_cast<IDirect3DDevice9**>(*ppWrapper + 0x11ac);
                if (real && *real) d = *real;
            }
        }
        return d;
    }

    static void DetectBackend()
    {
        backend = Backend::Native;
        if (!dev) return;
        // A DXVK device answers QueryInterface for ID3D9VkInteropDevice. The adapter
        // name is not a substitute: it said "RADV" on Linux and nothing DXVK-specific on
        // Windows, where this used to report "native" while running on DXVK.
        FusionFixDxvkInfo dxvk;
        if (FusionFixQueryDxvk(dev, &dxvk))
        {
            backend = Backend::DXVK;
            if (dxvk.haveDriver)
                Log("vulkan driver: %s %s (api %u.%u.%u)", dxvk.driverName, dxvk.driverInfo,
                    (dxvk.apiVersion >> 22) & 0x7F, (dxvk.apiVersion >> 12) & 0x3FF, dxvk.apiVersion & 0xFFF);
            else
                Log("vulkan driver: unknown (DXVK's Vulkan library could not be queried)");
        }
    }

    static bool FormatSupportedRT(D3DFORMAT fmt)
    {
        IDirect3D9* d3d = nullptr;
        if (!dev || FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        D3DDISPLAYMODE dm{};
        d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &dm);
        HRESULT hr = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format,
                                            D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, fmt);
        d3d->Release();
        return SUCCEEDED(hr);
    }

    static IDirect3DSurface9* ScratchFor(D3DFORMAT fmt)
    {
        for (auto& s : scratchColor) if (s.fmt == fmt) return s.surf;
        IDirect3DSurface9* surf = nullptr;
        if (FormatSupportedRT(fmt) &&
            SUCCEEDED(dev->CreateRenderTarget(kRTdim, kRTdim, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr)))
            scratchColor.push_back({ fmt, surf });
        else
            scratchColor.push_back({ fmt, nullptr });
        return surf;
    }

    // -------------------------------------------------------------------
    //  Build all the fixed dummy-draw resources.
    // -------------------------------------------------------------------
    static void CreateResources()
    {
        // Dummy VB/IB (zeroed → degenerate primitives). 64 KiB covers any stride.
        if (SUCCEEDED(dev->CreateVertexBuffer(64 * 1024, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &dummyVB, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(dummyVB->Lock(0, 0, &p, 0))) { memset(p, 0, 64 * 1024); dummyVB->Unlock(); }
        }
        if (SUCCEEDED(dev->CreateIndexBuffer(1024 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &dummyIB, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(dummyIB->Lock(0, 0, &p, 0))) { memset(p, 0, 1024 * sizeof(uint16_t)); dummyIB->Unlock(); }
        }

        // Representative textures for each sampler dimension (spec dword0 driver).
        dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex2D, nullptr);
        dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texCube, nullptr);
        dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texVol, nullptr);
        // ...and the two the sampler MODES need. Either may legitimately fail to be
        // created here (a device or driver that does not take the format); the
        // replay then falls back to the plain texture, which warms the default-mode
        // pipeline instead of the mode the key asked for, and says so once.
        dev->CreateTexture(4, 4, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &texFetch4, nullptr);
        dev->CreateTexture(4, 4, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &texShadow, nullptr);

        // Six non-degenerate user clip planes.
        //
        // DXVK counts only the planes that are BOTH enabled in
        // D3DRS_CLIPPLANEENABLE and not all-zero, and packs that COUNT into spec
        // constant 0 (UpdateClipPlanes -> setClipPlaneCount, d3d9_device.cpp:6112).
        // Every plane is zero unless somebody sets one, so setting
        // CLIPPLANEENABLE without setting a plane compiles clipPlaneCount = 0 --
        // which is what both the SpecVariants "clip" variant and the recorded
        // replay were doing, making that whole axis a no-op. 473 of this rig's
        // 14543 keys and 4.75 M draws carry a clip plane. The state block the
        // pass takes is D3DSBT_ALL, which covers clip planes, so these are put
        // back with everything else.
        //
        // SETTING THEM ONCE HERE IS NOT ENOUGH, and that is the bug this round
        // exists to fix. At the boot gate the pass runs in 8 ms slices and the
        // loading screen's own D3DSBT_ALL block is applied between every one of
        // them -- clip planes included -- so from the second slice onward every
        // clip-plane key was building a clipPlaneCount = 0 pipeline, i.e. the same
        // pipeline as its no-clip sibling. That is the 786-at-one-gate,
        // 735-at-the-other difference: collapsing this one axis costs 61 of the
        // replay's 1093 identities, measured offline against these same two cache
        // files. ApplySpecState now sets the planes PER DRAW from the key, and
        // re-applies after every slice boundary. This block stays because the
        // synthetic coverage pass draws before the first recorded key does.
        for (DWORD i = 0; i < 6; i++)
        {
            const float plane[4] = { 0.0f, 0.0f, 1.0f, (float)(i + 1) };
            dev->SetClipPlane(i, plane);
        }

        // Scratch depth (D24S8) and INTZ (readable-depth FOURCC) surfaces.
        dev->CreateDepthStencilSurface(kRTdim, kRTdim, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &scratchDepthD24S8, nullptr);
        if (SUCCEEDED(dev->CreateTexture(kRTdim, kRTdim, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT, &scratchDepthINTZTex, nullptr)) && scratchDepthINTZTex)
            scratchDepthINTZTex->GetSurfaceLevel(0, &scratchDepthINTZ);

        // Pre-create the colour scratch RTs the coverage format sets need, plus
        // the runtime backbuffer format and (scope decision 2/3) SDR + both
        // present-mode formats so coverage auto-tracks the live output.
        for (auto& f : kFormatSets)
            for (int i = 0; i < f.mrt; i++) ScratchFor(f.color[i]);
        ScratchFor(bbFmt);
        if (cfg.coverPresent) { ScratchFor(D3DFMT_A2B10G10R10); ScratchFor(D3DFMT_A16B16G16R16F); }
        if (cfg.coverSdr)     { ScratchFor(D3DFMT_A8R8G8B8);    ScratchFor(D3DFMT_A2B10G10R10);    }
    }

    static IDirect3DSurface9* DepthFor(D3DFORMAT depthFmt)
    {
        if (depthFmt == D3DFMT_UNKNOWN) return nullptr;
        return scratchDepthINTZ ? scratchDepthINTZ : scratchDepthD24S8;
    }

    // Build (and cache) a vertex declaration matching a VS's input signature, so
    // "VS input ⊆ vertex layout" and DXVK takes the GPL base-pipeline path.
    static IDirect3DVertexDeclaration9* DeclForVS(const ShaderIO& io, UINT& strideOut)
    {
        D3DVERTEXELEMENT9 elems[32];
        int e = 0; WORD off = 0;
        // usageIndex counters so repeated usages get distinct semantics.
        for (auto& in : io.inputs)
        {
            if (e >= 31) break;
            D3DDECLTYPE t = UsageToDeclType(in.usage);
            elems[e].Stream = 0;
            elems[e].Offset = off;
            elems[e].Type = (BYTE)t;
            elems[e].Method = D3DDECLMETHOD_DEFAULT;
            elems[e].Usage = in.usage;
            elems[e].UsageIndex = in.index;
            off += (WORD)DeclTypeSize(t);
            e++;
        }
        if (e == 0) { // VS with no declared inputs (fullscreen/no-VB style)
            elems[e++] = { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 };
            off = 12;
        }
        elems[e] = D3DDECL_END();
        strideOut = off ? off : 12;
        IDirect3DVertexDeclaration9* decl = nullptr;
        dev->CreateVertexDeclaration(elems, &decl);
        return decl;
    }

    static void ApplyBlend(const BlendConfig* b)
    {
        if (!b) return;
        const PerTargetBlend& t0 = b->t[0];
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, t0.abe);
        dev->SetRenderState(D3DRS_SRCBLEND, t0.src);
        dev->SetRenderState(D3DRS_DESTBLEND, t0.dst);
        dev->SetRenderState(D3DRS_BLENDOP, t0.op);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, t0.sepa);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, t0.srca);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, t0.dsta);
        dev->SetRenderState(D3DRS_BLENDOPALPHA, t0.opa);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE,  b->t[0].mask);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE1, b->mrt > 1 ? b->t[1].mask : b->t[0].mask);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE2, b->mrt > 2 ? b->t[2].mask : b->t[0].mask);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE3, b->mrt > 3 ? b->t[3].mask : b->t[0].mask);
    }

    // Bind scratch RTs for a format set. Returns true if RT0 was bound.
    static bool BindFormatSet(const FormatSet* f)
    {
        if (!f) return false;
        bool ok = false;
        for (int i = 0; i < 4; i++)
        {
            IDirect3DSurface9* rt = (i < f->mrt) ? ScratchFor(f->color[i]) : nullptr;
            if (i == 0)
            {
                if (!rt) return false;
                if (SUCCEEDED(dev->SetRenderTarget(0, rt))) ok = true;
            }
            else
            {
                dev->SetRenderTarget(i, rt); // may be null → unbind
            }
        }
        dev->SetDepthStencilSurface(DepthFor(f->depth));
        return ok;
    }

    // Bind the correct-TYPE textures to the samplers a PS declares (spec dword0/2),
    // and null the rest. This is the crux of matching DXVK's SamplerType/Null keys.
    static void BindSamplers(uint32_t psUnique)
    {
        for (int s = 0; s < 16; s++)
        {
            IDirect3DBaseTexture9* t = nullptr;
            if (psUnique < shaderIO.size() && shaderIO[psUnique].usesSampler[s])
            {
                switch (shaderIO[psUnique].samplerDim[s])
                {
                case 3: t = texCube; break; // D3DSTT_CUBE
                case 4: t = texVol;  break; // D3DSTT_VOLUME
                default: t = tex2D;  break; // D3DSTT_2D / unknown
                }
            }
            dev->SetTexture(s, t);
        }
    }

    // The same thing from a declared-dimension array rather than an index into
    // the .fxc table: the engine walk reads each pixel shader's declarations out
    // of the live shader object, so it does not need a file to have been parsed
    // and cannot bind the wrong pattern when the join fails.
    static void BindSamplersDim(const uint8_t dim[16])
    {
        for (int s = 0; s < 16; s++)
        {
            IDirect3DBaseTexture9* t = nullptr;
            switch (dim[s])
            {
            case 0: t = nullptr;  break;   // not declared -> leave null
            case 3: t = texCube;  break;   // D3DSTT_CUBE
            case 4: t = texVol;   break;   // D3DSTT_VOLUME
            default: t = tex2D;   break;   // D3DSTT_2D / unknown
            }
            dev->SetTexture(s, t);
        }
    }

    static void ApplySpec(const SpecVariant& v)
    {
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, v.alphaTest);
        dev->SetRenderState(D3DRS_ALPHAFUNC, v.alphaFunc);
        dev->SetRenderState(D3DRS_ALPHAREF, 0x40);
        dev->SetRenderState(D3DRS_FOGENABLE, v.fog);
        dev->SetRenderState(D3DRS_FOGTABLEMODE, v.fog ? D3DFOG_EXP : D3DFOG_NONE);
        dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, v.clip ? 0x1 : 0x0);
    }

    // One degenerate, off-screen draw with the current pipeline state.
    static void IssueDraw(D3DPRIMITIVETYPE topo, UINT stride)
    {
        dev->SetStreamSource(0, dummyVB, 0, stride ? stride : 12);
        dev->SetIndices(dummyIB);
        // 1×1 viewport keeps everything off-screen even if a vertex survives.
        D3DVIEWPORT9 vp{ 0, 0, 1, 1, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        UINT prims = (topo == D3DPT_TRIANGLESTRIP || topo == D3DPT_TRIANGLEFAN) ? 1 : 1;
        dev->DrawPrimitive(topo, 0, prims);
    }

    // -------------------------------------------------------------------
    //  Progress overlay (W5) — reuses the built-in loading screen as backdrop.
    // -------------------------------------------------------------------
    static void CreateOverlay()
    {
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc)
        {
            D3DPRESENT_PARAMETERS pp{};
            if (SUCCEEDED(sc->GetPresentParameters(&pp)))
            {
                bbW = pp.BackBufferWidth ? pp.BackBufferWidth : bbW;
                bbH = pp.BackBufferHeight ? pp.BackBufferHeight : bbH;
                if (pp.BackBufferFormat) bbFmt = pp.BackBufferFormat;
            }
            IDirect3DSurface9* bb = nullptr;
            // NO BACKDROP AT THE BOOT GATE. The game's own loading screen is up
            // and its own thread keeps drawing it, animated, for as long as the
            // pass runs; snapshotting a frame and compositing over it is
            // exactly the hijack this round exists to remove. The progress bar
            // is drawn INTO the engine's frame instead (bootgate::DrawProgress).
            if (!gateFromBoot &&
                SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
            {
                if (SUCCEEDED(dev->CreateRenderTarget(bbW, bbH, bbFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &backdrop, nullptr)) && backdrop)
                    dev->StretchRect(bb, nullptr, backdrop, nullptr, D3DTEXF_NONE);
                bb->Release();
            }
            sc->Release();
        }

        if (cfg.overlay && !gateFromBoot)
        {
            D3DXCreateFontW(dev, 22, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &font);
            D3DXCreateFontW(dev, 34, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &fontBig);
        }
    }

    // Soften the backdrop the overlay composites over.
    //
    // The engine warm phase draws the game's OWN shaders with the game's own
    // render targets and state, so anything that leaked to the screen would
    // look like gameplay -- which is exactly the thing that must not happen on
    // a loading screen. Nothing it draws is ever presented (every draw goes
    // into a private 64x64 scratch target), but the backdrop is blurred anyway
    // so the screen visibly says "this is not the game yet" for the whole pass.
    //
    // Not the pause menu's mechanism: 06-phases could not find a dedicated blur
    // entry point at all, and the menu's softening is the post-FX chain running
    // over a frozen frame, which would mean holding RAGE's post-FX state while
    // the pass owns the device. Two StretchRects down to an eighth and back,
    // twice, cost nothing and depend on no engine address.
    static void BlurBackdrop()
    {
        if (!dev || !backdrop) return;
        const UINT w = (std::max)(1u, bbW / 8), h = (std::max)(1u, bbH / 8);
        // NB not "small": <rpcndr.h> defines that as a macro for char.
        IDirect3DSurface9* tiny = nullptr;
        if (FAILED(dev->CreateRenderTarget(w, h, bbFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &tiny, nullptr)) || !tiny)
            return;
        bool ok = true;
        for (int pass = 0; pass < 2 && ok; pass++)
        {
            ok = SUCCEEDED(dev->StretchRect(backdrop, nullptr, tiny, nullptr, D3DTEXF_LINEAR)) &&
                 SUCCEEDED(dev->StretchRect(tiny, nullptr, backdrop, nullptr, D3DTEXF_LINEAR));
        }
        tiny->Release();
        // Force the cached overlay layer to be rebuilt over the blurred copy.
        SAFE_RELEASE(overlayBase);
        overlayTitle.clear(); overlaySub.clear();
        Log("overlay: backdrop blurred (%ux%u -> %ux%u -> back, twice, %s)",
            bbW, bbH, w, h, ok ? "linear" : "FAILED - left sharp");
    }

    // Overlay bar colour. Plain SDR: the stock/upstream backbuffer is an 8-bit
    // SDR surface, so linear 0..1 maps straight to 0..255. (No HDR module
    // dependency — on an HDR-container fork this is where paper-white scaling by
    // bbFmt would go, but that is deliberately not referenced here.)
    static D3DCOLOR HdrScale(float r, float g, float b, float a)
    {
        auto c = [](float x){ return (uint8_t)std::clamp(x * 255.0f, 0.0f, 255.0f); };
        return D3DCOLOR_ARGB(c(a), c(r), c(g), c(b));
    }

    // Put the device into a state where a 2D quad is actually guaranteed to appear.
    //
    // THIS is why the progress bar "flashed in and out of existence". The overlay is
    // drawn from inside the replay loop, right after an arbitrary captured key's 58
    // render states have been applied -- and the captured data contains
    // COLORWRITEENABLE = 0, CULLMODE = CW/CCW, ALPHATESTENABLE with ALPHAREF = 100,
    // STENCILENABLE, and the synthetic pass leaves a 1x1 scissor. Each of those
    // silently discards the bar's triangles. Whether the bar appeared depended on
    // which pipeline happened to be replayed last, so it blinked.
    //
    // Restoring the game's state at the END of the pass was already correct and
    // verified; what was missing is the overlay ever establishing its OWN.
    static void SetOverlayState()
    {
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        RECT full{ 0, 0, (LONG)bbW, (LONG)bbH };
        dev->SetScissorRect(&full);
    }

    static void DrawOverlayQuad(float x0, float y0, float x1, float y1, D3DCOLOR col)
    {
        struct V { float x, y, z, rhw; D3DCOLOR c; };
        V q[4] = {
            { x0, y0, 0, 1, col }, { x1, y0, 0, 1, col },
            { x0, y1, 0, 1, col }, { x1, y1, 0, 1, col },
        };
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->SetPixelShader(nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetTexture(0, nullptr);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    }

    // Minimum gap between overlay frames, derived from the display's refresh rate.
    //
    // This used to be a hard-coded 33 ms, capping the progress bar at 30 fps on any
    // monitor and making it visibly choppy next to the rest of the game. Presenting
    // at the refresh rate is what makes it look like a loading screen rather than a
    // stuttering one; there is nothing else competing for the thread while we wait
    // on a fence, so the frames are essentially free.
    static inline int overlayIntervalMs = 16;   // sane default until the mode is read

    static void ResolveOverlayInterval()
    {
        int hz = 60;
        if (dev)
        {
            D3DDISPLAYMODE dm{};
            if (SUCCEEDED(dev->GetDisplayMode(0, &dm)) && dm.RefreshRate >= 24 && dm.RefreshRate <= 480)
                hz = (int)dm.RefreshRate;
        }
        // Cap at 60 fps. Above that the overlay stops being the thing you notice and
        // starts being work queued in front of the compilation it is reporting on --
        // every frame is a full-screen backdrop blit plus two font draws, submitted
        // to the same CS thread that builds the pipelines.
        overlayIntervalMs = 1000 / hz;
        if (overlayIntervalMs < 16) overlayIntervalMs = 16;   // 60 fps ceiling
        if (overlayIntervalMs > 33) overlayIntervalMs = 33;   // 30 fps floor
        Log("overlay refresh: display %d Hz -> one frame per %d ms (capped at 60 fps)",
            hz, overlayIntervalMs);
    }

    // Backdrop + text, rendered once per text change rather than once per frame.
    static inline IDirect3DSurface9* overlayBase = nullptr;
    static inline std::string overlayTitle, overlaySub;

    static void RebuildOverlayBase()
    {
        if (!dev) return;
        if (!overlayBase &&
            FAILED(dev->CreateRenderTarget(bbW, bbH, bbFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &overlayBase, nullptr)))
        {
            overlayBase = nullptr;
            return;
        }

        IDirect3DSurface9* prevRT = nullptr;
        dev->GetRenderTarget(0, &prevRT);
        dev->SetRenderTarget(0, overlayBase);

        if (backdrop) dev->StretchRect(backdrop, nullptr, overlayBase, nullptr, D3DTEXF_NONE);
        else          dev->Clear(0, nullptr, D3DCLEAR_TARGET, HdrScale(0.02f, 0.02f, 0.03f, 1.0f), 1.0f, 0);

        D3DVIEWPORT9 full{ 0, 0, bbW, bbH, 0.0f, 1.0f };
        dev->SetViewport(&full);

        if (font && fontBig && dev->BeginScene() == D3D_OK)
        {
            // Same reason as the bar: this runs between replayed pipelines, so the
            // device state is whatever the last captured key set.
            SetOverlayState();
            float mx = bbW * 0.12f, barW = bbW * 0.76f;
            float by = bbH * 0.86f;
            RECT rTitle{ (LONG)mx, (LONG)(by - 78), (LONG)(mx + barW), (LONG)(by - 40) };
            fontBig->DrawTextA(nullptr, overlayTitle.c_str(), -1, &rTitle, DT_LEFT | DT_NOCLIP, HdrScale(1, 1, 1, 1));

            RECT rSub{ (LONG)mx, (LONG)(by - 34), (LONG)(mx + barW), (LONG)(by - 6) };
            font->DrawTextA(nullptr, overlaySub.c_str(), -1, &rSub, DT_LEFT | DT_NOCLIP, HdrScale(0.8f, 0.85f, 0.95f, 1));
            dev->EndScene();
        }
        overlayBaseRebuilds++;

        if (prevRT) { dev->SetRenderTarget(0, prevRT); prevRT->Release(); }
    }

    static inline uint32_t overlayBaseRebuilds = 0;

    // Where the pass asks for a frame.
    //
    // On the boot gate there is nothing to present: the game's own loading
    // screen is up and its own thread is drawing it, so the right thing to do
    // is hand that thread back and let it. Everywhere else -- the fail-safe
    // loading-screen gate, and any build where fibers are unavailable -- the
    // pass presents its own frame exactly as it always did.
    static void PresentOverlay(bool force)
    {
        if (passFiber) { YieldToLoadscreen(force); return; }
        PresentOwnOverlay(force);
    }

    // Present a progress frame of our own (throttled). frac in [0,1].
    static void PresentOwnOverlay(bool force)
    {
        // Always pump — keeps the window alive even on throttled (skipped) frames.
        PumpMessages();

        auto now = std::chrono::steady_clock::now();
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - tLastPresent).count();
        if (!force && gap < overlayIntervalMs)
            return;
        // Track the cadence. A bar that "flashes in and out" is usually not dropped
        // frames -- BeginScene and Present both reported success every frame -- but
        // an uneven interval, which the eye reads as flicker. Numbers beat adjectives.
        if (overlayFrames > 0 && gap < 100000)
        {
            if (gap > overlayGapMax) overlayGapMax = (uint32_t)gap;
            if (gap < overlayGapMin) overlayGapMin = (uint32_t)gap;
            overlayGapSum += (uint64_t)gap;
            overlayGapCount++;
        }
        tLastPresent = now;

        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

        IDirect3DSurface9* prevRT = nullptr; IDirect3DSurface9* prevDS = nullptr;
        dev->GetRenderTarget(0, &prevRT);
        dev->GetDepthStencilSurface(&prevDS);
        dev->SetRenderTarget(0, bb);
        dev->SetDepthStencilSurface(nullptr);
        for (int i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);

        float frac = (float)workDone.load() / (float)(workTotal ? workTotal : 1);
        frac = std::clamp(frac, 0.0f, 1.0f);

        // Rebuild the static layer only when its TEXT changes.
        //
        // ID3DXFont::DrawTextA is by far the most expensive thing here -- it updates
        // a glyph atlas and submits its own geometry -- and it was running twice per
        // frame while the strings change a handful of times a second. The backdrop
        // and text now live on a persistent surface that is re-rendered only when
        // the text actually differs; each frame just blits that and draws three
        // quads for the bar. Everything must still be redrawn per frame because the
        // swapchain discards, but redrawing a blit is far cheaper than re-rendering
        // glyphs.
        std::string title, sub;
        {
            auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tPhase).count() / 1000.0;
            char line[256];
            // Only show an ETA once there is enough elapsed time AND progress for the
            // extrapolation to mean anything. Previously it was computed from under a
            // second of samples and displayed "ETA 0:00" next to a bar that then sat
            // still for two minutes, which reads as a crash.
            if (secs >= 3.0 && frac >= 0.03f)
            {
                double eta = secs * (1.0 - frac) / frac;
                if (eta > 5999.0) eta = 5999.0;   // clamp the mm:ss field
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%s  %3d%%    ETA %d:%02d", curWhat, (int)(frac * 100.0f),
                            (int)eta / 60, (int)eta % 60);
            }
            else
            {
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%s  %3d%%    estimating...", curWhat, (int)(frac * 100.0f));
            }
            title = curTitle.empty() ? std::string(line) : curTitle;
            sub = curLabel.empty() ? std::string("Preparing shaders...") : ("Building: " + curLabel);
        }
        if (!overlayBase || title != overlayTitle || sub != overlaySub)
        {
            overlayTitle = title;
            overlaySub = sub;
            RebuildOverlayBase();
        }

        // Per-frame: the prepared layer, then the bar.
        if (overlayBase) dev->StretchRect(overlayBase, nullptr, bb, nullptr, D3DTEXF_NONE);
        else if (backdrop) dev->StretchRect(backdrop, nullptr, bb, nullptr, D3DTEXF_NONE);
        else dev->Clear(0, nullptr, D3DCLEAR_TARGET, HdrScale(0.02f, 0.02f, 0.03f, 1.0f), 1.0f, 0);

        D3DVIEWPORT9 full{ 0, 0, bbW, bbH, 0.0f, 1.0f };
        dev->SetViewport(&full);

        if (dev->BeginScene() == D3D_OK)
        {
            SetOverlayState();
            float mx = bbW * 0.12f, barW = bbW * 0.76f;
            float by = bbH * 0.86f, barH = 14.0f;
            // track + fill + thin frame
            DrawOverlayQuad(mx - 2, by - 2, mx + barW + 2, by + barH + 2, HdrScale(0.0f, 0.0f, 0.0f, 0.55f));
            DrawOverlayQuad(mx, by, mx + barW, by + barH, HdrScale(0.10f, 0.10f, 0.12f, 0.85f));
            DrawOverlayQuad(mx, by, mx + barW * frac, by + barH, HdrScale(0.55f, 0.78f, 1.0f, 1.0f));
            dev->EndScene();
        }
        else
        {
            // Silent failure here is what made the bar freeze at 44% while work
            // continued. Say so once rather than presenting a stale frame forever.
            static bool warnedScene = false;
            if (!warnedScene) { warnedScene = true; Log("overlay BeginScene failed — progress bar cannot update"); }
        }

        // Count and check. "The bar jumps 0% -> 100% with no feedback" has now been
        // diagnosed wrong twice by reasoning about this code; a frame counter and a
        // checked HRESULT turn the next occurrence into a fact.
        HRESULT hrPresent = dev->Present(nullptr, nullptr, nullptr, nullptr);
        overlayFrames++;
        if (FAILED(hrPresent))
        {
            static bool warnedPresent = false;
            if (!warnedPresent)
            {
                warnedPresent = true;
                Log("overlay Present failed (hr=0x%08lX) - the progress bar cannot reach the screen",
                    (unsigned long)hrPresent);
            }
        }

        if (prevRT) { dev->SetRenderTarget(0, prevRT); prevRT->Release(); }
        dev->SetDepthStencilSurface(prevDS);
        if (prevDS) prevDS->Release();
        bb->Release();

        // This frame wrote state the replay caches but does not read back: the
        // overlay's own render states, and whatever ID3DXFont left behind when the
        // text changed. On the boot gate the slice driver bumps sliceEpoch and the
        // cache is dropped for exactly this reason; here there are no slices, so
        // without this the cache would survive every overlay frame and the two
        // gates would not be comparable -- which is the one thing the old gate
        // exists for. A handful of redundant Set* calls on the next draw is the
        // whole cost.
        InvalidateSpecState();
    }

    // -------------------------------------------------------------------
    //  Create pass (W3a) — every unique shader → GPL per-stage ISA compile.
    // -------------------------------------------------------------------
    static void CreateAllShaders(fxc_db* db)
    {
        uint32_t n = fxc_unique_count(db);
        vsHandles.assign(n, nullptr);
        psHandles.assign(n, nullptr);
        shaderIO.assign(n, ShaderIO{});

        uint32_t made = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            // The gate's abort has to reach the longest loops too, and on a
            // cold first run this one -- ~1800 CreateVertexShader/
            // CreatePixelShader calls -- runs before anything else.
            if (BudgetSpent())
            {
                Log("created %u of %u shaders before being asked to stop (%s)", made, n,
                    gateAbort.load() ? "the gate" : "PrecompileBudgetSeconds");
                return;
            }
            const fxc_shader* s = fxc_unique_shader(db, i);
            if (!s) { workDone++; continue; }
            ParseShaderIO(s->bytecode, s->size, s->stage == FXC_STAGE_VS, shaderIO[i]);
            if (s->stage == FXC_STAGE_VS)
                dev->CreateVertexShader(reinterpret_cast<const DWORD*>(s->bytecode), &vsHandles[i]);
            else
                dev->CreatePixelShader(reinterpret_cast<const DWORD*>(s->bytecode), &psHandles[i]);

            made++;
            workDone++;
            if ((i & 15) == 0) curLabel = "shader " + std::to_string(i + 1) + " / " + std::to_string(n);
            PresentOverlay(false); // pumps every iter; presents on the ~33ms throttle
        }
        Log("created %u shaders", made);
    }

    // -------------------------------------------------------------------
    //  Dummy-draw pass (W3b) — walk real passes, reproduce DXVK pipeline keys.
    // -------------------------------------------------------------------
    static void DummyDrawPass(fxc_db* db)
    {
        if (cfg.breadth <= 0) return;

        // Fixed opaque, filled, front-facing defaults for the non-baked (dynamic)
        // states; the baked axes are set per draw below. Established again
        // whenever the slice driver has handed the device to the engine and
        // taken it back (sliceEpoch), because that applies the loading screen's
        // whole state block over ours.
        auto fixedState = [] {
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_ZENABLE, TRUE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            RECT sc{ 0, 0, 1, 1 }; dev->SetScissorRect(&sc);
        };
        fixedState();
        uint32_t epochAtBase = sliceEpoch;

        const FormatSet* fScene = FindFormat("FMT06"); // dominant fp16 scene + depth
        const BlendConfig* bOpaque = FindBlend("BL00");
        const BlendConfig* bAlpha  = FindBlend("BL03");

        int specN = (cfg.breadth >= 2) ? kSpecVariantCount : 1;

        // ---- (A) per-pass shader/spec coverage into the dominant scene format.
        // Iterating real (vs,ps) passes builds each shader set's base pipeline +
        // stage spec variants with the correct sampler-type bindings.
        uint32_t nEff = fxc_effect_count(db);
        for (uint32_t e = 0; e < nEff; e++)
        {
            const fxc_effect* ef = fxc_get_effect(db, e);
            if (!ef) continue;
            for (uint32_t t = 0; t < ef->technique_count; t++)
            {
                const fxc_technique& tech = ef->techniques[t];
                for (uint32_t p = 0; p < tech.pass_count; p++)
                {
                    const fxc_pass& pass = tech.passes[p];
                    if (BudgetSpent()) return;      // the gate's abort, and the budget
                    if (sliceEpoch != epochAtBase) { fixedState(); epochAtBase = sliceEpoch; }
                    if (pass.vs_unique == FXC_NO_SHADER || pass.vs_unique >= vsHandles.size()) { workDone += kPassW; continue; }
                    IDirect3DVertexShader9* vs = vsHandles[pass.vs_unique];
                    IDirect3DPixelShader9*  ps = (pass.ps_unique != FXC_NO_SHADER && pass.ps_unique < psHandles.size())
                                               ? psHandles[pass.ps_unique] : nullptr;
                    if (!vs) { workDone += kPassW; continue; }

                    UINT stride = 12;
                    IDirect3DVertexDeclaration9* decl = DeclForVS(shaderIO[pass.vs_unique], stride);

                    dev->SetVertexShader(vs);
                    dev->SetPixelShader(ps);
                    dev->SetVertexDeclaration(decl);
                    BindSamplers(pass.ps_unique);

                    if (BindFormatSet(fScene))
                    {
                        for (int sv = 0; sv < specN; sv++)
                        {
                            ApplySpec(kSpecVariants[sv]);
                            // opaque + (for breadth) one alpha-blended variant
                            ApplyBlend(bOpaque);
                            IssueDraw(D3DPT_TRIANGLELIST, stride);
                            if (cfg.breadth >= 2 && sv == 0)
                            {
                                ApplyBlend(bAlpha);
                                IssueDraw(D3DPT_TRIANGLELIST, stride);
                            }
                        }
                    }
                    SAFE_RELEASE(decl);
                    workDone += kPassW;
                    if (ef->name) curLabel = ef->name;
                    // Present every pass: pumps messages, advances the bar smoothly,
                    // and (on native D3D9) periodically flushes so the driver actually
                    // compiles the batched draws' ISA instead of deferring it all.
                    PresentOverlay(false);
                }
            }
        }

        // ---- (B) state-library warm: cover every observed (format,blend,topo)
        // tuple so the non-scene fragment-output libraries (G-buffer MRT, shadow
        // R32F, luminance G16R16F, 10-bit present, L8 mask...) are all built.
        // Uses a simple position-only VS/PS pairing; VI/FO libraries are shader-
        // independent under GPL, so any valid draw warms them.
        IDirect3DVertexShader9* anyVS = nullptr; IDirect3DPixelShader9* anyPS = nullptr;
        uint32_t anyVSidx = 0;
        for (uint32_t i = 0; i < vsHandles.size(); i++) if (vsHandles[i]) { anyVS = vsHandles[i]; anyVSidx = i; break; }
        for (uint32_t i = 0; i < psHandles.size(); i++) if (psHandles[i]) { anyPS = psHandles[i]; break; }
        if (anyVS)
        {
            UINT stride = 12;
            IDirect3DVertexDeclaration9* decl = DeclForVS(shaderIO[anyVSidx], stride);
            dev->SetVertexShader(anyVS);
            dev->SetPixelShader(anyPS);
            dev->SetVertexDeclaration(decl);
            BindSamplers(0);
            ApplySpec(kSpecVariants[0]);
            for (auto& tp : kTuples)
            {
                if (BudgetSpent()) break;           // decl is live here, so break, not return
                if (sliceEpoch != epochAtBase)
                {
                    // Everything (B) set up once, back: the engine's block has
                    // been over the device since the last tuple.
                    fixedState();
                    dev->SetVertexShader(anyVS);
                    dev->SetPixelShader(anyPS);
                    dev->SetVertexDeclaration(decl);
                    BindSamplers(0);
                    ApplySpec(kSpecVariants[0]);
                    epochAtBase = sliceEpoch;
                }
                const FormatSet* f = FindFormat(tp.fmt);
                const BlendConfig* b = FindBlend(tp.blend);
                if (BindFormatSet(f)) { ApplyBlend(b); IssueDraw(tp.topo, stride); }
                workDone += kPassW;
                curLabel = "state coverage";
                PresentOverlay(false);
            }
            // Present-format coverage (scope decision 3): both HDR output modes.
            if (cfg.coverPresent)
            {
                for (D3DFORMAT pf : { D3DFMT_A2B10G10R10, D3DFMT_A16B16G16R16F })
                {
                    IDirect3DSurface9* rt = ScratchFor(pf);
                    if (rt && SUCCEEDED(dev->SetRenderTarget(0, rt)))
                    {
                        for (int i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
                        dev->SetDepthStencilSurface(nullptr);
                        ApplyBlend(bOpaque); IssueDraw(D3DPT_TRIANGLELIST, stride);
                    }
                    workDone += kPassW;
                }
            }
            SAFE_RELEASE(decl);
        }
        PresentOverlay(true);
    }

    // -------------------------------------------------------------------
    //  Replay pass — rebuild the pipeline keys the game ACTUALLY used.
    //
    //  This is the answer to why the synthetic pass measured as useless: with
    //  graphics-pipeline-library off, a pipeline is keyed on the whole state
    //  vector, and synthetic draws match the game's in almost none of those
    //  fields, so they build different objects. shadercapture.ixx records the
    //  real keys during play; this replays them, which by construction produces
    //  the same keys rather than lookalikes.
    // -------------------------------------------------------------------

    static inline std::vector<pipelinekeys::KeyRecord> replayRecs;
    static inline uint32_t replayBaseCount = 0;   // leading drawList entries that are base identities
    static inline std::vector<std::vector<D3DVERTEXELEMENT9>> replayDecls;
    static inline std::vector<IDirect3DVertexDeclaration9*> replayDeclObjs;
    static inline std::unordered_map<uint64_t, IDirect3DVertexShader9*> vsByHash;
    static inline std::unordered_map<uint64_t, IDirect3DPixelShader9*>  psByHash;

    // Which sampler slots each pixel shader actually DECLARES, by bytecode hash.
    // DXVK's sampler-type spec constant only encodes declared slots, so two keys
    // differing on a slot the shader never reads are the same pipeline. Recording
    // all 20 slots blindly took the distinct-key count from 372 to 4452 against a
    // driver that reported 663 real pipelines.
    static inline std::unordered_map<uint64_t, std::array<bool, 16>> psSamplerUse;

    // The same thing for VERTEX shaders and their four vertex-texture slots
    // (D3DVERTEXTEXTURESAMPLER0..3 == KeyRecord::samplerType[16..19]). DXVK builds
    // one `nullOrUnusedMask = ~usedSamplerMask | ~usedTextureMask` from BOTH stages'
    // declarations (d3d9_device.cpp:7384,7466) and feeds it to setVsSamplers, so a
    // texture bound to a slot the vertex shader never declares changes nothing about
    // the pipeline.
    //
    // It was left unmasked because nothing parsed VS declarations, and the cost of
    // that is large: 14524 of this rig's 14805 recorded keys carry vertex sampler
    // slot 19 -- leftover device state from an earlier draw -- while only five of the
    // install's 853 vertex shaders declare it. Measured offline over the three cache
    // files the replay loads, masking takes the set from 2428 unique pipelines to
    // 1098, with the 602 base identities unchanged: ~1330 duplicate draws.
    static inline std::unordered_map<uint64_t, std::array<bool, pipelinekeys::kVSSamplers>> vsSamplerUse;

    // And the b# registers each shader reads, under the same hash. DXVK masks the
    // device's 16 bool constants with this before they become a spec constant
    // (d3d9_device.cpp:7443, 7459), so this is what keeps the recorded b# from
    // splitting keys on registers nothing looks at. An absent entry means the
    // bytecode was never seen or could not be walked: the key then keeps the raw
    // b#, which over-counts rather than under-warms, exactly as the sampler masks do.
    static inline std::unordered_map<uint64_t, uint32_t> shaderBoolMask;

    // Record a shader's declared sampler slots under its bytecode hash, from whichever
    // source resolved it first. One helper so the five call sites (the .fxc database,
    // the engine's own programs, the runtime registry, the cache's bytecode and our own
    // resources) cannot drift apart.
    static void NoteSamplerUse(uint64_t hash, bool isVS, const ShaderIO& io)
    {
        if (!hash) return;
        if (io.boolMaskValid) shaderBoolMask.emplace(hash, io.boolMask);
        if (isVS)
        {
            if (vsSamplerUse.count(hash)) return;
            std::array<bool, pipelinekeys::kVSSamplers> use{};
            for (uint32_t s = 0; s < pipelinekeys::kVSSamplers; s++) use[s] = io.usesSampler[s];
            vsSamplerUse.emplace(hash, use);
        }
        else
        {
            if (psSamplerUse.count(hash)) return;
            std::array<bool, 16> use{};
            for (int s = 0; s < 16; s++) use[s] = io.usesSampler[s];
            psSamplerUse.emplace(hash, use);
        }
    }

    // Not constexpr: a static data member of this class cannot be constant-
    // initialised from a member function of the same class, because the function is
    // not available until the class is complete. Resolved once at DLL load instead.
    static int RSIndexOf(D3DRENDERSTATETYPE rs)
    {
        for (uint32_t i = 0; i < pipelinekeys::kNumRS; i++)
            if (pipelinekeys::kTrackedRS[i].rs == rs) return (int)i;
        return -1;
    }
    static inline const int kRS_AlphaTestEnable = RSIndexOf(D3DRS_ALPHATESTENABLE);
    static inline const int kRS_AlphaFunc       = RSIndexOf(D3DRS_ALPHAFUNC);
    static inline const int kRS_FogEnable       = RSIndexOf(D3DRS_FOGENABLE);
    static inline const int kRS_ClipPlanes      = RSIndexOf(D3DRS_CLIPPLANEENABLE);
    static inline const int kRS_FogVertexMode   = RSIndexOf(D3DRS_FOGVERTEXMODE);
    static inline const int kRS_FogTableMode    = RSIndexOf(D3DRS_FOGTABLEMODE);
    static inline const int kRS_PointSprite     = RSIndexOf(D3DRS_POINTSPRITEENABLE);
    static inline const int kRS_PointScale      = RSIndexOf(D3DRS_POINTSCALEENABLE);
    static inline const int kRS_SpecularEnable  = RSIndexOf(D3DRS_SPECULARENABLE);
    // DxvkRsInfo / DxvkMsInfo: rasterizer and multisample state that is BAKED into
    // DxvkGraphicsPipelineStateInfo (dxvk_context.cpp:2956, 2972), not dynamic.
    static inline const int kRS_ShadeMode       = RSIndexOf(D3DRS_SHADEMODE);
    static inline const int kRS_FillMode        = RSIndexOf(D3DRS_FILLMODE);
    static inline const int kRS_MsaaEnable      = RSIndexOf(D3DRS_MULTISAMPLEANTIALIAS);
    static inline const int kRS_MsaaMask        = RSIndexOf(D3DRS_MULTISAMPLEMASK);
    static inline const int kRS_BlendEnable     = RSIndexOf(D3DRS_ALPHABLENDENABLE);
    static inline const int kRS_SeparateAlpha   = RSIndexOf(D3DRS_SEPARATEALPHABLENDENABLE);
    static inline const int kRS_ColorBlend[3]   = { RSIndexOf(D3DRS_SRCBLEND), RSIndexOf(D3DRS_DESTBLEND),
                                                    RSIndexOf(D3DRS_BLENDOP) };
    static inline const int kRS_AlphaBlend[3]   = { RSIndexOf(D3DRS_SRCBLENDALPHA), RSIndexOf(D3DRS_DESTBLENDALPHA),
                                                    RSIndexOf(D3DRS_BLENDOPALPHA) };
    static inline const int kRS_WriteMask[4]    = { RSIndexOf(D3DRS_COLORWRITEENABLE), RSIndexOf(D3DRS_COLORWRITEENABLE1),
                                                    RSIndexOf(D3DRS_COLORWRITEENABLE2), RSIndexOf(D3DRS_COLORWRITEENABLE3) };

    static uint32_t RSOr0(const pipelinekeys::KeyRecord& k, int idx) { return idx >= 0 ? k.rs[idx] : 0u; }

    // Replay identity comes in two tiers, because DXVK compiles in two tiers.
    //
    // TIER 1, the BASE pipeline: shaders, vertex input and fragment output. This is
    // what DXVK links from separately built libraries on a GPL driver, and what it
    // compiles from scratch, synchronously, on the draw that first needs it when the
    // driver cannot fast-link (dxvk_graphics.cpp, getPipelineHandle ->
    // createBasePipeline). AMD's Windows compiler is suspected to be in that second
    // group for every D3D9 pixel shader (zero-stutter-research.md, finding A), so on
    // that driver each base identity the replay misses is a stall in gameplay.
    //
    // The previous key was oriented the other way round. It dropped blend state and
    // write masks as "dynamic" -- they are not, they are the fragment-output library
    // -- and it never saw instancing at all. Measured offline against the raw
    // captures, that missed 16 base identities on the Windows capture and 32 on the
    // Linux one, plus every instanced draw.
    //
    // Blend is normalised the way D3D9 semantics allow: factors only matter while
    // blending is on and something is written, alpha factors only with separate
    // alpha. DXVK normalises further (write masks against the format, pass-through
    // blending); that is left to DXVK, which gets the full recorded state at replay.
    // Keying finer than DXVK costs a duplicate draw it resolves for free; keying
    // coarser costs a compile in gameplay.
    static uint64_t ReplayBaseKey(const pipelinekeys::KeyRecord& k)
    {
        struct Base
        {
            uint64_t vs, ps;
            uint32_t decl, fvf, prim;
            uint32_t streamFreq[pipelinekeys::kMaxStreams];
            uint32_t rt[pipelinekeys::kMaxRT], ds, ms, msq;
            uint32_t writeMask[pipelinekeys::kMaxRT];
            uint32_t blendEnable, color[3], alpha[3];
            // DxvkRsInfo and DxvkMsInfo, both members of
            // DxvkGraphicsPipelineStateInfo and both compared byte for byte.
            uint32_t flatShading, polygonMode, sampleCount, sampleMask;
        } b;
        memset(&b, 0, sizeof(b));   // hashed as bytes, so padding must be zero too

        b.vs = k.vsHash; b.ps = k.psHash;
        b.decl = k.declIndex; b.fvf = k.fvf; b.prim = k.primType;
        for (uint32_t s = 0; s < pipelinekeys::kMaxStreams; s++) b.streamFreq[s] = k.streamFreq[s];
        for (uint32_t i = 0; i < pipelinekeys::kMaxRT; i++) b.rt[i] = k.rtFmt[i];
        b.ds = k.dsFmt; b.ms = k.msType; b.msq = k.msQuality;

        bool writes = false;
        for (uint32_t i = 0; i < pipelinekeys::kMaxRT; i++)
            if (k.rtFmt[i])
            {
                b.writeMask[i] = RSOr0(k, kRS_WriteMask[i]) & 0xFu;
                writes |= b.writeMask[i] != 0;
            }
        if (writes && RSOr0(k, kRS_BlendEnable))
        {
            b.blendEnable = 1;
            const bool separate = RSOr0(k, kRS_SeparateAlpha) != 0;
            for (int j = 0; j < 3; j++)
            {
                b.color[j] = RSOr0(k, kRS_ColorBlend[j]);
                b.alpha[j] = separate ? RSOr0(k, kRS_AlphaBlend[j]) : b.color[j];
            }
        }

        // The rasterizer and multisample words, which the key dropped as "dynamic"
        // and are not: DxvkContext::setRasterizerState packs depth clip, polygon
        // mode, SAMPLE COUNT, conservative mode, FLAT SHADING and line mode into
        // DxvkRsInfo and puts it in m_state.gp.state.rs; setMultisampleState does
        // the same with the sample mask. Cull mode and front face really are
        // dynamic and stay out.
        //
        // D3DRS_MULTISAMPLEANTIALIAS is the exception, and it is worth being exact
        // about why, because two DIFFERENT DXVK counters answer differently and the
        // one that matters here is the second.
        //
        // Its D3D9 default is TRUE, not FALSE (d3d9_device.cpp:8768 sets it in the
        // device's own reset), and it really does vary on this install: 0 on 561 of
        // the 14,543 keys, with 95 of the 1093 identities carrying both values.
        //
        // In DXVK's pipeline STATE it is keyed: BindRasterizerState maps it to
        // sampleCount 0 ("take it from the render pass") or 1, that is a five-bit
        // field of DxvkRsInfo, DxvkRsInfo is a member of
        // DxvkGraphicsPipelineStateInfo, and that struct is compared with
        // bit::bcmpeq -- so two draws differing only in it are two entries in
        // m_pipelines and DXVK's "Graphics pipelines" HUD number counts both.
        //
        // In what DXVK COMPILES it is not. The VkPipeline is looked up by
        // DxvkGraphicsPipelineFastInstanceKey, which carries no rs.sampleCount of
        // its own: the word reaches msInfo.rasterizationSamples only when
        // state.ms.sampleCount() is ZERO (dxvk_graphics.cpp:356-361), and ms's
        // sample count comes from the framebuffer (dxvk_context.cpp:7660), so it is
        // zero only for a render pass with no attachments at all. Both values
        // therefore produce a byte-identical fast key, one m_fastPipelines entry and
        // one vkCreateGraphicsPipelines call. That is not a property of this driver;
        // it is a property of the D3D9 front end, and it needs no extension.
        //
        // Measured, which is what found it, same binary, same gate, cold both times
        // (2026-09-21, /tmp/ff-results-keygap/full/run1 and run2): keying on it draws
        // 1188 pipelines, NOT keying on it draws 1093, and both end at 786 -- our
        // counter is the vkCreateGraphicsPipelines hook, i.e. compiles, not cache
        // entries. So the axis is 95 draws of loading time in exchange for 95 state
        // entries DXVK would otherwise add during gameplay: a hash insert each, no
        // compile. It is off because compiles are what stutter.
        // PrecompileKeySampleCount = 1 turns it back on for one run.
        b.flatShading = RSOr0(k, kRS_ShadeMode) == D3DSHADE_FLAT;
        b.polygonMode = RSOr0(k, kRS_FillMode);
        b.sampleCount = cfg.keySampleCount ? (RSOr0(k, kRS_MsaaEnable) ? 0u : 1u) : 1u;
        // DXVK only honours the mask when RT0 is multisampled above NONMASKABLE
        // (m_validSampleMask, d3d9_device.cpp:1762); otherwise it forces 0xffff,
        // so keying on it there would split for nothing.
        b.sampleMask = (k.msType > D3DMULTISAMPLE_NONMASKABLE)
                     ? (RSOr0(k, kRS_MsaaMask) & 0xFFFFu) : 0xFFFFu;
        return pipelinekeys::Fnv1a(&b, sizeof(b));
    }

    // TIER 2, the full key: the base plus what DXVK turns into spec constants --
    // alpha test, fog, clip planes, sampler types. On a GPL driver these never cost
    // a synchronous compile (the base pipeline reads them from a buffer), but each
    // one is its own optimized pipeline DXVK builds in the background, which is
    // what replaying them warms.
    //
    // Measured 2026-09-18: replaying 6994 strict keys produced 634 Vulkan
    // pipelines -- the strict record is ~10x finer than the driver's identity. What
    // is dropped here (cull mode, depth/stencil ops, and the rest) is dynamic state
    // or fixed-function configuration these shader-based draws never run.
    // maskVS = false reproduces the key as it was before vertex sampler slots were
    // masked, which is only used to measure what the mask is worth on this install.
    //
    // 2026-09-21: the four fields this started with were not all of D3D9SpecData,
    // and the rest -- the b# registers, the clip-plane COUNT, the projected mask,
    // the sampler MODES and the fixed-function stage block -- were left to whatever
    // the device happened to be holding. That is what let the same 1093 draws build
    // 786 pipelines at one gate and 735 at another. They are in the key now, each
    // masked the way DXVK masks it, so a field a shader never looks at still splits
    // nothing.
    static uint64_t ReplayPipelineKey(const pipelinekeys::KeyRecord& k, bool maskVS = true)
    {
        struct Reduced
        {
            uint64_t base;
            uint32_t alphaEnable, alphaFunc, fogEnable, clipPlanes;
            uint8_t  samplerType[pipelinekeys::kNumSamplers];
            // spec ID 1, the rest of the fog dword. Only reached when fog is on,
            // because DXVK forces both modes to D3DFOG_NONE when it is not.
            uint32_t fogVertex, fogPixel;
            // spec ID 0 / 2: point mode is only evaluated for a POINTLIST draw,
            // and enablePointScale additionally needs a fixed-function vertex path.
            uint32_t pointSprite, pointScale;
            uint32_t specular;                 // spec ID 2, enableGlobalSpecular
            uint8_t  projMask;                 // spec ID 2, masked to the used slots
            uint16_t vsBools, psBools;         // spec IDs 3 / 6, masked per shader
            uint8_t  samplerMode[pipelinekeys::kNumSamplers];
            pipelinekeys::FFStage ffStage[pipelinekeys::kFFStages];
        } r;
        memset(&r, 0, sizeof(r));

        r.base = ReplayBaseKey(k);
        r.alphaEnable = RSOr0(k, kRS_AlphaTestEnable);
        r.alphaFunc   = RSOr0(k, kRS_AlphaFunc);
        r.fogEnable   = RSOr0(k, kRS_FogEnable);
        // The clip-plane COUNT is what DXVK specialises on, not the enable mask:
        // a plane that is enabled but zero does not count. Keying on the mask both
        // split keys that are one pipeline and merged keys that are two.
        r.clipPlanes  = k.clipPlaneCount;

        if (r.fogEnable)
        {
            r.fogVertex = RSOr0(k, kRS_FogVertexMode);
            r.fogPixel  = RSOr0(k, kRS_FogTableMode);
        }
        if (k.primType == D3DPT_POINTLIST)
        {
            r.pointSprite = RSOr0(k, kRS_PointSprite) != 0;
            r.pointScale  = (!k.vsHash && RSOr0(k, kRS_PointScale)) ? 1u : 0u;
        }
        r.specular = RSOr0(k, kRS_SpecularEnable) != 0;

        // The stage ops only reach a pipeline with no pixel shader; a programmable
        // one never declares those spec constants, so DXVK zeroes them.
        //
        // Canonicalised here and nowhere else, so every source agrees: the capture
        // writes a disabled stage all-zero and a widened pre-v3 record writes D3D9's
        // defaults, and a key that told those apart would draw the same pipeline
        // twice. It also drops the arguments the op does not consume, which DXVK
        // discards too.
        if (!k.psHash)
        {
            memcpy(r.ffStage, k.ffStage, sizeof(r.ffStage));
            pipelinekeys::CanonicalFFStages(r.ffStage);
        }

        // b#, masked to the registers each stage's shader actually reads.
        {
            auto masked = [](uint64_t hash, uint16_t bits) -> uint16_t {
                if (!hash) return 0;
                auto it = shaderBoolMask.find(hash);
                return it == shaderBoolMask.end() ? bits : (uint16_t)(bits & it->second);
            };
            r.vsBools = masked(k.vsHash, k.vsBools);
            r.psBools = masked(k.psHash, k.psBools);
        }

        // Mask each stage's sampler slots to the ones that stage's shader declares:
        // an undeclared slot is in DXVK's nullOrUnusedMask however the device state
        // left it, so keeping it would split one pipeline into several keys. Where a
        // shader cannot be resolved the slots stay as recorded, which over-counts
        // rather than under-warms.
        const std::array<bool, 16>* use = nullptr;
        if (auto it = psSamplerUse.find(k.psHash); it != psSamplerUse.end()) use = &it->second;
        for (uint32_t i = 0; i < pipelinekeys::kPSSamplers; i++)
        {
            const bool used = (!use || (*use)[i]);
            r.samplerType[i] = used ? k.samplerType[i] : 0;
            // The MODE rides the same mask -- DXVK clears it for null or unused
            // slots (D3D9SpecData::updateSamplers: `newModes & ~nullTypes`).
            r.samplerMode[i] = (used && k.samplerType[i]) ? k.samplerMode[i] : 0;
            // samplerProjMask is `projected & bound & declared`, and the shader
            // only consults it for stages 0..7.
            if (i < pipelinekeys::kFFStages && used && k.samplerType[i] && (k.projMask & (1u << i)))
                r.projMask |= (uint8_t)(1u << i);
        }

        const std::array<bool, pipelinekeys::kVSSamplers>* vuse = nullptr;
        if (maskVS)
            if (auto it = vsSamplerUse.find(k.vsHash); it != vsSamplerUse.end()) vuse = &it->second;
        for (uint32_t i = 0; i < pipelinekeys::kVSSamplers; i++)
        {
            const uint32_t slot = pipelinekeys::kPSSamplers + i;
            const bool used = (!vuse || (*vuse)[i]);
            r.samplerType[slot] = used ? k.samplerType[slot] : 0;
            r.samplerMode[slot] = (used && k.samplerType[slot]) ? k.samplerMode[slot] : 0;
        }

        return pipelinekeys::Fnv1a(&r, sizeof(r));
    }

    // The cache bundle for THIS graphics configuration, matching what the capture
    // writes. Keys carry render-target formats, so replaying another setup's bundle
    // would warm pipelines this one never uses and miss the ones it needs.
    static std::string ModuleDir()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(hSelf, path, MAX_PATH);
        std::string s(path);
        auto slash = s.find_last_of("\\/");
        return (slash == std::string::npos) ? std::string() : s.substr(0, slash + 1);
    }

    static std::string KeyFilePath()
    {
        std::string dir = ModuleDir();

        uint32_t w = bbW, h = bbH, fmt = (uint32_t)bbFmt;
        if (dev)
        {
            IDirect3DSwapChain9* sc = nullptr;
            if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc)
            {
                D3DPRESENT_PARAMETERS pp{};
                if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.BackBufferWidth)
                {
                    w = pp.BackBufferWidth; h = pp.BackBufferHeight;
                    fmt = (uint32_t)pp.BackBufferFormat;
                }
                sc->Release();
            }
        }
        CIniReader ini("");
        int msaa = ini.ReadInteger("EXPERIMENTAL", "ReflectionMSAAQuality", 0);

        (void)w; (void)h;   // resolution provably does not affect the keys, see BundleName
        return dir + pipelinekeys::CacheName(fmt, msaa);
    }

    // A shipped baseline, so the very first launch on a machine that has never
    // captured anything still warms something. Without it this feature does nothing
    // until the user has already played through the stutter it exists to remove,
    // which makes it a personal tool rather than a shipped one.
    //
    // Kept under its own name so the capture, which writes the per-configuration
    // bundle, can never overwrite it, and so it is always merged IN ADDITION to
    // whatever the user has recorded rather than being an either/or.
    static std::string BaselinePath() { return ModuleDir() + "FusionFix.pipelinecache.baseline.bin"; }

    // Provenance of the cache we loaded, carried into an exported baseline so the
    // shipped artifact says what it was built against -- the shader directory above
    // all, which is the field a merge tool buckets on.
    static inline std::string exportShaderDir = "unknown";
    static inline pipelinekeys::CacheMeta exportMeta{};
    static inline std::string exportStrings[pipelinekeys::kMetaStringCount];

    // Dedup state shared by every file merged into the replay set.
    static inline std::unordered_map<uint64_t, uint32_t> replayDeclByHash;
    static inline std::unordered_map<uint64_t, uint32_t> replayKeyIndex;
    // Bytecode carried by the same container as the keys, merged across this PC's own
    // capture and the shipped baseline -- never from drop-ins (see MergeKeyFile).
    static inline pipelinekeys::ShaderBlobMap replayBlobs;
    static inline size_t replayBlobBytes = 0;

    // Where each replayed key came from. A file's line at load says whether it was
    // read; this is what it then warmed, which is the number that says whether
    // dropping it in was worth anything -- and, for a file from another install or
    // configuration, how much of it this one cannot use. A key several files hold is
    // counted for the first one merged: the capture, then the baseline, then drop-ins.
    struct ReplaySource
    {
        std::string label;
        bool dropIn = false;
        // The container version the file was WRITTEN at. A file older than v3 has
        // its specialisation fields filled in by WidenToV3 rather than recorded, so
        // any count of "how many keys carry a b# register" over those records is
        // reporting the widening, not the game. Kept here so the log can say which.
        uint32_t version = 0;
        uint32_t added = 0, drawn = 0, samePipeline = 0, noShader = 0, noDecl = 0, noRT = 0;
    };
    static inline std::vector<ReplaySource> replaySources;
    static inline std::vector<uint32_t> replaySrc;   // parallel to replayRecs

    // What all files together may add, whatever they claim: the reader bounds each
    // file, this bounds the sum. A real capture is ~15,000 keys and ~50 KB of bytecode.
    static constexpr size_t kMaxReplayKeys      = 200000;     // ~85 MB of records at v3
    static constexpr size_t kMaxReplayBlobBytes = 4u << 20;

    static void LogStr(const std::string& s) { pipelinekeys::LogLine("[ShaderPrecompile] ", s.c_str()); }

    // Returns false (and leaves replayRecs empty) for any malformed or absent file,
    // so the caller can fall back to the synthetic pass rather than doing nothing.
    static bool LoadKeyFile()
    {
        replayRecs.clear();
        replayDecls.clear();
        replayDeclByHash.clear();
        replayKeyIndex.clear();
        replayBlobs.clear();
        replayBlobBytes = 0;
        replaySources.clear();
        replaySrc.clear();

        // Every file is read through d3d9cache::LoadFile: whole, bounded, validated,
        // and de-duplicated by content hash, so the same file under two names (this
        // PC's snapshot, a copy from another PC) is used once.
        std::unordered_map<uint64_t, std::string> seen;
        const std::string shaderDir = d3d9cache::ActiveShaderDir();

        // The user's own capture first: it is the one recorded at THIS graphics
        // configuration, and merging it first means its draw counts drive the
        // most-used-first ordering rather than a stranger's.
        const std::string capture = KeyFilePath();
        bool any = MergeKeyFile(d3d9cache::Wide(capture), "capture", shaderDir, seen, false);
        // Its snapshot in d3d9cache\ holds it as it was at launch. If capture has
        // flushed since, the file differs, but the snapshot is still this file.
        if (any)
        {
            const std::wstring leaf = d3d9cache::Lower(d3d9cache::Wide(capture.substr(capture.find_last_of("\\/") + 1)));
            if (auto it = d3d9cache::OwnLaunchHashes().find(leaf); it != d3d9cache::OwnLaunchHashes().end())
                seen.emplace(it->second, "capture");
        }
        any |= MergeKeyFile(d3d9cache::Wide(BaselinePath()), "baseline", shaderDir, seen, false);

        // Cache files from the player's other PCs: plugins\d3d9cache\, top level, any
        // name ending in .bin. Read where they are, never moved or written, and never
        // merged into this PC's own capture. Keys recorded at another configuration
        // are warmed too, where this device can build them (see ReplayPass).
        const d3d9cache::DropIns drop = d3d9cache::FindDropIns();
        if (drop.subdirs)
            LogStr("replay: d3d9cache\\ holds " + std::to_string(drop.subdirs) + " folder(s) (e.g. '" +
                   d3d9cache::Utf8(drop.firstSubdir) + "') - they are not read; put the .bin files directly "
                   "in plugins\\d3d9cache\\");
        // Once per launch, whichever of this and vkcapture finds them first.
        pipelinekeys::HintMisplaced("[ShaderPrecompile] ", pipelinekeys::kFozInD3d9Cache, drop.foz,
                                    d3d9cache::Utf8(drop.firstFoz));
        pipelinekeys::HintMisplaced("[ShaderPrecompile] ", pipelinekeys::kBinInPipelineCache, drop.misplacedBins,
                                    d3d9cache::Utf8(drop.firstMisplaced));
        for (const auto& name : drop.other)
            LogStr("replay: d3d9cache\\" + d3d9cache::Utf8(name) + ": skipped - not a .bin cache file");
        for (const auto& path : drop.files)
            any |= MergeKeyFile(path, "d3d9cache\\" + d3d9cache::Utf8(path.substr(path.find_last_of(L"\\/") + 1)),
                                shaderDir, seen, true);

        Log("replay: loaded %zu keys, %zu declarations from %zu file(s)", replayRecs.size(), replayDecls.size(),
            replaySources.size());
        return any && !replayRecs.empty();
    }

    // Merge one cache file into the replay set, after d3d9cache::LoadFile has
    // validated it: one line per file, whatever happens to it. Declaration indices
    // are file-local so they are remapped. A key the capture and the baseline share
    // is folded into one record with the draw counts summed, which keeps "warm the
    // most-used first" meaningful across the two; a drop-in's counts fold in as the
    // MAX, because drop-ins overlap each other and this PC's own capture (they go
    // back and forth between PCs), and summing would count that history again for
    // every copy.
    static bool MergeKeyFile(const std::wstring& path, const std::string& what, const std::string& shaderDir,
                             std::unordered_map<uint64_t, std::string>& seen, bool dropIn)
    {
        using namespace pipelinekeys;

        CacheContents c;
        const d3d9cache::FileResult f = d3d9cache::LoadFile(path, what, shaderDir, seen, c);
        if (f.verdict == d3d9cache::Verdict::Absent)
        {
            LogStr("replay: " + what + ": none at " + d3d9cache::Utf8(path));
            return false;
        }
        std::string line = "replay: " + what + ": " + d3d9cache::Describe(f, c);
        if (f.verdict != d3d9cache::Verdict::Accepted)
        {
            LogStr(line);
            return false;
        }

        const size_t recsBefore = replayRecs.size();
        const uint32_t src = (uint32_t)replaySources.size();
        replaySources.push_back({ what, dropIn, f.parse.version });

        // File-local declaration index -> our index.
        std::vector<uint32_t> remap(c.decls.size(), 0);
        for (size_t i = 0; i < c.decls.size(); i++)
        {
            uint64_t dh = Fnv1a(c.decls[i].data(), c.decls[i].size() * sizeof(D3DVERTEXELEMENT9));
            if (auto it = replayDeclByHash.find(dh); it != replayDeclByHash.end())
            {
                remap[i] = it->second;
            }
            else
            {
                remap[i] = (uint32_t)replayDecls.size();
                replayDeclByHash.emplace(dh, remap[i]);
                replayDecls.push_back(c.decls[i]);
            }
        }

        size_t merged = 0, full = 0;
        for (auto& rec : c.keys)
        {
            KeyRecord k = rec;
            if (k.declIndex != kDeclNone)
                k.declIndex = remap[k.declIndex];   // in range: the reader checked it

            uint64_t kh = Fnv1a(&k, kKeyHashBytes);
            if (auto it = replayKeyIndex.find(kh); it != replayKeyIndex.end())
            {
                uint32_t& n = replayRecs[it->second].count;
                n = dropIn ? (std::max)(n, k.count) : n + k.count;
                merged++;
            }
            else if (replayRecs.size() >= kMaxReplayKeys)
            {
                full++;
            }
            else
            {
                replayKeyIndex.emplace(kh, (uint32_t)replayRecs.size());
                replayRecs.push_back(k);
                replaySrc.push_back(src);
            }
        }

        // The bytecode arrives in the same file as the keys that name it, so a key set
        // can never be loaded without the shaders needed to replay it.
        //
        // Except from a drop-in. The reader checks bytecode as far as a token walk can
        // (d3d9cache::CheckBytecode), but what DXVK's shader converter accepts is far
        // more than that -- register types per opcode, balanced control flow, and much
        // else -- and a shader it throws on kills one of its threads: the loading
        // screen then hangs, on every launch, for as long as the file is there. So a
        // shader is only ever created from bytecode this PC recorded itself or that
        // FusionFix shipped. A drop-in's keys still name the game's .fxc shaders
        // (resolved from this install), FusionFix's own embedded ones, and whatever
        // the capture and baseline carry; a key naming anything else is counted as a
        // shader this install lacks, and is warmed once this PC has drawn it.
        uint32_t newShaders = 0;
        for (auto& [hash, blob] : c.shaders)
        {
            if (dropIn || replayBlobs.count(hash) || replayBlobBytes + blob.code.size() > kMaxReplayBlobBytes) continue;
            replayBlobBytes += blob.code.size();
            replayBlobs.emplace(hash, blob);
            newShaders++;
        }
        if (!c.strings[kMetaShaderDir].empty() && c.strings[kMetaShaderDir] != "unknown")
        {
            exportShaderDir = c.strings[kMetaShaderDir];
            exportMeta = c.meta;
            for (uint32_t i = 0; i < kMetaStringCount; i++) exportStrings[i] = c.strings[i];
        }

        replaySources[src].added = (uint32_t)(replayRecs.size() - recsBefore);
        line += "; " + std::to_string(replaySources[src].added) + " keys new to the replay set (" +
                std::to_string(merged) + " already known), " +
                (dropIn ? std::to_string(c.shaders.size()) + " shaders not used (bytecode from other PCs is never run)"
                        : std::to_string(newShaders) + " new shaders");
        if (full)
            line += "; " + std::to_string(full) + " more not taken, the replay set is full (" +
                    std::to_string(kMaxReplayKeys) + " keys)";
        LogStr(line);
        return !c.keys.empty();
    }

    // Write the deduplicated, use-ordered pipeline set as a baseline others can ship.
    //
    // Exported from the REPLAY's own reduction rather than recomputed offline: the
    // reduction depends on each pixel shader's declared sampler mask, and a second
    // implementation of that would be free to drift from this one in exactly the way
    // pipelinekeys.h exists to prevent. It also shrinks enormously -- 13621 strict
    // keys collapse to ~1968 pipelines, so the shipped artifact is a fraction of a
    // raw capture.
    static void ExportBaseline(const std::vector<uint32_t>& drawList)
    {
        using namespace pipelinekeys;

        std::string path = ModuleDir() + "FusionFix.pipelinecache.baseline.export.bin";

        // Only the declarations these records actually reference.
        std::unordered_map<uint32_t, uint32_t> declRemap;
        std::vector<uint32_t> declOrder;
        for (uint32_t r : drawList)
        {
            uint32_t d = replayRecs[r].declIndex;
            if (d == kDeclNone || d >= replayDecls.size()) continue;
            if (declRemap.emplace(d, (uint32_t)declOrder.size()).second) declOrder.push_back(d);
        }

        CacheContents c;
        c.meta = exportMeta;               // provenance of the capture this came from
        c.meta.numRS       = kNumRS;
        c.meta.numSamplers = kNumSamplers;
        for (uint32_t i = 0; i < kMetaStringCount; i++) c.strings[i] = exportStrings[i];
        c.strings[kMetaShaderDir] = exportShaderDir;

        c.rsTypes.reserve(kNumRS);
        for (uint32_t i = 0; i < kNumRS; i++) c.rsTypes.push_back((uint32_t)kTrackedRS[i].rs);

        for (uint32_t d : declOrder) c.decls.push_back(replayDecls[d]);

        std::unordered_set<uint64_t> needed;
        for (uint32_t r : drawList)
        {
            KeyRecord k = replayRecs[r];
            if (k.declIndex != kDeclNone)
                k.declIndex = declRemap.count(k.declIndex) ? declRemap[k.declIndex] : kDeclNone;
            c.keys.push_back(k);
            needed.insert(k.vsHash);
            needed.insert(k.psHash);
        }

        // Ship only the bytecode these keys actually name, and of that only what an
        // install cannot supply itself: .fxc shaders are resolved by hash from the
        // player's own files (pipelinekeys::CarryBytecode), so the baseline carries
        // FusionFix's runtime-built shaders and no Rockstar-derived bytecode.
        for (uint64_t hash : needed)
            if (auto it = replayBlobs.find(hash); it != replayBlobs.end() && pipelinekeys::CarryBytecode(hash))
                c.shaders.emplace(hash, it->second);

        if (!WriteCache(path, c)) { Log("baseline: cannot write %s", path.c_str()); return; }

        Log("baseline: wrote %zu pipelines, %zu declarations, %zu shaders to %s",
            drawList.size(), declOrder.size(), c.shaders.size(), path.c_str());
    }

    // Read a shader object's bytecode back out of it and parse its declarations.
    // Every D3D9 shader object can hand its own token stream back, which is cheaper
    // than finding the file it came from and works for shaders that have no file.
    template <typename T>
    static bool ParseShaderFunction(T* obj, bool isVS, ShaderIO& io)
    {
        if (!obj) return false;
        UINT size = 0;
        if (FAILED(obj->GetFunction(nullptr, &size)) || !size) return false;
        std::vector<uint8_t> code(size);
        if (FAILED(obj->GetFunction(code.data(), &size))) return false;
        ParseShaderIO(code.data(), size, isVS, io);
        return true;
    }

    // Index the shaders we just created by the same bytecode hash the capture used,
    // so a recorded key can name the exact shader objects to bind.
    static void IndexShadersByHash(fxc_db* db)
    {
        vsByHash.clear();
        psByHash.clear();
        uint32_t n = fxc_unique_count(db);
        for (uint32_t i = 0; i < n; i++)
        {
            const fxc_shader* s = fxc_unique_shader(db, i);
            if (!s || !s->bytecode || !s->size) continue;
            uint64_t h = pipelinekeys::Fnv1a(s->bytecode, s->size);
            const bool isVS = s->stage == FXC_STAGE_VS;
            if (isVS) { if (vsHandles[i]) vsByHash.emplace(h, vsHandles[i]); }
            else      { if (psHandles[i]) psByHash.emplace(h, psHandles[i]); }
            // Remember which sampler slots this shader declares; the replay key masks
            // the recorded sampler types down to these, per stage.
            NoteSamplerUse(h, isVS, shaderIO[i]);
        }
        size_t fromDb = vsByHash.size() + psByHash.size();

        // Fold in every other shader the process created. FusionFix compiles its own
        // (SMAA, FXAA, sun shafts, gamma, LOD lights) which are not in RAGE's .fxc
        // database, and draws using them were previously skipped outright -- 178 of
        // 5110 pipelines on the first capture.
        // Registry shaders have no .fxc entry, so parse their declarations here too --
        // otherwise their keys keep every sampler slot and over-expand.
        for (auto& [h, obj] : pipelinekeys::Registry().vs)
        {
            vsByHash.emplace(h, obj);
            ShaderIO io{};
            if (!vsSamplerUse.count(h) && ParseShaderFunction(obj, true, io)) NoteSamplerUse(h, true, io);
        }
        for (auto& [h, obj] : pipelinekeys::Registry().ps)
        {
            psByHash.emplace(h, obj);
            ShaderIO io{};
            if (!psSamplerUse.count(h) && ParseShaderFunction(obj, false, io)) NoteSamplerUse(h, false, io);
        }

        Log("replay: indexed %zu VS + %zu PS by bytecode hash (%zu from the .fxc db, %zu more from the registry)",
            vsByHash.size(), psByHash.size(), fromDb, vsByHash.size() + psByHash.size() - fromDb);

        // The b# masks, so the key's bool axis can be checked against the install
        // rather than trusted. Measured offline over this rig's 1734 .fxc shaders:
        // 159 read a bool register, every one of them exactly one, and only four
        // registers appear anywhere (b0, b8, b9, b11 -> union 0x0B01). A walk that
        // had desynchronised would show scattered high bits and a much larger
        // union, so these two numbers are the parser's own check.
        {
            uint32_t withBools = 0, unionMask = 0, most = 0;
            for (auto& [h, m] : shaderBoolMask)
            {
                if (!m) continue;
                withBools++;
                unionMask |= m;
                uint32_t bits = 0;
                for (uint32_t v = m; v; v &= v - 1) bits++;
                if (bits > most) most = bits;
            }
            Log("replay: b# masks parsed for %zu shaders - %u read a bool register, union 0x%04X, "
                "most read by any one shader %u",
                shaderBoolMask.size(), withBools, unionMask, most);
        }
    }

    // ---- RAGE's own shader objects -------------------------------------------
    //
    // The engine creates an IDirect3DVertex/PixelShader9 for every shader in every
    // .fxc it loads, eagerly, at load time (grcEffect::Load), and keeps it in the
    // grcProgram it belongs to. By the time the loading screen runs, the process
    // therefore already holds an object for every shader the game can draw -- and a
    // D3D9 shader object hands its own token stream back through GetFunction, which
    // DXVK implements (d3d9_shader.h:178). So the engine, not the file system, is the
    // complete and authoritative list of what this install can draw with.
    //
    // Reading it matters for keys whose shader our .fxc parse cannot supply: a mod's
    // effect, an episode's, or one of the five effects the engine loads from the base
    // directory rather than update/. Those keys are skipped as "no-shader" today. On
    // THIS install the parse happens to cover everything the recording names, so the
    // measurable effect here is the log line; the value is that it stops depending on
    // which directory the parse guessed.
    //
    // Cost: GetFunction on DXVK's 32-bit build maps the stored bytecode rather than
    // copying it (DXVK_USE_UNMAPPABLE_MEMORY, util/util_unmap.h:12), so each call is a
    // map pair into one reused 64 KB buffer.
    //
    // WHAT IS READ: only .data globals. Everything in 0x00401000-0x004FB000 is
    // SecuROM-encrypted in the shipped exe and is plaintext only once the game has
    // decrypted itself, so no code there may be read, hooked or fingerprinted -- this
    // walk touches none of it. Addresses and layout:
    // re/rage-shader-precompile/01-effects-static.md §2.1, §2.4, §3.
    // The addresses and the struct offsets come from enginewarm.h, which is the
    // one place they are written down: two walks of the same registry that each
    // spelled the program counts differently is exactly the kind of drift that
    // survives review because both readings happen to agree on one install.
    static constexpr uintptr_t kVA_Effects      = enginewarm::kVA_Effects;
    static constexpr uint32_t  kEffectSlots     = 128;
    static constexpr uintptr_t kVA_ExtraEffect  = enginewarm::kVA_EffectExtra;
    static constexpr uint32_t  kEffMaxPrograms  = 4096;         // sanity bound on a count field
    static constexpr uint32_t  kMaxEnginePrograms = 8192;       // this install has ~1864

    struct EngineProgram { void* obj; uint8_t isVS; uint16_t effect; };
    struct EngineScan
    {
        EngineProgram* out;
        const char**   names;        // the effect name per registry slot, [kEffectSlots + 1]
        uint32_t cap, n;             // programs collected
        uint32_t effects;            // effects that passed validation
        uint32_t rejected;           // non-null registry slots that did not
        uint32_t nullPrograms;       // program slots with no D3D object
        uint32_t unnamed;            // accepted effects with no readable name
        bool     extraValid;         // the effect outside the registry validated
        bool     faulted;
    };

    // POD only, so it can carry a __try: a fault here must cost the engine path and
    // nothing else. Never reads through a pointer it has not checked first, because
    // the whole point of a hard-coded address is that it may be wrong on another build.
    static bool ScanEngineEffect(uintptr_t base, uint32_t slot, EngineScan& s)
    {
        if (base < 0x10000 || IsBadReadPtr((void*)base, 0x28)) return false;

        // The name is for the log, not for the decision. It was a validation rule at
        // first, and it threw away the one effect that matters most for having the
        // walk at all: the gta_im outside the registry, which the name/UI code never
        // touches (01-effects §2.1: "only the registry array and the name/UI code use
        // base"). What actually proves this is an effect is that its program array
        // holds objects whose GetFunction returns a well-formed SM3 token stream,
        // which ReadShaderFunction checks for every one of them.
        const char* name = *reinterpret_cast<const char* const*>(base + 0x00);
        bool named = false;
        for (uint32_t i = 0; name && i < 64; i++)
        {
            // Probe before every byte, and never read the one that failed: a name
            // running off the end of a page is exactly the shape a wrong address
            // takes, and faulting here would abandon the whole walk.
            if (IsBadReadPtr(name + i, 1)) break;
            const unsigned char c = (unsigned char)name[i];
            if (!c) { named = i > 0; break; }
            if (c < 0x20 || c > 0x7E) break;
        }
        if (!named) name = nullptr;

        // m_VertexPrograms/+0x1C count, m_FragmentPrograms/+0x24 count; stride 0x0C,
        // the D3D object at +0x08 (grcProgram) -- all named in enginewarm.h, which
        // is where the choice of count over total is argued.
        const uint32_t arrOff[2] = { enginewarm::kEff_VertProgs, enginewarm::kEff_FragProgs };
        const uint32_t cntOff[2] = { enginewarm::kEff_VsCount,   enginewarm::kEff_PsCount   };
        bool anyArray = false;
        const uint32_t before = s.n;
        for (int stage = 0; stage < 2; stage++)
        {
            auto arr = *reinterpret_cast<uintptr_t const*>(base + arrOff[stage]);
            uint32_t n = *reinterpret_cast<const uint16_t*>(base + cntOff[stage]);
            if (!n || n > kEffMaxPrograms) continue;
            if (arr < 0x10000 || IsBadReadPtr((void*)arr, n * enginewarm::kProg_Stride)) continue;
            anyArray = true;
            for (uint32_t i = 0; i < n && s.n < s.cap; i++)
            {
                void* obj = *reinterpret_cast<void* const*>(
                    arr + i * enginewarm::kProg_Stride + enginewarm::kProg_D3D);
                if (!obj || IsBadReadPtr(obj, 4)) { s.nullPrograms++; continue; }
                s.out[s.n].obj    = obj;
                s.out[s.n].isVS   = stage == 0;
                s.out[s.n].effect = (uint16_t)slot;
                s.n++;
            }
        }
        if (!anyArray) { s.n = before; return false; }

        if (slot < kEffectSlots + 1) s.names[slot] = name;
        if (!name) s.unnamed++;
        s.effects++;
        return true;
    }

    static void ScanEngineEffects(EngineScan& s)
    {
        __try
        {
            const uintptr_t tbl = RebaseVA(kVA_Effects);
            if (IsBadReadPtr((void*)tbl, kEffectSlots * sizeof(uintptr_t))) { s.faulted = true; return; }
            for (uint32_t i = 0; i < kEffectSlots; i++)
            {
                const uintptr_t base = reinterpret_cast<const uintptr_t*>(tbl)[i];
                if (base < 0x10000) continue;
                if (!ScanEngineEffect(base, i, s)) s.rejected++;
            }
            // 01-effects §3: one gta_im lives OUTSIDE the array, at its own static
            // address, with its own 13 techniques and its own shader objects. Counted
            // apart from the registry slots, because "one slot rejected" cannot say
            // whether it was this known outlier or a registry entry we misread.
            s.extraValid = ScanEngineEffect(RebaseVA(kVA_ExtraEffect), kEffectSlots, s);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s.faulted = true;
        }
    }

    // Also POD-only: a shader object that is not one would fault inside GetFunction.
    static UINT ReadShaderFunction(void* obj, bool isVS, uint8_t* buf, UINT cap)
    {
        __try
        {
            UINT size = 0;
            if (isVS)
            {
                auto* sh = static_cast<IDirect3DVertexShader9*>(obj);
                if (FAILED(sh->GetFunction(nullptr, &size)) || !size || size > cap) return 0;
                if (FAILED(sh->GetFunction(buf, &size))) return 0;
            }
            else
            {
                auto* sh = static_cast<IDirect3DPixelShader9*>(obj);
                if (FAILED(sh->GetFunction(nullptr, &size)) || !size || size > cap) return 0;
                if (FAILED(sh->GetFunction(buf, &size))) return 0;
            }
            // A D3D9 token stream opens with a version token whose high word is
            // 0xFFFE (vertex) or 0xFFFF (pixel). Anything else is not a shader and the
            // pointer was not what we thought it was.
            if (size < 8 || (*reinterpret_cast<const uint32_t*>(buf) >> 16) != (isVS ? 0xFFFEu : 0xFFFFu))
                return 0;
            return size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void IndexShadersFromEngine()
    {
        std::vector<EngineProgram> programs(kMaxEnginePrograms);
        std::vector<const char*>   names(kEffectSlots + 1, nullptr);

        EngineScan s{};
        s.out = programs.data();
        s.names = names.data();
        s.cap = kMaxEnginePrograms;
        ScanEngineEffects(s);

        if (s.faulted && !s.n)
        {
            Log("replay: engine: the effect registry at %08X could not be read - "
                "falling back to the .fxc files alone", (unsigned)RebaseVA(kVA_Effects));
            return;
        }

        std::vector<uint8_t> buf(64 * 1024);
        uint32_t read = 0, known = 0, addedVS = 0, addedPS = 0, failed = 0, newMask = 0;
        // Effects whose shaders the .fxc parse does not have. Names them, because the
        // answer to "does the engine load effects our directory choice misses?" is a
        // list of effect names, not a count.
        std::unordered_map<const char*, uint32_t> unknownByEffect;

        const auto tStartEngine = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < s.n; i++)
        {
            const EngineProgram& p = programs[i];
            const UINT size = ReadShaderFunction(p.obj, p.isVS != 0, buf.data(), (UINT)buf.size());
            if (!size) { failed++; continue; }
            read++;

            const uint64_t h = pipelinekeys::Fnv1a(buf.data(), size);
            const bool have = p.isVS ? vsByHash.count(h) != 0 : psByHash.count(h) != 0;
            if (have) known++;
            else
            {
                // AddRef and never Release, like every other shader handle this pass
                // keeps. "The effects live for the whole session" is asserted, not
                // established -- grcEffect has a destructor and LoadOrCreateByName
                // hunts for a free slot, so an effect CAN be freed, and a freed one
                // releases its shader objects. 1850 refcount increments cost nothing
                // and a dangling bind would cost everything. The object has just
                // answered GetFunction with a well-formed token stream, so it is alive
                // at this instant; the window between the census and here is a few
                // milliseconds on the blocked render thread.
                if (p.isVS)
                {
                    auto* sh = static_cast<IDirect3DVertexShader9*>(p.obj);
                    sh->AddRef();
                    vsByHash.emplace(h, sh);
                    addedVS++;
                }
                else
                {
                    auto* sh = static_cast<IDirect3DPixelShader9*>(p.obj);
                    sh->AddRef();
                    psByHash.emplace(h, sh);
                    addedPS++;
                }
                if (p.effect <= kEffectSlots && names[p.effect]) unknownByEffect[names[p.effect]]++;
            }

            if (!(p.isVS ? vsSamplerUse.count(h) : psSamplerUse.count(h)))
            {
                ShaderIO io{};
                ParseShaderIO(buf.data(), size, p.isVS != 0, io);
                NoteSamplerUse(h, p.isVS != 0, io);
                newMask++;
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tStartEngine).count();

        std::string trouble;
        if (failed)      trouble += "; " + std::to_string(failed) + " objects gave no bytecode";
        if (s.rejected)  trouble += "; " + std::to_string(s.rejected) + " registry slots rejected";
        if (s.unnamed)   trouble += "; " + std::to_string(s.unnamed) + " unnamed";
        if (s.nullPrograms) trouble += "; " + std::to_string(s.nullPrograms) + " programs hold no shader";
        if (!s.extraValid)  trouble += "; the effect outside the registry was not there";
        if (s.faulted)   trouble += "; the walk faulted and stopped early";
        Log("replay: engine: %u effects, %u shader objects, %u bytecodes read in %u ms, "
            "%u already known, %u new (%u VS + %u PS), %u sampler masks parsed%s",
            s.effects, s.n, read, (unsigned)ms, known, addedVS + addedPS, addedVS, addedPS,
            newMask, trouble.c_str());

        // The point of the line above is this list: an effect named here is one the
        // .fxc parse did not cover, i.e. a key using it would have been skipped.
        for (const auto& [name, n] : unknownByEffect)
            Log("replay: engine: %s.fxc - %u shader(s) the .fxc parse does not have", name, n);
    }

    // Shader objects we created ourselves from captured bytecode. Held for the
    // process lifetime like the .fxc handles: releasing them would let DXVK drop the
    // shader module, and with it the pipelines this pass exists to build.
    static inline std::vector<IDirect3DVertexShader9*> sidecarVS;
    static inline std::vector<IDirect3DPixelShader9*>  sidecarPS;

    // Last resort for a hash neither the .fxc database nor the runtime registry can
    // resolve. That is not an edge case: FusionFix's own effect shaders are compiled
    // when their effect first runs, i.e. never before this pass, so 202 of 2034
    // pipelines had no handle to bind and were skipped. The capture keeps their
    // bytecode; create the shader from it and the key becomes replayable. Identical
    // bytes resolve to the same DXVK shader module, so this warms the pipeline the
    // game will actually use, not a lookalike.
    // The blobs came in with the keys, from the same container, so there is no second
    // file to find and no way for the two to disagree.
    static void IndexShadersFromBlobs()
    {
        if (replayBlobs.empty()) { Log("replay: the cache carried no shader bytecode"); return; }

        // Only what a key names: each shader created here lives as long as the
        // process, and GTA IV is a 32-bit one.
        std::unordered_set<uint64_t> named;
        for (const auto& k : replayRecs) { named.insert(k.vsHash); named.insert(k.psHash); }

        uint32_t madeVS = 0, madePS = 0, failed = 0, unnamed = 0;
        for (auto& [hash, blob] : replayBlobs)
        {
            if (!named.count(hash)) { unnamed++; continue; }
            const bool isVS = blob.stage == pipelinekeys::kStageVS;

            // Same reason as the registry path: without the declared-sampler mask the
            // key keeps every slot and expands into pipelines that do not exist. Done
            // before the "already resolved" check below, because a shader can be
            // resolvable from elsewhere and still have no mask recorded.
            if (!(isVS ? vsSamplerUse.count(hash) : psSamplerUse.count(hash)))
            {
                ShaderIO io{};
                ParseShaderIO(blob.code.data(), (uint32_t)blob.code.size(), isVS, io);
                NoteSamplerUse(hash, isVS, io);
            }

            const DWORD* fn = reinterpret_cast<const DWORD*>(blob.code.data());
            if (isVS)
            {
                if (vsByHash.count(hash)) continue;
                IDirect3DVertexShader9* sh = nullptr;
                if (FAILED(dev->CreateVertexShader(fn, &sh)) || !sh) { failed++; continue; }
                sidecarVS.push_back(sh);
                vsByHash.emplace(hash, sh);
                madeVS++;
            }
            else
            {
                if (psByHash.count(hash)) continue;
                IDirect3DPixelShader9* sh = nullptr;
                if (FAILED(dev->CreatePixelShader(fn, &sh)) || !sh) { failed++; continue; }
                sidecarPS.push_back(sh);
                psByHash.emplace(hash, sh);
                madePS++;
            }
        }

        Log("replay: created %u VS + %u PS from the cache's bytecode (%zu stored, %u named by no key, %u failed)",
            madeVS, madePS, replayBlobs.size(), unnamed, failed);
    }

    // ---- FusionFix's OWN embedded shaders ------------------------------------
    //
    // FusionFix ships compiled shaders as RT_RCDATA resources and hands the raw
    // resource bytes straight to CreatePixelShader/CreateVertexShader, so the
    // resource IS the bytecode the capture hashes. Reading them here makes every one
    // of them resolvable without depending on a capture session having happened to
    // enable the effect that uses it.
    //
    // A static audit of the .asi against the sidecar found 12 of 18 covered and SIX
    // missing: FXAA's pixel shader, the dithered console-gamma blit, the blit pair
    // and the snow pair. Each is gated behind a graphics setting, so whether they got
    // captured was down to which settings the capturing machine happened to run.
    //
    // This matters most for what it makes DETERMINISTIC. SMAA and FXAA are normally
    // compiled at runtime from HLSL resources (loadCompiledShader is only the
    // fallback), and runtime-compiled bytecode depends on the machine's d3dcompiler,
    // so its hash is not portable -- a shipped baseline can never reliably name those
    // shaders for someone else. The EMBEDDED ones are byte-identical everywhere, so
    // resolving them from resources works for every user by construction.
    //
    // NB this makes shaders resolvable; it does not by itself create pipelines. A key
    // naming one still has to come from a capture. What it removes is the failure
    // where such a key arrives and cannot be replayed.
    static inline std::vector<IDirect3DVertexShader9*> ownResVSObjs;
    static inline std::vector<IDirect3DPixelShader9*>  ownResPSObjs;
    static inline uint32_t ownResVS = 0, ownResPS = 0, ownResKnown = 0, ownResFailed = 0;

    static BOOL CALLBACK OnOwnRcData(HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR)
    {
        HRSRC hRes = FindResourceW(mod, name, type);
        if (!hRes) return TRUE;
        DWORD size = SizeofResource(mod, hRes);
        HGLOBAL hGlob = LoadResource(mod, hRes);
        if (!hGlob || size < 8 || (size & 3u)) return TRUE;
        const void* data = LockResource(hGlob);
        if (!data) return TRUE;

        // A D3D9 token stream opens with a version token: 0xFFFF in the high word for
        // a pixel shader, 0xFFFE for a vertex shader. Everything else in RCDATA
        // (lookup textures, blue noise, data blobs) fails this and is skipped.
        const uint32_t ver = *reinterpret_cast<const uint32_t*>(data);
        const uint32_t hi = ver >> 16;
        const bool isPS = (hi == 0xFFFFu);
        const bool isVS = (hi == 0xFFFEu);
        if (!isPS && !isVS) return TRUE;

        const uint64_t h = pipelinekeys::Fnv1a(data, size);
        const DWORD* fn = reinterpret_cast<const DWORD*>(data);

        // The mask first, whether or not the shader still has to be created: a
        // resource can be resolvable from the .fxc database or the engine and still
        // have no declared-sampler mask recorded.
        if (!(isVS ? vsSamplerUse.count(h) : psSamplerUse.count(h)))
        {
            ShaderIO io{};
            ParseShaderIO(reinterpret_cast<const uint8_t*>(data), (uint32_t)size, isVS, io);
            NoteSamplerUse(h, isVS, io);
        }

        if (isVS)
        {
            if (vsByHash.count(h)) { ownResKnown++; return TRUE; }
            IDirect3DVertexShader9* sh = nullptr;
            if (FAILED(dev->CreateVertexShader(fn, &sh)) || !sh) { ownResFailed++; return TRUE; }
            ownResVSObjs.push_back(sh);
            vsByHash.emplace(h, sh);
            ownResVS++;
        }
        else
        {
            if (psByHash.count(h)) { ownResKnown++; return TRUE; }
            IDirect3DPixelShader9* sh = nullptr;
            if (FAILED(dev->CreatePixelShader(fn, &sh)) || !sh) { ownResFailed++; return TRUE; }
            ownResPSObjs.push_back(sh);
            psByHash.emplace(h, sh);
            ownResPS++;
        }
        return TRUE;
    }

    static void IndexShadersFromOwnResources()
    {
        if (!hSelf) { Log("replay: no module handle, skipping our own shader resources"); return; }

        ownResVS = ownResPS = ownResKnown = ownResFailed = 0;
        EnumResourceNamesW(hSelf, RT_RCDATA, &OnOwnRcData, 0);

        Log("replay: created %u VS + %u PS from FusionFix's own resources "
            "(%u already resolvable, %u failed)",
            ownResVS, ownResPS, ownResKnown, ownResFailed);
    }

    static UINT DeclTypeSize(BYTE type)
    {
        switch (type)
        {
        case D3DDECLTYPE_FLOAT1:    return 4;
        case D3DDECLTYPE_FLOAT2:    return 8;
        case D3DDECLTYPE_FLOAT3:    return 12;
        case D3DDECLTYPE_FLOAT4:    return 16;
        case D3DDECLTYPE_D3DCOLOR:  return 4;
        case D3DDECLTYPE_UBYTE4:    return 4;
        case D3DDECLTYPE_SHORT2:    return 4;
        case D3DDECLTYPE_SHORT4:    return 8;
        case D3DDECLTYPE_UBYTE4N:   return 4;
        case D3DDECLTYPE_SHORT2N:   return 4;
        case D3DDECLTYPE_SHORT4N:   return 8;
        case D3DDECLTYPE_USHORT2N:  return 4;
        case D3DDECLTYPE_USHORT4N:  return 8;
        case D3DDECLTYPE_UDEC3:     return 4;
        case D3DDECLTYPE_DEC3N:     return 4;
        case D3DDECLTYPE_FLOAT16_2: return 4;
        case D3DDECLTYPE_FLOAT16_4: return 8;
        default:                    return 0;
        }
    }

    static IDirect3DVertexDeclaration9* ReplayDecl(uint32_t index)
    {
        if (index >= replayDecls.size()) return nullptr;
        if (replayDeclObjs.size() < replayDecls.size()) replayDeclObjs.resize(replayDecls.size(), nullptr);
        if (replayDeclObjs[index]) return replayDeclObjs[index];

        auto& elems = replayDecls[index];
        // The capture stores the array exactly as GetDeclaration returned it,
        // including D3DDECL_END, so it can be handed straight back.
        IDirect3DVertexDeclaration9* obj = nullptr;
        if (SUCCEEDED(dev->CreateVertexDeclaration(elems.data(), &obj)))
            replayDeclObjs[index] = obj;
        return obj;
    }

    // Multisampled scratch targets, cached per {format, type, quality}. Vulkan bakes
    // the sample count into the pipeline, so replaying an MSAA key against a
    // single-sampled target would build a pipeline gameplay never asks for. Only
    // reached when the user runs with ReflectionMSAAQuality set; the common path
    // stays on the plain ScratchFor cache.
    struct ScratchMS { D3DFORMAT fmt; uint32_t type; uint32_t quality; IDirect3DSurface9* surf; };
    static inline std::vector<ScratchMS> scratchMS;

    static IDirect3DSurface9* ScratchForMS(D3DFORMAT fmt, uint32_t type, uint32_t quality)
    {
        if (type == 0) return ScratchFor(fmt);
        for (auto& s : scratchMS)
            if (s.fmt == fmt && s.type == type && s.quality == quality) return s.surf;

        IDirect3DSurface9* surf = nullptr;
        if (FAILED(dev->CreateRenderTarget(kRTdim, kRTdim, fmt, (D3DMULTISAMPLE_TYPE)type,
                                           quality, FALSE, &surf, nullptr)))
            surf = nullptr;
        scratchMS.push_back({ fmt, type, quality, surf });
        return surf;
    }

    // Depth surface in the RECORDED format and sample count, cached per {format, type,
    // quality}: the depth format is baked into the pipeline too, so a key from a PC
    // or configuration that used another one is only warmed right with that one.
    // nullptr if this device cannot create it (the key is then skipped). The
    // readable-depth FOURCCs (INTZ, DF24, ...) exist only as single-sampled textures;
    // a multisampled key names the MSAA depth surface the game drew with, D24S8.
    struct ScratchDS { uint32_t fmt, type, quality; IDirect3DTexture9* tex; IDirect3DSurface9* surf; };
    static inline std::vector<ScratchDS> scratchDS;

    static IDirect3DSurface9* ReplayDepthForMS(uint32_t fmt, uint32_t type, uint32_t quality)
    {
        if (fmt == 0) return nullptr;
        if (type && d3d9cache::IsFourCCDepth(fmt)) fmt = (uint32_t)D3DFMT_D24S8;
        if (!type && fmt == (uint32_t)FOURCC_INTZ && scratchDepthINTZ) return scratchDepthINTZ;
        if (!type && fmt == (uint32_t)D3DFMT_D24S8 && scratchDepthD24S8) return scratchDepthD24S8;
        for (auto& s : scratchDS)
            if (s.fmt == fmt && s.type == type && s.quality == quality) return s.surf;

        ScratchDS s{ fmt, type, quality, nullptr, nullptr };
        if (d3d9cache::IsFourCCDepth(fmt))
        {
            if (SUCCEEDED(dev->CreateTexture(kRTdim, kRTdim, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)fmt,
                                             D3DPOOL_DEFAULT, &s.tex, nullptr)) && s.tex)
                s.tex->GetSurfaceLevel(0, &s.surf);
        }
        else if (FAILED(dev->CreateDepthStencilSurface(kRTdim, kRTdim, (D3DFORMAT)fmt, (D3DMULTISAMPLE_TYPE)type,
                                                       quality, FALSE, &s.surf, nullptr)))
        {
            s.surf = nullptr;
        }
        scratchDS.push_back(s);
        return s.surf;
    }

    // Wait for everything submitted so far to complete, presenting the overlay while
    // we wait so the progress frame keeps updating instead of the window going dead.
    static void FenceChunk(IDirect3DQuery9* q)
    {
        if (!q) return;
        q->Issue(D3DISSUE_END);
        // Flush ONCE to get the work moving, then poll without it. D3DGETDATA_FLUSH
        // on every poll re-flushes the command stream thousands of times per chunk,
        // which competes with the compilation we are waiting for and shows up as a
        // 25 ms average overlay frame gap against a 5 ms target.
        bool flushed = false;
        for (int spin = 0; spin < 200000; spin++)
        {
            DWORD flags = flushed ? 0 : D3DGETDATA_FLUSH;
            flushed = true;
            if (q->GetData(nullptr, 0, flags) == S_OK) break;
            PresentOverlay(false);
        }
    }

    // ===================================================================
    //  THE SPECIALISATION HALF OF A KEY, PUT BACK ON THE DEVICE
    //
    //  Everything below is device state DXVK folds into D3D9SpecData and
    //  therefore into DxvkGraphicsPipelineStateInfo::sc. With pipeline
    //  libraries off -- this rig -- each distinct value is its own compile,
    //  so a replay that leaves any of it to whatever the device happens to be
    //  holding builds a DIFFERENT set of pipelines depending on where it runs.
    //  That is exactly what the boot-gate move exposed, and it is not a gate
    //  bug: the gate only made an already-wrong key visible.
    //
    //  The cache is invalidated whenever the slice driver hands the device back
    //  to the loading screen, because RestoreEngineState applies a D3DSBT_ALL
    //  block that takes every one of these with it.
    // ===================================================================
    static inline uint32_t specEpoch = 0xFFFFFFFFu;
    static inline uint32_t specVsBools = 0xFFFFFFFFu, specPsBools = 0xFFFFFFFFu;
    static inline uint32_t specClipCount = 0xFFFFFFFFu, specClipMask = 0xFFFFFFFFu;
    static inline uint32_t specProjMask = 0xFFFFFFFFu;
    static inline uint32_t specFetch4Slots = 0;       // slots left in the Fetch4 sampler state
    static inline bool     specFetch4Valid = false;
    static inline bool     specFFValid = false;
    static inline pipelinekeys::FFStage specFF[pipelinekeys::kFFStages];
    static inline uint32_t specFallbacks = 0;        // modes a missing texture could not reach

    static void InvalidateSpecState()
    {
        specEpoch = 0xFFFFFFFFu;
        specVsBools = specPsBools = 0xFFFFFFFFu;
        specClipCount = specClipMask = specProjMask = 0xFFFFFFFFu;
        specFFValid = false;
        specFetch4Valid = false;
    }

    // The texture a slot needs for the mode the key recorded. Falls back to the
    // plain one when the device would not give us the format, which warms the
    // default-mode pipeline rather than nothing.
    static IDirect3DBaseTexture9* TexForSlot(uint8_t kind, uint8_t mode)
    {
        using namespace pipelinekeys;
        if (kind == kSamplerNone) return nullptr;
        // Both scratch textures are 2D, and the sampler TYPE and the sampler MODE
        // share one spec dword (psSamplerTypes is spec ID 4, and vsSamplerTypes
        // rides with vsSamplerModes in spec ID 3). Handing a cube or volume slot a
        // 2D texture to reach its mode would build a pipeline with the right mode
        // and the WRONG type: not the identity the key recorded, and not the plain
        // one either. Bind the plain texture of the right kind instead and count it
        // as the fallback it is.
        if (kind == kSampler2D)
        {
            if (mode == kModeFetch4 && texFetch4) return texFetch4;
            if ((mode == kModeDref || mode == kModeDrefClamp) && texShadow) return texShadow;
        }
        if (mode != kModeDefault) specFallbacks++;   // the format was not available
        switch (kind)
        {
        case kSampler2D:     return tex2D;
        case kSamplerCube:   return texCube;
        case kSamplerVolume: return texVol;
        default:             return nullptr;
        }
    }

    static void ApplySpecState(IDirect3DDevice9* d, const pipelinekeys::KeyRecord& k)
    {
        using namespace pipelinekeys;
        if (specEpoch != sliceEpoch) { InvalidateSpecState(); specEpoch = sliceEpoch; }

        if (specVsBools != k.vsBools)
        {
            BOOL b[16];
            for (int i = 0; i < 16; i++) b[i] = (k.vsBools >> i) & 1 ? TRUE : FALSE;
            d->SetVertexShaderConstantB(0, b, 16);
            specVsBools = k.vsBools;
        }
        if (specPsBools != k.psBools)
        {
            BOOL b[16];
            for (int i = 0; i < 16; i++) b[i] = (k.psBools >> i) & 1 ? TRUE : FALSE;
            d->SetPixelShaderConstantB(0, b, 16);
            specPsBools = k.psBools;
        }

        // Clip planes. The recorded COUNT is what DXVK specialises on; the enable
        // mask comes from the key's own render states, which the draw loop has
        // already set. Make exactly `clipPlaneCount` of the ENABLED planes
        // non-degenerate and zero the rest, which is the one arrangement that
        // reproduces the count whatever the mask is.
        const uint32_t clipMask = (uint32_t)RSOr0(k, kRS_ClipPlanes) & 0x3Fu;
        if (specClipMask != clipMask || specClipCount != k.clipPlaneCount)
        {
            uint32_t left = k.clipPlaneCount;
            for (DWORD i = 0; i < 6; i++)
            {
                const bool live = (clipMask & (1u << i)) && left;
                if (live) left--;
                const float on[4]  = { 0.0f, 0.0f, 1.0f, (float)(i + 1) };
                const float off[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                d->SetClipPlane(i, live ? on : off);
            }
            specClipMask = clipMask;
            specClipCount = k.clipPlaneCount;
        }

        // The projected-texture bit per stage. Not fixed-function-only: every
        // pixel shader that samples declares the spec constant it lives in.
        if (specProjMask != k.projMask)
        {
            for (DWORD s = 0; s < kFFStages; s++)
                d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS,
                                        (k.projMask >> s) & 1 ? D3DTTFF_PROJECTED : D3DTTFF_DISABLE);
            specProjMask = k.projMask;
        }

        // The fixed-function stage block, for the keys that have no pixel shader.
        // A programmable pipeline never declares spec IDs 8..19, so DXVK zeroes
        // them there and setting them would be wasted calls.
        if (!k.psHash && (!specFFValid || memcmp(specFF, k.ffStage, sizeof(specFF)) != 0))
        {
            for (DWORD s = 0; s < kFFStages; s++)
            {
                const FFStage& f = k.ffStage[s];
                d->SetTextureStageState(s, D3DTSS_COLOROP,   f.colorOp);
                d->SetTextureStageState(s, D3DTSS_ALPHAOP,   f.alphaOp);
                d->SetTextureStageState(s, D3DTSS_RESULTARG, f.resultArg);
                d->SetTextureStageState(s, D3DTSS_COLORARG0, f.colorArg[0]);
                d->SetTextureStageState(s, D3DTSS_COLORARG1, f.colorArg[1]);
                d->SetTextureStageState(s, D3DTSS_COLORARG2, f.colorArg[2]);
                d->SetTextureStageState(s, D3DTSS_ALPHAARG0, f.alphaArg[0]);
                d->SetTextureStageState(s, D3DTSS_ALPHAARG1, f.alphaArg[1]);
                d->SetTextureStageState(s, D3DTSS_ALPHAARG2, f.alphaArg[2]);
            }
            memcpy(specFF, k.ffStage, sizeof(specFF));
            specFFValid = true;
        }

        // Fetch4 is a sampler state, so it has to be taken OFF the slots that had
        // it as well as put on the ones that want it -- DXVK reads it per slot on
        // every draw, and a leftover 'GET4' would specialise the next key.
        //
        // "Off" is the FOURCC 'GET1', not 0. DXVK latches the bit on 'GET4' and
        // clears it ONLY on 'GET1'; any other value, zero included, leaves
        // fetch4SamplerState exactly as it was (SetStateSamplerState,
        // d3d9_device.cpp:4583-4586, which has no else branch). Writing 0 here
        // would leave the latch set on that slot for the rest of the session --
        // invisible to the state verifier, because the D3D9-visible bias value
        // does compare equal -- and the game's INTZ depth binds with POINT
        // filtering would then turn a real Fetch4 on during gameplay.
        uint32_t want = 0;
        for (uint32_t i = 0; i < kPSSamplers; i++)
            if (k.samplerMode[i] == kModeFetch4 && k.samplerType[i] == kSampler2D && texFetch4) want |= 1u << i;
        // After a slice boundary the loading screen's own sampler states are back
        // and we do not know what they hold, so every slot is written rather than
        // only the ones that changed.
        const uint32_t change = specFetch4Valid ? (want ^ specFetch4Slots) : ((1u << kPSSamplers) - 1u);
        for (uint32_t i = 0; i < kPSSamplers; i++)
        {
            if (!((change >> i) & 1)) continue;
            const bool on = (want >> i) & 1;
            d->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, on ? MAKEFOURCC('G','E','T','4') : MAKEFOURCC('G','E','T','1'));
            d->SetSamplerState(i, D3DSAMP_MAGFILTER, on ? D3DTEXF_POINT : D3DTEXF_LINEAR);
        }
        specFetch4Slots = want;
        specFetch4Valid = true;
    }

    static void ReplayPass()
    {
        using namespace pipelinekeys;

        uint32_t drawn = 0, skippedShader = 0, skippedDecl = 0, skippedRT = 0, skippedDup = 0;

        // Which shaders we could not resolve, and how many keys each cost. A bare
        // "202 no-shader" says nothing about whether that is one ubiquitous shader or
        // two hundred rare ones, and the answer decides whether it is worth chasing.
        std::unordered_map<uint64_t, uint32_t> missingVS, missingPS;

        // Bind the dummy geometry once; only the declaration changes per key.
        dev->SetIndices(dummyIB);

        // Deduplicate FIRST, as a separate cheap pass, then draw only the survivors.
        //
        // Doing this inline while drawing made the progress bar useless: of 11234
        // records, 9371 are instant skips and 1688 are pipeline compiles costing
        // ~10 ms each, but the weighting charged them equally. The bar raced to ~85%
        // in under a second and then crawled for fifteen. Counting the real work
        // before starting it makes progress linear in time, which is the only thing
        // a progress bar is for.
        std::unordered_set<uint64_t> seenPipeline;
        seenPipeline.reserve(replayRecs.size());
        // The same set as it would be without the vertex-sampler mask. Kept because
        // the mask's worth is an install-specific number -- it depends on how much
        // leftover vertex-texture state the recording happened to carry -- and
        // asserting it from one machine's measurement would be exactly the kind of
        // claim this pass keeps having to re-check.
        std::unordered_set<uint64_t> seenUnmaskedVS;
        seenUnmaskedVS.reserve(replayRecs.size());
        std::vector<uint32_t> drawList;
        drawList.reserve(replayRecs.size() / 4);
        for (size_t r = 0; r < replayRecs.size(); r++)
        {
            seenUnmaskedVS.insert(ReplayPipelineKey(replayRecs[r], false));
            if (seenPipeline.insert(ReplayPipelineKey(replayRecs[r])).second)
                drawList.push_back((uint32_t)r);
            else
            {
                skippedDup++;
                replaySources[replaySrc[r]].samePipeline++;
            }
        }
        if (seenUnmaskedVS.size() > seenPipeline.size())
            Log("replay: the vertex-sampler mask removed %zu duplicate draws (%zu keys without it, %zu with)",
                seenUnmaskedVS.size() - seenPipeline.size(), seenUnmaskedVS.size(), seenPipeline.size());

        // What the specialisation state added to the key is worth, and how much of
        // it this recording actually exercises. A field that never varies must not
        // split anything -- that is the whole dedup contract -- so say which ones
        // vary and by how many draws, rather than asserting the key got no coarser.
        //
        // COUNTED OVER RECORDED KEYS ONLY. A key from a pre-v3 file did not record
        // these fields: WidenToV3 fills them in with zeros, D3D9's default stage
        // block and the popcount of the clip-plane enable mask. Counting those in
        // would report the widening as if it were a fact about the game -- which is
        // how a previous round printed four zeroes and read them as a measurement --
        // so they are excluded and their number is stated instead.
        {
            std::unordered_set<uint64_t> without;
            without.reserve(replayRecs.size());
            uint32_t withBools = 0, withClip = 0, withProj = 0, withMode = 0, ff = 0;
            uint32_t recorded = 0, widened = 0;
            for (size_t r = 0; r < replayRecs.size(); r++)
            {
                const KeyRecord& k = replayRecs[r];
                KeyRecord flat = k;
                flat.vsBools = flat.psBools = 0;
                flat.clipPlaneCount = 0;
                flat.projMask = 0;
                memset(flat.samplerMode, 0, sizeof(flat.samplerMode));
                pipelinekeys::DefaultFFStages(flat.ffStage);
                without.insert(ReplayPipelineKey(flat));

                // Whether a key has a pixel shader is recorded by every version, so
                // it is counted over all of them; the four fields below are not.
                if (!k.psHash) ff++;
                if (replaySources[replaySrc[r]].version < pipelinekeys::kCacheVersion) { widened++; continue; }
                recorded++;
                if (k.vsBools || k.psBools) withBools++;
                if (k.clipPlaneCount) withClip++;
                if (k.projMask) withProj++;
                for (uint32_t i = 0; i < kNumSamplers; i++)
                    if (k.samplerMode[i]) { withMode++; break; }
            }
            Log("replay: the specialisation state is worth %zu pipelines (%zu keys without it, %zu with); "
                "%u of %zu keys have no pixel shader; of the %u that RECORDED the specialisation fields, "
                "%u carry a b# register, %u a clip plane, %u a projected stage, %u a Fetch4/depth-compare "
                "sampler (%u keys predate the fields and were widened, so they cannot carry any)",
                seenPipeline.size() - without.size(), without.size(), seenPipeline.size(),
                ff, replayRecs.size(), recorded, withBools, withClip, withProj, withMode, widened);
        }
        // Warm the pipelines the game uses MOST first. If a budget or an early exit
        // cuts the pass short, what got built is then the part that matters, not an
        // arbitrary prefix of the file.
        std::sort(drawList.begin(), drawList.end(), [](uint32_t a, uint32_t b) {
            return replayRecs[a].count > replayRecs[b].count;
        });

        // Then every BASE identity before any spec-constant variant (see
        // ReplayBaseKey). Where the driver cannot fast-link, the base pipelines are
        // the whole synchronous cost and the variants only queue background work, so
        // they go first; within each tier the most-used still lead. The first
        // (most-used) key of each identity is its representative.
        {
            std::unordered_set<uint64_t> seenBase;
            seenBase.reserve(drawList.size());
            std::vector<uint32_t> variants;
            size_t nBase = 0;
            for (uint32_t r : drawList)
            {
                if (seenBase.insert(ReplayBaseKey(replayRecs[r])).second) drawList[nBase++] = r;
                else variants.push_back(r);
            }
            drawList.resize(nBase);
            replayBaseCount = (uint32_t)nBase;
            drawList.insert(drawList.end(), variants.begin(), variants.end());
        }
        uint32_t instanced = 0;
        for (uint32_t r : drawList)
            for (uint32_t s = 0; s < kMaxStreams; s++)
                if (replayRecs[r].streamFreq[s]) { instanced++; break; }

        // The export keeps both tiers whatever this run replays: it is the input for
        // other machines, whose drivers may want the variants.
        if (cfg.exportBaseline) ExportBaseline(drawList);

        if (!cfg.specVariants && drawList.size() > replayBaseCount)
        {
            Log("replay: PrecompileSpecVariants = 0 - dropping %zu spec-constant variants",
                drawList.size() - replayBaseCount);
            drawList.resize(replayBaseCount);
        }

        // This PC's own keys and the baseline's first, then the ones only other PCs'
        // files name, each part in the order above. The part from other PCs is drawn
        // after a wait until DXVK is quiet and with vkcapture's recording paused
        // (VulkanReplayState::recordPaused): those pipelines are warmed every launch
        // from the files themselves, but they are not this PC's and must not end up
        // in its own Vulkan recording. And a PrecompileBudgetSeconds cut can then never
        // spend the budget on another PC's configuration before this PC's own.
        const size_t foreignAt = (size_t)(std::stable_partition(drawList.begin(), drawList.end(), [](uint32_t r) {
            return !replaySources[replaySrc[r]].dropIn;
        }) - drawList.begin());

        // workDone currently holds the create-pass units; everything left is drawing.
        workTotal = workDone + (uint32_t)drawList.size() * kPassW;
        if (workTotal == 0) workTotal = 1;
        Log("replay: %zu unique pipelines to build from %zu keys: %u base identities first "
            "(shaders + vertex input + output state), then %zu spec-constant variants; %u instanced; "
            "%zu named only by other PCs' files, last",
            drawList.size(), replayRecs.size(), replayBaseCount,
            drawList.size() - replayBaseCount, instanced, drawList.size() - foreignAt);

        // Adaptive exit: if pipelines are not actually being COMPILED, there is
        // nothing to win and the whole pass is a pure loading-time regression.
        //
        // That is the graphics-pipeline-library case: DXVK compiles per-stage
        // libraries at shader-create time and then fast-links them, so each draw
        // here costs microseconds instead of milliseconds. Rather than trying to
        // detect GPL -- which is DXVK's internal decision, not an extension bit, and
        // could change with any release -- measure the cost and decide. This also
        // handles a driver whose fast-linking is too slow to be worth using, where
        // the capability flag would lie to us.
        const auto tReplayStart = std::chrono::steady_clock::now();
        auto elapsedMs = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - tReplayStart).count();
        };
        const int kProbeDraws = 128;     // enough to average out one-off costs
        const double kFastLinkMs = 1.0;  // below this, nothing is being compiled
        bool exitedEarly = false;

        // Fence after EVERY pipeline by default.
        //
        // DXVK queues Present onto the same CS thread that compiles pipelines, so an
        // overlay frame cannot overtake queued compile work: with chunks of 8 the
        // frame gap averaged 25 ms against a 5 ms target and spiked to 305 ms, which
        // is the bar visibly stalling. One pipeline per fence gives the overlay a
        // slot between every compile, which is the smoothest this can be without a
        // second thread -- and loading time is explicitly not the constraint here,
        // an unwarmed pipeline is.
        const uint32_t kFenceChunk = (cfg.fenceChunk > 0) ? (uint32_t)cfg.fenceChunk : 1;
        uint32_t sinceFence = 0;
        IDirect3DQuery9* fence = nullptr;
        dev->CreateQuery(D3DQUERYTYPE_EVENT, &fence);

        size_t at = 0;
        for (uint32_t r : drawList)
        {
            if (at++ == foreignAt)
            {
                // Everything so far built and DXVK quiet, then no recording (see above).
                // The wait has a bar of its own; this one carries on where it was.
                if (sinceFence) { FenceChunk(fence); workDone += kPassW * sinceFence; sinceFence = 0; }
                const uint32_t total = workTotal, done = workDone;
                const auto phase = tPhase;
                WaitUntilIdle("this PC's own keys");
                workTotal = total; workDone = done; tPhase = phase;
                pipelinekeys::VulkanReplay().recordPaused = true;
                Log("replay: Vulkan recording paused for the %zu pipelines only other PCs' files name, t=%.3f",
                    drawList.size() - foreignAt, pipelinekeys::VulkanReplay().Seconds());
            }
            const KeyRecord& k = replayRecs[r];
            ReplaySource& from = replaySources[replaySrc[r]];

            // Shaders. A miss means the capture saw a shader this .fxc database does
            // not contain (a different episode, or a mod) -- skip rather than draw
            // with the wrong one, which would warm a pipeline nothing will use.
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9*  ps = nullptr;
            if (k.vsHash) { auto it = vsByHash.find(k.vsHash); if (it == vsByHash.end()) { missingVS[k.vsHash]++; skippedShader++; from.noShader++; workDone += kPassW; continue; } vs = it->second; }
            if (k.psHash) { auto it = psByHash.find(k.psHash); if (it == psByHash.end()) { missingPS[k.psHash]++; skippedShader++; from.noShader++; workDone += kPassW; continue; } ps = it->second; }

            // Render targets and depth, in the recorded formats and sample count --
            // the field the synthetic pass got most wrong; MRT count, formats and
            // samples are all part of the pipeline. A key from another configuration
            // or PC is warmed as long as this device can create every one of them;
            // if it cannot, a substitute would warm a pipeline nothing here uses.
            IDirect3DSurface9* rts[kMaxRT] = {};
            bool unsupported = false;
            for (uint32_t i = 0; i < kMaxRT; i++)
                if (k.rtFmt[i])
                {
                    rts[i] = ScratchForMS((D3DFORMAT)k.rtFmt[i], k.msType, k.msQuality);
                    unsupported |= !rts[i];
                }
            IDirect3DSurface9* ds = ReplayDepthForMS(k.dsFmt, k.msType, k.msQuality);
            unsupported |= k.dsFmt && !ds;
            if (!rts[0] || unsupported) { skippedRT++; from.noRT++; workDone += kPassW; continue; }

            // Vertex layout.
            UINT stride[4] = { 0, 0, 0, 0 };
            if (k.declIndex != kDeclNone)
            {
                auto decl = ReplayDecl(k.declIndex);
                if (!decl) { skippedDecl++; from.noDecl++; workDone += kPassW; continue; }
                dev->SetVertexDeclaration(decl);

                for (auto& e : replayDecls[k.declIndex])
                {
                    if (e.Stream == 0xFF) break;                 // D3DDECL_END
                    if (e.Stream >= 4) continue;
                    stride[e.Stream] = std::max(stride[e.Stream], (UINT)e.Offset + DeclTypeSize(e.Type));
                }
            }
            else
            {
                dev->SetVertexDeclaration(nullptr);
                dev->SetFVF(k.fvf);
            }
            for (UINT s = 0; s < 4; s++)
                if (stride[s]) dev->SetStreamSource(s, dummyVB, 0, stride[s]);

            // Instancing, as recorded. DXVK builds a per-instance binding (with the
            // recorded divisor) for each INSTANCEDATA stream, and only draws instances
            // when stream 0 carries INDEXEDDATA -- it rejects INSTANCEDATA on stream 0
            // outright. One instance is enough to create the pipeline.
            {
                bool anyInstanced = false;
                for (UINT s = 1; s < kMaxStreams; s++)
                {
                    dev->SetStreamSourceFreq(s, k.streamFreq[s] ? k.streamFreq[s] : 1u);
                    anyInstanced |= k.streamFreq[s] != 0;
                }
                dev->SetStreamSourceFreq(0, anyInstanced ? (D3DSTREAMSOURCE_INDEXEDDATA | 1u) : 1u);
            }

            dev->SetVertexShader(vs);
            dev->SetPixelShader(ps);

            for (uint32_t i = 0; i < kMaxRT; i++)
                dev->SetRenderTarget(i, rts[i]);
            dev->SetDepthStencilSurface(ds);

            D3DVIEWPORT9 vp{ 0, 0, kRTdim, kRTdim, 0.0f, 1.0f };
            dev->SetViewport(&vp);

            for (uint32_t i = 0; i < kNumRS; i++)
                dev->SetRenderState(kTrackedRS[i].rs, k.rs[i]);

            // Sampler dimensions are folded into DXVK's SPIR-V spec constants, so a
            // 2D texture where the game bound a cube is a different pipeline -- and
            // so is a slot DXVK samples with Fetch4 or depth compare, which is what
            // the recorded MODE picks the texture for.
            for (uint32_t i = 0; i < kPSSamplers; i++)
                dev->SetTexture(i, TexForSlot(k.samplerType[i], k.samplerMode[i]));
            for (uint32_t i = 0; i < kVSSamplers; i++)
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i,
                                TexForSlot(k.samplerType[kPSSamplers + i], k.samplerMode[kPSSamplers + i]));

            // The rest of D3D9SpecData: b#, the clip-plane count, the projected
            // mask, the fixed-function stages and the Fetch4 sampler state. After
            // the textures, because the Fetch4 state DXVK keeps per slot is only
            // meaningful once the texture that can do it is bound.
            ApplySpecState(dev, k);

            // One degenerate primitive: the vertex buffer is zeroed, so nothing is
            // rasterised, but the pipeline is created -- which is the whole point.
            dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)k.primType, 0, 0, 3, 0, 1);
            drawn++;
            from.drawn++;

            if (cfg.adaptiveExit && drawn == kProbeDraws)
            {
                double per = (double)elapsedMs() / kProbeDraws;
                if (per < kFastLinkMs)
                {
                    Log("replay: %.3f ms per pipeline over %d draws - the driver is not compiling "
                        "(pipeline libraries / fast linking), so warming buys nothing. Stopping.",
                        per, kProbeDraws);
                    exitedEarly = true;
                    break;
                }
                Log("replay: %.2f ms per pipeline - real compilation, continuing (%zu to go)",
                    per, drawList.size() - drawn);
            }

            // BudgetSpent(), not a budget test of its own: it is also what the
            // boot gate's PrecompileGateMaxSeconds raises, and a pass that only
            // honours the abort in the engine-warm loop cannot unwind from here.
            if (BudgetSpent())
            {
                Log("replay: stopped after %u of %zu pipelines (%s) - the most-used ones are warm",
                    drawn, drawList.size(),
                    gateAbort.load() ? "the gate asked the pass to stop"
                                     : "PrecompileBudgetSeconds reached");
                exitedEarly = true;
                break;
            }

            // Fence every chunk, and only THEN credit the work.
            //
            // Issuing a draw does not compile anything -- DXVK enqueues it, which is
            // why 1863 draws take ~0.13 s while the pass takes ~16 s. Crediting
            // progress per draw therefore filled the bar almost instantly and left
            // the remaining fifteen seconds looking like a freeze at 100%. Waiting
            // on an event query per chunk makes each step mean "these pipelines are
            // actually built", so the bar tracks compilation, and the overlay gets
            // presented throughout the wait instead of only at the end.
            if (++sinceFence >= kFenceChunk)
            {
                FenceChunk(fence);
                workDone += kPassW * sinceFence;
                sinceFence = 0;

                // Update the label a few times a second, not once per pipeline.
                // Re-rendering it forces a glyph pass; at one pipeline per fence that
                // was 573 rebuilds in 886 frames, which defeats caching it at all.
                // Nobody can read a counter changing 110 times a second anyway.
                auto nowLbl = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(nowLbl - tLastLabel).count() >= 250)
                {
                    tLastLabel = nowLbl;
                    curLabel = "pipeline " + std::to_string(drawn) + " / " + std::to_string(drawList.size());
                }
            }
            PresentOverlay(false);
        }

        // Credit and drain whatever is left in the final partial chunk.
        if (sinceFence)
        {
            FenceChunk(fence);
            workDone += kPassW * sinceFence;
        }
        SAFE_RELEASE(fence);

        // Unbind, so nothing below inherits a scratch target or dummy stream.
        for (uint32_t i = 1; i < kMaxRT; i++) dev->SetRenderTarget(i, nullptr);
        for (UINT s = 0; s < 4; s++) dev->SetStreamSource(s, nullptr, 0, 0);
        for (UINT s = 0; s < kMaxStreams; s++) dev->SetStreamSourceFreq(s, 1);
        dev->SetIndices(nullptr);
        for (uint32_t i = 0; i < kPSSamplers; i++) dev->SetTexture(i, nullptr);
        for (uint32_t i = 0; i < kVSSamplers; i++) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, nullptr);
        // Fetch4 is the one piece of state the D3DSBT_ALL restore cannot undo:
        // the game's own bias value goes through the same SetStateSamplerState
        // and only 'GET1' clears DXVK's latch, so sweep every pixel sampler here
        // unconditionally. It is self-healing -- if the game itself had Fetch4
        // on a slot, the state block writes 'GET4' back and re-latches it.
        for (uint32_t i = 0; i < kPSSamplers; i++)
            dev->SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, MAKEFOURCC('G','E','T','1'));

        // If we stopped early the bar must still reach 100%, or it reads as a hang.
        if (exitedEarly) workDone = workTotal;

        Log("replay: drew %u of %zu pipelines in %llds%s (skipped %u duplicate-state, %u no-shader, %u no-decl, %u no-RT)",
            drawn, drawList.size(), (long long)(elapsedMs() / 1000), exitedEarly ? " [stopped early]" : "",
            skippedDup, skippedShader, skippedDecl, skippedRT);
        // A sampler mode the device could not give us a texture for. Not fatal --
        // the slot warms its default-mode pipeline instead -- but it means the key
        // asked for something this device cannot reach, which is worth knowing
        // before anyone explains a stutter by the key being wrong.
        // Counted per slot per DRAW, not per distinct slot: one slot on five hundred
        // keys is five hundred here. Say "binds" so nobody reads it as coverage.
        if (specFallbacks)
            Log("replay: %u sampler binds wanted Fetch4 or depth compare and got the plain "
                "texture instead (R32F scratch %s, D24S8 scratch %s) - those binds warmed the "
                "default-mode pipeline, not the one the key recorded",
                specFallbacks, texFetch4 ? "created" : "REFUSED by the device",
                texShadow ? "created" : "REFUSED by the device");

        if (!missingVS.empty() || !missingPS.empty())
        {
            auto worst = [](const std::unordered_map<uint64_t, uint32_t>& m, char tag)
            {
                std::vector<std::pair<uint64_t, uint32_t>> v(m.begin(), m.end());
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t i = 0; i < v.size() && i < 5; i++)
                    Log("replay:   unresolved %cS %016llx costs %u keys",
                        tag, (unsigned long long)v[i].first, v[i].second);
            };
            Log("replay: %zu distinct VS and %zu distinct PS could not be resolved",
                missingVS.size(), missingPS.size());
            worst(missingVS, 'V');
            worst(missingPS, 'P');
        }

        // What each file's own keys came to. A file from another install names
        // shaders this one lacks (other mods, another FusionFix build); one from
        // another configuration may need targets this device cannot create. Both are
        // counted here, never drawn with a substitute.
        for (const auto& s : replaySources)
        {
            if (!s.added) continue;
            char buf[256];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        ": of its %u new keys, %u warmed, %u the same pipeline as another key, %u name a shader "
                        "this install lacks, %u render targets or depth this device cannot create, %u no declaration%s",
                        s.added, s.drawn, s.samePipeline, s.noShader, s.noRT, s.noDecl,
                        (exitedEarly || !cfg.specVariants) ? " (the pass did not reach all of them)" : "");
            LogStr("replay:   " + s.label + buf);
        }
    }

    // ===================================================================
    //  Engine warm phase — a render phase of our own, over every coordinate
    //  RAGE's own tables say gameplay can produce.
    //
    //  The recorded replay above warms what somebody drew. This warms what the
    //  ENGINE can draw, which is a different and much larger set: 04-poc drove
    //  one coordinate and DXVK built 1559 pipelines of which the whole
    //  accumulated recording contained 13. What it adds over 04-poc is the
    //  three axes an effect walk cannot see -- the render-target/depth formats
    //  and sample count, the blend/colour-write half of the state, and the
    //  vertex declaration -- all read from the engine at this gate
    //  (enginewarm.h; evidence in re/rage-shader-precompile/06-phases.md and
    //  07-geometry.md).
    //
    //  WHOSE DEVICE. RAGE's wrapper at 0x017ED8D8 is never touched. Its
    //  redundancy filter (the shadow at 0x017ED9A8) stays coherent precisely
    //  BECAUSE we only ever drive DXVK's real device and hand it back
    //  byte-identical; routing state through grcDevice::SetRenderState
    //  (0x00437140) instead would rewrite the engine-wide default block at
    //  0x0106B420 and the damage would only surface the next time
    //  ApplyAllDefaultRenderStates ran -- a delayed, silent corruption. The
    //  values still come from the engine: the default block, the phase state
    //  vectors and each pass's own PASS_VALUE delta, mapped through the live
    //  g_RageStateToD3DRS, which is exactly what grcEffectPass::Apply does.
    // ===================================================================
    static inline std::vector<IDirect3DVertexDeclaration9*> engineDeclObjs;

    // ===================================================================
    //  A D3D9 DEVICE OF OUR OWN, FOR THE PIPELINES WE DO NOT WANT TO KEEP
    //
    //  DXVK never frees a graphics pipeline while its device lives. The proof
    //  is in DXVK 3.1.1's own source, and it is three facts deep:
    //
    //   * DxvkPipelineManager::m_graphicsPipelines is an unordered_map that is
    //     only ever inserted into (createGraphicsPipeline, dxvk_pipemanager.cpp
    //     :222); nothing erases from it and the destructor is the only teardown.
    //   * DxvkGraphicsPipeline::releasePipeline (dxvk_graphics.cpp:1172) returns
    //     on its first line unless DxvkDevice::mustTrackPipelineLifetime().
    //   * mustTrackPipelineLifetime (dxvk_device.cpp:133) needs
    //     canUseGraphicsPipelineLibrary() on EVERY branch -- so with
    //     `dxvk.enableGraphicsPipelineLibrary = False` it is false whatever the
    //     trackPipelineLifetime option says -- and on Auto it is false for
    //     VK_DRIVER_ID_MESA_RADV_KHR anyway. Even when it does run it drops only
    //     the fast-linked BASE pipelines and keeps the optimized ones, which are
    //     the only kind this pass builds.
    //
    //  So there is nothing a D3D9 caller can release, reset or configure that
    //  gives an optimized pipeline back. Only ~DxvkDevice does, and a DxvkDevice
    //  belongs to exactly one D3D9 device.
    //
    //  Hence: the engine walk gets a D3D9 device of its own and we destroy it
    //  when the walk is done. What survives is the DRIVER's on-disk cache, which
    //  is keyed on the SPIR-V and the pipeline state and not on the device --
    //  which is the same thing vkcapture's Fossilize replay relies on when it
    //  creates a pipeline and destroys it in the next statement.
    //
    //  The device is created from the game's OWN IDirect3D9 with the game's own
    //  adapter ordinal and behaviour flags, so DXVK gives it the same options,
    //  the same feature set and therefore the same SPIR-V for the same bytecode.
    // ===================================================================
    static inline IDirect3D9* warmD3D = nullptr;
    static inline IDirect3DDevice9* edev = nullptr;
    static inline HWND warmWnd = nullptr;
    static inline bool warmClassOk = false;

    static bool CreateWarmDevice()
    {
        if (edev) return true;
        if (!dev) return false;

        if (!warmClassOk)
        {
            WNDCLASSEXA wc{ sizeof(wc) };
            wc.lpfnWndProc = DefWindowProcA;
            wc.hInstance = (HINSTANCE)hSelf;
            wc.lpszClassName = "FusionFixWarmDevice";
            warmClassOk = RegisterClassExA(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            if (!warmClassOk)
            {
                Log("warm device: RegisterClassEx failed (%lu)", GetLastError());
                return false;
            }
        }
        // Never shown and never activated. DXVK builds a Vulkan surface for the
        // device window at creation time (Presenter's constructor, unless
        // deferSurfaceCreation), so it has to be a real HWND -- but nothing is
        // ever presented to it.
        warmWnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "FusionFixWarmDevice",
                                  "FusionFix warm", WS_POPUP, 0, 0, 8, 8,
                                  nullptr, nullptr, (HINSTANCE)hSelf, nullptr);
        if (!warmWnd)
        {
            Log("warm device: CreateWindowEx failed (%lu)", GetLastError());
            return false;
        }

        if (FAILED(dev->GetDirect3D(&warmD3D)) || !warmD3D)
        {
            Log("warm device: GetDirect3D failed");
            DestroyWarmDevice();
            return false;
        }
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (FAILED(dev->GetCreationParameters(&cp)))
        {
            Log("warm device: GetCreationParameters failed");
            DestroyWarmDevice();
            return false;
        }
        // The game's own flags, so DXVK's shader translation and device options
        // match -- minus PUREDEVICE (which would refuse the Get* calls the pass
        // makes) and plus the two that say "this window is not yours to touch".
        DWORD flags = (cp.BehaviorFlags & ~(DWORD)D3DCREATE_PUREDEVICE)
                    | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED;
        D3DPRESENT_PARAMETERS pp{};
        pp.BackBufferWidth  = 64;
        pp.BackBufferHeight = 64;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount  = 1;
        pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow    = warmWnd;
        pp.Windowed         = TRUE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        const int64_t t0 = pipelinekeys::ClockUs();
        // vkcapture tells this VkDevice from the game's by this flag: it wraps
        // it for the counters but must not record, replay or report on it.
        pipelinekeys::VulkanReplay().modOwnedDevice = true;
        HRESULT hr = warmD3D->CreateDevice(cp.AdapterOrdinal, cp.DeviceType, warmWnd, flags, &pp, &edev);
        pipelinekeys::VulkanReplay().modOwnedDevice = false;
        if (FAILED(hr) || !edev)
        {
            edev = nullptr;
            Log("warm device: CreateDevice(adapter %u, type %u, flags 0x%08lX) failed (hr=0x%08lX)",
                cp.AdapterOrdinal, (unsigned)cp.DeviceType, (unsigned long)flags, (unsigned long)hr);
            DestroyWarmDevice();
            return false;
        }
        Log("warm device: created in %.2f s on adapter %u, flags 0x%08lX (the game's own, less "
            "PUREDEVICE) - every pipeline the engine walk builds goes here and dies with it",
            (pipelinekeys::ClockUs() - t0) / 1e6, cp.AdapterOrdinal, (unsigned long)flags);
        return true;
    }

    // Destroying this device is the whole point of having it, so it says what
    // happened rather than assuming: how many references were left (anything but
    // 0 means one of our own objects outlived the release and DXVK's
    // D3D9DeviceChild kept the device alive with it), how long DXVK took to tear
    // ~61,000 pipelines down, and what the address space looked like on the
    // other side.
    static void DestroyWarmDevice()
    {
        const bool real = edev != nullptr;
        const int64_t t0 = pipelinekeys::ClockUs();
        if (edev)
        {
            // THE OBJECTS FIRST, ALWAYS. DXVK's D3D9DeviceChild holds a
            // reference on its device for as long as it lives
            // (d3d9_device_child.h), so releasing the device while one of our
            // render targets, buffers or shader mirrors is still alive does not
            // destroy it -- it leaves a live DxvkDevice with nobody holding the
            // pointer, its pipelines still resident, and, a few lines further
            // down, its Vulkan surface pointing at a window we just destroyed.
            // The failure paths out of CreateEngineResources used to do exactly
            // that, so the invariant lives here rather than in every caller.
            ReleaseWarmDeviceObjects();
            const ULONG left = edev->Release();
            edev = nullptr;
            if (real)
                Log("warm device: released, %lu references left (0 is what destroys it), "
                    "torn down in %.2f s", (unsigned long)left,
                    (pipelinekeys::ClockUs() - t0) / 1e6);
        }
        if (warmD3D) { warmD3D->Release(); warmD3D = nullptr; }
        if (warmWnd) { DestroyWindow(warmWnd); warmWnd = nullptr; }
        if (real) LogVa("engine: the instant the warm device was released");
    }

    // Everything the engine walk binds, on the device the walk is running on.
    // A D3D9 object belongs to the device that created it, so when the walk is
    // on a device of ours NONE of the pass's shared resources may be used -- it
    // gets its own copies of all of them, including a mirror of every engine
    // shader object, built from the same bytecode (which is what DXVK keys its
    // shader modules on, so the pipeline identity is unchanged).
    struct EngineRes
    {
        IDirect3DVertexBuffer9*  vb = nullptr;
        IDirect3DIndexBuffer9*   ib = nullptr;
        IDirect3DTexture9*       t2 = nullptr;
        IDirect3DCubeTexture9*   tc = nullptr;
        IDirect3DVolumeTexture9* tv = nullptr;
    };
    static inline EngineRes eres;
    static inline std::unordered_map<void*, IDirect3DVertexShader9*> eVsByObj;
    static inline std::unordered_map<void*, IDirect3DPixelShader9*>  ePsByObj;
    // The engine's shader BYTECODE, kept for the life of the walk so a mirror
    // can be rebuilt on each new warm device (ResolveEngineShaderIO reads it
    // once; the walk goes through several devices). ~2000 shaders, a few KB
    // each. The mirrors themselves are made lazily, so a device only ever
    // carries the shaders its own slice of the job list actually binds.
    static inline std::unordered_map<void*, std::vector<uint8_t>> eShaderBytes;
    static inline uint32_t eMirrorFailed = 0;

    // The device the engine walk draws on: ours when there is one, the game's
    // otherwise (PrecompileWarmDevice = 0, or a device we could not create).
    static IDirect3DDevice9* EDev() { return edev ? edev : dev; }

    static IDirect3DVertexBuffer9* EVB() { return edev ? eres.vb : dummyVB; }
    static IDirect3DIndexBuffer9*  EIB() { return edev ? eres.ib : dummyIB; }
    static IDirect3DBaseTexture9*  ETex(uint8_t dim)
    {
        if (edev)
            return dim == 3 ? (IDirect3DBaseTexture9*)eres.tc
                 : dim == 4 ? (IDirect3DBaseTexture9*)eres.tv
                            : (IDirect3DBaseTexture9*)eres.t2;
        return dim == 3 ? (IDirect3DBaseTexture9*)texCube
             : dim == 4 ? (IDirect3DBaseTexture9*)texVol
                        : (IDirect3DBaseTexture9*)tex2D;
    }
    static IDirect3DVertexShader9* EVS(IDirect3DVertexShader9* obj)
    {
        if (!edev) return obj;
        if (!obj) return nullptr;
        auto it = eVsByObj.find((void*)obj);
        if (it != eVsByObj.end()) return it->second;
        IDirect3DVertexShader9* mine = nullptr;
        auto b = eShaderBytes.find((void*)obj);
        if (b == eShaderBytes.end() ||
            FAILED(edev->CreateVertexShader((const DWORD*)b->second.data(), &mine)))
            mine = nullptr;
        if (!mine) eMirrorFailed++;
        eVsByObj[(void*)obj] = mine;
        return mine;
    }
    static IDirect3DPixelShader9* EPS(IDirect3DPixelShader9* obj)
    {
        if (!edev) return obj;
        if (!obj) return nullptr;
        auto it = ePsByObj.find((void*)obj);
        if (it != ePsByObj.end()) return it->second;
        IDirect3DPixelShader9* mine = nullptr;
        auto b = eShaderBytes.find((void*)obj);
        if (b == eShaderBytes.end() ||
            FAILED(edev->CreatePixelShader((const DWORD*)b->second.data(), &mine)))
            mine = nullptr;
        if (!mine) eMirrorFailed++;
        ePsByObj[(void*)obj] = mine;
        return mine;
    }

    static bool CreateEngineResources()
    {
        if (!edev) return true;
        bool ok = true;
        if (SUCCEEDED(edev->CreateVertexBuffer(64 * 1024, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &eres.vb, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(eres.vb->Lock(0, 0, &p, 0))) { memset(p, 0, 64 * 1024); eres.vb->Unlock(); }
        }
        else ok = false;
        if (SUCCEEDED(edev->CreateIndexBuffer(1024 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                              D3DPOOL_DEFAULT, &eres.ib, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(eres.ib->Lock(0, 0, &p, 0))) { memset(p, 0, 1024 * sizeof(uint16_t)); eres.ib->Unlock(); }
        }
        else ok = false;
        ok &= SUCCEEDED(edev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &eres.t2, nullptr));
        ok &= SUCCEEDED(edev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &eres.tc, nullptr));
        ok &= SUCCEEDED(edev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &eres.tv, nullptr));
        if (!ok) Log("warm device: could not create every scratch resource on it");
        return ok;
    }

    // What the walk is allowed to build on the GAME's device when a device of
    // our own could not be created. 13.7 KB of address space per pipeline was
    // measured on this rig (2026-09-21: 60,821 pipelines cost 836 MB of the
    // 3093 MB the process had, and took the largest free RUN from 578 MB to
    // 27 MB), so 8192 jobs is ~112 MB of pipelines against the walk's 836.
    static constexpr uint32_t kCappedJobs = 8192;

    // How many jobs one warm device gets before it is destroyed and replaced.
    //
    // This is the number that makes the feature safe, and why it is needed at
    // all is not obvious, so: destroying the device DOES free everything the
    // pipelines held -- measured, `0 references left`, 61,564 pipelines torn
    // down in 0.08 s -- but the free virtual ADDRESS SPACE does not come back.
    // The per-pipeline cost is ordinary C++ allocation on the PE-side heap, and
    // a Windows heap does not decommit an interleaved free list. What it does
    // do is REUSE it, and that is measurable: with one device per 8192 jobs the
    // walk's whole cost is its FIRST chunk and every chunk after it is free.
    //   8192 jobs in (device 2): free 2916 MB, largest run 442 MB
    //  16384 jobs in (device 3): free 2916 MB, largest run 442 MB
    //  ...
    //  49152 jobs in (device 7): free 2914 MB, largest run 442 MB
    // 179 MB for the whole 61,191-job walk instead of 836, and the largest free
    // run holds at 442 MB instead of collapsing to 27. A smaller chunk would
    // shrink the 179 MB further, at one more device swap (~0.03 s) per chunk.
    static constexpr uint32_t kWarmChunkJobs = 8192;

    static void ReleaseEngineResources()
    {
        for (auto& kv : eVsByObj) SAFE_RELEASE(kv.second);
        for (auto& kv : ePsByObj) SAFE_RELEASE(kv.second);
        eVsByObj.clear();
        ePsByObj.clear();
        SAFE_RELEASE(eres.vb);
        SAFE_RELEASE(eres.ib);
        SAFE_RELEASE(eres.t2);
        SAFE_RELEASE(eres.tc);
        SAFE_RELEASE(eres.tv);
    }

    // Everything the walk owns, in the order COM needs it: the objects first,
    // the device they belong to last. When that device is ours, this is the
    // moment DXVK destroys the pipelines -- ~DxvkDevice is the only code in it
    // that ever does (see the block above), so this call IS the fix.
    // Everything the CURRENT warm device owns, without touching what the walk
    // needs to carry on somewhere else (the engine's shader references and the
    // bytecode the mirrors are built from).
    static void ReleaseWarmDeviceObjects()
    {
        for (auto& d : engineDeclObjs) SAFE_RELEASE(d);
        for (auto& r : engineRTs) SAFE_RELEASE(r.surf);
        engineRTs.clear();
        for (auto& s : engineDepths) { SAFE_RELEASE(s.surf); SAFE_RELEASE(s.tex); }
        engineDepths.clear();
        ReleaseEngineResources();
    }

    static void ReleaseEngineWalk()
    {
        ReleaseWarmDeviceObjects();
        engineDeclObjs.clear();
        eShaderBytes.clear();
        ReleaseEngineShaderRefs();
        DestroyWarmDevice();
    }

    // Swap the walk onto a fresh warm device: everything the old one holds goes
    // with it, and the new one starts empty. False if a new device could not be
    // made, in which case the caller stops the walk rather than carry on
    // building pipelines the game will have to live with.
    static bool RotateWarmDevice(const enginewarm::Plan& plan)
    {
        ReleaseWarmDeviceObjects();
        DestroyWarmDevice();
        if (!CreateWarmDevice() || !CreateEngineResources())
        {
            DestroyWarmDevice();
            return false;
        }
        for (size_t i = 0; i < plan.decls.size() && i < engineDeclObjs.size(); i++)
        {
            std::vector<D3DVERTEXELEMENT9> e = plan.decls[i].elems;
            e.push_back(D3DDECL_END());
            engineDeclObjs[i] = nullptr;
            if (FAILED(EDev()->CreateVertexDeclaration(e.data(), &engineDeclObjs[i])))
                engineDeclObjs[i] = nullptr;
        }
        ApplyDefaultBlock(plan);
        return true;
    }

    // Diagnostic arm of PrecompileWarmDevice: can a second D3D9 device be made
    // here at all, and what does one cost while it exists? Answered before the
    // long pass, so a run that dies later still says it in the log.
    static void ProbeWarmDevice()
    {
        LogVa("before the warm-device probe");
        const bool ok = CreateWarmDevice();
        if (ok) LogVa("with a second D3D9 device alive");
        DestroyWarmDevice();
        LogVa("after the second D3D9 device was destroyed");
        Log("warm device: probe %s", ok ? "OK - a device of our own can be created at this gate"
                                        : "FAILED - the engine walk cannot be moved off the game's device");
    }

    // A distinct surface per (format, MRT slot). The recorded replay reuses one
    // surface per format, which means the G-buffer's three A8R8G8B8 targets are
    // the same image in three attachments -- a Vulkan feedback loop that is
    // only harmless because nothing is ever rasterised. The engine walk hits
    // that set on most of its jobs, so it gets its own targets instead; a
    // 64x64 surface costs 16 KB and there are at most a dozen.
    struct EngineRT { D3DFORMAT fmt; int slot; uint32_t ms, msq; IDirect3DSurface9* surf; };
    static inline std::vector<EngineRT> engineRTs;

    static IDirect3DSurface9* EngineScratch(D3DFORMAT fmt, int slot, uint32_t ms, uint32_t msq)
    {
        // Slot 0 shares the replay's surface -- but only when the walk is on the
        // game's device. On a device of our own nothing of the pass's is usable.
        if (!edev && slot == 0) return ScratchForMS(fmt, ms, msq);
        for (auto& r : engineRTs)
            if (r.fmt == fmt && r.slot == slot && r.ms == ms && r.msq == msq) return r.surf;
        IDirect3DSurface9* s = nullptr;
        if (FAILED(EDev()->CreateRenderTarget(kRTdim, kRTdim, fmt, (D3DMULTISAMPLE_TYPE)ms, msq, FALSE, &s, nullptr)))
            s = nullptr;
        engineRTs.push_back({ fmt, slot, ms, msq, s });
        return s;
    }

    // The same for depth. ReplayDepthForMS's cache lives on the game's device,
    // so the walk keeps its own when it is somewhere else.
    static inline std::vector<ScratchDS> engineDepths;

    static IDirect3DSurface9* EngineDepth(uint32_t fmt, uint32_t type, uint32_t quality)
    {
        if (!edev) return ReplayDepthForMS(fmt, type, quality);
        if (fmt == 0) return nullptr;
        if (type && d3d9cache::IsFourCCDepth(fmt)) fmt = (uint32_t)D3DFMT_D24S8;
        for (auto& s : engineDepths)
            if (s.fmt == fmt && s.type == type && s.quality == quality) return s.surf;

        ScratchDS s{ fmt, type, quality, nullptr, nullptr };
        if (d3d9cache::IsFourCCDepth(fmt))
        {
            if (SUCCEEDED(edev->CreateTexture(kRTdim, kRTdim, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)fmt,
                                              D3DPOOL_DEFAULT, &s.tex, nullptr)) && s.tex)
                s.tex->GetSurfaceLevel(0, &s.surf);
        }
        else if (FAILED(edev->CreateDepthStencilSurface(kRTdim, kRTdim, (D3DFORMAT)fmt, (D3DMULTISAMPLE_TYPE)type,
                                                        quality, FALSE, &s.surf, nullptr)))
        {
            s.surf = nullptr;
        }
        engineDepths.push_back(s);
        return s.surf;
    }

    // MSVC refuses __try in a function that needs C++ unwinding, so the SEH
    // frame lives in this trivial one and the work that allocates lives in
    // the callees.
    struct BuildArgs
    {
        fxc_db* db;
        int level;
        enginewarm::Plan* out;
        D3DFORMAT backBuffer;
        const std::vector<bootgate::PhaseSet>* phases;
        const std::vector<bootgate::Target>*   registry;
    };
    static void DoBuildTables(void* a)
    {
        auto* b = static_cast<BuildArgs*>(a);
        enginewarm::BuildTables(*b->out, b->db, *b->phases, *b->registry, b->backBuffer);
    }
    static void DoBuildJobs(void* a)
    {
        auto* b = static_cast<BuildArgs*>(a);
        enginewarm::BuildPlanJobs(*b->out, b->level);
    }
    static bool GuardedCall(void (*fn)(void*), void* arg)
    {
        __try { fn(arg); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Apply RAGE's engine-wide default render state, then a phase's vector,
    // the way a render phase does before it emits a bucket draw.
    static void ApplyDefaultBlock(const enginewarm::Plan& p)
    {
        IDirect3DDevice9* d = EDev();
        for (uint32_t i = 0; i < enginewarm::kRageStates; i++)
            if (enginewarm::RSReaches(p.stateToRS[i]))
                d->SetRenderState((D3DRENDERSTATETYPE)p.stateToRS[i], p.defaultRS[i]);
        // Not in the 43-entry table, and every one of them is a DXVK spec
        // constant -- so whatever the last thing to touch the device left here
        // rides along in the key of every job that follows. On the boot gate
        // that "last thing" is the game's own loading screen, once every slice.
        // 10-key-spec measured pointSprite at 0 across all 757 route keys and
        // fog off; those are the values a real draw is built with.
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        // The other two thirds of spec constant 2, which every pixel shader
        // that SAMPLES declares (d3d9_shader.cpp:951) -- not just the
        // fixed-function ones. Left ambient, the projected-texture mask and
        // D3DRS_SPECULARENABLE ride along in the key of every job, and they are
        // the gate's on the game's device and zero on a device of our own.
        d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        for (DWORD s = 0; s < pipelinekeys::kFFStages; s++)
            d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        // And Fetch4, which is a SAMPLER state: the recorded replay runs before
        // this walk and may have left 'GET4' on a slot, which would specialise
        // spec constant 5 on every job that binds a texture there. 'GET1' is the
        // only value that clears DXVK's latch -- 0 leaves it exactly as it was.
        for (DWORD s = 0; s < pipelinekeys::kPSSamplers; s++)
            d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, MAKEFOURCC('G','E','T','1'));
        // The four vertex-texture samplers are half of DXVK's spec constant 3
        // and nothing in this pass ever binds them, so whatever the recorded
        // replay last left there would ride along on every engine draw. Null
        // them wherever the default block is re-established, which is at the
        // start of the walk and after every overlay frame.
        for (uint32_t i = 0; i < pipelinekeys::kVSSamplers; i++)
            d->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, nullptr);
        // The OTHER half of spec constant 3, and all of spec constant 6: the
        // bool registers, masked by each shader's own boolMask. Nothing in the
        // walk sets them, so on the game's device they are the gate's and on a
        // brand-new device of ours they are all FALSE -- which would make the
        // two arms build two different pipeline sets from the same jobs and
        // call it an A/B. Pin the game's, on whichever device is being used.
        if (ambientBoolsRead)
        {
            d->SetVertexShaderConstantB(0, ambientVsB, 16);
            d->SetPixelShaderConstantB(0, ambientPsB, 16);
        }
    }

    // A phase callback pushes exactly this much that DXVK can bake: the four
    // write masks, blending on or off, alpha test on or off. The blend
    // FACTORS stay where the engine's default block put them, because that is
    // where they come from in gameplay -- the callbacks do not touch them and
    // the pass's own delta does (enginewarm::ResolveRS).
    static void ApplyStateVec(const enginewarm::StateVec& v)
    {
        IDirect3DDevice9* d = EDev();
        d->SetRenderState(D3DRS_COLORWRITEENABLE,  v.cwe[0]);
        d->SetRenderState(D3DRS_COLORWRITEENABLE1, v.cwe[1]);
        d->SetRenderState(D3DRS_COLORWRITEENABLE2, v.cwe[2]);
        d->SetRenderState(D3DRS_COLORWRITEENABLE3, v.cwe[3]);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, v.blendEnable);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, v.alphaTest);
    }

    // References taken on the engine's own shader objects for the duration of
    // the walk, one per distinct object.
    static inline std::vector<IUnknown*> engineShaderRefs;

    static void ReleaseEngineShaderRefs()
    {
        for (auto* u : engineShaderRefs) if (u) u->Release();
        engineShaderRefs.clear();
    }

    // Read every pass's shader objects, and only then agree to bind them.
    //
    // Two things come out of this, and both used to come from the .fxc database
    // joined to the engine by ARRAY INDEX: the VS input signature the vertex
    // declarations are projected onto, and the sampler slots the pixel shader
    // declares. A D3D9 shader object hands its own token stream back through
    // GetFunction, so the engine can answer both itself -- no join, no
    // assumption about which directory the file came from, and it works for a
    // mod's effect that has no .fxc entry here at all.
    //
    // It is also where the reference is taken. The walk holds these pointers
    // for minutes while the streamer runs on another thread, and grcEffect has
    // a destructor (0x00435640) that LoadOrCreateByName can reach; a freed
    // effect releases its shader objects, and the next SetVertexShader would be
    // a virtual call through a dangling COM pointer. The object has just
    // answered GetFunction with a well-formed SM3 token stream, so it is alive
    // at this instant: AddRef it once here and release at the end of the pass.
    // A pass whose vertex shader does not answer is dropped outright rather
    // than drawn with a pointer nothing validated.
    static void ResolveEngineShaderIO(enginewarm::Plan& plan)
    {
        struct Info
        {
            std::vector<std::pair<uint8_t, uint8_t>> sig;
            uint8_t  dim[16]{};
            uint64_t hash = 0;
            bool ok = false;
        };
        std::unordered_map<void*, Info> byObj;
        std::vector<uint8_t> buf(64 * 1024);
        uint32_t objs = 0, read = 0, failed = 0;

        auto resolve = [&](void* obj, bool isVS) -> const Info*
        {
            if (!obj) return nullptr;
            if (auto it = byObj.find(obj); it != byObj.end())
                return it->second.ok ? &it->second : nullptr;
            objs++;
            Info info;
            const UINT size = ReadShaderFunction(obj, isVS, buf.data(), (UINT)buf.size());
            if (size)
            {
                read++;
                ShaderIO io{};
                ParseShaderIO(buf.data(), size, isVS, io);
                // The bytecode hash is the name every recorded key and the
                // containment scorer know a shader by, so the emission log can
                // be joined to either without a .fxc lookup.
                info.hash = pipelinekeys::Fnv1a(buf.data(), size);
                if (isVS)
                {
                    for (auto& in : io.inputs) info.sig.push_back({ in.usage, in.index });
                    // A vertex shader declares s0..s3, which are
                    // D3DVERTEXTEXTURESAMPLER0..3 and are the low byte of
                    // DXVK's specialization constant 3.
                    for (int s = 0; s < 4; s++)
                        info.dim[s] = io.usesSampler[s] ? (io.samplerDim[s] ? io.samplerDim[s] : 2) : 0;
                    auto* sh = static_cast<IDirect3DVertexShader9*>(obj);
                    sh->AddRef();
                    engineShaderRefs.push_back(sh);
                }
                else
                {
                    for (int s = 0; s < 16; s++)
                        info.dim[s] = io.usesSampler[s] ? (io.samplerDim[s] ? io.samplerDim[s] : 2) : 0;
                    auto* sh = static_cast<IDirect3DPixelShader9*>(obj);
                    sh->AddRef();
                    engineShaderRefs.push_back(sh);
                }
                // The walk's own device cannot bind the engine's object, so it
                // binds one built from the same bytes -- DXVK keys its shader
                // modules on a hash of the bytecode, so the two are the same
                // DxvkShader as far as a pipeline identity goes, which is the
                // same thing that makes the .fxc replay warm pipelines the
                // game's own objects then reuse. The mirror is made on demand
                // (EVS / EPS) because the walk goes through several devices and
                // each one should only carry the shaders its own slice binds;
                // what is kept here is the bytecode to build them from.
                if (cfg.warmDevice == 1)
                    eShaderBytes[obj].assign(buf.data(), buf.data() + size);
                info.ok = true;
            }
            else failed++;
            auto ins = byObj.emplace(obj, std::move(info));
            return ins.first->second.ok ? &ins.first->second : nullptr;
        };

        for (auto& pass : plan.passes)
        {
            const Info* v = resolve(pass.vs, true);
            if (!v) { pass.vs = nullptr; continue; }
            pass.sig = v->sig;
            pass.vsHash = v->hash;
            memcpy(pass.vsDim, v->dim, sizeof(pass.vsDim));
            const Info* p = resolve(pass.ps, false);
            if (p) { memcpy(pass.psDim, p->dim, sizeof(pass.psDim)); pass.psHash = p->hash; }
            else   pass.ps = nullptr;        // no bytecode, no binding
        }
        const size_t before = plan.passes.size();
        plan.passes.erase(std::remove_if(plan.passes.begin(), plan.passes.end(),
                                         [](const enginewarm::PassInfo& p) { return p.vs == nullptr; }),
                          plan.passes.end());
        plan.census.shaderObjects = objs;
        plan.census.shaderRead    = read;
        plan.census.shaderFailed  = failed;
        plan.census.passesDropped = (uint32_t)(before - plan.passes.size());
        plan.census.passes        = (uint32_t)plan.passes.size();
    }

    // One job's draw, in a function of its own so GuardedCall can put an SEH
    // frame round it: everything it touches is a pointer read out of the
    // engine, and a fault here should cost the phase, not the session.
    struct DrawArgs
    {
        const enginewarm::Plan* plan;
        const enginewarm::Job*  job;
        IDirect3DSurface9* rts[4];
        IDirect3DSurface9* ds;
        IDirect3DVertexDeclaration9* declObj;
        bool failed;
        bool noMirror;      // the shaders would not build on this warm device
    };

    static void DoDrawJob(void* a)
    {
        auto* d = static_cast<DrawArgs*>(a);
        const enginewarm::Plan& plan = *d->plan;
        const enginewarm::Job&  j    = *d->job;
        const enginewarm::PassInfo& pass = plan.passes[j.pass];
        const enginewarm::StateVec& SV   = enginewarm::kStateVecs[j.state];
        const enginewarm::Decl& D = plan.decls[j.decl];
        IDirect3DDevice9* dv = EDev();

        // NO MIRROR, NO DRAW. On a device of our own the engine's shader
        // objects cannot be bound and a copy built from the same bytecode is,
        // but a copy that would not build caches as null -- and binding null is
        // not "no pipeline", it is the FIXED-FUNCTION pipeline, which DXVK
        // builds happily, which succeeds, and which would then be counted as a
        // job warmed. It warms the wrong thing and lies about it, so it is
        // skipped and counted where it can be seen.
        IDirect3DVertexShader9* vsObj = EVS(pass.vs);
        IDirect3DPixelShader9*  psObj = EPS(pass.ps);
        if ((pass.vs && !vsObj) || (pass.ps && !psObj))
        {
            d->noMirror = true;
            return;
        }

        for (int i = 0; i < 4; i++) dv->SetRenderTarget(i, d->rts[i]);
        dv->SetDepthStencilSurface(d->ds);
        D3DVIEWPORT9 vp{ 0, 0, 1, 1, 0.0f, 1.0f };
        dv->SetViewport(&vp);

        dv->SetVertexDeclaration(d->declObj);
        for (UINT s = 0; s < 4; s++)
            if (D.stride[s]) dv->SetStreamSource(s, EVB(), 0, D.stride[s]);
        // Rebound every job rather than once before the walk: the overlay's
        // ID3DXFont drives an ID3DXSprite between jobs whenever the progress
        // text changes, and whether its state block covers SetIndices is
        // D3DX's business, not ours. One cached call against ~10 ms of work.
        dv->SetIndices(EIB());
        if (D.instanced)
        {
            // DXVK only draws instances when stream 0 carries INDEXEDDATA,
            // and only stream 1's INSTANCEDATA is in the pipeline key.
            dv->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1u);
            dv->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        }

        dv->SetVertexShader(vsObj);
        dv->SetPixelShader(psObj);
        // The material's binding pattern: every declared slot, or one of them
        // left null, or none of them. It is the axis the engine cannot answer
        // -- which of the slots a shader declares actually receives a texture
        // is decided by the .wtd a model references -- and it is the residue
        // the render-target axis is not (10-key-spec.md §7).
        {
            uint8_t dim[16];
            memcpy(dim, pass.psDim, sizeof(dim));
            if (j.psVariant == enginewarm::kPSV_NoneBound) memset(dim, 0, sizeof(dim));
            else if (j.psVariant != enginewarm::kPSV_AllBound) dim[j.psVariant - 1] = 0;
            for (int s = 0; s < 16; s++)
                dv->SetTexture(s, dim[s] ? ETex(dim[s]) : nullptr);
        }
        for (int s = 0; s < 4; s++)
        {
            IDirect3DBaseTexture9* t = nullptr;
            if ((j.flags & enginewarm::kJF_VSSampler) && pass.vsDim[s])
                t = ETex(pass.vsDim[s]);
            dv->SetTexture(D3DVERTEXTEXTURESAMPLER0 + s, t);
        }

        // Phase vector, then the pass's own delta: the order gameplay uses.
        ApplyStateVec(SV);
        for (uint16_t s = 0; s < pass.stateCount; s++)
        {
            const uint32_t key = plan.stateWords[pass.stateOff + s * 2];
            const uint32_t val = plan.stateWords[pass.stateOff + s * 2 + 1];
            if (key < enginewarm::kRageStates && enginewarm::RSReaches(plan.stateToRS[key]))
                dv->SetRenderState((D3DRENDERSTATETYPE)plan.stateToRS[key], val);
        }
        // DXVK counts the clip planes that are ENABLED **and non-zero**
        // (UpdateClipPlanes, d3d9_device.cpp:6107), so setting the enable
        // without a plane equation produces count 0 and warms the wrong
        // pipeline. The equation has to be real.
        if (j.flags & enginewarm::kJF_ClipPlane)
        {
            static const float kPlane[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
            dv->SetClipPlane(0, kPlane);
            dv->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
        }

        static const D3DPRIMITIVETYPE kTopo[3] =
            { D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN };
        const uint32_t ti = j.flags & enginewarm::kJF_TopoMask;
        d->failed = FAILED(dv->DrawIndexedPrimitive(kTopo[ti < 3 ? ti : 0], 0, 0, 3, 0, 1));

        // grcEffectPass::RestoreState: put the pass's own keys back to the
        // engine's default block, so the next job starts where a phase does.
        for (uint16_t s = 0; s < pass.stateCount; s++)
        {
            const uint32_t key = plan.stateWords[pass.stateOff + s * 2];
            if (key < enginewarm::kRageStates && enginewarm::RSReaches(plan.stateToRS[key]))
                dv->SetRenderState((D3DRENDERSTATETYPE)plan.stateToRS[key], plan.defaultRS[key]);
        }
        if (j.flags & enginewarm::kJF_ClipPlane)
        {
            static const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            dv->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            dv->SetClipPlane(0, kZero);
        }
        if (D.instanced) { dv->SetStreamSourceFreq(0, 1); dv->SetStreamSourceFreq(1, 1); }
    }

    // -------------------------------------------------------------------
    //  Don't redo the long pass when nothing has changed.
    //
    //  The expensive half of this feature is not the draws, it is the driver
    //  compiles behind them, and those land in the driver's OWN on-disk cache
    //  and survive the session. So: write down what was enumerated and on what
    //  rig; on a later launch, if all of that still matches, check whether the
    //  driver still has them -- by drawing a small sample and counting how many
    //  of the pipelines behind it took the driver >= 5 ms. A warm driver cache
    //  answers in microseconds. That is a measurement of the thing that
    //  matters rather than a guess about a file on disk.
    // -------------------------------------------------------------------
    struct WarmStamp
    {
        uint32_t version = 2;
        uint64_t enumeration = 0;   // level + contexts + every identity emitted
        uint64_t rig = 0;           // adapter, Vulkan driver, DXVK build, back buffer, MSAA
        uint64_t shaders = 0;       // the install's .fxc set
        uint32_t jobs = 0;
        uint32_t pipelines = 0;     // creations the pass caused last time
    };

    static std::string StampPath() { return ModuleDir() + "FusionFix.enginewarm.stamp"; }

    static bool ReadStamp(WarmStamp& s)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, StampPath().c_str(), "rb") != 0 || !f) return false;
        WarmStamp in{};
        const size_t n = fread(&in, 1, sizeof(in), f);
        fclose(f);
        if (n != sizeof(in) || in.version != s.version) return false;
        const uint32_t want = s.version;
        s = in;
        s.version = want;
        return true;
    }

    static void WriteStamp(const WarmStamp& s)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, StampPath().c_str(), "wb") != 0 || !f) return;
        fwrite(&s, 1, sizeof(s), f);
        fclose(f);
    }

    // Everything outside the enumeration that decides whether a pipeline the
    // pass built is still usable: the adapter, the Vulkan driver that compiled
    // it, the exact DXVK build that produced the SPIR-V and the state, the
    // back-buffer format and the one option that changes the sample count.
    static uint64_t RigFingerprint()
    {
        // NOT the level: that is part of the enumeration, and counting it here
        // too would make the log say the rig changed when only the setting did.
        uint64_t h = pipelinekeys::Fnv1a(&bbFmt, sizeof(bbFmt));
        h = pipelinekeys::Fnv1a(&cfg.reflectionMsaa, sizeof(cfg.reflectionMsaa), h);

        IDirect3D9* d3d = nullptr;
        if (dev && SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
        {
            D3DADAPTER_IDENTIFIER9 id{};
            if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id)))
            {
                h = pipelinekeys::Fnv1a(id.Description, strnlen(id.Description, sizeof(id.Description)), h);
                h = pipelinekeys::Fnv1a(&id.VendorId, sizeof(id.VendorId), h);
                h = pipelinekeys::Fnv1a(&id.DeviceId, sizeof(id.DeviceId), h);
            }
            d3d->Release();
        }
        FusionFixDxvkInfo dxvk{};
        if (dev && FusionFixQueryDxvk(dev, &dxvk) && dxvk.haveDriver)
        {
            h = pipelinekeys::Fnv1a(dxvk.driverName, strlen(dxvk.driverName), h);
            h = pipelinekeys::Fnv1a(dxvk.driverInfo, strlen(dxvk.driverInfo), h);
        }
        // The DXVK build: its SPIR-V and its pipeline state change between
        // builds, so a pipeline warmed by one is not one the next can use.
        const std::string build = dev ? FusionFixDxvkBuild(dev) : std::string();
        if (!build.empty()) h = pipelinekeys::Fnv1a(build.data(), build.size(), h);
        return h;
    }

    static uint64_t ShaderSetFingerprint()
    {
        std::vector<uint64_t> all(pipelinekeys::InstalledFxcHashes().begin(),
                                  pipelinekeys::InstalledFxcHashes().end());
        std::sort(all.begin(), all.end());
        return all.empty() ? 0 : pipelinekeys::Fnv1a(all.data(), all.size() * sizeof(uint64_t));
    }

    static void EngineWarmPass(fxc_db* db)
    {
        using namespace enginewarm;

        // The render phases and the target registry, as the boot gate read
        // them. They are not probed for here: at the gate the engine has
        // already been asked to build its phases and it has, synchronously,
        // with its own code (bootgate::BuildRenderPhases). If this pass is
        // running from the legacy loading-screen gate instead -- the fail-safe
        // -- both are empty, the shipped stand-ins fire, and the log says so.
        // Which gate, AND whether that gate actually produced anything: the
        // boot gate can open and still read nothing (a faulted or refused phase
        // build), and a log that says "boot gate" over a plan built entirely
        // from the stand-in table is the exact confusion this feature was
        // re-sited to end.
        const bool haveEngineSets = !gatePhases.empty() || !gateTargets.empty();
        Log("engine: %zu target-bearing render phases and %zu registry targets from the %s%s",
            gatePhases.size(), gateTargets.size(),
            gateFromBoot ? "boot gate" : "FALLBACK loading-screen gate",
            haveEngineSets ? "" : " - NOTHING was read from the engine, so every render-target "
                                  "set below is a shipped stand-in and is marked as one");

        // The enumeration reads engine memory through Readable() checks, but a
        // build this does not know could still put a bad pointer where a table
        // belongs. Contain a fault here rather than losing the whole pass.
        BuildArgs args{ db, cfg.engineWarm, nullptr, bbFmt, &gatePhases, &gateTargets };
        Plan plan;
        args.out = &plan;
        // The sample count cannot be read from a render target at this gate --
        // there is no D3D texture behind one yet -- and it IS in the pipeline
        // key. It comes from the option that puts one there.
        plan.msType = (uint32_t)cfg.reflectionMsaa;
        plan.msQuality = 0;
        plan.keepKeys = cfg.emitLog;
        if (!GuardedCall(&DoBuildTables, &args))
        {
            curLabel.clear();
            Log("engine: enumeration faulted - falling back to the recorded replay alone");
            return;
        }
        if (!plan.ok)
        {
            curLabel.clear();
            Log("engine: OFF - %s. This is the fail-safe: another build, EFLC, or a mod that "
                "moves the effect set. The recorded d3d9cache replay is unaffected.",
                plan.why.empty() ? "an engine table did not validate" : plan.why.c_str());
            return;
        }

        // THE DEVICE THE WALK DRAWS ON. Before the shader objects are read,
        // because each one is mirrored onto it as it is read. If it cannot be
        // created the walk still runs, on the game's device and with the job
        // cap below, because a warm pass that keeps 61,000 live pipelines in a
        // 32-bit process is the thing this is here to stop.
        if (cfg.warmDevice == 1)
        {
            LogVa("engine: before the warm device");
            if (CreateWarmDevice() && CreateEngineResources())
            {
                LogVa("engine: with the warm device up");
                // A brand-new device starts at the D3D9 defaults, which is not
                // the state the game's device is in; ApplyDefaultBlock pins the
                // difference that DXVK bakes into a pipeline, and this is the
                // before picture.
                LogAmbientSpec("on the fresh warm device", edev);
            }
            else
            {
                DestroyWarmDevice();
                Log("engine: no warm device - the walk runs on the game's device and is capped "
                    "at %u jobs so it cannot spend the address space the game needs", kCappedJobs);
            }
        }

        // Between the two halves of the build: read every pass's shader objects
        // and take a reference on them. See ResolveEngineShaderIO -- this is
        // what makes the declaration projection and the sampler binding come
        // from the live objects instead of a .fxc join by array index, and what
        // makes the pointers the draw loop binds minutes later safe to bind.
        ResolveEngineShaderIO(plan);
        if (plan.passes.empty())
        {
            curLabel.clear();
            ReleaseEngineWalk();
            Log("engine: OFF - no pass survived reading its shader objects. The recorded "
                "d3d9cache replay is unaffected.");
            return;
        }
        if (!GuardedCall(&DoBuildJobs, &args) || !plan.ok)
        {
            curLabel.clear();
            ReleaseEngineWalk();
            Log("engine: OFF - %s. The recorded d3d9cache replay is unaffected.",
                plan.why.empty() ? "the job walk produced nothing" : plan.why.c_str());
            return;
        }

        // On the FALLBACK gate the pass still composites over a snapshot of the
        // loading screen, and softening it is what says "this is not the game
        // yet" while the game's own shaders are drawn behind it. On the boot
        // gate there is no snapshot to soften: the engine is drawing its real
        // loading screen the whole time.
        if (!gateFromBoot) BlurBackdrop();

        const Census& c = plan.census;
        Log("engine: enumerated %u effects (%u joined to .fxc), %u techniques (%u named: "
            "%u by index, %u by hash, %u unmatched), %u passes, %u VS + %u PS programs",
            c.effects, c.effectsJoined, c.techniques, c.techniquesJoined,
            c.techByIndex, c.techByHash, c.techUnmatched, c.passes, c.vs, c.ps);
        Log("engine: read %u of %u distinct shader objects with GetFunction "
            "(%u refused, %u passes dropped for it)",
            c.shaderRead, c.shaderObjects, c.shaderFailed, c.passesDropped);
        Log("engine: passes by group: deferred %u, deferredbs %u, deferredalphaclip %u, "
            "forward %u, named %u",
            c.groupPasses[kG_Deferred], c.groupPasses[kG_DeferredBS], c.groupPasses[kG_DeferredClip],
            c.groupPasses[kG_Forward], c.groupPasses[kG_Named]);
        Log("engine: %u render phases bind %u targets; the factory registry holds %u targets "
            "(%u with a live texture) -> %zu contexts (%u from phases, %u from the registry, "
            "%u SHIPPED STAND-IN)",
            c.phases, c.phaseTargets, c.registryTargets, c.registryTextured, plan.contexts.size(),
            c.contextsFromPhases, c.contextsFromRegistry, c.contextsShipped);
        for (size_t i = 0; i < plan.contexts.size(); i++)
        {
            const Context& x = plan.contexts[i];
            Log("engine:   ctx %zu %-12s %-14s %u colour (%u/%u/%u/%u) depth %u samples %u%s",
                i, KindName(x.kind), x.name.c_str(), x.mrt,
                (uint32_t)x.color[0], (uint32_t)x.color[1], (uint32_t)x.color[2],
                (uint32_t)x.color[3], (uint32_t)x.depth, x.msType,
                x.fromEngine ? "" : "   <- SHIPPED, not read from the engine");
        }
        Log("engine: %u vertex declarations (%u shipped, %u from the live registry which holds %u), "
            "mean %.2f declaration projections per pass",
            c.declsTotal, c.declsShipped, c.declsLive, c.liveRegistryEntries,
            c.projectionsPerPass100 / 100.0);
        // HALF of a context is read from the engine and half is not, and the
        // log used to print "%u state vectors" beside the engine counts above,
        // where it reads as a census of something. It is not: it is the length
        // of a constant table. Say which it is, per entry, the same way the
        // render-target sets say it.
        Log("engine: %u phase state vectors, ALL SHIPPED - the write-mask / blend-enable / "
            "alpha-test half of a context is still a table, not an engine read. It is mined "
            "from the 15 ragePhase_State_* callbacks by hand "
            "(re/rage-shader-precompile/09-phase-contexts.md 4.2), and the vectors those "
            "callbacks push that are NOT in it -- CW0=0 (rows 9-12), CW0=8 (row 13) and "
            "{7,15,15,15} (row 3 from a forward resting state) -- are where the scorer charges "
            "most of the blend-axis residue.", (uint32_t)kSV_Count);
        for (int i = 0; i < kSV_Count; i++)
            Log("engine:   sv %d %-13s write mask %u/%u/%u/%u blend %s alpha test %s"
                "   <- SHIPPED, not read from the engine",
                i, kStateVecs[i].name,
                (uint32_t)kStateVecs[i].cwe[0], (uint32_t)kStateVecs[i].cwe[1],
                (uint32_t)kStateVecs[i].cwe[2], (uint32_t)kStateVecs[i].cwe[3],
                kStateVecs[i].blendEnable ? "on" : "off",
                kStateVecs[i].alphaTest ? "on" : "off");
        const char* capped = c.jobsCapped ? ", STOPPED AT THE JOB CAP - raise it or lower the level"
                                          : "";
        Log("engine: level %d emits %u jobs after dedup (%u collapsed onto an identity already "
            "emitted%s)", cfg.engineWarm, c.jobsEmitted, c.jobsDeduped, capped);
        // What the enumeration VARIES, per axis of the DXVK pipeline key, said
        // before a single draw is issued. An axis at 1 is an axis this level
        // does not reach at all, which is the thing worth seeing in a log.
        Log("engine: per axis: vs+ps %u, proj %u, rt %u, blend %u, prim %u, spec0 %u, "
            "psTypes %u, vsTypes %u, clip %u",
            c.axisShaders, c.axisProj, c.axisRT, c.axisBlend, c.axisPrim,
            c.axisSpec0, c.axisPSTypes, c.axisVSTypes, c.axisClip);

        if (cfg.emitLog)
        {
            const std::string path = ModuleDir() + "FusionFix.enginewarm.emitted.jsonl";
            Log("engine: emission log -> %s (%s)", path.c_str(),
                enginewarm::WriteEmissionLog(plan, path, cfg.engineWarm)
                    ? "written" : "COULD NOT BE WRITTEN");
        }

        // The declarations, on the real device. The element bytes are what
        // DXVK keys on, so an object we create is the same pipeline as one the
        // engine creates from the same array.
        engineDeclObjs.assign(plan.decls.size(), nullptr);
        uint32_t declMade = 0, declFailed = 0;
        for (size_t i = 0; i < plan.decls.size(); i++)
        {
            std::vector<D3DVERTEXELEMENT9> e = plan.decls[i].elems;
            e.push_back(D3DDECL_END());
            if (SUCCEEDED(EDev()->CreateVertexDeclaration(e.data(), &engineDeclObjs[i]))) declMade++;
            else declFailed++;
        }
        Log("engine: created %u vertex declarations (%u refused by the device)", declMade, declFailed);

        // Progress is this phase's own from here.
        workTotal = (uint32_t)plan.jobs.size();
        if (workTotal == 0) workTotal = 1;
        workDone = 0;
        tPhase = std::chrono::steady_clock::now();
        curWhat = "Warming engine pipelines";
        curLabel.clear();

        const auto t0 = std::chrono::steady_clock::now();
        auto elapsedMs = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
        };

        ApplyDefaultBlock(plan);

        IDirect3DQuery9* fence = nullptr;
        EDev()->CreateQuery(D3DQUERYTYPE_EVENT, &fence);
        const uint32_t kFenceChunk = (cfg.fenceChunk > 0) ? (uint32_t)cfg.fenceChunk : 1;
        uint32_t sinceFence = 0;

        uint32_t drawn = 0, skippedRT = 0, skippedDecl = 0, failedDraw = 0, faulted = 0;
        uint32_t skippedMirror = 0, mirrorRun = 0;
        uint32_t perCtx[16] = {}, perState[kSV_Count] = {};
        bool stopped = false;
        const char* stopWhy = "";

        // Which path this launch takes, and why -- see WarmStamp. The probe is
        // the first `probeN` jobs of the plan, which are its highest-ranked
        // ones, so a cold driver cache is caught on the work that matters most.
        WarmStamp stamp{};
        const bool stampRead = cfg.reuseStamp && ReadStamp(stamp);
        // The enumeration's own fingerprint, PLUS the two things about this
        // build that change which pipelines the same enumeration produces: the
        // arm the walk runs on (a device of our own starts at the D3D9 defaults
        // and the game's device does not), and the bool constants the walk pins
        // on it. A stamp written by one arm must not let the other take the
        // cheap path over a 512-job probe of a different pipeline set.
        uint64_t fpEnum = EnumerationFingerprint(plan, cfg.engineWarm);
        {
            const uint32_t arm = (uint32_t)(cfg.warmDevice == 0 ? 0 : 1);
            uint32_t bools = 0;
            if (ambientBoolsRead)
                for (int i = 0; i < 16; i++)
                    bools |= (ambientVsB[i] ? 1u : 0u) << i | (ambientPsB[i] ? 1u : 0u) << (i + 16);
            fpEnum = pipelinekeys::Fnv1a(&arm, sizeof(arm), fpEnum);
            fpEnum = pipelinekeys::Fnv1a(&bools, sizeof(bools), fpEnum);
        }
        const uint64_t fpRig = RigFingerprint(), fpShaders = ShaderSetFingerprint();
        const bool stampMatches = stampRead && stamp.enumeration == fpEnum &&
                                  stamp.rig == fpRig && stamp.shaders == fpShaders;
        const size_t probeN = stampMatches ? (std::min)((size_t)512, plan.jobs.size()) : 0;
        const uint32_t compiles0 = pipelinekeys::VulkanReplay().compiles.load();
        bool skippedWarm = false;
        if (!stampRead)
            Log("engine: no warm stamp beside the ASI - this is the full pass");
        else if (!stampMatches)
            Log("engine: the warm stamp does not match (enumeration %s, rig %s, shader set %s) "
                "- this is the full pass",
                stamp.enumeration == fpEnum ? "same" : "CHANGED",
                stamp.rig == fpRig ? "same" : "CHANGED",
                stamp.shaders == fpShaders ? "same" : "CHANGED");
        else
            Log("engine: the warm stamp matches (%u jobs, %u pipelines last time) - sampling "
                "%zu jobs to see whether the driver still has them",
                stamp.jobs, stamp.pipelines, probeN);

        // TWO things can take the default block away, and both have to be
        // watched or most of this walk builds pipelines it did not intend.
        //
        //  * The overlay establishes its own render state to get a 2D quad on
        //    the screen (SetOverlayState). That is the fail-safe gate.
        //  * On the BOOT gate there is no overlay of ours at all, and instead
        //    every 8 ms slice hands the device to the engine and takes it back
        //    with the loading screen's whole D3DSBT_ALL block applied over
        //    ours (RunSlice). ApplyStateVec and the pass delta only set the
        //    write masks, blend ENABLE, alpha-test enable and whatever keys the
        //    pass itself pushes -- the blend factors, the blend op, the
        //    separate-alpha block and ALPHAFUNC come from the default block and
        //    are all in DxvkGraphicsPipelineStateInfo. Missing this meant ~6
        //    jobs per slice carried the identity the emission log claims and
        //    the rest carried the loading screen's.
        //
        // 49 SetRenderState calls once per slice, against ~6 jobs of real work
        // in it, is not a cost worth optimising away.
        uint32_t framesAtBase = overlayFrames;
        uint32_t epochAtBase = sliceEpoch;

        // The address space, every 8192 jobs. This walk is the one thing in the
        // mod that can spend hundreds of megabytes of a 32-bit process, and a
        // curve through the walk is what says whether it spends them evenly
        // (pipelines) or in steps (something else).
        LogVa("engine: before the walk");
        size_t vaAt = 0;
        constexpr size_t kVaEvery = 8192;

        // Without a device of our own the pipelines are the game's for the rest
        // of the session, so the walk is bounded rather than complete. With one,
        // nothing survives the walk and there is nothing to bound.
        //
        // KEYED ON THE DEVICE, NOT ON THE INI VALUE. The cap used to fire only
        // for PrecompileWarmDevice = 1, which meant the diagnostic arm (2) and
        // any value the ini did not expect ran the full 61,191-job walk on the
        // game's device -- the exact configuration that killed the game a
        // minute into gameplay on 2026-09-20, arrived at by a typo. The one
        // value that may still do it is an explicit 0, which is the documented
        // A/B arm and says so in the ini and in the log.
        const bool unprotected = !edev;
        const bool deliberate = unprotected && cfg.warmDevice == 0;
        const size_t jobCap = (unprotected && !deliberate)
                            ? (std::min)((size_t)kCappedJobs, plan.jobs.size()) : plan.jobs.size();
        if (deliberate && plan.jobs.size() > kCappedJobs)
            Log("engine: WARNING - PrecompileWarmDevice = 0, so all %zu jobs build on the GAME's "
                "device and their pipelines are the session's to carry. This is the A/B arm; it "
                "measured ~13.5 KB of address space per pipeline and a dead game.",
                plan.jobs.size());

        // And a floor under both arms. Even with a device of its own the walk
        // holds every pipeline it has built until that device goes, so the
        // TROUGH is the same either way -- it is only the state gameplay starts
        // in that differs. The game's world is already loaded at this gate and
        // its streamer is idle behind the loading screen, but nothing about a
        // 32-bit address space is worth betting a session on: if free space
        // reaches the floor the walk stops where it is and releases what it has.
        const uint64_t vaFloorMB = 1024;
        const uint64_t vaAtStart = pipelinekeys::AvailVaMB();
        const uint64_t vaBudgetMB = 192;   // only on the game's device
        uint32_t sinceVaCheck = 0;

        uint32_t warmDevices = 1, rotateFailed = 0;
        size_t rotateAt = 0;

        for (size_t ji = 0; ji < jobCap; ji++)
        {
            const Job& j = plan.jobs[ji];
            // A fresh device every kWarmChunkJobs jobs: the old one's pipelines
            // go with it and the next chunk builds in the space they gave back.
            // The fence first, so nothing of this chunk's is still in flight
            // when its device is destroyed.
            if (edev && ji && ji - rotateAt >= kWarmChunkJobs)
            {
                rotateAt = ji;
                if (sinceFence) { FenceChunk(fence); workDone += sinceFence; sinceFence = 0; }
                else FenceChunk(fence);
                SAFE_RELEASE(fence);
                // THE FULL WAIT, not just the fence. An event query drains the
                // GPU and DXVK's CS thread, which is everything ON THIS RIG,
                // where graphics pipeline libraries are off and DXVK therefore
                // builds every optimized pipeline inline on the CS thread. It
                // is NOT everything in general: with GPL enabled
                // (canCreateBasePipeline) the draw gets a fast-linked base
                // pipeline and the optimized one is queued to m_workers, and
                // ~DxvkPipelineManager's stopWorkers() drops whatever is still
                // in that queue -- so a fenced rotation would silently throw
                // away the compilation this whole pass exists to do. Waiting
                // until DXVK is QUIET covers both, and costs one settle second
                // per rotation out of a walk measured in hundreds.
                //
                // It owns the progress counters while it runs -- it shows the
                // settle as a bar of its own and ends by filling it -- so the
                // walk's own are saved and put back, or the band would read
                // 100 % from the first rotation to the end of the load.
                {
                    const uint32_t wTotal = workTotal;
                    const uint32_t wDone = workDone.load();
                    const auto wPhase = tPhase;
                    WaitUntilIdle("a warm device chunk, before it is replaced", edev);
                    workTotal = wTotal;
                    workDone = wDone;
                    tPhase = wPhase;
                }
                if (!RotateWarmDevice(plan))
                {
                    rotateFailed++;
                    stopped = true;
                    stopWhy = " [could not make another warm device]";
                    break;
                }
                warmDevices++;
                EDev()->CreateQuery(D3DQUERYTYPE_EVENT, &fence);
                framesAtBase = overlayFrames;
                epochAtBase = sliceEpoch;
            }
            if (ji - vaAt >= kVaEvery)
            {
                vaAt = ji;
                char what[64];
                _snprintf_s(what, sizeof(what), _TRUNCATE, "engine: %zu jobs in (device %u)",
                            ji, warmDevices);
                LogVa(what);
            }
            if (overlayFrames != framesAtBase || sliceEpoch != epochAtBase)
            {
                ApplyDefaultBlock(plan);
                framesAtBase = overlayFrames;
                epochAtBase = sliceEpoch;
            }

            const Context& C = plan.contexts[j.ctx];

            // Our own targets in the engine's formats. A Vulkan pipeline is
            // keyed on attachment FORMAT, never size, so 64x64 reproduces the
            // identity exactly (proven live, 06-phases §5.1).
            IDirect3DSurface9* rts[4] = {};
            bool unsupported = false;
            for (int i = 0; i < 4; i++)
                if (C.color[i] != D3DFMT_UNKNOWN)
                {
                    rts[i] = EngineScratch(C.color[i], i, C.msType, C.msQuality);
                    unsupported |= !rts[i];
                }
            IDirect3DSurface9* ds = (C.depth != D3DFMT_UNKNOWN)
                                  ? EngineDepth((uint32_t)C.depth, C.msType, C.msQuality) : nullptr;
            unsupported |= (C.depth != D3DFMT_UNKNOWN) && !ds;
            if (!rts[0] || unsupported) { skippedRT++; workDone++; continue; }

            IDirect3DVertexDeclaration9* declObj =
                (j.decl < engineDeclObjs.size()) ? engineDeclObjs[j.decl] : nullptr;
            if (!declObj) { skippedDecl++; workDone++; continue; }

            DrawArgs da{};
            da.plan = &plan; da.job = &j; da.ds = ds; da.declObj = declObj;
            for (int i = 0; i < 4; i++) da.rts[i] = rts[i];
            if (!GuardedCall(&DoDrawJob, &da))
            {
                // One bad bind is one job, not the session. Stop anyway: the
                // only way to get here is an engine pointer that stopped being
                // what it was, and the rest of the plan holds more of them.
                faulted++;
                stopped = true; stopWhy = " [faulted on an engine pointer]";
                break;
            }
            if (da.noMirror)
            {
                // One shader that will not build on this device will not build
                // on the next one either, and a walk that skips every job is
                // burning the load for nothing. Ten in a row is not a stray
                // resource failure.
                skippedMirror++;
                if (++mirrorRun >= 10)
                {
                    stopped = true; stopWhy = " [the shader mirrors stopped building]";
                    break;
                }
                workDone++;
                continue;
            }
            mirrorRun = 0;
            if (da.failed) failedDraw++;
            drawn++;
            if (j.ctx < 16) perCtx[j.ctx]++;
            if (j.state < kSV_Count) perState[j.state]++;

            if (++sinceFence >= kFenceChunk)
            {
                FenceChunk(fence);
                workDone += sinceFence;
                sinceFence = 0;

                auto nowLbl = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(nowLbl - tLastLabel).count() >= 250)
                {
                    tLastLabel = nowLbl;
                    // Short, because the band prints it after "stage k of n"
                    // in a column the headline does not reach into.
                    curLabel = "pipeline " + std::to_string(drawn) + " / " + std::to_string(plan.jobs.size());
                }
                // A lost device here would turn every remaining draw into a
                // silent failure; stop and let the game have its frame back.
                // BOTH devices, and they mean different things: the game's
                // being lost has to end the whole pass (the state block the
                // restore applies would go to a device that cannot take it),
                // while ours being lost only ends the walk. The warm device
                // owns an 8x8 popup nobody touches, so it is the game's that
                // actually moves -- and polling only ours, which the merge
                // arrived at, would have missed every one of them.
                if (dev->TestCooperativeLevel() != D3D_OK)
                {
                    stopped = true; stopWhy = " [the game's device was lost]";
                    break;
                }
                if (edev && edev->TestCooperativeLevel() != D3D_OK)
                {
                    stopped = true; stopWhy = " [the warm device was lost]";
                    break;
                }
            }
            if (BudgetSpent())
            {
                stopped = true;
                stopWhy = gateAbort.load() ? " [the gate asked the pass to stop]"
                                           : " [PrecompileBudgetSeconds reached]";
                break;
            }
            if (++sinceVaCheck >= 256)
            {
                sinceVaCheck = 0;
                const uint64_t freeMB = pipelinekeys::AvailVaMB();
                if (freeMB && freeMB < vaFloorMB)
                {
                    stopped = true; stopWhy = " [address space floor reached]";
                    Log("engine: STOPPING at job %zu - only %llu MB of address space is free, "
                        "and this walk will not take a 32-bit process below %llu",
                        ji, (unsigned long long)freeMB, (unsigned long long)vaFloorMB);
                    break;
                }
                if (unprotected && !deliberate && vaAtStart && freeMB && freeMB < vaAtStart &&
                    vaAtStart - freeMB > vaBudgetMB)
                {
                    stopped = true; stopWhy = " [address-space budget spent, no device of our own]";
                    Log("engine: STOPPING at job %zu - the walk has spent %llu MB of address "
                        "space on the GAME's device, which is the whole session's to lose",
                        ji, (unsigned long long)(vaAtStart - freeMB));
                    break;
                }
            }
            // The cheap path. A fence first, so every pipeline the sample
            // needed has been created -- with graphics pipeline libraries off
            // DXVK builds them on its CS thread as the draws execute, and a
            // drained event query is exactly the point at which that is done.
            if (probeN && ji + 1 == probeN)
            {
                if (sinceFence) { FenceChunk(fence); workDone += sinceFence; sinceFence = 0; }
                else FenceChunk(fence);
                const uint32_t made = pipelinekeys::VulkanReplay().compiles.load() - compiles0;
                const bool warm = made * 20 <= (uint32_t)probeN;
                Log("engine: probe drew %zu of %zu jobs; the driver compiled %u of them >= 5 ms "
                    "-> its cache is %s. %s",
                    probeN, plan.jobs.size(), made, warm ? "WARM" : "COLD",
                    warm ? "Skipping the rest: everything this enumeration builds is already on "
                           "disk and nothing would be compiled."
                         : "Running the whole pass.");
                if (warm)
                {
                    skippedWarm = true;
                    stopped = true;
                    stopWhy = " [driver cache already warm, stamp matched]";
                    // Say so on the loading screen too: this load is seconds,
                    // not the twenty minutes the full pass takes, and a player
                    // watching a bar jump to the end deserves the reason. Give
                    // it long enough to be read -- the loading screen keeps
                    // drawing while the pass fiber sleeps, and 1.2 s on a load
                    // that just saved twenty minutes is not a cost.
                    cheapStage = stageNo;
                    curLabel.clear();
                    workDone = workTotal;
                    lastProgressUs = 0;
                    PublishProgress();
                    // Only when the band is what the player is looking at: on
                    // the fail-safe gate the mod presents its own frames and
                    // this would be 1.2 s of frozen overlay for nothing.
                    if (bootgate::Prog().active) PassSleep(1200);
                    break;
                }
            }
            PresentOverlay(false);
        }
        if (!stopped && jobCap < plan.jobs.size())
        {
            stopped = true;
            stopWhy = " [capped: no device of our own, so the walk is bounded]";
        }

        if (sinceFence) { FenceChunk(fence); workDone += sinceFence; }
        SAFE_RELEASE(fence);
        LogVa("engine: at the end of the walk");

        // Unbind everything this phase touched, then put RAGE's default block
        // back, so the pass-level restore below only has the overlay's own
        // state left to undo. On a device of our own that is cosmetic -- it is
        // about to be destroyed -- but on the game's device it is not.
        {
            IDirect3DDevice9* d = EDev();
            for (uint32_t i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
            for (UINT s = 0; s < 4; s++) { d->SetStreamSource(s, nullptr, 0, 0); d->SetStreamSourceFreq(s, 1); }
            d->SetIndices(nullptr);
            d->SetVertexDeclaration(nullptr);
            for (uint32_t i = 0; i < pipelinekeys::kNumSamplers; i++)
                d->SetTexture(i < pipelinekeys::kPSSamplers
                                  ? i
                                  : D3DVERTEXTEXTURESAMPLER0 + (i - pipelinekeys::kPSSamplers),
                              nullptr);
            ApplyDefaultBlock(plan);
        }
        // NOTHING may still be compiling when the device goes: DXVK creates a
        // graphics pipeline on the CS thread as the draw that needs it executes
        // (with pipeline libraries off there is no worker to hand it to), and a
        // half-built one at ~DxvkDevice is a use-after-free waiting to happen.
        // This is also the point the user's "wait for the pipelines to compile"
        // names -- the wait after the whole D3D9 pass is too late to matter for
        // a device that no longer exists.
        if (edev) WaitUntilIdle("the engine walk, before its device goes", edev);
        const bool hadWarmDevice = edev != nullptr;
        ReleaseEngineWalk();
        LogVa("engine: after the walk's device and resources went");
        if (hadWarmDevice)
        {
            // A second reading a moment later, in case anything about the
            // teardown is deferred to another thread.
            PassSleep(1000);
            LogVa("engine: a second after the walk's device went");
        }

        if (stopped) workDone = workTotal;

        Log("engine: drew %u of %zu jobs in %llds%s (%u failed, %u faulted, %u no render target, "
            "%u no declaration, %u no shader mirror)",
            drawn, plan.jobs.size(), (long long)(elapsedMs() / 1000), stopWhy,
            failedDraw, faulted, skippedRT, skippedDecl, skippedMirror);
        if (cfg.warmDevice == 1)
            Log("engine: the walk went through %u warm device(s) of %u jobs each%s, and %u shader "
                "mirror(s) could not be built on one", warmDevices, kWarmChunkJobs,
                rotateFailed ? " (one could not be replaced, which is why it stopped)" : "",
                eMirrorFailed);
        Log("engine: PATH = %s", skippedWarm
                ? "CHEAP - the stamp matched and the driver cache is warm"
                : (stampMatches ? "FULL - the stamp matched but the driver cache had gone cold"
                                : "FULL - first run of this enumeration on this rig"));

        // Only a pass that went all the way through gets to say it is done.
        // A stopped one would otherwise let the next launch take the cheap
        // path over a plan it never finished.
        if (!stopped || skippedWarm)
        {
            WarmStamp out{};
            out.enumeration = fpEnum;
            out.rig = fpRig;
            out.shaders = fpShaders;
            out.jobs = (uint32_t)plan.jobs.size();
            out.pipelines = skippedWarm
                ? stamp.pipelines
                : pipelinekeys::VulkanReplay().creations.load();
            WriteStamp(out);
        }
        {
            std::string per = "engine: per render-target set:";
            for (size_t i = 0; i < plan.contexts.size() && i < 16; i++)
                per += " " + std::to_string(i) + ":" + plan.contexts[i].name +
                       "=" + std::to_string(perCtx[i]);
            LogStr(per);
            std::string ps = "engine: per phase state vector:";
            for (int i = 0; i < kSV_Count; i++)
                ps += " " + std::string(kStateVecs[i].name) + "=" + std::to_string(perState[i]);
            LogStr(ps);
        }
        // The live grcState shadow, DIAGNOSTIC ONLY -- nothing in the plan is
        // built from it. It is here because it is the one place a future build
        // whose callbacks push something other than the shipped vectors above
        // would show up, and because at this gate it is the loading screen's
        // state rather than gameplay's, which is exactly why it cannot be the
        // source for them.
        Log("engine: live grcState shadow [0..7] (diagnostic, not used) = %u %u %u %u %u %u %u %u",
            plan.liveState[0], plan.liveState[1], plan.liveState[2], plan.liveState[3],
            plan.liveState[4], plan.liveState[5], plan.liveState[6], plan.liveState[7]);
    }

    // -------------------------------------------------------------------
    //  Completion gate (W4) — nothing may still be compiling when the loading
    //  screen goes.
    // -------------------------------------------------------------------
    // Also the abort: every loop in the pass already asks this before it
    // carries on, so wiring the gate's "stop now" into it is what makes the
    // pass unwind from wherever it is rather than being left on a fiber
    // nothing will schedule again.
    static bool BudgetSpent()
    {
        if (gateAbort.load()) return true;
        return cfg.budgetSeconds > 0 && std::chrono::steady_clock::now() - tStart > std::chrono::seconds(cfg.budgetSeconds);
    }

    // First the GPU: an event query drains everything submitted, so DXVK's CS thread
    // has also executed every draw so far and created every pipeline they needed.
    // Then DXVK itself, until it is QUIET: none of its pipeline creations in flight,
    // none started or finished and no CPU time used by its compiler threads for a
    // whole second, while the overlay keeps presenting (what that covers: the top of
    // vkcapture.ixx; the signals: VulkanReplayState in pipelinekeys.h). Without
    // vkcapture watching DXVK -- native D3D9, or no DXVK device seen -- there is
    // nothing to observe, and a few presents have to do. Logs what it saw:
    //   wait after <what>: <how it ended> at t=<s> after <s>s (GPU drain <s>s) - DXVK
    //     started <n> creations (at most <n> at once), its <n> compiler threads used
    //     <s>s CPU (<s>s since they started; its CS thread <s>s), last activity
    //     t=<s>; settle <s>s
    static void WaitUntilIdle(const char* after, IDirect3DDevice9* on = nullptr)
    {
        auto& vr = pipelinekeys::VulkanReplay();
        const int64_t w0 = pipelinekeys::ClockUs();
        IDirect3DQuery9* q = nullptr;
        if (!on) on = dev;
        if (SUCCEEDED(on->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q)
        {
            q->Issue(D3DISSUE_END);
            for (int spin = 0; spin < 200000; spin++)
            {
                if (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) break;
                PresentOverlay(false);
            }
            q->Release();
        }
        const int64_t d0 = pipelinekeys::ClockUs();
        if (!vr.watching || !vr.compilerCpuUs)
        {
            for (int i = 0; i < 6; i++) { PresentOverlay(true); }
            Log("wait after %s: GPU drained in %.2fs; DXVK not watched, so whether it is quiet cannot be seen",
                after, (d0 - w0) / 1e6);
            return;
        }

        constexpr int64_t kSettleUs = 1000000, kSampleUs = 100000, kGiveUpUs = 600000000;
        const uint32_t creations0 = vr.creations.load();
        uint32_t threads = 0;
        uint64_t cs = 0;
        const uint64_t cpu0 = vr.compilerCpuUs(&threads, nullptr);
        uint64_t cpu = cpu0;
        int64_t cpuAt = d0, sampledAt = d0, labelAt = 0;   // cpuAt: when that CPU time last grew
        bool cpuGrew = false;
        int32_t most = 0;
        const char* ended = "quiet";
        tPhase = std::chrono::steady_clock::now();
        workTotal = 1000;
        workDone = 0;
        for (;;)
        {
            const int64_t now = pipelinekeys::ClockUs();
            if (now - sampledAt >= kSampleUs)
            {
                const uint64_t c = vr.compilerCpuUs(&threads, &cs);
                if (c != cpu) { cpu = c; cpuAt = now; cpuGrew = true; }
                sampledAt = now;
            }
            const int32_t inFlight = vr.inFlight.load();
            most = (std::max)(most, inFlight);
            const int64_t quietFor = inFlight > 0 ? 0 : now - (std::max)(vr.lastActivityUs.load(), cpuAt);
            if (quietFor >= kSettleUs) break;
            if (BudgetSpent())
            {
                ended = gateAbort.load() ? "stopped by PrecompileGateMaxSeconds"
                                         : "stopped by PrecompileBudgetSeconds";
                break;
            }
            if (now - d0 >= kGiveUpUs) { ended = "NOT quiet, gave up"; break; }

            // The bar fills as the quiet second does, and starts over whenever DXVK
            // compiles something: it is full exactly when the wait is over.
            workDone = (uint32_t)std::clamp<int64_t>(quietFor * 1000 / kSettleUs, 0, 1000);
            if (now - labelAt >= 250000)
            {
                labelAt = now;
                const int64_t s = (now - w0) / 1000000;
                char text[128];
                _snprintf_s(text, sizeof(text), _TRUNCATE, "Waiting for DXVK to finish compiling    %d:%02d",
                            (int)(s / 60), (int)(s % 60));
                curTitle = text;
                if (inFlight > 0)
                    _snprintf_s(text, sizeof(text), _TRUNCATE, "DXVK compiling (%d pipelines at once)", inFlight);
                else if (cpuGrew && now - cpuAt < kSettleUs)
                    _snprintf_s(text, sizeof(text), _TRUNCATE, "DXVK compiler threads busy");
                else
                    _snprintf_s(text, sizeof(text), _TRUNCATE, "DXVK quiet for %.1f of %.1f s", quietFor / 1e6, kSettleUs / 1e6);
                curLabel = text;
            }
            PresentOverlay(false);
            PassSleep(5);
        }
        const int64_t end = pipelinekeys::ClockUs(), last = vr.lastActivityUs.load();
        Log("wait after %s: %s at t=%.3f after %.2fs (GPU drain %.2fs) - DXVK started %u creations (at most %d at once), "
            "its %u compiler threads used %.2fs CPU (%.2fs since they started; its CS thread %.2fs), last activity "
            "t=%.3f; settle %.1fs", after, ended, vr.Seconds(end), (end - w0) / 1e6, (d0 - w0) / 1e6,
            vr.creations.load() - creations0, most, threads, (cpu - cpu0) / 1e6, cpu / 1e6, cs / 1e6,
            last ? vr.Seconds(last) : 0.0, kSettleUs / 1e6);
        curTitle.clear();
        workDone = workTotal;
        PresentOverlay(true);
    }

    // Keep the loading screen up for the Vulkan replay (vkcapture), which starts the
    // moment this sets passDone: this PC's own recording, then other PCs' files and
    // the player's Steam pre-cache, on most of the cores while held. A pipeline it
    // has not warmed yet is one gameplay would compile itself, and that is a stutter;
    // the time spent here is not. PrecompileBudgetSeconds, if set, still bounds the
    // whole load: past it the replay carries on in the background. True if the
    // replay went through something and finished while held.
    static bool HoldForVulkanReplay()
    {
        auto& vr = pipelinekeys::VulkanReplay();
        if (!vr.running || BudgetSpent())
        {
            vr.passDone = true;
            // The stage was counted from the configuration and it is not going
            // to happen, so the band must stop promising it.
            if (stageCount > stageNo) stageCount = stageNo;
            Log("order: 2. no Vulkan replay to hold the loading screen for (%s), t=%.3f",
                vr.running ? "PrecompileBudgetSeconds reached, it runs in the background" : "none running", vr.Seconds());
            return false;
        }
        vr.holding = true;
        vr.passDone = true;
        Log("order: 2. Vulkan replay with the loading screen held, t=%.3f", vr.Seconds());

        const auto h0 = std::chrono::steady_clock::now();
        tPhase = h0;
        workDone = 0;
        EnterStage("Warming Vulkan pipelines");
        bool budgetHit = false;
        auto labelAt = h0 - std::chrono::seconds(1);
        while (vr.running)
        {
            if (BudgetSpent())
            {
                budgetHit = true;
                break;
            }
            const uint32_t total = (std::max)(1u, vr.total.load());
            workTotal = total;
            workDone = (std::min)(vr.done.load(), total);
            const auto now = std::chrono::steady_clock::now();
            if (now - labelAt > std::chrono::milliseconds(250))
            {
                labelAt = now;
                char label[128];
                const uint32_t phase = vr.phase.load();
                // The stage already says "Warming Vulkan pipelines" in the
                // headline, so the label says only which recordings and how
                // far: it shares its row with the time estimate.
                if (phase == pipelinekeys::VulkanReplayState::kPreparing)
                    _snprintf_s(label, sizeof(label), _TRUNCATE, "reading the recordings");
                else
                    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s, %u of %u",
                                phase == pipelinekeys::VulkanReplayState::kOwn ? "this PC" : "other PCs",
                                workDone.load(), total);
                curLabel = label;
            }
            PresentOverlay(false);
            PassSleep(5);
        }
        vr.holding = false;
        Log("held the loading screen %.1fs for the Vulkan replay (%u of %u entries), t=%.3f%s",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - h0).count(), vr.done.load(), vr.total.load(),
            vr.Seconds(), budgetHit ? " - PrecompileBudgetSeconds reached, it carries on in the background" : "");
        PresentOverlay(true);
        return !budgetHit && vr.total.load() > 0;
    }

    static void ReleaseResources()
    {
        for (auto& s : scratchColor) SAFE_RELEASE(s.surf);
        scratchColor.clear();
        SAFE_RELEASE(scratchDepthD24S8);
        SAFE_RELEASE(scratchDepthINTZ);
        if (scratchDepthINTZTex) { scratchDepthINTZTex->Release(); scratchDepthINTZTex = nullptr; }
        for (auto& d : replayDeclObjs) SAFE_RELEASE(d);
        replayDeclObjs.clear();
        for (auto& s : scratchMS) SAFE_RELEASE(s.surf);
        scratchMS.clear();
        for (auto& s : scratchDS) { SAFE_RELEASE(s.surf); SAFE_RELEASE(s.tex); }
        scratchDS.clear();
        SAFE_RELEASE(dummyVB);
        SAFE_RELEASE(dummyIB);
        SAFE_RELEASE(tex2D);
        SAFE_RELEASE(texCube);
        SAFE_RELEASE(texVol);
        SAFE_RELEASE(texFetch4);
        // texShadow is the one texture here that has to live in D3DPOOL_DEFAULT
        // (D3DUSAGE_DEPTHSTENCIL admits no other pool), so it is a LOSABLE
        // resource: DXVK refuses Reset outright while one is alive
        // (D3D9DeviceEx::Reset -> "device still has alive losable resources"),
        // and GTA IV resets for a resolution or fullscreen change. Leaving it
        // behind would not leak memory, it would break every later Reset for
        // the rest of the session.
        SAFE_RELEASE(texShadow);
        SAFE_RELEASE(backdrop);
        SAFE_RELEASE(overlayBase);
        overlayTitle.clear(); overlaySub.clear();
        if (font)    { font->Release();    font = nullptr; }
        if (fontBig) { fontBig->Release(); fontBig = nullptr; }
        // The created shader OBJECTS are intentionally KEPT alive: releasing them
        // would let DXVK free the warmed pipeline libraries. The game re-creates
        // its own handles (a cache hit on the already-translated DxvkShader), and
        // ~200 KB of COM wrappers is a negligible, deliberate leak that guarantees
        // the compiled stage libraries persist for the whole session.
    }

    // ===================================================================
    //  THE BOOT GATE
    //
    //  WHERE. The return of rageBoot_InitSession 0x005C1360: main thread,
    //  once per session start, after the frontend AND the episode/DLC flow,
    //  with the game's own loading screen up, and one instruction before the
    //  CALL that would take it down. Everything the warm pass needs to read
    //  exists there except one thing (the D3D textures behind the render
    //  targets, which it does not need); nothing that matters exists at the
    //  loading screen the pass used to fire on, which is the legals screen at
    //  t+2.2 s. The evidence, the timeline and the counter-experiments are in
    //  re/rage-shader-precompile/08-boot-gate.md.
    //
    //  HOW IT KEEPS THE GAME'S OWN LOADING SCREEN ALIVE. While a loading
    //  screen is up, a dedicated thread free-runs it: it calls
    //  rageBoot_LoadscreenRenderTick 0x005CC760 in a spin, ~7 kHz, and the
    //  tick draws a frame whenever its own 15.67 ms timer has elapsed. That
    //  is the thread the pass runs on -- but it must not BLOCK it, or the
    //  artwork freezes. So the pass runs in SLICES on a fiber of that same
    //  thread: the tick draws its frame, we switch to the pass fiber for a
    //  few milliseconds, we switch back, and the pump loop carries on. One
    //  thread, so nothing about the device is concurrent; the engine's frame
    //  finds the device exactly as its last one left it, because the slice is
    //  bracketed by a full state save and restore.
    //
    //  WHY THE STATE SAVE IS NOT OPTIONAL. RAGE's device wrapper FILTERS
    //  redundant sets (02-draw §1.1: the 0x60-0xA0 byte "cached" methods) and
    //  grcState keeps shadow globals of its own. Our draws go to DXVK's real
    //  device and never through either, so the engine's beliefs stay true --
    //  as long as what it finds when it draws again is what it left. That is
    //  what the state block guarantees, per slice.
    // ===================================================================
    static inline bootgate::Validation gateCheck{};
    static inline bool gateArmed = false;                 // the boot-gate hooks are in
    static inline std::atomic<bool> gateOpen{ false };    // the main thread is holding at it
    static inline std::atomic<bool> gateDone{ false };    // the pass has finished
    static inline std::atomic<bool> gateAbort{ false };   // it has been asked to stop
    static inline std::atomic<bool> gateFailed{ false };  // fall back to the loading-screen gate
    static inline bool gateFromBoot = false;              // this pass IS the boot-gate one
    static inline std::vector<bootgate::PhaseSet> gatePhases;
    static inline std::vector<bootgate::Target>   gateTargets;
    static inline SafetyHookInline shInitSession{};
    static inline SafetyHookInline shDrawLayers{};

    // The slice mechanism.
    static inline void* pumpFiber = nullptr;
    static inline void* passFiber = nullptr;
    // True while the loading-screen thread is inside the pass fiber. Only the
    // gate's own thread reads it, and only to know whether it may release what
    // the pass owns (AbandonPass).
    static inline std::atomic<bool> inPassFiber{ false };
    static inline int64_t sliceDeadlineUs = 0;
    static inline int64_t passResumeUs = 0;
    static inline uint32_t sliceCount = 0;
    // Bumped every time the slice driver has handed the device back to the
    // engine and taken it again. Any pass loop that establishes render state
    // ONCE and then relies on it surviving must re-establish it when this
    // changes -- see the comment on it in RunSlice.
    static inline uint32_t sliceEpoch = 0;
    static inline IDirect3DStateBlock9* engineSB = nullptr;
    static inline IDirect3DSurface9* engineRT[4] = {};
    static inline IDirect3DSurface9* engineDS = nullptr;
    static inline D3DVIEWPORT9 engineVP{};
    static inline bool engineStateOK = false;

    // The device, even after the pass has released its own reference: the
    // LAST restore happens after RunBlocking has returned, and handing the
    // engine back a device it did not leave is exactly the bug this bracket
    // exists to prevent.
    static IDirect3DDevice9* SliceDevice()
    {
        return dev ? dev : AcquireDevice();
    }

    static void SaveEngineState()
    {
        IDirect3DDevice9* dv = SliceDevice();
        if (!dv) return;
        if (!engineSB)
        {
            HRESULT hr = dv->CreateStateBlock(D3DSBT_ALL, &engineSB);
            if (FAILED(hr) || !engineSB)
            {
                engineSB = nullptr;
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    Log("gate: CreateStateBlock failed (hr=0x%08lX) - the loading screen's own "
                        "frames will see whatever the warm draws left; watch for artefacts",
                        (unsigned long)hr);
                }
            }
        }
        if (engineSB) engineStateOK = SUCCEEDED(engineSB->Capture());
        for (int i = 0; i < 4; i++)
        {
            SAFE_RELEASE(engineRT[i]);
            dv->GetRenderTarget(i, &engineRT[i]);
        }
        SAFE_RELEASE(engineDS);
        dv->GetDepthStencilSurface(&engineDS);
        dv->GetViewport(&engineVP);
    }

    static void RestoreEngineState()
    {
        IDirect3DDevice9* dv = SliceDevice();
        if (!dv) return;
        // Slots 1..3 go first: D3D9 refuses an RT0 smaller than a bound
        // secondary target, and our scratch targets are 64x64.
        for (int i = 1; i < 4; i++) dv->SetRenderTarget(i, nullptr);
        dv->SetRenderTarget(0, engineRT[0]);
        for (int i = 1; i < 4; i++) if (engineRT[i]) dv->SetRenderTarget(i, engineRT[i]);
        dv->SetDepthStencilSurface(engineDS);
        dv->SetViewport(&engineVP);
        if (engineSB && engineStateOK) engineSB->Apply();
    }

    static void ReleaseEngineState()
    {
        for (int i = 0; i < 4; i++) SAFE_RELEASE(engineRT[i]);
        SAFE_RELEASE(engineDS);
        SAFE_RELEASE(engineSB);
        engineStateOK = false;
    }

    // -------------------------------------------------------------------
    //  What the loading screen shows.
    //
    //  Four strings for the band bootgate.h draws inside the game's own loading
    //  screen: a headline and a percentage on the top row, the detail and a
    //  time estimate on the bottom one. Every one of them comes from a counter
    //  the pass already keeps and already logs, so what the player reads and
    //  what FusionFix.shaders.log says are the same thing.
    //
    //  THE ESTIMATE IS SCOPED, DELIBERATELY. The bar measures ONE phase, and a
    //  cold first launch runs three of them; extrapolating the current phase's
    //  rate and calling the answer "time left" would under-report a 40-minute
    //  load by a factor of three. So the band says which stage of how many it
    //  is on, and the estimate says "left in this stage" -- and says nothing at
    //  all until there is enough of the stage behind it to extrapolate from.
    //  Uppercase throughout, which is how the game sets its own UI labels.
    // -------------------------------------------------------------------
    static inline int64_t lastProgressUs = 0;

    static void Upper(char* s)
    {
        for (char* c = s; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }

    static void FormatClock(char* out, size_t cap, double seconds)
    {
        if (seconds < 0.0) seconds = 0.0;
        if (seconds > 359999.0) seconds = 359999.0;     // 99:59:59
        const int s = (int)seconds;
        if (s >= 3600) _snprintf_s(out, cap, _TRUNCATE, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
        else           _snprintf_s(out, cap, _TRUNCATE, "%d:%02d", s / 60, s % 60);
    }

    static void PublishProgress()
    {
        if (!bootgate::Prog().active) return;
        // Called from inside the job loop, so once every frame is plenty and
        // a snprintf per job is not.
        const int64_t now = pipelinekeys::ClockUs();
        if (now - lastProgressUs < 100000) return;
        lastProgressUs = now;

        const uint32_t done = workDone.load();
        const uint32_t total = workTotal ? workTotal : 1;
        float frac = std::clamp((float)done / (float)total, 0.0f, 1.0f);

        const auto nowTp = std::chrono::steady_clock::now();
        const double inStage = std::chrono::duration<double>(nowTp - tPhase).count();
        const double overall = std::chrono::duration<double>(nowTp - tStart).count();
        // curTitle is set only by the settles, which measure a quiet second and
        // not work done; their bar is not progress and must not be estimated
        // from. The headline is then the settle's own sentence.
        const bool settling = !curTitle.empty();

        // The cheap-path banner belongs to the WORK that took the cheap path,
        // not to the rest of the stage. EngineWarmPass clears cheapStage when
        // it returns, and a settle wins over it in any case: a settle can run
        // for minutes if the driver's cache turned out colder than the probe
        // suggested, and "this launch is seconds, not minutes" over a wait
        // nobody can time is the one thing on this band that could be a lie.
        const bool cheap = !settling && cheapStage.load() == stageNo;

        // FOUR SHORT STRINGS, AND THEY HAVE TO STAY SHORT. Each one is a
        // COLUMN of the band, and the first live run of the merged build drew
        // the detail and the estimate straight through each other because the
        // two together were half again as wide as the band. bootgate.h shrinks
        // a column that still does not fit, but shrinking is a fallback and
        // not a layout: the elapsed clock belongs beside the percentage, where
        // there is room, rather than on the end of the longest string in the
        // band, and the settles say what they are in four words.
        char head[64], pct[24], detail[96], eta[48], clock[24];
        _snprintf_s(head, sizeof(head), _TRUNCATE, "%s",
                    settling ? "Waiting for the driver"
                             : (cheap ? "Reusing the warm cache" : curWhat));
        FormatClock(clock, sizeof(clock), overall);
        if (settling)
            _snprintf_s(pct, sizeof(pct), _TRUNCATE, "%s", clock);
        else
            _snprintf_s(pct, sizeof(pct), _TRUNCATE, "%d%%   %s", (int)(frac * 100.0f), clock);

        if (cheap)
            _snprintf_s(detail, sizeof(detail), _TRUNCATE, "the driver already has these");
        else if (!curLabel.empty())
            _snprintf_s(detail, sizeof(detail), _TRUNCATE, "stage %d of %d   %s",
                        stageNo, stageCount, curLabel.c_str());
        else
            _snprintf_s(detail, sizeof(detail), _TRUNCATE, "stage %d of %d", stageNo, stageCount);

        if (cheap)
        {
            _snprintf_s(eta, sizeof(eta), _TRUNCATE, "seconds, not minutes");
        }
        else if (settling)
        {
            // Still the honest sentence, in a column's worth of words: this
            // wait is not work done and cannot be estimated from.
            _snprintf_s(eta, sizeof(eta), _TRUNCATE, "not timed - it ends when DXVK does");
        }
        else if (inStage >= 10.0 && frac >= 0.02f)
        {
            char left[24];
            FormatClock(left, sizeof(left), inStage * (1.0 - frac) / frac);
            _snprintf_s(eta, sizeof(eta), _TRUNCATE, "about %s left in this stage", left);
        }
        else
        {
            _snprintf_s(eta, sizeof(eta), _TRUNCATE, "working out how long this takes");
        }

        Upper(head); Upper(pct); Upper(detail); Upper(eta);
        bootgate::SetProgress(frac, head, pct, detail, eta);
    }

    // Called from the pass fiber wherever the pass used to present a frame of
    // its own. Hands the thread back to the loading screen when this slice's
    // time is up, so the game's artwork keeps animating.
    static void YieldToLoadscreen(bool force)
    {
        PumpMessages();
        PublishProgress();
        if (!passFiber || !pumpFiber) return;
        if (!force && pipelinekeys::ClockUs() < sliceDeadlineUs) return;
        SwitchToFiber(pumpFiber);
    }

    // The pass's Sleep: never block the loading-screen thread, hand it back
    // and ask not to be scheduled again for a while.
    static void PassSleep(int ms)
    {
        if (!passFiber || !pumpFiber) { Sleep(ms); return; }
        passResumeUs = pipelinekeys::ClockUs() + (int64_t)ms * 1000;
        SwitchToFiber(pumpFiber);
    }

    // THE WALK'S TEARDOWN, FROM OUTSIDE THE WALK.
    //
    // ReleaseEngineWalk() runs on every path THROUGH EngineWarmPass, and until
    // this existed those were the only paths it ran on. Two of them do not go
    // through it: a fault anywhere in the pass outside GuardedCall unwinds
    // straight to PassFiberProc's handler, and a gate that times out abandons
    // the pass fiber where it stands. Either one leaves a live warm D3D9 device
    // holding up to a chunk's worth of pipelines -- and a Vulkan surface on a
    // window -- for the whole session, which is precisely the leak the warm
    // device exists to prevent.
    //
    // It is idempotent (SAFE_RELEASE and null checks throughout), so calling it
    // again on the normal path costs nothing. Its own SEH frame, because the
    // reason we are here at all may be that something under it faulted.
    static void EmergencyReleaseWarmWalk()
    {
        if (!edev && !warmD3D && !warmWnd && engineRTs.empty() && engineDepths.empty()) return;
        __try
        {
            Log("engine: releasing the warm walk from outside it - the pass did not get to");
            ReleaseEngineWalk();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("engine: the warm walk's teardown faulted; its device may outlive the load");
        }
    }

    static void FinishPass()
    {
        EmergencyReleaseWarmWalk();
        pipelinekeys::PassThread() = 0;
        auto& vr = pipelinekeys::VulkanReplay();
        vr.passDone = true;
        vr.holding = false;
        vr.recordPaused = false;
        finished = true;
        gateDone = true;
    }

    // The pass is not going to run (or is not going to come back), and every
    // other layer has to be told.
    //
    // The whole rest of the mod waits on VulkanReplayState: vkcapture parks the
    // Fossilize replay for up to an hour on `passDone`, and capture stops
    // recording anything the pass thread draws. A boot gate that gives up
    // without saying so leaves a session with NO warming of any kind -- not the
    // engine walk it abandoned, not the recorded replay, not the Vulkan one --
    // which is strictly worse than the behaviour it was replacing. It is also
    // the one place `started` has to be latched: once this has been said, a
    // loading-screen tick that arrives late must not begin a pass nobody is
    // holding the screen for.
    static void AbandonPass(const char* why)
    {
        if (finished.exchange(true)) return;
        started = true;
        gateOpen = false;
        gateDone = true;
        // The pass fiber is suspended mid-walk and will never be resumed, so
        // whatever the engine walk was holding -- a warm D3D9 device and a
        // chunk's worth of pipelines -- has to be released from here or it is
        // the session's. This runs on the GATE's thread, not the pass's, so it
        // first waits for the loading-screen thread to be out of the fiber:
        // gateDone above stops the next slice, and a slice already in flight is
        // ~8 ms. If it does not come out, leaking the device is the lesser of
        // the two outcomes and the log says which one happened.
        for (int i = 0; i < 400 && inPassFiber.load(); i++) Sleep(5);
        if (inPassFiber.load())
            Log("gate: the pass fiber is still running, so its warm device is left alone - "
                "it will cost this session address space rather than a crash");
        else
            EmergencyReleaseWarmWalk();
        pipelinekeys::PassThread() = 0;
        auto& vr = pipelinekeys::VulkanReplay();
        vr.passDone = true;
        vr.holding = false;
        vr.recordPaused = false;
        Log("gate: the pass will not run from here (%s) - releasing every layer that was "
            "waiting for it, so the Vulkan replay starts in the background instead of "
            "waiting out its hour", why);
    }

    static void WINAPI PassFiberProc(void*)
    {
        __try { RunBlocking(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("precompile faulted - continuing to game"); }
        FinishPass();
        // Never scheduled again: the slice driver checks gateDone first.
        for (;;) SwitchToFiber(pumpFiber);
    }

    // One slice, from the loading-screen thread, after the engine's own tick.
    static void RunSlice()
    {
        if (gateDone.load()) return;
        const int64_t now = pipelinekeys::ClockUs();
        if (now < passResumeUs) return;

        if (!pumpFiber)
        {
            pumpFiber = ConvertThreadToFiber(nullptr);
            if (!pumpFiber)
            {
                Log("gate: ConvertThreadToFiber failed (%lu) - running the pass blocking, "
                    "with its own overlay, as it used to", GetLastError());
                gateFromBoot = false;   // keep the contexts; lose the native screen
            }
        }
        if (pumpFiber && !passFiber)
        {
            passFiber = CreateFiber(1 << 20, &PassFiberProc, nullptr);
            if (!passFiber)
            {
                Log("gate: CreateFiber failed (%lu) - running the pass blocking, with its own "
                    "overlay, as it used to", GetLastError());
                // Same as the ConvertThreadToFiber arm: RunBlocking is about to
                // present its OWN frames, and CreateOverlay/PresentOverlay both
                // read this flag to decide whether there is a backdrop and a
                // font. Left true, the player gets a bar with no text over a
                // loading screen that has stopped animating.
                gateFromBoot = false;
            }
        }
        if (!passFiber)
        {
            // Fail-safe: no fibers, so do it the old way -- one blocking call
            // that presents its own frames.
            pipelinekeys::PassThread() = GetCurrentThreadId();
            __try { RunBlocking(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("precompile faulted - continuing to game"); }
            FinishPass();
            return;
        }

        // Taken here rather than inside the pass, so the FIRST slice -- which
        // loads the .fxc database and creates every shader object -- is
        // bracketed by the same save and restore as the rest.
        if (!dev)
        {
            dev = AcquireDevice();
            if (dev) dev->AddRef();
        }

        // THE OLD GATE'S SITE, for the comparison the control run asked for.
        // This is the loading-screen render thread with the loading screen's own
        // state live -- exactly what the pass inherited when it ran from here
        // on 4c85a78, and what it does NOT inherit at the boot gate, where the
        // first ambient reading is taken on the main thread before any of this
        // has run. If the two lines are the same, the 51 pipelines the boot gate
        // costs the default path are not in the spec constants and the search
        // moves on; if they differ, the difference is the answer.
        if (sliceCount == 0 || sliceCount == 64)
            LogAmbientSpec(sliceCount ? "at slice 64, on the loading-screen thread (the old gate's site)"
                                      : "at the first slice, on the loading-screen thread (the old gate's site)",
                           dev);

        SaveEngineState();
        sliceDeadlineUs = now + (int64_t)(cfg.sliceMs > 0 ? cfg.sliceMs : 8) * 1000;
        sliceCount++;
        // THE PASS DOES NOT RESUME WHERE IT LEFT OFF -- not on the device.
        // RestoreEngineState() ends in engineSB->Apply(), a D3DSBT_ALL block
        // captured from the engine, and the engine then drew a loading-screen
        // frame with it. So every slice the pass wakes up holding the LOADING
        // SCREEN's render state, not the state its last draw left. A loop that
        // sets the blend factors, the blend op, the alpha func and the rest
        // once, and then varies only the handful of keys it means to vary, is
        // building pipelines with somebody else's half of the key from the
        // second slice onward. Every such loop watches this counter.
        sliceEpoch++;
        // While this is true the pass fiber is RUNNING on this thread, and the
        // gate's own thread must not pull anything out from under it -- see
        // AbandonPass, which is the one place that would.
        inPassFiber = true;
        SwitchToFiber(passFiber);
        inPassFiber = false;
        RestoreEngineState();
        if (gateDone.load()) ReleaseEngineState();
    }

    // -------------------------------------------------------------------
    //  The gate itself, on the main thread.
    // -------------------------------------------------------------------
    static void OpenBootGate()
    {
        const int64_t t0 = pipelinekeys::ClockUs();
        if (!bootgate::LoadscreenShown())
            Log("gate: rageBoot_InitSession returned but no loading screen is up - "
                "running anyway; there may be nothing on the screen while it works");

        // One byte, and the guard puts it back on every path including a fault
        // inside the pass. If it is ever left in place the loading screen never
        // comes down and the game is dead, so it is the first thing set up and
        // the last thing undone.
        bootgate::LoadscreenPin pin;
        if (!pin.Acquire())
        {
            Log("gate: could not pin rageBoot_LoadscreenEnd - leaving the boot gate alone");
            gateFailed = true;
            // And there is nowhere left for the pass to run. The caller's very
            // next instruction is the CALL to rageBoot_LoadscreenEnd, which
            // unregisters the render-thread loadscreen callback 0x005CC760 --
            // the hook the fail-safe gate lives on. So the D3D9 pass is lost
            // for this session; say so, and let the Fossilize replay get on
            // with it in the background rather than wait out its cap for a pass
            // that is never coming.
            AbandonPass("rageBoot_LoadscreenEnd could not be pinned");
            return;
        }

        // THE PHASE BUILD IS FOR THE ENGINE WARM PHASE ONLY, so it happens only
        // when that is on.
        //
        // Asking CViewportGame to build its render phases and allocate its
        // render targets two seconds before the engine would is the one real
        // intervention in this whole feature, and nothing else reads what it
        // produces: EngineWarmPass is the only consumer of gatePhases and
        // gateTargets, and it only runs at PrecompileEngineWarm > 0. With the
        // shipped default (PrecompileShaders = 1, PrecompileEngineWarm = 0) the
        // gate is here purely to put the recorded replay and the Fossilize hold
        // inside the game's own loading screen -- for which the pin and the
        // slice driver are the whole mechanism. So a default install does not
        // touch the engine at all, and a phase build that does not take cannot
        // cost it its warming.
        if (cfg.engineWarm > 0)
        {
            struct Reads { void* vp; uint16_t had, phases; } r{ nullptr, 0, 0 };
            // Every one of these is a pointer walk through engine memory, and
            // BuildRenderPhases is a call INTO the engine. BuildTables and
            // BuildJobs -- far less exposed -- are already behind GuardedCall;
            // these had nothing, on the main thread, with the loading screen
            // pinned. A fault here must cost the warm phase, not the session.
            const bool read = GuardedCall([](void* a) {
                auto* o = static_cast<Reads*>(a);
                o->vp = bootgate::FindGameViewport();
                o->had = bootgate::PhaseCount(o->vp);
                o->phases = bootgate::BuildRenderPhases(o->vp);
                if (o->vp && o->phases)
                {
                    bootgate::ReadPhaseSets(o->vp, gatePhases);
                    bootgate::ReadTargetRegistry(gateTargets);
                }
            }, &r);

            if (!read)
                Log("gate: reading the engine's render phases faulted - the warm phase will use "
                    "its shipped stand-in contexts, and the log marks every one of them");
            else
                Log("gate: game viewport %p, render phases %u -> %u (%s)", r.vp, r.had, r.phases,
                    r.had ? "the engine had already built them"
                          : "built here, by the engine's own vtable slot 12; its own build is now "
                            "suppressed by the viewport's word[+0x404] guard");
            if (read && (!r.vp || !r.phases))
                Log("gate: the engine built no render phase here - the warm phase will use its "
                    "shipped stand-in contexts, and the log marks every one of them");
        }
        gateFromBoot = true;
        bootgate::Prog().active = true;
        bootgate::SetProgress(0.0f, "PREPARING SHADERS", "0%",
                              "READING THE SHADER DATABASE", "");
        gateOpen = true;

        // The music, before the hold: the loading screen's own six stems loop
        // every forty seconds and this hold is measured in tens of minutes.
        // Engine calls only, on this thread, and End() below runs on every path.
        //
        // _dwCurrentEpisode is comvars', found by pattern and read by every
        // other episodic fix in the mod; warmmusic validates the pointer and
        // the value itself and falls back to the tracks every episode has.
        // This gate is past the episode/DLC menu, so the index is settled.
        warmmusic::Log() = &Log;
        {
            warmmusic::Options mo;
            mo.enabled      = cfg.warmMusic;
            mo.tracks       = cfg.warmMusicTracks;
            mo.tracksIV     = cfg.warmMusicTracksIV;
            mo.tracksTLAD   = cfg.warmMusicTracksTLAD;
            mo.tracksTBoGT  = cfg.warmMusicTracksTBoGT;
            mo.episode      = _dwCurrentEpisode;
            warmmusic::Begin(t0, mo);
        }
        // Belt and braces: a fault anywhere below this point must not leave a
        // track of ours playing into gameplay. End() is idempotent, so the
        // explicit call at the end of the hold stays where it reads best.
        struct MusicOff { ~MusicOff() { warmmusic::End(); } } musicOff;

        // Hold. The loading-screen thread picks the pass up on its next tick
        // and runs it in slices between the frames it draws; nothing else runs
        // on this thread, so the OS and Steam-overlay pump has to come from
        // here -- which is exactly what the game's own legals spin does with
        // the same function.
        //
        // THE EXIT TESTS COME BEFORE THE PUMP, not after it. The legals spin
        // holds for a second or two; this holds for as long as the pass takes,
        // and a player will alt-tab. bootgate::PumpOs now declines to call the
        // engine's pump in exactly the states where it would spin instead of
        // returning, but the ordering is what guarantees that gateDone and
        // PrecompileGateMaxSeconds are still evaluated even if it ever does.
        const int64_t startBy = t0 + 5 * 1000000ll;
        int64_t abortAt = 0;
        for (;;)
        {
            if (gateDone.load()) break;
            const int64_t now = pipelinekeys::ClockUs();
            if (!started.load() && now > startBy)
            {
                // exchange, not load: `started` is what the loading-screen tick
                // sets to claim the pass, so testing and latching it in one
                // operation is the only way the tick cannot begin a pass in the
                // window between this test and the gate walking away from it.
                if (!started.exchange(true))
                {
                    Log("gate: the loading-screen thread did not pick the pass up in %.1f s - "
                        "giving up on it", (now - t0) / 1e6);
                    gateFailed = true;
                    // rageBoot_LoadscreenEnd (which is about to run, five
                    // instructions from here) unregisters the render-thread
                    // loadscreen callback, so 0x005CC760 is not called again
                    // this session and the fail-safe gate has nothing to run
                    // on. Say so rather than leaving every layer waiting.
                    AbandonPass("the loading-screen thread never took it");
                    break;
                }
            }
            if (!abortAt && cfg.gateMaxSeconds > 0 &&
                now - t0 > (int64_t)cfg.gateMaxSeconds * 1000000ll)
            {
                Log("gate: PrecompileGateMaxSeconds (%d) reached after %.1f s - asking the pass "
                    "to stop and unwind", cfg.gateMaxSeconds, (now - t0) / 1e6);
                gateAbort = true;
                abortAt = now;
            }
            if (abortAt && now - abortAt > 120 * 1000000ll)
            {
                Log("gate: the pass did not unwind 120 s after being asked to - releasing the "
                    "loading screen anyway. The device may not have been handed back.");
                // The pass fiber is suspended mid-phase and the tick that would
                // resume it is about to be unregistered, so it never will be.
                // Nothing may go on believing a pass is running.
                AbandonPass("it did not unwind and can no longer be resumed");
                break;
            }
            warmmusic::Tick(now);
            bootgate::PumpOs();
            Sleep(1);
        }
        gateOpen = false;
        // Nothing of ours may still be audible when the loading screen comes
        // down, and the engine's own loading music has to be playing again if
        // it was when we arrived -- the game's first frame is what stops it.
        warmmusic::End();
        bootgate::Prog().active = false;
        {
            bootgate::Progress& pr = bootgate::Prog();
            char how[96];
            if (!pr.nativeChecked)
                _snprintf_s(how, sizeof(how), _TRUNCATE, "nothing - the loading screen never drew a frame");
            else if (pr.nativeFont)
                _snprintf_s(how, sizeof(how), _TRUNCATE, "the game's own font (CFont style %d)",
                            (int)pr.fontStyle);
            else if (pr.faultStep)
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "the 5x7 glyph set - CFont faulted at step %d", (int)pr.faultStep);
            else
                _snprintf_s(how, sizeof(how), _TRUNCATE,
                            "the 5x7 glyph set - no font texture is resident here");
            // Two reads, two verdicts. The layout float2[] and the colour u32[]
            // are different tables and fail independently, so one flag for both
            // would have the line contradict itself on a boot where only one of
            // them answered. The rule's own fraction is in neither table: the
            // frontend hard-codes 0.002 * H and so do we.
            Log("gate: progress drew with %s; band %.1f%% of the screen (%s), rule %.3f "
                "(the frontend's own literal), colour 0x%08X, text 0x%08X / 0x%08X (%s)",
                how, (double)pr.bandFrac * 100.0,
                pr.layoutFromGame ? "the pause menu's own layout table"
                                  : "the shipped fallback - the layout table was not readable",
                (double)pr.ruleFrac, (unsigned)pr.ruleARGB,
                (unsigned)pr.textARGB, (unsigned)pr.detailARGB,
                pr.colourFromGame ? "the pause menu's own colour table"
                                  : "the shipped fallback - the colour table was not readable");
        }
        Log("gate: released after %.1f s, %u slices", (pipelinekeys::ClockUs() - t0) / 1e6,
            sliceCount);
        // The pin is released here, and the very next instruction the engine
        // executes after this hook returns is its own CALL to
        // rageBoot_LoadscreenEnd -- so the loading screen comes down exactly
        // when and how it would have.
    }

    static void __fastcall InitSessionDetour(char a, int edx)
    {
        shInitSession.fastcall<void>(a, edx);
        // Act on the RETURN, and latch to the first fire: the episode flow can
        // call rageBoot_LoadEpisodeAndStartSession twice.
        static std::atomic<bool> fired{ false };
        if (!cfg.enabled || finished.load() || started.load()) return;
        if (fired.exchange(true)) return;
        OpenBootGate();
    }

    // The progress overlay rides the loading screen's own draw. The leave of
    // rageBoot_LoadscreenDrawLayers is the last thing in a frame the loading
    // screen actually drew -- the tick's own leave is not, because it is
    // called ~7 kHz and 99.9 % of those calls draw nothing.
    static void __fastcall DrawLayersDetour(char cl, int edx)
    {
        shDrawLayers.fastcall<void>(cl, edx);
        if (!bootgate::Prog().active) return;
        // Once per drawn frame, on the LAST of the layer passes. The tick has
        // three call sites: 0x005CCB89 with CL = 1, then exactly one of
        // 0x005CCC37 / 0x005CCEF4 with CL = 0 (they are the two arms of the
        // same test on 0x018B6F2D). Drawing on both put every quad of the
        // overlay -- three for the bar plus up to 512 for the text, each its
        // own grmShaderFx BeginPass/EndPass -- on the screen twice.
        if (cl != 0) return;
        __try { bootgate::DrawProgress(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            bootgate::Prog().active = false;
            Log("gate: the progress overlay faulted - it is off for the rest of the load");
        }
    }

    // -------------------------------------------------------------------
    //  Orchestration — the whole blocking pass, on the render thread.
    // -------------------------------------------------------------------
    // The same directory cache files are trimmed against (pipelinekeys.h), so every
    // shader whose bytecode a file leaves out is one this database can resolve.
    static std::string ResolveShaderDir()
    {
        return pipelinekeys::InstalledShaderDir().string();
    }

    static void RunBlocking()
    {
        tStart = std::chrono::steady_clock::now();
        tPhase = tStart;
        tLastPresent = tStart - std::chrono::milliseconds(1000);

        // How many stages this load will have, from the configuration, so the
        // band can tell the player where in the whole thing they are. The
        // stages are: the shader/replay pass, the engine walk if it is on, and
        // the Vulkan replay if it is.
        stageNo = 0;
        stageCount = 1 + (cfg.engineWarm > 0 ? 1 : 0) + (cfg.replayVulkan ? 1 : 0);
        cheapStage = 0;
        EnterStage("Compiling shaders");

        // The slice driver may already have taken the device, so that the very
        // first slice -- the one that loads the .fxc database and creates every
        // shader object -- is bracketed by the same state save and restore as
        // all the others. One reference either way.
        if (!dev)
        {
            dev = AcquireDevice();
            if (!dev) { Log("no device — aborting precompile"); return; }
            dev->AddRef();
        }
        DetectBackend();
        Log("backend = %s, device = %p", backend == Backend::DXVK ? "DXVK" : "native", (void*)dev);
        LogVa("at the gate, before the pass");
        // The ambient half of the key, at the moment the pass takes the device,
        // and the b# snapshot the engine walk pins on whichever device it runs
        // on. This is ALSO the measurement the 2026-09-21 control run asked for:
        // compare it with the same line from the first slice -- which is the
        // state the OLD loading-screen gate's pass inherited -- and the 51
        // pipelines that gate was worth are either in this line or they are not.
        {
            const AmbientSpec a = ReadAmbientSpec(dev);
            ambientBoolsRead = a.readable;
            if (a.readable)
            {
                for (int i = 0; i < 16; i++)
                {
                    ambientVsB[i] = (a.vsBools >> i) & 1 ? TRUE : FALSE;
                    ambientPsB[i] = (a.psBools >> i) & 1 ? TRUE : FALSE;
                }
            }
            LogAmbientSpec("at the gate, before the pass", dev);
        }
        if (cfg.warmDevice == 2) ProbeWarmDevice();
        ResolveOverlayInterval();

        // We are armed on the loading-screen render, so the game is very likely already
        // inside a BeginScene/EndScene pair. D3D9 forbids CreateStateBlock there (it
        // returns D3DERR_INVALIDCALL) and BeginScene fails too, which previously meant:
        //   - sb stayed null, Apply() was skipped, and NOTHING we changed was restored
        //     -> the game resumed with our render states/textures/constants live
        //        (black sky, depth/occlusion mismatches), and
        //   - the overlay's BeginScene failed, so the progress bar froze after one frame.
        // Detect the enclosing scene by probing BeginScene, leave it for the duration,
        // and re-enter it before handing control back.
        HRESULT hrProbe = dev->BeginScene();
        bool insideGameScene = FAILED(hrProbe);   // INVALIDCALL => the game owns a scene
        dev->EndScene();                          // ends ours, or leaves the game's
        if (insideGameScene) Log("entered inside the game's scene — suspended it for the pass");

        // Snapshot ALL device state so the game resumes byte-identical afterwards.
        IDirect3DStateBlock9* sb = nullptr;
        HRESULT hrSB = dev->CreateStateBlock(D3DSBT_ALL, &sb);
        if (FAILED(hrSB) || !sb)
        {
            // Without this we cannot put the device back the way we found it. Abort
            // rather than corrupt the frame -- a missing precompile is a perf issue,
            // a corrupted device is a visual bug.
            Log("CreateStateBlock failed (hr=0x%08lX) — aborting precompile to avoid corrupting device state", (unsigned long)hrSB);
            if (insideGameScene) dev->BeginScene();
            dev->Release(); dev = nullptr;
            return;
        }
        IDirect3DSurface9* saveRT = nullptr; IDirect3DSurface9* saveDS = nullptr;
        IDirect3DSurface9* saveRT1 = nullptr; IDirect3DSurface9* saveRT2 = nullptr; IDirect3DSurface9* saveRT3 = nullptr;
        dev->GetRenderTarget(0, &saveRT);
        dev->GetRenderTarget(1, &saveRT1);
        dev->GetRenderTarget(2, &saveRT2);
        dev->GetRenderTarget(3, &saveRT3);
        dev->GetDepthStencilSurface(&saveDS);
        D3DVIEWPORT9 saveVP{}; dev->GetViewport(&saveVP);

        // Full snapshot for the neutrality check at the end.
        StateSnapshot before{};
        CaptureSnapshot(before);
        presentsAtStart = pipelinekeys::DevicePresentCount();

        CreateOverlay();
        // The backdrop is blurred by EngineWarmPass itself, once its plan has
        // validated: softening it here meant a fail-safe run spent the whole
        // load behind a blur with nothing running behind it.
        PresentOverlay(true);

        // The order on the loading screen (VulkanReplayState in pipelinekeys.h):
        //   1. the D3D9 pass, then wait until DXVK is quiet;
        //   2. the whole Vulkan replay, held, then wait until DXVK is quiet again;
        //   3. only then is the loading screen let go.
        Log("order: 1. D3D9 pass, t=%.3f", pipelinekeys::VulkanReplay().Seconds());
        fxc_db* db = fxc_load_all(ResolveShaderDir().c_str());
        if (!db) { Log("fxc_load_all failed: %s", fxc_last_error()); }
        else
        {
            fxc_stats st{}; fxc_get_stats(db, &st);
            Log("parsed %u effects, %u unique shaders, %u passes (errors %u)",
                st.effect_count, st.unique_total, st.pass_count, st.parse_errors);
            // Log the install's .fxc hash set now, so it is in every log -- capture
            // would otherwise only compute it at its first flush, and not at all
            // when capture is off.
            (void)pipelinekeys::InstalledFxcHashes();

            // Prefer replaying REAL captured keys over the synthetic coverage set:
            // the synthetic pass measured as buying nothing (59 isolated spikes ON
            // vs 55 OFF) because its draws build different pipeline keys than the
            // game's. Fall back to it only when there is no usable capture.
            bool useReplay = cfg.replay && LoadKeyFile();

            // Exact work units so the bar tracks real progress and reaches 100%:
            //   creates (unique shaders) + one unit per pass + tuple warms
            //   + present-format warms. Must match every workDone++ site below.
            uint32_t passUnits = (cfg.breadth > 0) ? st.pass_count : 0;
            uint32_t tupleUnits = (cfg.breadth > 0) ? (uint32_t)kTupleCount + (cfg.coverPresent ? 2u : 0u) : 0;
            workTotal = useReplay
                ? st.unique_total * kCreateW + (uint32_t)replayRecs.size() * kPassW
                : st.unique_total * kCreateW + (passUnits + tupleUnits) * kPassW;
            if (workTotal == 0) workTotal = 1;
            workDone = 0;

            CreateResources();
            CreateAllShaders(db);
            if (useReplay)
            {
                IndexShadersByHash(db);
                // After the .fxc database, which clears the handle maps: the engine
                // then adds only what the files could not supply. Either object warms
                // the same pipeline -- DXVK keys its shader modules on the bytecode
                // hash -- so the order is about which one is bound, not what is built.
                IndexShadersFromEngine();
                IndexShadersFromBlobs();
                IndexShadersFromOwnResources();
                ReplayPass();
            }
            else
            {
                DummyDrawPass(db);
            }
            LogVa("after the recorded D3D9 replay");
            // Then the engine's own space, which needs no recording at all.
            // It runs AFTER the recorded replay on purpose: the recorded keys
            // are the ones this PC has proven it draws, so a
            // PrecompileBudgetSeconds cut sheds the speculative work first.
            if (cfg.engineWarm > 0)
            {
                EnterStage("Warming engine pipelines");
                Log("order: 1b. engine warm phase (level %d), t=%.3f",
                    cfg.engineWarm, pipelinekeys::VulkanReplay().Seconds());
                EngineWarmPass(db);
                LogVa("after the engine warm phase");
                // The banner was about the walk, and the walk is over. What
                // follows in this stage is the wait for DXVK, which is timed by
                // nothing and must not wear "this launch is seconds".
                cheapStage = 0;
            }
            WaitUntilIdle("the D3D9 pass");
            LogVa("after waiting for DXVK to go quiet");
            // Everything the other PCs' keys started is built: record again.
            if (pipelinekeys::VulkanReplay().recordPaused.exchange(false))
                Log("replay: Vulkan recording resumed, t=%.3f - %u pipelines DXVK created meanwhile were not recorded",
                    pipelinekeys::VulkanReplay().Seconds(), pipelinekeys::VulkanReplay().notRecorded.load());
            fxc_free(db);
        }
        if (HoldForVulkanReplay())
            WaitUntilIdle("the Vulkan replay");
        LogVa("after the Vulkan replay");
        // NB: ReleaseResources() deliberately happens AFTER the state restore below.
        // Releasing our scratch targets and textures while they are still bound, and
        // only then putting the game's state back, is the wrong order -- COM keeps
        // them alive so it is survivable, but it leaves a window where the device
        // references objects we have dropped, and it is not a window worth having
        // while chasing a state-corruption bug.

        // Restore device state fully.
        dev->SetRenderTarget(0, saveRT);
        dev->SetRenderTarget(1, saveRT1);
        dev->SetRenderTarget(2, saveRT2);
        dev->SetRenderTarget(3, saveRT3);
        dev->SetDepthStencilSurface(saveDS);
        dev->SetViewport(&saveVP);
        SAFE_RELEASE(saveRT); SAFE_RELEASE(saveRT1); SAFE_RELEASE(saveRT2); SAFE_RELEASE(saveRT3);
        SAFE_RELEASE(saveDS);
        if (sb) { sb->Apply(); sb->Release(); }

        // Now that the game's state is back, drop our scratch resources.
        ReleaseResources();

        // Did we actually hand the device back unchanged?
        //
        // A live run once produced a black sky and depth/occlusion mismatches after
        // this pass. That was recorded as "fixed in 55e7156", but re-reading that
        // commit shows it changed nothing about state handling on this path -- the
        // scene probe is a no-op unless the game owns a scene, and the logs show it
        // never does. So the corruption was never explained, only unobserved.
        // Guessing again is worthless; compare the state we found against the state
        // we left and name whatever differs.
        VerifyStateRestored(before);

        // Re-enter the scene we suspended, so the game's own EndScene still pairs up.
        if (insideGameScene) dev->BeginScene();

        auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart).count() / 1000.0;
        Log("precompile complete in %.1fs (%u overlay frames, %.1f/s; frame gap min %u ms, "
            "avg %llu ms, max %u ms, target %d ms; %u text rebuilds)",
            secs, overlayFrames, secs > 0.0 ? overlayFrames / secs : 0.0,
            overlayGapMin == 0xFFFFFFFFu ? 0u : overlayGapMin,
            (unsigned long long)(overlayGapCount ? overlayGapSum / overlayGapCount : 0),
            overlayGapMax, overlayIntervalMs, overlayBaseRebuilds);

        // Every frame we present is fully redrawn, so a presented frame cannot be
        // missing the bar. If the device presented MORE times than we did, those
        // extra frames came from somewhere else and are what flickers.
        //
        // On the boot gate that comparison is meaningless and would fire every
        // single time: the pass presents nothing at all (it draws into the
        // engine's frame off rageBoot_LoadscreenDrawLayers) while the
        // loading-screen thread free-runs at ~64 fps for the whole pass, so the
        // difference is tens of thousands of frames BY DESIGN. The interesting
        // number there is the opposite one -- that the game's own screen kept
        // drawing -- so log that instead of a flicker verdict nobody should act
        // on.
        unsigned devPresents = pipelinekeys::DevicePresentCount() - presentsAtStart;
        if (gateFromBoot)
            Log("the pass presented %u frames of its own (0 is correct on this gate: progress "
                "is drawn into the engine's frame off rageBoot_LoadscreenDrawLayers). The "
                "loading-screen thread lent it %u slices, which is the number that says the "
                "game's own screen kept running; %u presents were seen on the device, and the "
                "loading screen does not go through the hooked one",
                overlayFrames, sliceCount, devPresents);
        else if (devPresents > overlayFrames + 2)
            Log("FLICKER SOURCE: device presented %u times during the pass but the overlay only "
                "drew %u of them - %u frames came from elsewhere",
                devPresents, overlayFrames, devPresents - overlayFrames);
        else
            Log("presents accounted for: %u device presents vs %u overlay frames - nothing else "
                "is presenting during the pass", devPresents, overlayFrames);
        LogVa("at the gate, with the pass over");
        Log("order: 3. loading screen released, t=%.3f", pipelinekeys::VulkanReplay().Seconds());
        dev->Release();
        dev = nullptr;
    }

    // Readiness gate (fixes the "ran before windowed-borderless / half-ready" bug).
    // Run only once the swapchain has held a FINAL, stable size for N frames — by
    // which point FusionFix's windowed-borderless window setup and any resolution
    // device-reset have completed — and a loading screen is genuinely up (still
    // before gameplay). Returns true when it is time to run.
    static bool GateReady()
    {
        IDirect3DDevice9* d = AcquireDevice();
        if (!d) { stableFrames = 0; return false; }

        // Must be on a loading screen (pre-gameplay).
        if (!(CMenuManager::bLoadscreenShown && *CMenuManager::bLoadscreenShown))
            return false;

        // Live swapchain size; must hold steady, meaning the borderless restyle /
        // resolution reset is done and the back buffer is final.
        UINT w = 0, h = 0;
        IDirect3DSwapChain9* sc = nullptr;
        if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc)
        {
            D3DPRESENT_PARAMETERS pp{};
            if (SUCCEEDED(sc->GetPresentParameters(&pp)))
            {
                w = pp.BackBufferWidth; h = pp.BackBufferHeight;
                if (pp.hDeviceWindow) gameWnd = pp.hDeviceWindow;
            }
            sc->Release();
        }
        if (w == 0 || h == 0) { stableFrames = 0; return false; }

        if (w == gateW && h == gateH) stableFrames++;
        else { gateW = w; gateH = h; stableFrames = 0; }

        return stableFrames >= kStableFramesNeeded;
    }

    // ---- probe: does the game present what we draw? ---------------------
    // The incremental stepper (831a527, reverted in ab9c958) assumed that if we
    // draw after the game's own loadscreen render and let the GAME present, our
    // pixels reach the screen. They never appeared. Before rebuilding the stepper
    // on that assumption, test it directly rather than reasoning about it: clear
    // the CURRENT render target to magenta every Nth loadscreen frame.
    //
    // The first version of this asked a human to watch for the flash, which is a
    // bad instrument — it needs someone looking at the right moment, and a "no"
    // is indistinguishable from "wasn't paying attention". So the probe now reads
    // the answer back itself: after clearing, it grabs the FRONT buffer (what the
    // display is actually scanning out, via GetFrontBufferData) on the following
    // few frames and reports whether magenta ever reached the screen. Back buffer
    // vs front buffer matters here: sampling the back buffer would only prove our
    // Clear landed, not that anything presented it.
    static inline IDirect3DSurface9* frontCopy = nullptr;
    static inline UINT frontW = 0, frontH = 0;
    static inline int  flashWatch = 0;      // frames left to look for magenta
    static inline bool flashAnswered = false;

    static bool FrontBufferIsMagenta(IDirect3DDevice9* d)
    {
        if (!frontCopy)
        {
            // GetFrontBufferData always wants a SYSTEMMEM A8R8G8B8 surface the size
            // of the DISPLAY MODE, not the back buffer.
            D3DDISPLAYMODE dm{};
            if (FAILED(d->GetDisplayMode(0, &dm)) || !dm.Width) return false;
            frontW = dm.Width; frontH = dm.Height;
            if (FAILED(d->CreateOffscreenPlainSurface(frontW, frontH, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &frontCopy, nullptr)))
            {
                frontCopy = nullptr;
                Log("probe: could not create a %ux%u readback surface", frontW, frontH);
                return false;
            }
        }

        if (FAILED(d->GetFrontBufferData(0, frontCopy))) return false;

        D3DLOCKED_RECT lr{};
        if (FAILED(frontCopy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;

        // Sample a coarse grid rather than one pixel: the window may be letterboxed
        // or composited somewhere other than the origin.
        int magenta = 0, sampled = 0;
        for (UINT y = frontH / 8; y < frontH; y += frontH / 8)
        {
            auto row = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
            for (UINT x = frontW / 8; x < frontW; x += frontW / 8)
            {
                auto px = (const uint32_t*)(row + (size_t)x * 4);
                uint32_t b = (*px) & 0xFF, g = (*px >> 8) & 0xFF, r = (*px >> 16) & 0xFF;
                if (r > 200 && b > 200 && g < 60) magenta++;
                sampled++;
            }
        }
        frontCopy->UnlockRect();

        if (magenta > 0)
            Log("probe: FRONT BUFFER IS MAGENTA (%d of %d sampled pixels) - our draws DO reach the screen",
                magenta, sampled);
        return magenta > 0;
    }

    static void FlashProbe()
    {
        static int  calls = 0;
        static bool describedRT = false;

        IDirect3DDevice9* d = AcquireDevice();
        if (!d) return;

        if (!describedRT)
        {
            describedRT = true;

            // Which thread are we on? shaderprecompile.ixx's PumpMessages() assumes
            // the loadscreen render callback runs on the thread that pumps the game
            // window. Ghidra says otherwise: 0x008da370 is a THREAD ENTRY stub that
            // tails into the pump loop at 0x008da240, so the callback runs on a
            // dedicated loading-screen thread. One of those is wrong; logging the id
            // here and in ShaderCapture (which runs on the game's EndScene thread)
            // settles it without another bisect.
            Log("probe: loadscreen render callback thread = %lu", GetCurrentThreadId());

            // PumpMessages() assumes this thread owns the game window's message
            // queue. Windows only delivers messages to the thread that CREATED the
            // window, so if these differ, that pump has been draining the wrong
            // queue and cannot be why the window survives a long blocking pass.
            {
                HWND w = gameWnd;
                if (!w) w = FindWindowW(nullptr, L"Grand Theft Auto IV");
                if (w)
                {
                    DWORD owner = GetWindowThreadProcessId(w, nullptr);
                    Log("probe: window %p owned by thread %lu, we are %lu -> PumpMessages %s",
                        w, owner, GetCurrentThreadId(),
                        (owner == GetCurrentThreadId()) ? "pumps the RIGHT queue" : "pumps the WRONG queue");
                }
                else
                {
                    Log("probe: could not find the game window to check message-queue ownership");
                }
            }

            IDirect3DSurface9* rt = nullptr;
            if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt)
            {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(rt->GetDesc(&sd)))
                    Log("probe: RT0 after loadscreen render = %ux%u fmt=%u usage=%u pool=%u",
                        sd.Width, sd.Height, (uint32_t)sd.Format, sd.Usage, (uint32_t)sd.Pool);

                // Is RT0 the actual back buffer, or an offscreen surface the game
                // later blits? That distinction is the whole question.
                IDirect3DSurface9* bb = nullptr;
                if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
                {
                    Log("probe: RT0 %s the back buffer (rt=%p bb=%p)", (rt == bb) ? "IS" : "is NOT", rt, bb);
                    bb->Release();
                }
                rt->Release();
            }
            else
            {
                Log("probe: no RT0 bound after the loadscreen render");
            }
        }

        // Look for the previous flash before issuing a new one, so a magenta hit is
        // unambiguously from a clear the game had a chance to present.
        if (flashWatch > 0 && !flashAnswered)
        {
            flashWatch--;
            if (FrontBufferIsMagenta(d))
            {
                flashAnswered = true;
                Log("probe: VERDICT - the loadscreen presents what we draw into RT0; "
                    "the reverted stepper's bug is in its work loop, not its target");
            }
            else if (flashWatch == 0)
            {
                Log("probe: no magenta in the front buffer after %d frames - "
                    "RT0 is the back buffer but nothing presents it during the loadscreen",
                    cfg.flashProbe);
            }
        }

        if ((calls++ % cfg.flashProbe) == 0 && !flashAnswered)
        {
            d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0);
            flashWatch = cfg.flashProbe - 1;   // watch every frame until the next flash
        }
    }

    // ---- injection anchor: FUN_005cc760 (loadscreen-thread render tick) ------
    //
    // The ORIGINAL RUNS FIRST now. On the boot gate this detour is a slice
    // driver, not a gate: the engine draws its own loading-screen frame, then
    // we lend the thread to the pass for a few milliseconds, then the pump
    // loop that called us carries on. Doing it the other way round would mean
    // the pass ran before the frame it is meant to be drawn over.
    static void __cdecl LoadscreenRenderDetour()
    {
        shLoadscreenRender.call<void>();

        if (cfg.enabled && !finished.load())
        {
            if (gateOpen.load())
            {
                if (!started.exchange(true))
                {
                    // Our own draws from here on are not the game's: capture skips them.
                    pipelinekeys::PassThread() = GetCurrentThreadId();
                    Log("gate: the boot gate is open - starting the pass on the "
                        "loading-screen thread, in slices, with the game's own screen up");
                }
                RunSlice();
            }
            else if (gateFailed.load() && !started.load())
            {
                // The fail-safe: the boot gate could not be used on this build,
                // so the pass runs where it always did -- one blocking call on
                // a loading screen, presenting its own frames.
                loadscreenFramesSeen++;
                if (GateReady())
                {
                    started = true;
                    pipelinekeys::PassThread() = GetCurrentThreadId();
                    __try { RunBlocking(); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { Log("precompile faulted - continuing to game"); }
                    FinishPass();
                    Log("gate: FALLBACK - ran after %d loadscreen frames at %ux%u",
                        loadscreenFramesSeen, gateW, gateH);
                }
            }
        }

        if (cfg.flashProbe > 0) FlashProbe();
    }

    static void ReadConfig()
    {
        CIniReader ini("");
        cfg.enabled      = ini.ReadInteger("SHADERS", "PrecompileShaders", 1) != 0;
        cfg.breadth      = ini.ReadInteger("SHADERS", "PrecompileCoverage", 2);
        cfg.overlay      = ini.ReadInteger("SHADERS", "PrecompileOverlay", 1) != 0;
        cfg.coverSdr     = ini.ReadInteger("SHADERS", "PrecompileCoverSdr", 0) != 0;
        cfg.coverPresent = ini.ReadInteger("SHADERS", "PrecompileCoverPresentModes", 1) != 0;
        cfg.flashProbe   = ini.ReadInteger("SHADERS", "PrecompileFlashProbe", 0);
        cfg.replay       = ini.ReadInteger("SHADERS", "PrecompileReplayCapturedKeys", 1) != 0;
        cfg.adaptiveExit = ini.ReadInteger("SHADERS", "PrecompileAdaptiveExit", 0) != 0;
        cfg.budgetSeconds = ini.ReadInteger("SHADERS", "PrecompileBudgetSeconds", 0);
        cfg.fenceChunk    = ini.ReadInteger("SHADERS", "PrecompileFenceChunk", 1);
        cfg.specVariants  = ini.ReadInteger("SHADERS", "PrecompileSpecVariants", 1) != 0;
        cfg.exportBaseline = ini.ReadInteger("SHADERS", "PrecompileExportBaseline", 0) != 0;
        cfg.engineWarm    = ini.ReadInteger("SHADERS", "PrecompileEngineWarm", 0);
        // Clamped, and it says so when it clamps: 0 is the documented A/B arm
        // that builds 61,000 pipelines on the game's device and has killed a
        // session, so it may only be reached by asking for it exactly.
        cfg.warmDevice    = ini.ReadInteger("SHADERS", "PrecompileWarmDevice", 1);
        if (cfg.warmDevice < 0 || cfg.warmDevice > 2)
        {
            Log("PrecompileWarmDevice = %d is not 0, 1 or 2 - using 1 (a warm device of our own)",
                cfg.warmDevice);
            cfg.warmDevice = 1;
        }
        cfg.crashLog      = ini.ReadInteger("SHADERS", "PrecompileCrashLog", 0) != 0;
        cfg.emitLog       = ini.ReadInteger("SHADERS", "PrecompileEmitLog", 0) != 0;
        cfg.sliceMs       = ini.ReadInteger("SHADERS", "PrecompileSliceMs", 8);
        cfg.gateMaxSeconds = ini.ReadInteger("SHADERS", "PrecompileGateMaxSeconds", 0);
        cfg.bootGate       = ini.ReadInteger("SHADERS", "PrecompileBootGate", 1) != 0;
        cfg.keySampleCount = ini.ReadInteger("SHADERS", "PrecompileKeySampleCount", 0) != 0;
        cfg.reuseStamp    = ini.ReadInteger("SHADERS", "PrecompileReuseWarmCache", 1) != 0;
        cfg.warmMusic     = ini.ReadInteger("SHADERS", "PrecompileWarmMusic", 1) != 0;
        cfg.warmMusicTracks = ini.ReadString("SHADERS", "PrecompileWarmMusicTracks", "");
        cfg.warmMusicTracksIV    = ini.ReadString("SHADERS", "PrecompileWarmMusicTracksIV", "");
        cfg.warmMusicTracksTLAD  = ini.ReadString("SHADERS", "PrecompileWarmMusicTracksTLAD", "");
        cfg.warmMusicTracksTBoGT = ini.ReadString("SHADERS", "PrecompileWarmMusicTracksTBoGT", "");
        cfg.replayVulkan  = ini.ReadInteger("SHADERS", "ReplayVulkanPipelines", 1) != 0;
        // Not ours, but it is the only thing that can put a sample count on a
        // render target, and the sample count is in the pipeline key. The
        // engine cannot be asked at the gate (no D3D texture exists behind a
        // target yet), so the setting is the source.
        cfg.reflectionMsaa = ini.ReadInteger("EXPERIMENTAL", "ReflectionMSAAQuality", 0);
    }

public:
    ShaderPrecompiler()
    {
        FusionFix::onInitEvent() += []()
        {
            ReadConfig();
            // The flash probe rides the same hook, so arm for either.
            if (!cfg.enabled && cfg.flashProbe <= 0) { Log("disabled via ini"); return; }

            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&RebaseVA, &hSelf);
            ArmCrashLogging();

            // Install the code hook on the render-thread loadscreen render
            // (FUN_005cc760 @ 0x005cc760, 1.2.0.59). It does NOT run on first call:
            // the GateReady() check holds off until the swapchain size is final and
            // stable (windowed-borderless + any device reset settled) with a loading
            // screen up — still before gameplay. The blocking pass pumps window
            // messages so the window never goes "not responding".
            void* target = reinterpret_cast<void*>(RebaseVA(0x005cc760));
            shLoadscreenRender = safetyhook::create_inline(target, reinterpret_cast<void*>(&LoadscreenRenderDetour));
            if (shLoadscreenRender)
                Log("armed at loadscreen render %p (breadth=%d, overlay=%d, gate=%d frames)", target, cfg.breadth, cfg.overlay, kStableFramesNeeded);
            else
                Log("FAILED to hook loadscreen render at %p", target);

            // The boot gate. Every address it needs is above 0x004FB000, so it
            // is readable now, before the game has decrypted its RAGE core --
            // and it is fingerprinted now, so a build this does not know never
            // gets a 0xC3 written into it. On any mismatch this says so once
            // and the loading-screen gate above carries the feature exactly as
            // it always did.
            gateCheck = bootgate::ValidateCode();
            if (!cfg.bootGate)
            {
                gateFailed = true;
                Log("boot gate OFF by PrecompileBootGate = 0: the pass runs at the old "
                    "loading-screen gate instead. That is the control arm, not a shipping "
                    "configuration - the engine walk has no render phases there.");
            }
            else if (!gateCheck.ok)
            {
                gateFailed = true;
                Log("boot gate OFF: %s. Falling back to the loading-screen gate - the warm "
                    "pass still runs, but its render-target and phase contexts come from the "
                    "shipped stand-in table.", gateCheck.why.c_str());
            }
            else
            {
                void* init = reinterpret_cast<void*>(RebaseVA(bootgate::kVA_InitSession));
                void* lay  = reinterpret_cast<void*>(RebaseVA(bootgate::kVA_LoadscreenLayers));
                shInitSession = safetyhook::create_inline(init, reinterpret_cast<void*>(&InitSessionDetour));
                shDrawLayers  = safetyhook::create_inline(lay,  reinterpret_cast<void*>(&DrawLayersDetour));
                gateArmed = (bool)shInitSession;
                if (!gateArmed)
                {
                    gateFailed = true;
                    Log("boot gate: FAILED to hook rageBoot_InitSession at %p - falling back "
                        "to the loading-screen gate", init);
                }
                else
                {
                    Log("boot gate armed at rageBoot_InitSession %p (progress overlay %s at "
                        "rageBoot_LoadscreenDrawLayers %p, %d ms of warm work per "
                        "loading-screen frame)",
                        init, shDrawLayers ? "on" : "OFF - could not hook", lay, cfg.sliceMs);
                }
            }
            // The whole Vulkan replay waits for this pass, and then runs with the loading
            // screen held; without the pass -- off, or no hook to run it from -- it runs
            // in the background from the start.
            pipelinekeys::VulkanReplay().passPlanned = cfg.enabled && shLoadscreenRender;
        };
    }
} ShaderPrecompiler;
