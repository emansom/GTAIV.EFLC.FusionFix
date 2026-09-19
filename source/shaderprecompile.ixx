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
// COMPLETION GATE (W4): GPL is left at the DXVK default (Auto). After all creates
// + draws are submitted we drain the GPU with a D3DQUERYTYPE_EVENT fence plus a
// short present-settle, so nothing is left compiling when gameplay starts. DXVK
// exposes no app-readable compiler counter; with the default Auto, background
// "optimized" recompiles are non-blocking (they never stall a draw). No dxvk.conf
// change is required or made.
//
// INTEGRATION (W2): a FusionFix ASI module. Injection anchor is the render-thread
// per-frame loading-screen render function FUN_005cc760 (GTA IV 1.2.0.59), found
// by Ghidra: it runs on the render thread, only while bLoadscreenShown, after the
// device+swapchain are created and before any gameplay scene render. On its first
// call we run the whole (blocking) precompile there, driving our own Clear +
// overlay + Present frames, then hand the loading screen back untouched.
// ===========================================================================

#include <common.hxx>
#include <d3d9.h>
#include <d3dx9.h>
#include <d3dx9core.h>
#include <cstdarg>
#include <cstdio>
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
    // PS: per used sampler slot, its texture dimension (2=2D, 3=CUBE, 4=VOLUME, 0=unknown).
    // samplerDim[slot] == 0 means "not declared / leave null".
    uint8_t samplerDim[16] = {};
    bool    usesSampler[16] = {};
};

static void ParseShaderIO(const uint8_t* bytecode, uint32_t size, bool isVS, ShaderIO& out)
{
    if (!bytecode || size < 8) return;
    const uint32_t* dw = reinterpret_cast<const uint32_t*>(bytecode);
    uint32_t n = size / 4;
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
            } else if (!isVS && regtype == 10 /*SAMPLER*/ && regnum < 16) {
                out.usesSampler[regnum] = true;
                out.samplerDim[regnum]  = (uint8_t)((dcl >> 27) & 0xF); // D3DSTT_*
            }
            i += 3;                              // dcl is always 2 operand dwords in SM3
            continue;
        }
        uint32_t len = (tok >> 24) & 0xF;
        i += 1 + len;
        if (len == 0) i++;                       // safety: never stall on a 0-length token
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
    static inline std::chrono::steady_clock::time_point tStart;
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
        bool valid{};
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

        if (diffs == 0)
            Log("state verified: device handed back byte-identical across %u render states, "
                "%u samplers, shaders, streams (incl. frequency) and targets",
                pipelinekeys::kNumRS, pipelinekeys::kNumSamplers);
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
            if (SUCCEEDED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
            {
                // Snapshot the current loading-screen frame to composite our bar over.
                if (SUCCEEDED(dev->CreateRenderTarget(bbW, bbH, bbFmt, D3DMULTISAMPLE_NONE, 0, FALSE, &backdrop, nullptr)) && backdrop)
                    dev->StretchRect(bb, nullptr, backdrop, nullptr, D3DTEXF_NONE);
                bb->Release();
            }
            sc->Release();
        }

        if (cfg.overlay)
        {
            D3DXCreateFontW(dev, 22, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &font);
            D3DXCreateFontW(dev, 34, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &fontBig);
        }
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

    // Present a progress frame (throttled). frac in [0,1].
    static void PresentOverlay(bool force)
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
            auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart).count() / 1000.0;
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
                            "Compiling shaders  %3d%%    ETA %d:%02d", (int)(frac * 100.0f),
                            (int)eta / 60, (int)eta % 60);
            }
            else
            {
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "Compiling shaders  %3d%%    estimating...", (int)(frac * 100.0f));
            }
            title = line;
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

        for (uint32_t i = 0; i < n; i++)
        {
            const fxc_shader* s = fxc_unique_shader(db, i);
            if (!s) { workDone++; continue; }
            ParseShaderIO(s->bytecode, s->size, s->stage == FXC_STAGE_VS, shaderIO[i]);
            if (s->stage == FXC_STAGE_VS)
                dev->CreateVertexShader(reinterpret_cast<const DWORD*>(s->bytecode), &vsHandles[i]);
            else
                dev->CreatePixelShader(reinterpret_cast<const DWORD*>(s->bytecode), &psHandles[i]);

            workDone++;
            if ((i & 15) == 0) curLabel = "shader " + std::to_string(i + 1) + " / " + std::to_string(n);
            PresentOverlay(false); // pumps every iter; presents on the ~33ms throttle
        }
        Log("created %u shaders", n);
    }

    // -------------------------------------------------------------------
    //  Dummy-draw pass (W3b) — walk real passes, reproduce DXVK pipeline keys.
    // -------------------------------------------------------------------
    static void DummyDrawPass(fxc_db* db)
    {
        if (cfg.breadth <= 0) return;

        // Fixed opaque, filled, front-facing defaults for the non-baked (dynamic)
        // states; the baked axes are set per draw below.
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        RECT sc{ 0, 0, 1, 1 }; dev->SetScissorRect(&sc);

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
    static uint64_t ReplayPipelineKey(const pipelinekeys::KeyRecord& k)
    {
        struct Reduced
        {
            uint64_t base;
            uint32_t alphaEnable, alphaFunc, fogEnable, clipPlanes;
            uint8_t  samplerType[pipelinekeys::kNumSamplers];
        } r;
        memset(&r, 0, sizeof(r));

        r.base = ReplayBaseKey(k);
        r.alphaEnable = RSOr0(k, kRS_AlphaTestEnable);
        r.alphaFunc   = RSOr0(k, kRS_AlphaFunc);
        r.fogEnable   = RSOr0(k, kRS_FogEnable);
        r.clipPlanes  = RSOr0(k, kRS_ClipPlanes) & 0x3Fu;

        // Mask pixel-shader sampler slots to the ones the shader declares. Vertex
        // samplers stay unmasked: there are only four and we do not parse VS
        // sampler declarations.
        const std::array<bool, 16>* use = nullptr;
        if (auto it = psSamplerUse.find(k.psHash); it != psSamplerUse.end()) use = &it->second;
        for (uint32_t i = 0; i < pipelinekeys::kPSSamplers; i++)
            r.samplerType[i] = (!use || (*use)[i]) ? k.samplerType[i] : 0;
        for (uint32_t i = pipelinekeys::kPSSamplers; i < pipelinekeys::kNumSamplers; i++)
            r.samplerType[i] = k.samplerType[i];

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
    // Bytecode carried by the same container as the keys, merged across every cache
    // file we load (local capture, then the shipped baseline).
    static inline pipelinekeys::ShaderBlobMap replayBlobs;

    // Returns false (and leaves replayRecs empty) for any malformed or absent file,
    // so the caller can fall back to the synthetic pass rather than doing nothing.
    static bool LoadKeyFile()
    {
        replayRecs.clear();
        replayDecls.clear();
        replayDeclByHash.clear();
        replayKeyIndex.clear();
        replayBlobs.clear();

        // The user's own capture first: it is the one recorded at THIS graphics
        // configuration, and merging it first means its draw counts drive the
        // most-used-first ordering rather than a stranger's.
        bool any = MergeKeyFile(KeyFilePath(), "capture");
        any |= MergeKeyFile(BaselinePath(), "baseline");

        // Caches from the player's other devices (plugins\pipelinecache\). Capture
        // merges them into the local file too, but only writes that out after its
        // first flush, which can be after this pass -- so read them directly here as
        // well, and the first launch after dropping a file in is already warm.
        const std::string importDir = pipelinekeys::ImportDir();
        WIN32_FIND_DATAA fd{};
        HANDLE fh = importDir.empty() ? INVALID_HANDLE_VALUE
                                      : FindFirstFileA((importDir + "*.bin").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                any |= MergeKeyFile(importDir + fd.cFileName, "import");
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }

        Log("replay: loaded %zu keys, %zu declarations", replayRecs.size(), replayDecls.size());
        return any && !replayRecs.empty();
    }

    // Merge one key file into the replay set. Declaration indices are file-local so
    // they are remapped; a key present in both files is folded into one record with
    // the draw counts summed, which keeps "warm the most-used first" meaningful
    // across a shipped baseline and a local capture.
    static bool MergeKeyFile(const std::string& path, const char* what)
    {
        using namespace pipelinekeys;

        CacheContents c;
        if (!ReadCache(path, c))
        {
            Log("replay: no usable %s cache at %s (absent, or not format v%u)", what, path.c_str(), kCacheVersion);
            return false;
        }

        const size_t recsBefore = replayRecs.size();

        // A file written by a build tracking a different state set cannot be
        // replayed field-for-field, and guessing would be worse than not trying.
        // No migration path by design (see kCacheVersion): reject rather than misread.
        if (c.meta.numRS != kNumRS || c.meta.numSamplers != kNumSamplers ||
            c.rsTypes.size() != kNumRS)
        {
            Log("replay: %s tracks %u states / %u samplers, this build expects %u / %u",
                what, c.meta.numRS, c.meta.numSamplers, kNumRS, kNumSamplers);
            return false;
        }
        for (uint32_t i = 0; i < kNumRS; i++)
            if (c.rsTypes[i] != (uint32_t)kTrackedRS[i].rs)
            {
                Log("replay: state %u is %u in the file but %u here - refusing",
                    i, c.rsTypes[i], (uint32_t)kTrackedRS[i].rs);
                return false;
            }

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

        size_t merged = 0;
        for (auto& rec : c.keys)
        {
            KeyRecord k = rec;
            if (k.declIndex != kDeclNone)
            {
                if (k.declIndex >= remap.size()) continue;   // corrupt index: drop the key, keep the file
                k.declIndex = remap[k.declIndex];
            }

            uint64_t kh = Fnv1a(&k, kKeyHashBytes);
            if (auto it = replayKeyIndex.find(kh); it != replayKeyIndex.end())
            {
                replayRecs[it->second].count += k.count;
                merged++;
            }
            else
            {
                replayKeyIndex.emplace(kh, (uint32_t)replayRecs.size());
                replayRecs.push_back(k);
            }
        }

        // The bytecode arrives in the same file as the keys that name it, so a key set
        // can never be loaded without the shaders needed to replay it.
        uint32_t newShaders = 0;
        for (auto& [hash, blob] : c.shaders)
            if (replayBlobs.emplace(hash, blob).second) newShaders++;
        if (newShaders)
            Log("replay: %s carried %u shader blobs", what, newShaders);
        if (!c.strings[kMetaShaderDir].empty() && c.strings[kMetaShaderDir] != "unknown")
        {
            exportShaderDir = c.strings[kMetaShaderDir];
            exportMeta = c.meta;
            for (uint32_t i = 0; i < kMetaStringCount; i++) exportStrings[i] = c.strings[i];
        }

        Log("replay: %s contributed %zu new keys (%zu already known) from %zu records",
            what, replayRecs.size() - recsBefore, merged, c.keys.size());
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
            if (s->stage == FXC_STAGE_VS) { if (vsHandles[i]) vsByHash.emplace(h, vsHandles[i]); }
            else
            {
                if (psHandles[i]) psByHash.emplace(h, psHandles[i]);
                // Remember which sampler slots this shader declares; the replay key
                // masks the recorded sampler types down to these.
                std::array<bool, 16> use{};
                for (int s2 = 0; s2 < 16; s2++) use[s2] = shaderIO[i].usesSampler[s2];
                psSamplerUse.emplace(h, use);
            }
        }
        size_t fromDb = vsByHash.size() + psByHash.size();

        // Fold in every other shader the process created. FusionFix compiles its own
        // (SMAA, FXAA, sun shafts, gamma, LOD lights) which are not in RAGE's .fxc
        // database, and draws using them were previously skipped outright -- 178 of
        // 5110 pipelines on the first capture.
        for (auto& [h, obj] : pipelinekeys::Registry().vs) vsByHash.emplace(h, obj);
        for (auto& [h, obj] : pipelinekeys::Registry().ps)
        {
            psByHash.emplace(h, obj);
            // Registry shaders have no .fxc entry, so parse their declarations here
            // too -- otherwise their keys keep all 16 sampler slots and over-expand.
            if (psSamplerUse.find(h) == psSamplerUse.end())
            {
                UINT size = 0;
                if (SUCCEEDED(obj->GetFunction(nullptr, &size)) && size)
                {
                    std::vector<uint8_t> code(size);
                    if (SUCCEEDED(obj->GetFunction(code.data(), &size)))
                    {
                        ShaderIO io{};
                        ParseShaderIO(code.data(), size, false, io);
                        std::array<bool, 16> use{};
                        for (int s2 = 0; s2 < 16; s2++) use[s2] = io.usesSampler[s2];
                        psSamplerUse.emplace(h, use);
                    }
                }
            }
        }

        Log("replay: indexed %zu VS + %zu PS by bytecode hash (%zu from the .fxc db, %zu more from the registry)",
            vsByHash.size(), psByHash.size(), fromDb, vsByHash.size() + psByHash.size() - fromDb);
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

        uint32_t madeVS = 0, madePS = 0, failed = 0;
        for (auto& [hash, blob] : replayBlobs)
        {
            const DWORD* fn = reinterpret_cast<const DWORD*>(blob.code.data());
            if (blob.stage == pipelinekeys::kStageVS)
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

                // Same reason as the registry path: without the declared-sampler mask
                // the key keeps all 16 slots and expands into pipelines that do not
                // exist.
                if (psSamplerUse.find(hash) == psSamplerUse.end())
                {
                    ShaderIO io{};
                    ParseShaderIO(blob.code.data(), (uint32_t)blob.code.size(), false, io);
                    std::array<bool, 16> use{};
                    for (int s = 0; s < 16; s++) use[s] = io.usesSampler[s];
                    psSamplerUse.emplace(hash, use);
                }
            }
        }

        Log("replay: created %u VS + %u PS from the cache's bytecode (%zu stored, %u failed)",
            madeVS, madePS, replayBlobs.size(), failed);
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

            if (psSamplerUse.find(h) == psSamplerUse.end())
            {
                ShaderIO io{};
                ParseShaderIO(reinterpret_cast<const uint8_t*>(data), (uint32_t)size, false, io);
                std::array<bool, 16> use{};
                for (int s = 0; s < 16; s++) use[s] = io.usesSampler[s];
                psSamplerUse.emplace(h, use);
            }
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

    // Depth surface matching the RECORDED format. The synthetic pass always
    // preferred INTZ; here the format is part of the key, so honour it.
    static IDirect3DSurface9* ReplayDepthFor(uint32_t fmt)
    {
        if (fmt == 0) return nullptr;
        if (fmt == (uint32_t)FOURCC_INTZ) return scratchDepthINTZ ? scratchDepthINTZ : scratchDepthD24S8;
        return scratchDepthD24S8 ? scratchDepthD24S8 : scratchDepthINTZ;
    }

    // Multisampled scratch targets, cached per {format, type, quality}. Vulkan bakes
    // the sample count into the pipeline, so replaying an MSAA key against a
    // single-sampled target would build a pipeline gameplay never asks for. Only
    // reached when the user runs with ReflectionMSAAQuality set; the common path
    // stays on the plain ScratchFor cache.
    struct ScratchMS { D3DFORMAT fmt; uint32_t type; uint32_t quality; IDirect3DSurface9* surf; };
    static inline std::vector<ScratchMS> scratchMS;
    static inline IDirect3DSurface9* scratchDepthMS = nullptr;
    static inline uint32_t scratchDepthMSType = 0;

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

    // A multisampled colour target needs a depth surface with the SAME sample count.
    static IDirect3DSurface9* ReplayDepthForMS(uint32_t fmt, uint32_t type, uint32_t quality)
    {
        if (fmt == 0) return nullptr;
        if (type == 0) return ReplayDepthFor(fmt);
        if (scratchDepthMS && scratchDepthMSType == type) return scratchDepthMS;
        SAFE_RELEASE(scratchDepthMS);
        if (SUCCEEDED(dev->CreateDepthStencilSurface(kRTdim, kRTdim, D3DFMT_D24S8,
                                                     (D3DMULTISAMPLE_TYPE)type, quality,
                                                     FALSE, &scratchDepthMS, nullptr)))
            scratchDepthMSType = type;
        else
            scratchDepthMS = nullptr;
        return scratchDepthMS;
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
        std::vector<uint32_t> drawList;
        drawList.reserve(replayRecs.size() / 4);
        for (size_t r = 0; r < replayRecs.size(); r++)
        {
            if (seenPipeline.insert(ReplayPipelineKey(replayRecs[r])).second)
                drawList.push_back((uint32_t)r);
            else
                skippedDup++;
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

        // workDone currently holds the create-pass units; everything left is drawing.
        workTotal = workDone + (uint32_t)drawList.size() * kPassW;
        if (workTotal == 0) workTotal = 1;
        Log("replay: %zu unique pipelines to build from %zu keys: %u base identities first "
            "(shaders + vertex input + output state), then %zu spec-constant variants; %u instanced",
            drawList.size(), replayRecs.size(), replayBaseCount,
            drawList.size() - replayBaseCount, instanced);

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

        for (uint32_t r : drawList)
        {
            const KeyRecord& k = replayRecs[r];

            // Shaders. A miss means the capture saw a shader this .fxc database does
            // not contain (a different episode, or a mod) -- skip rather than draw
            // with the wrong one, which would warm a pipeline nothing will use.
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9*  ps = nullptr;
            if (k.vsHash) { auto it = vsByHash.find(k.vsHash); if (it == vsByHash.end()) { missingVS[k.vsHash]++; skippedShader++; workDone += kPassW; continue; } vs = it->second; }
            if (k.psHash) { auto it = psByHash.find(k.psHash); if (it == psByHash.end()) { missingPS[k.psHash]++; skippedShader++; workDone += kPassW; continue; } ps = it->second; }

            // Vertex layout.
            UINT stride[4] = { 0, 0, 0, 0 };
            if (k.declIndex != kDeclNone)
            {
                auto decl = ReplayDecl(k.declIndex);
                if (!decl) { skippedDecl++; workDone += kPassW; continue; }
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

            // Render targets, in the recorded formats -- this is the field the
            // synthetic pass got most wrong, and MRT count is part of the key.
            IDirect3DSurface9* rt0 = nullptr;
            for (uint32_t i = 0; i < kMaxRT; i++)
            {
                IDirect3DSurface9* surf = k.rtFmt[i]
                    ? ScratchForMS((D3DFORMAT)k.rtFmt[i], k.msType, k.msQuality)
                    : nullptr;
                if (i == 0) rt0 = surf;
                dev->SetRenderTarget(i, surf);
            }
            if (!rt0) { skippedRT++; workDone += kPassW; continue; }
            dev->SetDepthStencilSurface(ReplayDepthForMS(k.dsFmt, k.msType, k.msQuality));

            D3DVIEWPORT9 vp{ 0, 0, kRTdim, kRTdim, 0.0f, 1.0f };
            dev->SetViewport(&vp);

            for (uint32_t i = 0; i < kNumRS; i++)
                dev->SetRenderState(kTrackedRS[i].rs, k.rs[i]);

            // Sampler dimensions are folded into DXVK's SPIR-V spec constants, so a
            // 2D texture where the game bound a cube is a different pipeline.
            for (uint32_t i = 0; i < kPSSamplers; i++)
            {
                IDirect3DBaseTexture9* t = nullptr;
                switch (k.samplerType[i])
                {
                case kSampler2D:     t = tex2D;   break;
                case kSamplerCube:   t = texCube; break;
                case kSamplerVolume: t = texVol;  break;
                default:             t = nullptr; break;
                }
                dev->SetTexture(i, t);
            }
            for (uint32_t i = 0; i < kVSSamplers; i++)
            {
                IDirect3DBaseTexture9* t = nullptr;
                switch (k.samplerType[kPSSamplers + i])
                {
                case kSampler2D:     t = tex2D;   break;
                case kSamplerCube:   t = texCube; break;
                case kSamplerVolume: t = texVol;  break;
                default:             t = nullptr; break;
                }
                dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, t);
            }

            // One degenerate primitive: the vertex buffer is zeroed, so nothing is
            // rasterised, but the pipeline is created -- which is the whole point.
            dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)k.primType, 0, 0, 3, 0, 1);
            drawn++;

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

            if (cfg.budgetSeconds > 0 && elapsedMs() > (long long)cfg.budgetSeconds * 1000)
            {
                Log("replay: hit the %ds budget after %u of %zu pipelines - the most-used ones are warm",
                    cfg.budgetSeconds, drawn, drawList.size());
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

        // If we stopped early the bar must still reach 100%, or it reads as a hang.
        if (exitedEarly) workDone = workTotal;

        Log("replay: drew %u of %zu pipelines in %llds%s (skipped %u duplicate-state, %u no-shader, %u no-decl, %u no-RT)",
            drawn, drawList.size(), (long long)(elapsedMs() / 1000), exitedEarly ? " [stopped early]" : "",
            skippedDup, skippedShader, skippedDecl, skippedRT);

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
    }

    // -------------------------------------------------------------------
    //  Completion gate (W4) — drain the GPU so nothing compiles in gameplay.
    // -------------------------------------------------------------------
    static void WaitUntilIdle()
    {
        IDirect3DQuery9* q = nullptr;
        if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q)
        {
            q->Issue(D3DISSUE_END);
            for (int spin = 0; spin < 200000; spin++)
            {
                if (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK) break;
                PresentOverlay(false);
            }
            q->Release();
        }
        // Settle: a few extra presents so the DXVK worker queue (background GPL
        // "optimized" upgrades under the default Auto) drains before gameplay.
        for (int i = 0; i < 6; i++) { PresentOverlay(true); }
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
        SAFE_RELEASE(scratchDepthMS);
        scratchDepthMSType = 0;
        SAFE_RELEASE(dummyVB);
        SAFE_RELEASE(dummyIB);
        SAFE_RELEASE(tex2D);
        SAFE_RELEASE(texCube);
        SAFE_RELEASE(texVol);
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
        tLastPresent = tStart - std::chrono::milliseconds(1000);

        dev = AcquireDevice();
        if (!dev) { Log("no device — aborting precompile"); return; }
        dev->AddRef();
        DetectBackend();
        Log("backend = %s, device = %p", backend == Backend::DXVK ? "DXVK" : "native", (void*)dev);
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
        PresentOverlay(true);

        fxc_db* db = fxc_load_all(ResolveShaderDir().c_str());
        if (!db) { Log("fxc_load_all failed: %s", fxc_last_error()); }
        else
        {
            fxc_stats st{}; fxc_get_stats(db, &st);
            Log("parsed %u effects, %u unique shaders, %u passes (errors %u)",
                st.effect_count, st.unique_total, st.pass_count, st.parse_errors);

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
                IndexShadersFromBlobs();
                IndexShadersFromOwnResources();
                ReplayPass();
            }
            else
            {
                DummyDrawPass(db);
            }
            WaitUntilIdle();
            fxc_free(db);
        }
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
        unsigned devPresents = pipelinekeys::DevicePresentCount() - presentsAtStart;
        if (devPresents > overlayFrames + 2)
            Log("FLICKER SOURCE: device presented %u times during the pass but the overlay only "
                "drew %u of them - %u frames came from elsewhere",
                devPresents, overlayFrames, devPresents - overlayFrames);
        else
            Log("presents accounted for: %u device presents vs %u overlay frames - nothing else "
                "is presenting during the pass", devPresents, overlayFrames);
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

    // ---- injection anchor: FUN_005cc760 (render-thread loadscreen render) -----
    static void __cdecl LoadscreenRenderDetour()
    {
        if (cfg.enabled && !finished.load() && !started.load())
        {
            loadscreenFramesSeen++;
            if (GateReady())
            {
                started = true;   // guard against any re-entry
                __try { RunBlocking(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("precompile faulted - continuing to game"); }
                finished = true;
                Log("gate: ran after %d loadscreen frames at %ux%u", loadscreenFramesSeen, gateW, gateH);
            }
        }
        shLoadscreenRender.call<void>();

        // After the original, so we overwrite what it just drew.
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
        };
    }
} ShaderPrecompiler;
