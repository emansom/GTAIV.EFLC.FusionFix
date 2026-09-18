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
#include "fxc_parse.h"
#include "dxvk_d3d9_interfaces.h"

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

    // Draw a progress frame (throttled). When doPresent is false the frame is drawn
    // onto the current back buffer and left there for the GAME to present, which is
    // what the incremental stepper wants: our own Present() from inside a blocking
    // loadscreen call never reached the compositor (the bar appeared stuck).
    static void PresentOverlay(bool force, bool doPresent = true)
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

        if (doPresent) dev->Present(nullptr, nullptr, nullptr, nullptr);

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
            PumpMessages();        // overlay is drawn once per slice by RunSlice
        }
        Log("created %u shaders", n);
    }

    // -------------------------------------------------------------------
    //  Dummy-draw pass (W3b) — walk real passes, reproduce DXVK pipeline keys.
    // -------------------------------------------------------------------
    // Resumable section (A): walk the real (vs,ps) passes. Returns true when the walk
    // is complete; otherwise stores its cursor and returns false so the caller can hand
    // the frame back to the game.
    static bool StepPasses(const std::chrono::steady_clock::time_point& tSlice)
    {
        if (cfg.breadth <= 0) return true;
        fxc_db* db = stepDb;
        if (!db) return true;

        // Re-applied every slice: the game renders between slices, so these dynamic
        // (non-baked) defaults cannot be assumed to have survived.
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
        // Resume where the previous slice stopped. Local copies, because the cursor
        // globals are rewritten the moment we run out of budget.
        const uint32_t e0 = stepEff, t0 = stepTech, p0 = stepPass;
        uint32_t nEff = fxc_effect_count(db);
        for (uint32_t e = e0; e < nEff; e++)
        {
            const fxc_effect* ef = fxc_get_effect(db, e);
            if (!ef) continue;
            for (uint32_t t = (e == e0 ? t0 : 0); t < ef->technique_count; t++)
            {
                const fxc_technique& tech = ef->techniques[t];
                for (uint32_t p = (e == e0 && t == t0 ? p0 : 0); p < tech.pass_count; p++)
                {
                    if (SliceBudgetExpired(tSlice))
                    {
                        stepEff = e; stepTech = t; stepPass = p;
                        return false;       // resume here next loading-screen frame
                    }
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
                    // Pump only. The overlay is drawn once per slice by RunSlice and
                    // presented by the GAME; presenting from inside this loop is what
                    // used to leave the window unresponsive with a stale bar.
                    PumpMessages();
                }
            }
        }
        return true;    // whole walk complete
    }

    // Section (B): state-library + present-format coverage. Small and bounded
    // (kTupleCount + 2 draws), so it runs as one final slice rather than resumably.
    static void TupleAndPresentWarm()
    {
        if (cfg.breadth <= 0) return;

        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        RECT scB{ 0, 0, 1, 1 }; dev->SetScissorRect(&scB);

        const BlendConfig* bOpaque = FindBlend("BL00");

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
                PumpMessages();
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
                PumpMessages();
            }
            q->Release();
        }
        // Settle: give the DXVK worker queue (background GPL "optimized" upgrades
        // under the default Auto) a moment to drain before gameplay. We no longer
        // present our own frames here — the game owns Present now — so just pump.
        for (int i = 0; i < 6; i++) { PumpMessages(); }
    }

    static void ReleaseResources()
    {
        for (auto& s : scratchColor) SAFE_RELEASE(s.surf);
        scratchColor.clear();
        SAFE_RELEASE(scratchDepthD24S8);
        SAFE_RELEASE(scratchDepthINTZ);
        if (scratchDepthINTZTex) { scratchDepthINTZTex->Release(); scratchDepthINTZTex = nullptr; }
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

    // ---- incremental stepper -------------------------------------------------
    //
    // The pass used to do all ~115s of work inside ONE loadscreen-render call. That
    // blocks the game's render thread outright: the window stops being serviced, and
    // the progress frames we presented ourselves never reached the compositor, so the
    // bar appeared frozen (observed live: "jumps from 0% to 100%"). Users reasonably
    // read that as a hang.
    //
    // Instead, do a small slice of work per loading-screen frame and RETURN, letting
    // the game finish and present its own frame with our overlay drawn on top. The
    // device is shared with the game between slices, so every slice snapshots and
    // restores full device state.
    enum class Phase : int { Idle, Passes, Finish, Done };
    static inline Phase phase = Phase::Idle;
    static inline fxc_db* stepDb = nullptr;
    static inline uint32_t stepEff = 0, stepTech = 0, stepPass = 0;
    static constexpr int kSliceMs = 20;     // work budget per loading-screen frame

    // Per-slice device-state guard. Non-copyable; restores on scope exit.
    struct SliceState
    {
        IDirect3DStateBlock9* sb = nullptr;
        IDirect3DSurface9 *rt0 = nullptr, *rt1 = nullptr, *rt2 = nullptr, *rt3 = nullptr, *ds = nullptr;
        D3DVIEWPORT9 vp{};
        bool ok = false;

        bool Begin()
        {
            if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb)
            {
                Log("CreateStateBlock failed — aborting precompile rather than corrupt device state");
                return false;
            }
            dev->GetRenderTarget(0, &rt0);
            dev->GetRenderTarget(1, &rt1);
            dev->GetRenderTarget(2, &rt2);
            dev->GetRenderTarget(3, &rt3);
            dev->GetDepthStencilSurface(&ds);
            dev->GetViewport(&vp);
            ok = true;
            return true;
        }

        ~SliceState()
        {
            if (!ok) { if (sb) sb->Release(); return; }
            dev->SetRenderTarget(0, rt0);
            dev->SetRenderTarget(1, rt1);
            dev->SetRenderTarget(2, rt2);
            dev->SetRenderTarget(3, rt3);
            dev->SetDepthStencilSurface(ds);
            dev->SetViewport(&vp);
            SAFE_RELEASE(rt0); SAFE_RELEASE(rt1); SAFE_RELEASE(rt2); SAFE_RELEASE(rt3);
            SAFE_RELEASE(ds);
            if (sb) { sb->Apply(); sb->Release(); }
        }
    };

    static bool SliceBudgetExpired(const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count() >= kSliceMs;
    }

    // One-time setup: device, shader DB, resources, and the (fast) shader creates.
    static bool StepInit()
    {
        tStart = std::chrono::steady_clock::now();
        tLastPresent = tStart - std::chrono::milliseconds(1000);

        dev = AcquireDevice();
        if (!dev) { Log("no device — aborting precompile"); return false; }
        dev->AddRef();
        DetectBackend();
        Log("backend = %s, device = %p", backend == Backend::DXVK ? "DXVK" : "native", (void*)dev);

        // A slice must not run inside the game's BeginScene (CreateStateBlock is
        // illegal there). Probe once and record it; the detour calls us after the
        // game's own loadscreen render, so we expect to be outside a scene.
        HRESULT hrProbe = dev->BeginScene();
        if (SUCCEEDED(hrProbe)) dev->EndScene();
        else Log("note: loadscreen hook runs INSIDE the game's scene");

        CreateOverlay();

        stepDb = fxc_load_all(ResolveShaderDir().c_str());
        if (!stepDb) { Log("fxc_load_all failed: %s", fxc_last_error()); return false; }

        fxc_stats st{}; fxc_get_stats(stepDb, &st);
        Log("parsed %u effects, %u unique shaders, %u passes (errors %u)",
            st.effect_count, st.unique_total, st.pass_count, st.parse_errors);

        // Exact work units so the bar tracks real progress and reaches 100%.
        uint32_t passUnits  = (cfg.breadth > 0) ? st.pass_count : 0;
        uint32_t tupleUnits = (cfg.breadth > 0) ? (uint32_t)kTupleCount + (cfg.coverPresent ? 2u : 0u) : 0;
        workTotal = st.unique_total * kCreateW + (passUnits + tupleUnits) * kPassW;
        if (workTotal == 0) workTotal = 1;
        workDone = 0;

        CreateResources();
        CreateAllShaders(stepDb);       // fast (<1s measured); fine within one slice
        stepEff = stepTech = stepPass = 0;
        return true;
    }

    // Final slice: state-library + present-format coverage, then drain the GPU.
    static void StepFinish()
    {
        TupleAndPresentWarm();
        WaitUntilIdle();
        ReleaseResources();
        if (stepDb) { fxc_free(stepDb); stepDb = nullptr; }

        auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tStart).count() / 1000.0;
        Log("precompile complete in %.1fs", secs);
        if (dev) { dev->Release(); dev = nullptr; }
    }

    // Called once per loading-screen frame, AFTER the game has drawn its loading
    // screen. Does a bounded amount of work, leaves the overlay on the back buffer
    // for the game to present, and returns so the frame can complete.
    static void RunSlice()
    {
        auto t0 = std::chrono::steady_clock::now();

        if (phase == Phase::Idle)
        {
            if (!StepInit()) { phase = Phase::Done; finished = true; return; }
            phase = Phase::Passes;
        }

        {
            SliceState guard;               // snapshot + restore around this slice
            if (!guard.Begin()) { phase = Phase::Done; finished = true; return; }

            if (phase == Phase::Passes && StepPasses(t0))
                phase = Phase::Finish;
            else if (phase == Phase::Finish)
                phase = Phase::Done;

            // Draw the bar onto the back buffer WITHOUT presenting — the game's own
            // Present carries it, which is what keeps the window live.
            PresentOverlay(true, /*doPresent=*/false);
        }   // guard restores device state here

        if (phase == Phase::Done && !finished.load())
        {
            StepFinish();
            finished = true;
        }
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

    // ---- injection anchor: FUN_005cc760 (render-thread loadscreen render) -----
    static void __cdecl LoadscreenRenderDetour()
    {
        // Let the game draw its loading screen FIRST, then put our overlay on top of
        // the finished frame and let the game present it. The old order (work first,
        // game's render second) meant the game painted over our overlay, which is why
        // the pass had to present its own frames — and those never reached the
        // compositor while we held the render thread.
        shLoadscreenRender.call<void>();

        if (finished.load()) return;

        if (phase == Phase::Idle)
        {
            loadscreenFramesSeen++;
            if (!GateReady()) return;
            Log("gate: starting after %d loadscreen frames at %ux%u", loadscreenFramesSeen, gateW, gateH);
            started = true;
        }

        __try { RunSlice(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("precompile faulted - continuing to game");
            phase = Phase::Done;
            finished = true;
        }
    }

    static void ReadConfig()
    {
        CIniReader ini("");
        cfg.enabled      = ini.ReadInteger("SHADERS", "PrecompileShaders", 1) != 0;
        cfg.breadth      = ini.ReadInteger("SHADERS", "PrecompileCoverage", 2);
        cfg.overlay      = ini.ReadInteger("SHADERS", "PrecompileOverlay", 1) != 0;
        cfg.coverSdr     = ini.ReadInteger("SHADERS", "PrecompileCoverSdr", 0) != 0;
        cfg.coverPresent = ini.ReadInteger("SHADERS", "PrecompileCoverPresentModes", 1) != 0;
    }

public:
    ShaderPrecompiler()
    {
        FusionFix::onInitEvent() += []()
        {
            ReadConfig();
            if (!cfg.enabled) { Log("disabled via ini"); return; }

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
