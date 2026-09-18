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
    bool    adaptiveExit  = true;   // PrecompileAdaptiveExit: stop if the driver is not actually compiling
    // No time cap by default: the whole point is to finish the job. A pipeline left
    // unwarmed is a stutter during gameplay, which is far worse than a longer load.
    // Set a positive value only if you specifically want loading bounded.
    int     budgetSeconds = 0;      // PrecompileBudgetSeconds: 0 = run until everything is warm
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

    // -------------------------------------------------------------------
    static void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        OutputDebugStringA("[ShaderPrecompile] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
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
        struct { void* vb; UINT offset, stride; } stream[4]{};
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
                "%u samplers, shaders, streams and targets",
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
        // A DXVK device answers QueryInterface for ID3D9VkExtInterface.
        IUnknown* p = nullptr;
        if (SUCCEEDED(dev->QueryInterface(FusionFix_IID_ID3D9VkExtInterface, (void**)&p)) && p)
        {
            backend = Backend::DXVK;
            p->Release();
            return;
        }
        // Cross-check the adapter description.
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
        {
            D3DDEVICE_CREATION_PARAMETERS cp{};
            D3DADAPTER_IDENTIFIER9 ai{};
            if (SUCCEEDED(dev->GetCreationParameters(&cp)) &&
                SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &ai)))
            {
                if (strstr(ai.Description, "DXVK") || strstr(ai.Description, "RADV") ||
                    strstr(ai.Description, "llvmpipe") || strstr(ai.Driver, "dxvk"))
                    backend = Backend::DXVK;
            }
            d3d->Release();
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

    // Present a progress frame (throttled). frac in [0,1].
    static void PresentOverlay(bool force)
    {
        // Always pump — keeps the window alive even on throttled (skipped) frames.
        PumpMessages();

        auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - tLastPresent).count() < 33)
            return;
        tLastPresent = now;

        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

        IDirect3DSurface9* prevRT = nullptr; IDirect3DSurface9* prevDS = nullptr;
        dev->GetRenderTarget(0, &prevRT);
        dev->GetDepthStencilSurface(&prevDS);
        dev->SetRenderTarget(0, bb);
        dev->SetDepthStencilSurface(nullptr);
        for (int i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);

        // Restore the loading-screen art, else a dark backdrop.
        if (backdrop) dev->StretchRect(backdrop, nullptr, bb, nullptr, D3DTEXF_NONE);
        else          dev->Clear(0, nullptr, D3DCLEAR_TARGET, HdrScale(0.02f, 0.02f, 0.03f, 1.0f), 1.0f, 0);

        float frac = (float)workDone.load() / (float)(workTotal ? workTotal : 1);
        frac = std::clamp(frac, 0.0f, 1.0f);

        D3DVIEWPORT9 full{ 0, 0, bbW, bbH, 0.0f, 1.0f };
        dev->SetViewport(&full);

        if (dev->BeginScene() == D3D_OK)
        {
            float mx = bbW * 0.12f, barW = bbW * 0.76f;
            float by = bbH * 0.86f, barH = 14.0f;
            // track + fill + thin frame
            DrawOverlayQuad(mx - 2, by - 2, mx + barW + 2, by + barH + 2, HdrScale(0.0f, 0.0f, 0.0f, 0.55f));
            DrawOverlayQuad(mx, by, mx + barW, by + barH, HdrScale(0.10f, 0.10f, 0.12f, 0.85f));
            DrawOverlayQuad(mx, by, mx + barW * frac, by + barH, HdrScale(0.55f, 0.78f, 1.0f, 1.0f));

            if (font && fontBig)
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
                RECT rTitle{ (LONG)mx, (LONG)(by - 78), (LONG)(mx + barW), (LONG)(by - 40) };
                fontBig->DrawTextA(nullptr, line, -1, &rTitle, DT_LEFT | DT_NOCLIP, HdrScale(1, 1, 1, 1));

                RECT rSub{ (LONG)mx, (LONG)(by - 34), (LONG)(mx + barW), (LONG)(by - 6) };
                std::string sub = curLabel.empty() ? std::string("Preparing shaders...") : ("Building: " + curLabel);
                font->DrawTextA(nullptr, sub.c_str(), -1, &rSub, DT_LEFT | DT_NOCLIP, HdrScale(0.8f, 0.85f, 0.95f, 1));
            }
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

    // The fields that actually select a distinct pipeline object.
    //
    // Measured 2026-09-18: replaying 6994 strict keys produced 634 Vulkan
    // pipelines, and 226 new keys during play produced 29 new pipelines -- our key
    // was ~10x more granular than the driver's, and that excess was most of the 16s
    // stall. Everything dropped here is either set dynamically (colour-write masks,
    // cull mode, depth/stencil ops on any driver with extended dynamic state) or
    // configures fixed-function stages these shader-based draws never run.
    //
    // Deliberately KEPT even though this driver may treat them dynamically: vertex
    // declaration and topology. They are classic pipeline state, and a driver
    // without dynamic vertex input -- plausibly the Windows target -- will bake
    // them. Over-warming there costs a few draws; under-warming costs a stutter.
    static uint64_t ReplayPipelineKey(const pipelinekeys::KeyRecord& k)
    {
        struct Reduced
        {
            uint64_t vs, ps;
            uint32_t decl, fvf, prim;
            uint32_t rt[pipelinekeys::kMaxRT], ds, ms, msq;
            uint32_t alphaEnable, alphaFunc, fogEnable;
            uint8_t  samplerType[pipelinekeys::kNumSamplers];
        } r{};

        r.vs = k.vsHash; r.ps = k.psHash;
        r.decl = k.declIndex; r.fvf = k.fvf; r.prim = k.primType;
        for (uint32_t i = 0; i < pipelinekeys::kMaxRT; i++) r.rt[i] = k.rtFmt[i];
        r.ds = k.dsFmt; r.ms = k.msType; r.msq = k.msQuality;
        if (kRS_AlphaTestEnable >= 0) r.alphaEnable = k.rs[kRS_AlphaTestEnable];
        if (kRS_AlphaFunc >= 0)       r.alphaFunc   = k.rs[kRS_AlphaFunc];
        if (kRS_FogEnable >= 0)       r.fogEnable   = k.rs[kRS_FogEnable];

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
    static std::string KeyFilePath()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(hSelf, path, MAX_PATH);
        std::string s(path);
        auto slash = s.find_last_of("\\/");
        std::string dir = (slash == std::string::npos) ? std::string() : s.substr(0, slash + 1);

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

        std::string bundle = dir + pipelinekeys::BundleName(w, h, fmt, msaa, "bin");
        if (FILE* f = fopen(bundle.c_str(), "rb")) { fclose(f); return bundle; }

        // Fall back to the pre-bundle name so an existing cache still works.
        return dir + pipelinekeys::LegacyBundleName("bin");
    }

    // Returns false (and leaves replayRecs empty) for any malformed or absent file,
    // so the caller can fall back to the synthetic pass rather than doing nothing.
    static bool LoadKeyFile()
    {
        using namespace pipelinekeys;

        replayRecs.clear();
        replayDecls.clear();

        FILE* f = fopen(KeyFilePath().c_str(), "rb");
        if (!f) { Log("replay: no key file at %s", KeyFilePath().c_str()); return false; }

        FileHeader h{};
        if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return false; }
        if (h.magic != kMagic) { Log("replay: bad magic 0x%08X", h.magic); fclose(f); return false; }
        // v1 predates the multisample fields; every v1 capture was taken with MSAA
        // off, so it migrates rather than being thrown away.
        if (h.version != kVersion && h.version != kVersionV1)
        {
            Log("replay: file is v%u, this build reads v%u and v%u", h.version, kVersion, kVersionV1);
            fclose(f);
            return false;
        }
        const bool isV1 = (h.version == kVersionV1);
        // A file written by a build tracking a different state set cannot be
        // replayed field-for-field, and guessing would be worse than not trying.
        if (h.numRS != kNumRS || h.numSamplers != kNumSamplers)
        {
            Log("replay: file tracks %u states / %u samplers, this build expects %u / %u",
                h.numRS, h.numSamplers, kNumRS, kNumSamplers);
            fclose(f);
            return false;
        }

        std::vector<uint32_t> rsTypes(h.numRS);
        if (fread(rsTypes.data(), sizeof(uint32_t), h.numRS, f) != h.numRS) { fclose(f); return false; }
        for (uint32_t i = 0; i < h.numRS; i++)
            if (rsTypes[i] != (uint32_t)kTrackedRS[i].rs)
            {
                Log("replay: state %u is %u in the file but %u here - refusing", i, rsTypes[i], (uint32_t)kTrackedRS[i].rs);
                fclose(f);
                return false;
            }

        replayDecls.resize(h.declCount);
        for (uint32_t i = 0; i < h.declCount; i++)
        {
            uint32_t n = 0;
            if (fread(&n, sizeof(n), 1, f) != 1 || n == 0 || n > MAXD3DDECLLENGTH + 1) { fclose(f); return false; }
            replayDecls[i].resize(n);
            if (fread(replayDecls[i].data(), sizeof(D3DVERTEXELEMENT9), n, f) != n) { fclose(f); return false; }
        }

        replayRecs.resize(h.recCount);
        size_t got = 0;
        if (isV1)
        {
            for (uint32_t i = 0; i < h.recCount; i++)
            {
                KeyRecordV1 v1{};
                if (fread(&v1, sizeof(v1), 1, f) != 1) break;
                MigrateV1(v1, replayRecs[i]);
                got++;
            }
        }
        else if (h.recCount)
        {
            got = fread(replayRecs.data(), sizeof(KeyRecord), h.recCount, f);
        }
        fclose(f);

        // A capture flushed while the game was killed can be short; keep what is
        // whole rather than discarding a useful file over its last record.
        if (got != h.recCount)
        {
            Log("replay: file claims %u records, read %zu - using the complete ones", h.recCount, got);
            replayRecs.resize(got);
        }

        Log("replay: loaded %zu keys, %zu declarations", replayRecs.size(), replayDecls.size());
        return !replayRecs.empty();
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

    static void ReplayPass()
    {
        using namespace pipelinekeys;

        uint32_t drawn = 0, skippedShader = 0, skippedDecl = 0, skippedRT = 0, skippedDup = 0;

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

        // workDone currently holds the create-pass units; everything left is drawing.
        workTotal = workDone + (uint32_t)drawList.size() * kPassW;
        if (workTotal == 0) workTotal = 1;
        Log("replay: %zu unique pipelines to build from %zu keys (ordered by use)",
            drawList.size(), replayRecs.size());

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

        for (uint32_t r : drawList)
        {
            const KeyRecord& k = replayRecs[r];

            // Shaders. A miss means the capture saw a shader this .fxc database does
            // not contain (a different episode, or a mod) -- skip rather than draw
            // with the wrong one, which would warm a pipeline nothing will use.
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9*  ps = nullptr;
            if (k.vsHash) { auto it = vsByHash.find(k.vsHash); if (it == vsByHash.end()) { skippedShader++; workDone += kPassW; continue; } vs = it->second; }
            if (k.psHash) { auto it = psByHash.find(k.psHash); if (it == psByHash.end()) { skippedShader++; workDone += kPassW; continue; } ps = it->second; }

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

            workDone += kPassW;
            if ((r & 15) == 0)
                curLabel = "pipeline " + std::to_string(r + 1) + " / " + std::to_string(replayRecs.size());
            PresentOverlay(false);
        }

        // Unbind, so nothing below inherits a scratch target or dummy stream.
        for (uint32_t i = 1; i < kMaxRT; i++) dev->SetRenderTarget(i, nullptr);
        for (UINT s = 0; s < 4; s++) dev->SetStreamSource(s, nullptr, 0, 0);
        dev->SetIndices(nullptr);
        for (uint32_t i = 0; i < kPSSamplers; i++) dev->SetTexture(i, nullptr);
        for (uint32_t i = 0; i < kVSSamplers; i++) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + i, nullptr);

        // If we stopped early the bar must still reach 100%, or it reads as a hang.
        if (exitedEarly) workDone = workTotal;

        Log("replay: drew %u of %zu pipelines in %llds%s (skipped %u duplicate-state, %u no-shader, %u no-decl, %u no-RT)",
            drawn, drawList.size(), (long long)(elapsedMs() / 1000), exitedEarly ? " [stopped early]" : "",
            skippedDup, skippedShader, skippedDecl, skippedRT);
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
    static std::string ResolveShaderDir()
    {
        // <gameroot>/update/common/shaders/win32_30 (the effective, installed set;
        // update/ overrides common/). Derive gameroot from GTAIV.exe.
        auto exe = GetModulePath<std::filesystem::path>(GetModuleHandleW(nullptr));
        auto root = exe.parent_path();
        auto upd = root / "update" / "common" / "shaders" / "win32_30";
        std::error_code ec;
        if (std::filesystem::exists(upd, ec)) return upd.string();
        auto base = root / "common" / "shaders" / "win32_30";
        return base.string();
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
        Log("precompile complete in %.1fs (%u overlay frames presented, %.1f/s)",
            secs, overlayFrames, secs > 0.0 ? overlayFrames / secs : 0.0);
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
        cfg.adaptiveExit = ini.ReadInteger("SHADERS", "PrecompileAdaptiveExit", 1) != 0;
        cfg.budgetSeconds = ini.ReadInteger("SHADERS", "PrecompileBudgetSeconds", 0);
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
