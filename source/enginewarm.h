#pragma once
// ===========================================================================
//  enginewarm.h — enumerate, from RAGE's OWN live tables, the pipeline
//  coordinates gameplay can produce, so the loading-screen pass can warm them
//  without ever having played.
//
//  WHY THIS EXISTS. The recorded-key replay (shaderprecompile.ixx ReplayPass)
//  can only warm what someone already drew. Driving every effect PASS instead
//  removes that dependency for the shader half, but a Vulkan pipeline under
//  DXVK is keyed on four things a pass does NOT determine:
//
//      * the render-target / depth formats and sample count  -> render phase
//      * the blend + colour-write half of the render state   -> render phase
//      * the vertex declaration                              -> model geometry
//      * the sampler-binding pattern                         -> material
//
//  All four are enumerable from the engine AT A LATE ENOUGH GATE, and this
//  header is that enumeration. The evidence is in
//  re/rage-shader-precompile/{06-phases,07-geometry,08-boot-gate,
//  09-phase-contexts,10-key-spec}.md; every address below is named there and
//  every one is validated at run time before it is used.
//
//  WHAT CHANGED, and it is the whole point of this revision: the contexts are
//  no longer walked out of the render-phase singleton at whatever moment the
//  first loading screen happens to render. bootgate.h puts the pass at the
//  return of rageBoot_InitSession, asks the engine to build its phases there,
//  and hands this header the phase sets and the texture factory's whole
//  render-target registry. What used to come from a shipped constant table --
//  12 of 14 render-target sets and nearly all the phase state -- now comes
//  from the engine, and the three sets that still cannot are logged as
//  'shipped' rather than passed off as engine reads.
//
//  THE TIMING RULE. Everything in 0x00401000-0x004FB000 is SecuROM-encrypted
//  at rest, so nothing here may be touched at ASI load. BuildTables() must be
//  called from the gate, where 04-poc proved the region is plaintext. Nothing
//  in this header is a static initializer for that reason.
//
//  WHAT IT DOES NOT DO. It reads. It never writes an engine global, never
//  calls into the encrypted range, and never touches RAGE's device wrapper at
//  0x017ED8D8 -- the draw loop runs on DXVK's real device exactly as the
//  existing pass does, which is what keeps the wrapper's render-state shadow
//  at 0x017ED9A8 coherent for free (README.md §4).
// ===========================================================================

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include "fxc_parse.h"
#include "pipelinekeys.h"
#include "bootgate.h"

namespace enginewarm
{
    // ---------------------------------------------------------------------
    //  Addresses (GTA IV 1.2.0.59, image base 0x400000). Every one is
    //  validated by Resolve() before anything reads through it; on any
    //  mismatch the whole feature turns itself off and says why once.
    // ---------------------------------------------------------------------
    constexpr uintptr_t kVA_Effects        = 0x017F5638;  // grcEffect*[128]
    constexpr uintptr_t kVA_EffectExtra    = 0x018DD508;  // the gta_im outside the array
    constexpr uintptr_t kVA_RageStateToRS  = 0x01111978;  // u32[43] rage state id -> D3DRENDERSTATETYPE
    constexpr uintptr_t kVA_DefaultRS      = 0x0106B420;  // u32[43] engine-wide default block
    // The render targets and the render phases now come from bootgate.h, which
    // reads them at a gate where the engine has actually built them. The
    // 0x0118D7F0 singleton this file used to walk is a TRAP before the first
    // frame: it holds pointers to uninitialised memory, and reading through
    // them faults (09-phase-contexts §1.3).
    constexpr uintptr_t kVA_DeclMapBuckets = 0x018DDE9C;  // node**
    constexpr uintptr_t kVA_DeclMapCount   = 0x018DDEA0;  // u16 bucket count
    constexpr uintptr_t kVA_DeclMapSize    = 0x018DDEA2;  // u16 entries
    constexpr uintptr_t kVA_InstancingDecl = 0x017475DC;  // raw IDirect3DVertexDeclaration9*
    constexpr uintptr_t kVA_GrcStateShadow = 0x017F5848;  // u32[0x25] the live phase state vector

    // Engine struct offsets, all from 01-effects §2 / 06-phases / 07-geometry,
    // each live-verified against the running game before being written here.
    //
    // ONE definition of the program counts, for both walks. 01-effects §2.1 has
    // a pair for each stage: the loader writes the TOTAL at +0x1E/+0x26 when it
    // allocates the array, and increments a running COUNT at +0x1C/+0x24 as each
    // program loads. They agree for a finished effect (measured on this install:
    // 1864 objects + 112 empty slots = 1976 = the sum of either field), and the
    // count is the safer read because it can only ever under-report a partly
    // loaded effect, never walk a slot that was never filled. The engine-shader
    // index in shaderprecompile.ixx reads the same two offsets through these
    // names, so the two walks cannot drift apart again.
    constexpr uint32_t kEff_Name        = 0x00, kEff_Techniques = 0x08, kEff_TechTotal = 0x0E;
    constexpr uint32_t kEff_VertProgs   = 0x18, kEff_VsCount    = 0x1C;
    constexpr uint32_t kEff_FragProgs   = 0x20, kEff_PsCount    = 0x24;
    constexpr uint32_t kEff_NameHash    = 0x2C;
    constexpr uint32_t kTech_Stride     = 0x10, kTech_NameHash  = 0x00;
    constexpr uint32_t kTech_Passes     = 0x08, kTech_PassTotal = 0x0E;
    constexpr uint32_t kPass_Stride     = 0x20, kPass_Vs = 0x00, kPass_Ps = 0x0C;
    constexpr uint32_t kPass_States     = 0x18, kPass_StateCount = 0x1C;
    constexpr uint32_t kProg_Stride     = 0x0C, kProg_D3D = 0x08;
    constexpr uint32_t kDecl_D3D        = 0x00, kDecl_Ref = 0x04, kDecl_Elems = 0x0C;

    constexpr uint32_t kRageStates = 43;

    // 'INTZ' little-endian; the engine creates every depth target as one.
    constexpr uint32_t FOURCC_INTZ_EW = ('I' | ('N' << 8) | ('T' << 16) | ('Z' << 24));

    // The 43-entry rage-state -> D3DRENDERSTATETYPE table, as the exe holds it.
    // Used ONLY as a fingerprint: the live table is what the code reads.
    // The seven 0xFFFFFFFF entries are the rage states that never reach D3D at
    // all (ids 0x0D, 0x0E, 0x1B, 0x20, 0x22-0x24 of 06-phases §3.1). They are
    // 0xFFFFFFFF, not 0 -- read out of the running game, because the first
    // version of this table guessed 0 and the fail-safe correctly refused to
    // run: "matches only 36 of 43 entries".
    static const uint32_t kExpectedStateToRS[kRageStates] = {
        7, 8, 14, 15, 19, 20, 22, 23, 24, 25,
        27, 52, 53, 54, 55, 56, 57, 58, 59, 168,
        190, 191, 192, 171, 209, 206, 207, 208, 0xFFFFFFFFu, 175,
        195, 193, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 185, 186, 187, 188, 189,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
    };

    // Does a rage-state index map to a render state that actually reaches D3D?
    inline bool RSReaches(uint32_t rs) { return rs != 0 && rs != 0xFFFFFFFFu && rs < 256; }

    // RAGE's string hash (FUN_0040ba60 @ 0x0040ba60), reimplemented rather than
    // called: the function lives in the SecuROM range, and this is the same
    // arithmetic FusionFix already carries as hashStringLowercaseFromSeed.
    // Verified against the four hashes 01-effects §2.2 records from the running
    // game -- draw 0x86023e32, unlit_draw 0x68523925, deferred_draw 0xa18eb342,
    // gta_default 0x7b2b443a.
    //
    // It is what makes an effect's and a technique's identity checkable: the
    // objects keep only the hash, so joining a .fxc file to a loaded effect by
    // ARRAY INDEX is an assumption, and this turns it into a test.
    inline uint32_t RageHash(const char* s)
    {
        uint32_t h = 0;
        for (const char* c = s; *c; c++)
        {
            uint32_t ch = (uint8_t)*c;
            if (ch - 'A' <= 25u) ch += 32;
            else if (ch == '\\') ch = '/';
            const uint32_t t = 1025u * (h + ch);
            h = (t >> 6) ^ t;
        }
        return 32769u * ((9u * h) ^ ((9u * h) >> 11));
    }

    // Safe reads: one implementation, in bootgate.h. Nothing here may fault --
    // an enumeration that crashes the loading screen is far worse than one that
    // gives up and says why.
    using bootgate::Rebase;
    using bootgate::Readable;
    using bootgate::Peek;
    using bootgate::PeekName;

    // ---------------------------------------------------------------------
    //  Axis 1 — render-target / depth / sample-count contexts.
    //
    //  READ FROM THE ENGINE, at a gate late enough to have them (bootgate.h):
    //  the texture factory's own registry of every render target it has ever
    //  created is the format axis, and the render phases say which targets are
    //  bound TOGETHER, which is the only source for the MRT sets. The shipped
    //  table below is a fail-safe that should never fire, and when it does the
    //  log names every set it stood in for.
    //
    //  A CONTEXT KIND is the vocabulary the group->context tie speaks. The
    //  names are 10-key-spec.md §5's RT table; what makes them right is that
    //  each one is matched against what the engine actually declared rather
    //  than assumed to exist.
    // ---------------------------------------------------------------------
    enum CtxKind : uint8_t
    {
        kCtx_GBuffer = 0,   // [A8R8G8B8 x3, R16F] + INTZ -- the deferred set
        kCtx_Scene,         // the fp16 scene RT + INTZ
        kCtx_Cascade,       // R32F + INTZ (cascade / warp shadows)
        kCtx_Point,         // G16R16F + INTZ (point shadows)
        kCtx_Height,        // R16F + INTZ (the height map)
        kCtx_Ldr,           // A8R8G8B8 + INTZ
        kCtx_Water,         // two colour, NO depth (the water surface pair)
        kCtx_Scene2,        // fp16 + A8R8G8B8 + INTZ (the UI-layer composite)
        kCtx_Reflect,       // the fp16 scene set with a user clip plane
        kCtx_LdrNoDepth,
        kCtx_Fp16NoDepth,
        kCtx_R32NoDepth,
        kCtx_R16NoDepth,
        kCtx_L8,
        kCtx_G16R16FNoDepth,
        kCtx_Other,
        kCtx_KindCount
    };

    inline const char* KindName(uint8_t k)
    {
        static const char* kNames[kCtx_KindCount] = {
            "gbuffer", "scene", "cascade", "point", "height", "ldr", "water", "scene2",
            "reflect", "ldrnodepth", "fp16nodepth", "r32nodepth", "r16", "l8", "g16r16f", "other"
        };
        return k < kCtx_KindCount ? kNames[k] : "?";
    }

    struct Context
    {
        std::string name;           // for the log: the engine target that produced it
        D3DFORMAT color[4]{};       // D3DFMT_UNKNOWN = slot unbound
        int       mrt = 0;
        D3DFORMAT depth = D3DFMT_UNKNOWN;
        uint32_t  msType = 0, msQuality = 0;
        uint8_t   kind = kCtx_Other;
        bool      clip = false;     // the phase runs with a user clip plane
        bool      fromEngine = false;   // false = shipped stand-in, and the log says so
    };

    // ---------------------------------------------------------------------
    //  Axis 2 — the write-mask / blend-enable / alpha-test vector a render
    //  phase establishes before it emits any bucket draw.
    //
    //  MUCH SMALLER THAN IT LOOKS, and that is a measurement, not a
    //  simplification. With a depth target bound, DXVK 3.1.1 makes cull mode,
    //  front face, depth bias, depth test/write/compare and the WHOLE stencil
    //  block dynamic (dxvk_graphics.cpp:776-804), and there is no DxvkDsInfo in
    //  the tree at all, so depth and stencil cannot fork a pipeline even in a
    //  depth-less context (10-key-spec.md §3, correcting 09-phase-contexts
    //  §4.4). What is left of a phase callback's push is:
    //
    //      COLORWRITEENABLE0..3, ALPHABLENDENABLE, ALPHATESTENABLE
    //
    //  and everything else -- the blend factors, the blend op, the alpha
    //  compare func -- comes from the engine's own default block at
    //  0x0106B420 and then from the PASS's own PASS_VALUE delta, which is read
    //  live. That is why this table has 8 entries where the previous one had
    //  33: the other 25 were blend triples that belong to the pass, not to the
    //  phase, and crossing them emitted pipelines no phase can produce.
    //
    //  The three write-mask vectors are the ones the 15 phase-state callbacks
    //  actually push (09-phase-contexts §4.2), each crossed with blend on/off
    //  and alpha test on/off, because all three sources (callback, default
    //  block, pass delta) reach both values.
    // ---------------------------------------------------------------------
    struct StateVec
    {
        const char* name;
        DWORD cwe[4];           // COLORWRITEENABLE 0..3
        BOOL  blendEnable;      // D3DRS_ALPHABLENDENABLE
        BOOL  alphaTest;        // D3DRS_ALPHATESTENABLE
    };

    static const StateVec kStateVecs[] = {
        // the G-buffer callbacks: ragePhase_State_GBufferStandard 0x00AD2510
        { "gbuf_std",     { 7, 7, 7, 15 }, FALSE, FALSE },
        { "gbuf_alpha",   { 7, 7, 7, 15 }, FALSE, TRUE  },
        // ragePhase_State_GBufferAlphaClip 0x00AD2430
        { "gbuf_clip",    { 7, 0, 3, 15 }, TRUE,  FALSE },
        { "gbuf_clipa",   { 7, 0, 3, 15 }, TRUE,  TRUE  },
        // every forward phase: all channels, and the default block's own
        // ALPHABLENDENABLE = 1 / SRCALPHA / INVSRCALPHA underneath
        { "fwd",          { 15, 15, 15, 15 }, FALSE, TRUE  },
        { "fwd_noalpha",  { 15, 15, 15, 15 }, FALSE, FALSE },
        { "fwd_blend",    { 15, 15, 15, 15 }, TRUE,  TRUE  },
        { "fwd_blend_na", { 15, 15, 15, 15 }, TRUE,  FALSE },
    };
    enum { kSV_GBufStd = 0, kSV_GBufAlpha, kSV_GBufClip, kSV_GBufClipA,
           kSV_Fwd, kSV_FwdNoAlpha, kSV_FwdBlend, kSV_FwdBlendNA, kSV_Count };

    // ---------------------------------------------------------------------
    //  D3D9 -> Vulkan, the pieces of DXVK 3.1.1 that decide the key.
    //
    //  The warm pass has to know what pipeline a job WILL build, not just how
    //  to draw it: that is what makes dedup honest, and it is what the
    //  emission log carries so the containment scorer can be run against this
    //  implementation rather than against a model of it.
    // ---------------------------------------------------------------------
    inline uint32_t VkBlendFactor(uint32_t d3d)
    {
        switch (d3d)
        {
        case 1: return 0;  case 2: return 1;  case 3: return 2;  case 4: return 3;
        case 5: return 4;  case 6: return 5;  case 7: return 6;  case 8: return 7;
        case 9: return 8;  case 10: return 9; case 11: return 14; case 14: return 10;
        case 15: return 11; case 16: return 12; case 17: return 13;
        default: return d3d;
        }
    }
    inline uint32_t VkBlendOp(uint32_t d3d)
    {
        return (d3d >= 1 && d3d <= 5) ? d3d - 1 : 0;
    }
    inline uint32_t VkCompareOp(uint32_t d3d)
    {
        return (d3d >= 1 && d3d <= 8) ? d3d - 1 : 5;
    }

    // DXVK's alpha-test precision is a pure function of the RT0 format
    // (d3d9_device.cpp GetAlphaTestPrecision), so it rides in specialization
    // constant 0 and is TIED to the render-target set, never crossed.
    inline uint32_t AlphaPrecision(D3DFORMAT rt0)
    {
        switch ((uint32_t)rt0)
        {
        case 111: case 112: case 113: return 0x7;   // R16F, G16R16F, A16B16G16R16F
        case 34: case 36: case 60: case 81: case 110: return 0x8;
        case 114: case 115: case 116: return 0xF;   // R32F, G32R32F, A32B32G32R32F
        default: return 0;
        }
    }

    // DxvkBlendMode::normalize (dxvk_constant_state.cpp:69). A blend that
    // cannot change the result is recorded as no blend at all, a zero write
    // mask turns it off, and the factors of a channel nobody writes are
    // zeroed -- so two D3D9 states that differ only in a factor no one reads
    // are ONE pipeline, and one job, not two.
    struct BlendKey
    {
        uint8_t enable, cs, cd, co, sa, da, oa, wm;
        bool operator==(const BlendKey& o) const
        { return memcmp(this, &o, sizeof(o)) == 0; }
    };

    inline BlendKey NormaliseBlend(uint32_t enable, uint32_t src, uint32_t dst, uint32_t op,
                                   uint32_t sep, uint32_t srcA, uint32_t dstA, uint32_t opA,
                                   uint32_t wm)
    {
        uint32_t cs = VkBlendFactor(src), cd = VkBlendFactor(dst), co = VkBlendOp(op);
        uint32_t sa = cs, da = cd, oa = co;
        if (sep) { sa = VkBlendFactor(srcA); da = VkBlendFactor(dstA); oa = VkBlendOp(opA); }
        bool en = enable != 0;
        wm &= 0xF;
        if (!wm) en = false;
        if (en)
        {
            if (co == 0 && cs == 0 && cd == 1) wm &= ~0x7u;
            if (oa == 0 && sa == 0 && da == 1) wm &= ~0x8u;
            bool need = false;
            if (wm & 0x7) need = need || (cs != 1 || cd != 0 || co != 0);
            if (wm & 0x8) need = need || (sa != 1 || da != 0 || oa != 0);
            if (!need) en = false;
        }
        if (!en || !(wm & 0x7)) { cs = cd = co = 0; }
        if (!en || !(wm & 0x8)) { sa = da = oa = 0; }
        BlendKey k{};
        k.enable = (uint8_t)(en ? 1 : 0);
        k.cs = (uint8_t)cs; k.cd = (uint8_t)cd; k.co = (uint8_t)co;
        k.sa = (uint8_t)sa; k.da = (uint8_t)da; k.oa = (uint8_t)oa;
        k.wm = (uint8_t)wm;
        return k;
    }

    // ---------------------------------------------------------------------
    //  Axis 3 — vertex declarations.
    //
    //  07-geometry proved the complete set for an install is three disjoint
    //  sources: 20 formats that live in the player's .wdr/.wdd/.wft archives,
    //  9 engine-internal constants, and whatever the live registry at
    //  0x018DDE9C already holds. The 20 were harvested offline from all 280
    //  archives of THIS install and reproduce 20 of the 26 single-stream
    //  declarations the recording holds with zero waste; they are shipped here
    //  as their element arrays (byte-identical to what the engine builds from
    //  the same grcFvf -- verified 20 of 20 live) rather than re-derived at
    //  launch, because the harvest costs ~5 minutes over 12.5 GB and must
    //  never run at startup. The live-registry union below is what covers an
    //  install whose archives differ from this one.
    //
    //  The weights are recorded draw counts over a full route: they are the
    //  ordering signal, so a budget cut sheds the rarest layouts first.
    // ---------------------------------------------------------------------
    struct DeclSpec
    {
        const D3DVERTEXELEMENT9* elems;
        uint32_t count;
        uint32_t weight;        // recorded draws; ordering only
        const char* origin;
    };

#define EW_E(s, o, t, u, ui) { (WORD)(s), (WORD)(o), (BYTE)(t), 0, (BYTE)(u), (BYTE)(ui) }
    // Archive-derived (20), most-drawn first.
    static const D3DVERTEXELEMENT9 kD00[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD01[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD02[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,44,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD03[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD04[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,44,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,52,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD05[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,44,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1) };
    static const D3DVERTEXELEMENT9 kD06[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,40,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,1) };
    static const D3DVERTEXELEMENT9 kD07[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,40,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,44,D3DDECLTYPE_FLOAT16_4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD08[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,40,D3DDECLTYPE_FLOAT16_4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD09[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,32,D3DDECLTYPE_FLOAT16_4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD10[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD11[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,44,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,2) };
    static const D3DVERTEXELEMENT9 kD12[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,44,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,2), EW_E(0,52,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,3), EW_E(0,60,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,4) };
    static const D3DVERTEXELEMENT9 kD13[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,44,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,2), EW_E(0,52,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,3), EW_E(0,60,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,4), EW_E(0,68,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,5) };
    static const D3DVERTEXELEMENT9 kD14[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,44,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD15[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1) };
    static const D3DVERTEXELEMENT9 kD16[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,16,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD17[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD18[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,32,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,36,D3DDECLTYPE_FLOAT16_4,D3DDECLUSAGE_TANGENT,0) };
    static const D3DVERTEXELEMENT9 kD19[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,32,D3DDECLTYPE_FLOAT16_2,D3DDECLUSAGE_TEXCOORD,1) };
    // Engine-internal (9): blit quads, position-only depth, the two post-fx
    // formats, the flattened instancing base, the instancing declaration, the
    // R2VB pair and the vehicle-damage deform second stream.
    static const D3DVERTEXELEMENT9 kD20[] = { EW_E(0,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITION,0), EW_E(0,16,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD21[] = { EW_E(0,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITIONT,0), EW_E(0,16,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0) };
    static const D3DVERTEXELEMENT9 kD22[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,20,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1) };
    static const D3DVERTEXELEMENT9 kD23[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0) };
    static const D3DVERTEXELEMENT9 kD24[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(1,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,1), EW_E(1,16,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,2), EW_E(1,32,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,3), EW_E(1,48,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,4) };
    static const D3DVERTEXELEMENT9 kD25[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,24,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,28,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,44,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,1), EW_E(0,60,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,2), EW_E(0,76,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,3), EW_E(0,92,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,4) };
    static const D3DVERTEXELEMENT9 kD26[] = { EW_E(0,0,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(1,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITION,0) };
    static const D3DVERTEXELEMENT9 kD27[] = { EW_E(0,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITIONT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0) };
    static const D3DVERTEXELEMENT9 kD28[] = { EW_E(0,0,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_POSITION,0), EW_E(0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDWEIGHT,0), EW_E(0,16,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_BLENDINDICES,0), EW_E(0,20,D3DDECLTYPE_FLOAT3,D3DDECLUSAGE_NORMAL,0), EW_E(0,32,D3DDECLTYPE_D3DCOLOR,D3DDECLUSAGE_COLOR,0), EW_E(0,36,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0), EW_E(0,44,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,0), EW_E(1,0,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITION,1), EW_E(1,16,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_NORMAL,1), EW_E(1,32,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TANGENT,1) };
#undef EW_E

#define EW_D(a, w, o) { a, (uint32_t)(sizeof(a) / sizeof(a[0])), (uint32_t)(w), o }
    static const DeclSpec kShippedDecls[] = {
        EW_D(kD00, 491553162, "archive"), EW_D(kD01, 196943222, "archive"), EW_D(kD02,  56966403, "archive"),
        EW_D(kD03,  37729902, "archive"), EW_D(kD04,  11982433, "archive"), EW_D(kD05,  19316694, "archive"),
        EW_D(kD06,  45136250, "archive"), EW_D(kD07,  28840974, "archive"), EW_D(kD08,  44443014, "archive"),
        EW_D(kD09,  10365748, "archive"), EW_D(kD10,  38768338, "archive"), EW_D(kD11,   6338232, "archive"),
        EW_D(kD12,   8057489, "archive"), EW_D(kD13,   5807126, "archive"), EW_D(kD14,    386867, "archive"),
        EW_D(kD15,    287294, "archive"), EW_D(kD16,   1592893, "archive"), EW_D(kD17,   1163581, "archive"),
        EW_D(kD18,   1151743, "archive"), EW_D(kD19,   2453573, "archive"),
        EW_D(kD20,   1051385, "engine"),  EW_D(kD21,  17235132, "engine"),  EW_D(kD22,   6012015, "engine"),
        EW_D(kD23,  18979763, "engine"),  EW_D(kD24,   4178575, "engine"),  EW_D(kD25,  42176799, "engine"),
        EW_D(kD26,    853314, "engine"),  EW_D(kD27,     18072, "engine"),  EW_D(kD28,    753800, "engine"),
    };
#undef EW_D
    constexpr uint32_t kShippedDeclCount = (uint32_t)(sizeof(kShippedDecls) / sizeof(kShippedDecls[0]));
    constexpr uint32_t kInstancingDeclIdx = 24;   // kD24: the one declaration DrawInstancedBatch binds

    struct Decl
    {
        std::vector<D3DVERTEXELEMENT9> elems;   // without D3DDECL_END
        uint32_t weight = 0;
        const char* origin = "";
        uint32_t stride[4]{};
        bool     instanced = false;             // the one DrawInstancedBatch binds
        IDirect3DVertexDeclaration9* obj = nullptr;
    };

    // ---------------------------------------------------------------------
    //  The tie — which coordinate a pass can actually be drawn at.
    //
    //  Source: ragePhase_EmitRenderListDrawCommands 0x00B1DEE0 ->
    //  ragePhase_EmitBucketDraw 0x00AE2A90 -> the forward (0x00AE38B0) /
    //  deferred (0x00AE2B70) emitters (06-phases §6). phaseFlags == 8 selects
    //  the deferred emitter; the stencil push/pop at 0x00AC68B0 is gated on the
    //  forced group being deferred or deferredalphaclip, which is what binds
    //  the whole gbuf_* half to the G-buffer phase and nothing else.
    //
    //  This is code-derived, and it has NOT been joined against the 14805-key
    //  recording -- see the note in 06-phases' risks. If it is wrong the walk
    //  warms coordinates gameplay never reaches, which costs loading time and
    //  buys nothing; it cannot make gameplay worse.
    // ---------------------------------------------------------------------
    enum GroupClass : uint8_t
    {
        kG_Deferred = 0,     // deferred, imposterdeferred
        kG_DeferredBS,       // deferredbs
        kG_DeferredClip,     // deferredalphaclip
        kG_Forward,          // every OTHER grouped technique: entity geometry
        kG_Named,            // reached only by LookupTechnique: post-fx, lighting, blits, HUD
        kG_Count
    };

    // A technique is GROUPED when its name is "<group>_draw", "_drawskinned" or
    // "_drawblit" -- that suffix is what the technique-group selector matches,
    // and it is the only thing that decides whether a phase's bucket emitter
    // can reach the pass at all. Everything else is reached by name from
    // inside a phase's DrawList: post-FX, the lighting resolve, blits, the HUD.
    //
    // ONE CORRECTION, and it changes which pipelines get warmed: `paraboloid`
    // is the dual-paraboloid REFLECTION group and draws into the fp16 scene
    // set, not into the shadow set 06-phases §6 sent it to -- measured, 76,500
    // draws at A16B16G16R16F/INTZ over a route (09-phase-contexts §6). It is a
    // forward group here, along with `reflection`, and the shadow contexts are
    // reached the way every other forward group reaches them: because a shadow
    // phase binds them.
    inline GroupClass ClassifyTechnique(const char* name)
    {
        if (!name || !*name) return kG_Named;
        const size_t n = strlen(name);
        const char* suffix = nullptr;
        for (const char* s : { "_drawskinned", "_drawblit", "_draw" })
        {
            const size_t sn = strlen(s);
            if (n > sn && strcmp(name + n - sn, s) == 0) { suffix = s; break; }
        }
        // The group literally named "default": its techniques carry no prefix.
        if (!suffix)
        {
            if (strcmp(name, "draw") == 0 || strcmp(name, "drawskinned") == 0 ||
                strcmp(name, "drawblit") == 0 || strcmp(name, "draw_inst") == 0 ||
                strcmp(name, "unlit_draw_inst") == 0)
                return kG_Forward;
            return kG_Named;
        }
        const std::string g(name, n - strlen(suffix));
        if (g == "deferredalphaclip")                    return kG_DeferredClip;
        if (g == "deferredbs")                           return kG_DeferredBS;
        if (g == "deferred" || g == "imposterdeferred")  return kG_Deferred;
        return kG_Forward;
    }

    // ---------------------------------------------------------------------
    //  What Resolve() found, and what BuildPlan() produced.
    // ---------------------------------------------------------------------
    struct PassInfo
    {
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9*  ps = nullptr;
        // The pass's own PASS_VALUE delta, COPIED into Plan::stateWords as
        // (key, value) u32 pairs rather than pointed at. The engine array it
        // came from belongs to a grcEffect, and grcEffect has a destructor
        // (0x00435640) that LoadOrCreateByName can reach while the streamer
        // runs; the walk holds these for minutes, so it holds its own copy.
        uint32_t stateOff = 0;
        uint16_t stateCount = 0;
        // Read from the LIVE shader objects with GetFunction, not joined from a
        // .fxc file by array index: the VS input signature the declaration is
        // projected onto, the sampler slots the PS declares with their texture
        // dimension (0 = not declared), the same for the VS's four vertex
        // samplers, and the bytecode hash of each -- which is the name the
        // containment scorer and every recorded key know a shader by.
        std::vector<std::pair<uint8_t, uint8_t>> sig;
        uint8_t  psDim[16]{};
        uint8_t  vsDim[4]{};
        uint64_t vsHash = 0, psHash = 0;
        uint8_t  group = kG_Named;
        uint16_t effect = 0;                // index into effectNames
    };

    // Job::flags
    enum : uint8_t
    {
        kJF_TopoMask  = 0x03,   // 0 = list, 1 = strip, 2 = fan
        kJF_VSSampler = 0x04,   // bind the VS's declared vertex samplers
        kJF_ClipPlane = 0x08,   // one user clip plane enabled (DXVK spec constant 0)
    };
    // Job::psVariant: 0 = every declared slot bound, 1..16 = that slot left
    // null, 17 = none bound. The material's binding pattern is the residue the
    // render-target axis is not (10-key-spec.md §7): of 271 observed (PS,
    // psSamplerTypes) pairs in a route, 223 bind every declared slot, 32 leave
    // exactly one null and 2 leave all null -- so the family is LINEAR in the
    // slot count and covers 94.8 % of what occurs. Crossing subsets would be
    // exponential and buys the last 5 %.
    constexpr uint8_t kPSV_AllBound = 0, kPSV_NoneBound = 17;

    // NOT an axis, and the evidence for leaving it out, because it looks like
    // one: DXVK's per-sampler MODE (spec ids 4/5, psSamplerModes) can only be
    // non-zero through depthTextureMask or fetch4TextureMask, and this game can
    // set neither. depthTextureMask comes from D3D9CommonTexture::IsShadow,
    // whose DetermineShadowState explicitly BLACKLISTS INTZ, DF16 and DF24
    // (d3d9_common_texture.cpp:514) -- and RAGE creates every depth target as
    // INTZ, which is why TargetFormat reads the texture rather than the rage
    // format byte. fetch4TextureMask needs D3DSAMP_MIPMAPLODBIAS set to the
    // FOURCC 'GET4' (d3d9_device.cpp:4584); that constant occurs nowhere in
    // GTAIV.exe, neither in the 16.4 MB that ship as plaintext nor in the
    // decrypted 0x401000-0x4FB100. So binding a depth texture here would warm a
    // pipeline the engine cannot produce.

    struct Job
    {
        uint32_t pass;
        uint16_t decl;      // index into decls, 0xFFFF = no declaration (FVF-less blit)
        uint8_t  ctx;
        uint8_t  state;
        uint8_t  flags;     // kJF_*
        uint8_t  psVariant; // kPSV_*
        uint32_t rank;      // ascending draw order
    };

    // The pipeline identity a job intends to build, in DXVK 3.1.1's own terms
    // (10-key-spec.md §2). It is the dedup key AND what the emission log
    // carries, so the containment scorer measures what this code emits rather
    // than a model of it.
    struct JobKey
    {
        uint64_t vs, ps;
        uint64_t proj;                  // the declaration projected onto the VS signature
        uint32_t rt[4], ds;
        BlendKey blend[4];
        uint32_t prim;                  // D3DPRIMITIVETYPE
        uint32_t spec0;                 // alpha compare op | precision << 4
        uint32_t psTypes, vsTypes;      // specialization constants 4 and 3
        uint32_t clip;                  // enabled, non-zero user clip planes
        uint32_t freq[4];               // SetStreamSourceFreq per stream
        uint32_t srgb;
    };

    struct Census
    {
        uint32_t effects = 0, techniques = 0, passes = 0, vs = 0, ps = 0;
        uint32_t effectsJoined = 0, techniquesJoined = 0;
        uint32_t techByIndex = 0, techByHash = 0, techUnmatched = 0;
        uint32_t phases = 0, phaseTargets = 0, registryTargets = 0, registryTextured = 0;
        uint32_t contextsFromPhases = 0, contextsFromRegistry = 0, contextsShipped = 0;
        uint32_t declsShipped = 0, declsLive = 0, declsTotal = 0;
        uint32_t liveRegistryEntries = 0;
        uint32_t jobsEmitted = 0, jobsDeduped = 0, jobsCapped = 0;
        uint32_t shaderObjects = 0, shaderRead = 0, shaderFailed = 0, passesDropped = 0;
        uint32_t projectionsPerPass100 = 0;   // mean x100
        uint32_t groupPasses[kG_Count] = {};
        // per-axis multiplicity of the emission, for the log
        uint32_t axisProj = 0, axisRT = 0, axisBlend = 0, axisPrim = 0;
        uint32_t axisSpec0 = 0, axisPSTypes = 0, axisVSTypes = 0, axisClip = 0, axisShaders = 0;
    };

    struct Plan
    {
        bool ok = false;
        std::string why;                 // why it is off, when it is
        std::vector<PassInfo> passes;
        std::vector<Context>  contexts;
        std::vector<Decl>     decls;
        std::vector<Job>      jobs;
        // Parallel to jobs, and kept only when the emission log is on: a
        // JobKey is 120 bytes and there can be a third of a million of them,
        // which is 40 MB of a 32-bit process that already holds GTA IV's heap.
        // The fingerprint below is accumulated as they are emitted instead, so
        // the "has anything changed" test costs nothing.
        bool                  keepKeys = false;
        std::vector<JobKey>   keys;
        uint64_t              enumFingerprint = 1469598103934665603ull;
        std::vector<uint32_t> stateWords;   // (key, value) pairs, PassInfo::stateOff
        std::vector<std::string> effectNames;
        uint32_t stateToRS[kRageStates]{};
        uint32_t defaultRS[kRageStates]{};
        uint32_t liveState[0x25]{};
        uint32_t msType = 0, msQuality = 0; // from FusionFix's own setting, not the engine
        Census   census;
    };

    // ---------------------------------------------------------------------
    //  Resolve + validate every address this needs. One call, at the gate.
    // ---------------------------------------------------------------------
    inline bool ResolveTables(Plan& p)
    {
        // The rage-state -> D3DRS table is the best single fingerprint we have:
        // 43 entries, most of them distinct, and every pass state delta and
        // every phase state vector is expressed through it.
        const uint32_t* tbl = (const uint32_t*)Rebase(kVA_RageStateToRS);
        if (!Readable(tbl, kRageStates * 4)) { p.why = "g_RageStateToD3DRS is not readable"; return false; }
        int match = 0;
        for (uint32_t i = 0; i < kRageStates; i++)
        {
            p.stateToRS[i] = tbl[i];
            if (tbl[i] == kExpectedStateToRS[i]) match++;
        }
        if (match < 40)
        {
            char b[128];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "g_RageStateToD3DRS matches only %d of %u entries - another build", match, kRageStates);
            p.why = b;
            return false;
        }

        const uint32_t* def = (const uint32_t*)Rebase(kVA_DefaultRS);
        if (!Readable(def, kRageStates * 4)) { p.why = "g_DefaultRenderState is not readable"; return false; }
        memcpy(p.defaultRS, def, sizeof(p.defaultRS));

        const uint32_t* live = (const uint32_t*)Rebase(kVA_GrcStateShadow);
        if (Readable(live, sizeof(p.liveState))) memcpy(p.liveState, live, sizeof(p.liveState));
        return true;
    }

    // ---------------------------------------------------------------------
    //  Axis 1 — the contexts, classified.
    // ---------------------------------------------------------------------
    inline bool IsLdr(D3DFORMAT f) { return f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8; }

    inline uint8_t ClassifyContext(const Context& c)
    {
        const bool depth = c.depth != D3DFMT_UNKNOWN;
        if (c.mrt >= 3) return kCtx_GBuffer;
        if (c.mrt == 2) return depth ? kCtx_Scene2 : kCtx_Water;
        if (depth)
        {
            if (c.color[0] == D3DFMT_A16B16G16R16F) return c.clip ? kCtx_Reflect : kCtx_Scene;
            if (c.color[0] == D3DFMT_R32F)          return kCtx_Cascade;
            if (c.color[0] == D3DFMT_G16R16F)       return kCtx_Point;
            if (c.color[0] == D3DFMT_R16F)          return kCtx_Height;
            if (IsLdr(c.color[0]))                  return kCtx_Ldr;
            return kCtx_Other;
        }
        if (IsLdr(c.color[0]))                      return kCtx_LdrNoDepth;
        if (c.color[0] == D3DFMT_A16B16G16R16F)     return kCtx_Fp16NoDepth;
        if (c.color[0] == D3DFMT_R32F)              return kCtx_R32NoDepth;
        if (c.color[0] == D3DFMT_R16F)              return kCtx_R16NoDepth;
        if (c.color[0] == D3DFMT_L8)                return kCtx_L8;
        if (c.color[0] == D3DFMT_G16R16F)           return kCtx_G16R16FNoDepth;
        return kCtx_Other;
    }

    inline bool AddContext(Plan& p, std::unordered_set<uint64_t>& seen, Context c)
    {
        if (c.mrt == 0 || c.color[0] == D3DFMT_UNKNOWN) return false;
        c.kind = ClassifyContext(c);
        uint64_t key = pipelinekeys::Fnv1a(c.color, sizeof(c.color));
        key = pipelinekeys::Fnv1a(&c.depth, sizeof(c.depth), key);
        key = pipelinekeys::Fnv1a(&c.msType, sizeof(c.msType), key);
        const uint32_t clip = c.clip ? 1u : 0u;
        key = pipelinekeys::Fnv1a(&clip, sizeof(clip), key);
        if (!seen.insert(key).second) return false;
        p.contexts.push_back(std::move(c));
        return true;
    }

    // Every context the engine declares at the gate, plus the ones it can only
    // be asked for indirectly.
    //
    //   * The PHASE SETS say which targets are bound TOGETHER. That is the only
    //     source for the G-buffer MRT, the water-surface pair and the fp16 +
    //     A8R8G8B8 composite -- no table and no registry walk can tell you that
    //     four targets share a frame.
    //   * The FACTORY REGISTRY is the whole format axis: 73 named targets in
    //     gameplay, of which only 11 are reachable from any phase's +0x890
    //     block. FullScreenTex_temp1/2, the post-FX chain, SMAA's edgesTex and
    //     blendTex, SSAO's AOTex, CASCADE_ATLAS and script_rt live only there.
    //   * The one thing the engine cannot answer here is the SAMPLE COUNT:
    //     there is no D3D texture behind a target at this gate, and the sample
    //     count is not in the rage struct. It comes from FusionFix's own
    //     ReflectionMSAAQuality, which is the setting that puts one there.
    inline void EnumContexts(Plan& p, const std::vector<bootgate::PhaseSet>& phases,
                             const std::vector<bootgate::Target>& registry,
                             D3DFORMAT backBuffer)
    {
        std::unordered_set<uint64_t> seen;
        std::vector<D3DFORMAT> singleColour;
        D3DFORMAT engineDepth = D3DFMT_UNKNOWN;

        auto noteColour = [&](D3DFORMAT f) {
            if (f == D3DFMT_UNKNOWN) return;
            if (std::find(singleColour.begin(), singleColour.end(), f) == singleColour.end())
                singleColour.push_back(f);
        };
        auto isDepth = [](D3DFORMAT f) {
            return f == (D3DFORMAT)FOURCC_INTZ_EW || f == D3DFMT_D24S8 || f == D3DFMT_D16 ||
                   f == D3DFMT_D24X8 || f == D3DFMT_D32;
        };
        // A target whose phase clips the world against a reflection plane. The
        // mirror and the two reflection phases do, so every draw in them
        // carries a non-zero clipPlaneCount in specialization constant 0 -- 75
        // route keys, all in the reflection group and in named techniques. It
        // is a property of the CONTEXT, not a free axis.
        //
        // NAME MATCHING IS THE WEAK PART and is worth saying plainly: the
        // authoritative source is the phase itself -- 09-phase-contexts
        // identifies the reflection and mirror phases by their BuildRenderList
        // callbacks (0x00D514F0, 0x00D77130) and their phase ids, both readable
        // off the same viewport array the gate already walks. A name test
        // cannot see a clipped phase whose first colour target is named
        // anything else, and it would wrongly clip an unclipped phase that
        // happened to render INTO a reflection map. Until the callback read
        // exists, an over-broad match is the cheaper error: a spurious clipped
        // context costs jobs, a missing one costs pipelines.
        auto clipped = [](const std::string& n) {
            return n.find("REFLECTION") != std::string::npos ||
                   n.find("MIRROR") != std::string::npos;
        };

        // --- the phase sets: which targets are bound together ---------------
        p.census.phases = (uint32_t)phases.size();
        for (const bootgate::PhaseSet& s : phases)
        {
            Context c;
            c.name = s.name;
            c.mrt = s.mrt;
            for (int i = 0; i < 4; i++)
            {
                c.color[i] = s.colour[i].fmt;
                if (c.color[i] != D3DFMT_UNKNOWN) { noteColour(c.color[i]); p.census.phaseTargets++; }
            }
            if (s.hasDepth)
            {
                c.depth = s.depth.fmt;
                if (engineDepth == D3DFMT_UNKNOWN) engineDepth = c.depth;
                p.census.phaseTargets++;
            }
            c.fromEngine = true;
            c.clip = clipped(s.name);
            if (c.msType == 0 && c.clip) { c.msType = p.msType; c.msQuality = p.msQuality; }
            AddContext(p, seen, c);
        }
        p.census.contextsFromPhases = (uint32_t)p.contexts.size();

        // --- the registry: every colour format the engine renders to --------
        p.census.registryTargets = (uint32_t)registry.size();
        for (const bootgate::Target& t : registry)
        {
            if (t.hasTexture) p.census.registryTextured++;
            if (isDepth(t.fmt))
            {
                if (engineDepth == D3DFMT_UNKNOWN) engineDepth = t.fmt;
                continue;
            }
            noteColour(t.fmt);
        }
        if (backBuffer != D3DFMT_UNKNOWN) noteColour(backBuffer);
        if (engineDepth == D3DFMT_UNKNOWN) engineDepth = (D3DFORMAT)FOURCC_INTZ_EW;

        for (D3DFORMAT f : singleColour)
        {
            Context a; a.mrt = 1; a.color[0] = f; a.depth = engineDepth;
            a.name = "engine+depth"; a.fromEngine = true;
            AddContext(p, seen, a);
            Context b; b.mrt = 1; b.color[0] = f; b.name = "engine"; b.fromEngine = true;
            AddContext(p, seen, b);
        }
        // The reflection context: the scene set with a clip plane. It exists as
        // a separate coordinate because clipPlaneCount is in the key and
        // nothing else in the enumeration can reach it.
        {
            Context r; r.mrt = 1; r.color[0] = D3DFMT_A16B16G16R16F; r.depth = engineDepth;
            r.clip = true; r.name = "engine reflect"; r.fromEngine = true;
            r.msType = p.msType; r.msQuality = p.msQuality;
            AddContext(p, seen, r);
        }
        p.census.contextsFromRegistry =
            (uint32_t)p.contexts.size() - p.census.contextsFromPhases;

        // --- the fail-safe --------------------------------------------------
        // SHIPPED CONSTANTS, and every one the log prints as 'shipped' rather
        // than passing it off as an engine read. With the gate where it now is
        // these should not fire at all: if any of them does, the engine did not
        // declare a set the game is known to use, and that is worth seeing.
        // Two of them cannot come from the engine at any gate -- the
        // A32B32G32R32F pair is the GPU-particle target the factory only makes
        // when particles first run, and the fp16+A8R8G8B8 composite is bound
        // from inside a DrawList.
        struct FB { int mrt; D3DFORMAT c0, c1, c2, c3; bool depth; };
        static const FB kFallback[] = {
            { 1, D3DFMT_A16B16G16R16F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true  },
            { 4, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R16F,       true  },
            { 1, D3DFMT_G16R16F,       D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true  },
            { 1, D3DFMT_R32F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true  },
            { 1, D3DFMT_R16F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true  },
            { 1, D3DFMT_A8R8G8B8,      D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true  },
            { 1, D3DFMT_A16B16G16R16F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 1, D3DFMT_R32F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 1, D3DFMT_G16R16F,       D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 1, D3DFMT_R16F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 1, D3DFMT_L8,            D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 1, D3DFMT_A8R8G8B8,      D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
            { 2, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN,     false },
            { 2, D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, true },
            { 2, D3DFMT_A32B32G32R32F, D3DFMT_A32B32G32R32F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, false },
        };
        for (const FB& f : kFallback)
        {
            Context c;
            c.mrt = f.mrt; c.color[0] = f.c0; c.color[1] = f.c1; c.color[2] = f.c2; c.color[3] = f.c3;
            c.depth = f.depth ? engineDepth : D3DFMT_UNKNOWN;
            c.name = "shipped";
            if (AddContext(p, seen, c)) p.census.contextsShipped++;
        }
    }

    // ---------------------------------------------------------------------
    //  Axis 3 — the declarations: shipped constants unioned with whatever the
    //  live registry already holds, deduplicated on the ELEMENT BYTES (the
    //  engine's own map key over-counts; see 07-geometry §3).
    // ---------------------------------------------------------------------
    inline uint64_t DeclHash(const std::vector<D3DVERTEXELEMENT9>& e)
    {
        return pipelinekeys::Fnv1a(e.data(), e.size() * sizeof(D3DVERTEXELEMENT9));
    }

    inline void ComputeStrides(Decl& d)
    {
        auto size = [](BYTE t) -> UINT {
            switch (t)
            {
            case D3DDECLTYPE_FLOAT1: return 4;   case D3DDECLTYPE_FLOAT2: return 8;
            case D3DDECLTYPE_FLOAT3: return 12;  case D3DDECLTYPE_FLOAT4: return 16;
            case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4: case D3DDECLTYPE_SHORT2:
            case D3DDECLTYPE_UBYTE4N: case D3DDECLTYPE_SHORT2N: case D3DDECLTYPE_USHORT2N:
            case D3DDECLTYPE_UDEC3: case D3DDECLTYPE_DEC3N: case D3DDECLTYPE_FLOAT16_2: return 4;
            case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_USHORT4N:
            case D3DDECLTYPE_FLOAT16_4: return 8;
            default: return 16;
            }
        };
        memset(d.stride, 0, sizeof(d.stride));
        for (auto& e : d.elems)
            if (e.Stream < 4)
                d.stride[e.Stream] = (std::max)(d.stride[e.Stream], (UINT)e.Offset + size(e.Type));
    }

    inline void EnumDecls(Plan& p)
    {
        std::unordered_map<uint64_t, uint32_t> byHash;

        for (uint32_t i = 0; i < kShippedDeclCount; i++)
        {
            Decl d;
            d.elems.assign(kShippedDecls[i].elems, kShippedDecls[i].elems + kShippedDecls[i].count);
            d.weight = kShippedDecls[i].weight;
            d.origin = kShippedDecls[i].origin;
            // Stream 1 here is a per-instance 4x4 matrix, not a second vertex
            // stream: DXVK tests D3DSTREAMSOURCE_INSTANCEDATA and sets
            // inputRate INSTANCE, which is in the pipeline key (07-geometry §7).
            d.instanced = (i == kInstancingDeclIdx);
            ComputeStrides(d);
            if (byHash.emplace(DeclHash(d.elems), (uint32_t)p.decls.size()).second)
            {
                p.decls.push_back(std::move(d));
                p.census.declsShipped++;
            }
        }

        // Union in the live registry. This is what covers an install whose
        // archives are not the ones the shipped 29 came from: a mod's geometry
        // format appears here as soon as anything using it has streamed.
        uintptr_t buckets = 0; uint16_t nBuckets = 0, nEntries = 0;
        if (Peek(Rebase(kVA_DeclMapBuckets), buckets) &&
            Peek<uint16_t>(Rebase(kVA_DeclMapCount), nBuckets) &&
            Peek<uint16_t>(Rebase(kVA_DeclMapSize), nEntries) &&
            buckets && nBuckets && nBuckets <= 4096)
        {
            p.census.liveRegistryEntries = nEntries;
            for (uint16_t b = 0; b < nBuckets; b++)
            {
                uintptr_t node = 0;
                if (!Peek(buckets + b * 4, node)) continue;
                for (int guard = 0; node && guard < 256; guard++)
                {
                    uintptr_t decl = 0, next = 0;
                    Peek(node + 4, decl);
                    Peek(node + 8, next);
                    node = next;
                    if (!decl) continue;

                    uint32_t ref = 0;
                    Peek(decl + kDecl_Ref, ref);
                    Decl d;
                    for (int e = 0; e < 64; e++)
                    {
                        D3DVERTEXELEMENT9 el{};
                        if (!Peek(decl + kDecl_Elems + e * sizeof(D3DVERTEXELEMENT9), el)) break;
                        if (el.Stream == 0xFF) break;
                        if (el.Stream >= 4 || el.Type > D3DDECLTYPE_UNUSED) { d.elems.clear(); break; }
                        d.elems.push_back(el);
                    }
                    if (d.elems.empty()) continue;
                    d.weight = ref;                 // the engine's own use signal
                    d.origin = "live";
                    ComputeStrides(d);
                    if (byHash.emplace(DeclHash(d.elems), (uint32_t)p.decls.size()).second)
                    {
                        p.decls.push_back(std::move(d));
                        p.census.declsLive++;
                    }
                }
            }
        }
        p.census.declsTotal = (uint32_t)p.decls.size();
    }

    // ---------------------------------------------------------------------
    //  Axis 4 — the projection DXVK actually keys on.
    //
    //  d3d9_device.cpp BindInputLayout maps declaration elements to the VS
    //  input signature by (Usage, UsageIndex) -- POSITIONT folded to POSITION
    //  -- keeps (Stream, DecodeDecltype(Type), Offset) per signature slot, and
    //  substitutes a null binding for a semantic the declaration lacks. So two
    //  declarations that agree on what a shader READS are one pipeline, which
    //  is why the axis costs ~4.9x per pass and not 29x.
    // ---------------------------------------------------------------------
    struct ProjSlot { uint8_t stream, type; uint16_t offset; };   // stream 0xFF = null binding

    inline uint32_t ProjectSlots(const std::vector<std::pair<uint8_t, uint8_t>>& sig,
                                 const Decl& d, ProjSlot* slots, size_t cap)
    {
        uint32_t matched = 0;
        const size_t n = (std::min)(sig.size(), cap);
        for (size_t i = 0; i < n; i++)
        {
            uint8_t want = sig[i].first;
            if (want == D3DDECLUSAGE_POSITIONT) want = D3DDECLUSAGE_POSITION;
            slots[i] = { 0xFF, 0xFF, 0 };
            for (auto& e : d.elems)
            {
                uint8_t have = e.Usage;
                if (have == D3DDECLUSAGE_POSITIONT) have = D3DDECLUSAGE_POSITION;
                if (have == want && e.UsageIndex == sig[i].second)
                {
                    slots[i] = { (uint8_t)e.Stream, e.Type, e.Offset };
                    matched++;
                    break;
                }
            }
        }
        return matched;
    }

    inline bool Project(const std::vector<std::pair<uint8_t, uint8_t>>& sig, const Decl& d,
                        uint64_t& out, uint32_t& matched)
    {
        ProjSlot slots[32]{};
        const size_t n = (std::min)(sig.size(), (size_t)32);
        matched = ProjectSlots(sig, d, slots, 32);
        out = pipelinekeys::Fnv1a(slots, n * sizeof(ProjSlot));
        return matched == n;
    }

    // ---------------------------------------------------------------------
    //  The coordinate tie: which (render-target set, state vector) pairs a
    //  group's passes can actually be drawn at.
    //
    //  Source: ragePhase_EmitRenderListDrawCommands -> ragePhase_EmitBucketDraw
    //  -> the forward (0x00AE38B0) / deferred (0x00AE2B70) emitters. Only the
    //  deferred emitter installs state; the forward one installs nothing that
    //  can fork a pipeline. So the deferred groups are pinned to the G-buffer
    //  set and its two write-mask vectors, and every forward group can occur on
    //  any forward target set a phase binds.
    //
    //  The two lists are CROSSED, because a phase binds its targets and pushes
    //  its vector independently. They are level-ordered: the head of each list
    //  is what that group's own phase always does, the tail is what other
    //  phases can also draw it at. 10-key-spec.md §5.
    // ---------------------------------------------------------------------
    struct Tie { const uint8_t* rts; uint8_t nRts; const uint8_t* svs; uint8_t nSvs; };

    static const uint8_t kRT_Deferred[] = { kCtx_GBuffer };
    static const uint8_t kRT_Forward[]  = { kCtx_Scene, kCtx_Cascade, kCtx_Point, kCtx_Ldr,
                                            kCtx_Reflect, kCtx_Height, kCtx_Water, kCtx_Scene2 };
    // A named technique binds its own target from inside DrawList rather than
    // declaring it in the phase's +0x890 block: post-FX, the lighting resolve,
    // the blits, the HUD. These are the single-colour sets those targets have.
    static const uint8_t kRT_Named[]    = { kCtx_Scene, kCtx_LdrNoDepth, kCtx_Fp16NoDepth,
                                            kCtx_R32NoDepth, kCtx_Ldr, kCtx_Reflect, kCtx_Cascade,
                                            kCtx_R16NoDepth, kCtx_L8, kCtx_G16R16FNoDepth,
                                            kCtx_Scene2, kCtx_Point };

    static const uint8_t kSV_DeferredList[]  = { kSV_GBufStd, kSV_GBufAlpha, kSV_Fwd, kSV_FwdNoAlpha };
    static const uint8_t kSV_DeferredBSList[]= { kSV_GBufStd, kSV_GBufAlpha };
    static const uint8_t kSV_DeferredClipL[] = { kSV_GBufClip, kSV_GBufClipA, kSV_GBufStd, kSV_GBufAlpha };
    static const uint8_t kSV_ForwardList[]   = { kSV_Fwd, kSV_FwdBlend, kSV_FwdNoAlpha, kSV_FwdBlendNA };

#define EW_TIE(r, s) { r, (uint8_t)(sizeof(r) / sizeof(r[0])), s, (uint8_t)(sizeof(s) / sizeof(s[0])) }
    static const Tie kTie[kG_Count] = {
        /* deferred     */ EW_TIE(kRT_Deferred, kSV_DeferredList),
        /* deferredbs   */ EW_TIE(kRT_Deferred, kSV_DeferredBSList),
        /* deferredclip */ EW_TIE(kRT_Deferred, kSV_DeferredClipL),
        /* forward      */ EW_TIE(kRT_Forward,  kSV_ForwardList),
        /* named        */ EW_TIE(kRT_Named,    kSV_ForwardList),
    };
#undef EW_TIE

    // Budget levels: how many render-target sets and how many state vectors are
    // taken from the head of those lists, and whether the material's
    // sampler-binding family is emitted. 10-key-spec.md §6 measured what each
    // costs and what each contains; the ini text carries the same numbers.
    struct LevelSpec { uint8_t nRts, nSvs; bool samplerVariants; };
    static const LevelSpec kLevels[] = {
        { 1,  1, false },   // 0 legacy   -- one context per pass
        { 2,  4, false },   // 1 core     -- the contexts that group's own phase binds
        { 2,  4, true  },   // 2 material -- core + the sampler-binding variants
        { 12, 4, true  },   // 3 wide     -- every context any phase can bind, + variants
    };
    constexpr int kMaxLevel = 3;

    // ---------------------------------------------------------------------
    //  Walk RAGE's own effect registry. The .fxc database supplies the
    //  technique NAMES (the engine stores only a hash) and the shader I/O;
    //  the engine supplies which effects are actually loaded, their pass
    //  state deltas and -- the point -- the live shader objects.
    // ---------------------------------------------------------------------
    inline void EnumPasses(Plan& p, fxc_db* db)
    {
        // Two indexes into the .fxc database: by name, and by the hash of the
        // name. The hash one is what reaches an effect whose own name string is
        // not readable -- the gta_im outside the registry is exactly that, and
        // requiring a name used to throw it away.
        std::unordered_map<std::string, const fxc_effect*> fxcByName;
        std::unordered_map<uint32_t, const fxc_effect*>    fxcByHash;
        if (db)
            for (uint32_t e = 0, n = fxc_effect_count(db); e < n; e++)
                if (const fxc_effect* ef = fxc_get_effect(db, e))
                    if (ef->name)
                    {
                        fxcByName.emplace(ef->name, ef);
                        fxcByHash.emplace(RageHash(ef->name), ef);
                    }

        std::vector<uintptr_t> effects;
        const uintptr_t* tbl = (const uintptr_t*)Rebase(kVA_Effects);
        if (Readable(tbl, 128 * sizeof(uintptr_t)))
            for (int i = 0; i < 128; i++)
                if (tbl[i]) effects.push_back(tbl[i]);
        // One gta_im lives outside the array (03-live's correction) and
        // produced 646883 draws over a route, so it is not an oddity.
        //
        // kVA_EffectExtra is the grcEffect ITSELF, not a pointer to one. This
        // was read as a pointer at first and the effect was silently lost: the
        // census said 111 effects and 1850 shader objects where the other walk
        // of the same registry, which takes the address as a base, says 112 and
        // 1864 -- the difference being exactly its 15 programs less the one
        // that holds no shader.
        const uintptr_t extra = Rebase(kVA_EffectExtra);
        if (Readable((const void*)extra, 0x30) &&
            std::find(effects.begin(), effects.end(), extra) == effects.end())
            effects.push_back(extra);

        for (uintptr_t ef : effects)
        {
            // The NAME is advisory. It labels the log and finds the .fxc file,
            // but it is not what proves this is an effect -- the program array
            // and the technique array are. Requiring it silently dropped the one
            // effect kVA_EffectExtra exists to reach: the gta_im outside the
            // registry has no readable name because the name/UI code never
            // touches it, and with it went its 13 techniques and 15 programs.
            uintptr_t nameP = 0; std::string name;
            bool named = Peek(ef + kEff_Name, nameP) && PeekName(nameP, name);
            uint32_t effHash = 0;
            Peek<uint32_t>(ef + kEff_NameHash, effHash);

            uint16_t techTotal = 0;
            uintptr_t techs = 0, vprogs = 0, fprogs = 0;
            uint16_t vsTotal = 0, psTotal = 0;
            if (!Peek<uint16_t>(ef + kEff_TechTotal, techTotal) || techTotal == 0 || techTotal > 4096) continue;
            if (!Peek(ef + kEff_Techniques, techs) || !techs) continue;
            Peek(ef + kEff_VertProgs, vprogs);
            Peek(ef + kEff_FragProgs, fprogs);
            Peek<uint16_t>(ef + kEff_VsCount, vsTotal);
            Peek<uint16_t>(ef + kEff_PsCount, psTotal);
            if (!vprogs || !Readable((const void*)vprogs, (size_t)vsTotal * kProg_Stride)) continue;

            // Join by name, then by the effect's own m_NameHash, so an unnamed
            // effect still gets its technique names and its real group
            // classification instead of defaulting every technique to kG_Named.
            const fxc_effect* fx = nullptr;
            if (named)
                if (auto it = fxcByName.find(name); it != fxcByName.end()) fx = it->second;
            if (!fx && effHash)
                if (auto it = fxcByHash.find(effHash); it != fxcByHash.end())
                {
                    fx = it->second;
                    if (!named && fx->name) { name = fx->name; named = true; }
                }
            if (!named) name = "<unnamed>";

            const uint16_t effIdx = (uint16_t)p.effectNames.size();
            p.effectNames.push_back(name);
            p.census.effects++;
            p.census.vs += vsTotal;
            p.census.ps += psTotal;
            if (fx) p.census.effectsJoined++;

            for (uint16_t t = 0; t < techTotal; t++)
            {
                const uintptr_t tech = techs + (uintptr_t)t * kTech_Stride;
                uint16_t passTotal = 0; uintptr_t passes = 0;
                if (!Peek<uint16_t>(tech + kTech_PassTotal, passTotal) || passTotal == 0 || passTotal > 64) continue;
                if (!Peek(tech + kTech_Passes, passes) || !passes) continue;
                p.census.techniques++;

                // The technique name decides the group, and the group decides
                // every coordinate this technique's passes are drawn at, so a
                // wrong name is a silently wrong walk. The engine keeps the name
                // only as a hash, and taking the .fxc entry at the same ARRAY
                // INDEX is an assumption about file order -- so check it. On a
                // mismatch, search the effect's technique array by hash; if
                // nothing matches, leave the name null and count it, so a
                // divergent install shows up as a number rather than silence.
                const char* techName = nullptr;
                uint32_t techHash = 0;
                const bool haveHash = Peek<uint32_t>(tech + kTech_NameHash, techHash) && techHash != 0;
                if (fx && t < fx->technique_count && fx->techniques[t].name &&
                    (!haveHash || RageHash(fx->techniques[t].name) == techHash))
                {
                    techName = fx->techniques[t].name;
                    p.census.techByIndex++;
                }
                else if (fx && haveHash)
                {
                    for (uint32_t k = 0; k < fx->technique_count; k++)
                        if (fx->techniques[k].name && RageHash(fx->techniques[k].name) == techHash)
                        { techName = fx->techniques[k].name; p.census.techByHash++; break; }
                }
                if (techName) p.census.techniquesJoined++;
                else          p.census.techUnmatched++;
                const GroupClass group = ClassifyTechnique(techName);

                for (uint16_t q = 0; q < passTotal; q++)
                {
                    const uintptr_t pass = passes + (uintptr_t)q * kPass_Stride;
                    uint32_t vsIdx = 0, psIdx = 0; uintptr_t statesP = 0; uint16_t stateCount = 0;
                    if (!Peek(pass + kPass_Vs, vsIdx)) continue;
                    Peek(pass + kPass_Ps, psIdx);
                    Peek(pass + kPass_States, statesP);
                    Peek<uint16_t>(pass + kPass_StateCount, stateCount);
                    if (vsIdx >= vsTotal) continue;

                    PassInfo pi;
                    Peek(vprogs + (uintptr_t)vsIdx * kProg_Stride + kProg_D3D, pi.vs);
                    // psIndex 0 is the reserved NULL fragment every effect carries.
                    if (psIdx && psIdx < psTotal && fprogs &&
                        Readable((const void*)fprogs, (size_t)psTotal * kProg_Stride))
                        Peek(fprogs + (uintptr_t)psIdx * kProg_Stride + kProg_D3D, pi.ps);
                    if (!pi.vs) continue;

                    if (stateCount && stateCount <= 64 &&
                        Readable((const void*)statesP, (size_t)stateCount * 8))
                    {
                        pi.stateOff = (uint32_t)p.stateWords.size();
                        pi.stateCount = stateCount;
                        for (uint16_t s = 0; s < stateCount; s++)
                        {
                            uint32_t kv[2] = {};
                            memcpy(kv, (const void*)(statesP + (uintptr_t)s * 8), sizeof(kv));
                            p.stateWords.push_back(kv[0]);
                            p.stateWords.push_back(kv[1]);
                        }
                    }
                    pi.group = (uint8_t)group;
                    pi.effect = effIdx;
                    p.passes.push_back(std::move(pi));
                    p.census.passes++;
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    //  The effective render state one (phase state vector, pass) pair
    //  produces: the engine's own default block at 0x0106B420, then the phase
    //  callback's overrides, then the pass's own PASS_VALUE delta.
    //
    //  The order matters. The default block has ALPHABLENDENABLE = 1 with
    //  SRCALPHA/INVSRCALPHA, most phase callbacks turn it off, and a pass can
    //  turn it back on -- which is why one technique group shows both a
    //  blending and a non-blending pipeline in a trace, and why the blend
    //  triple belongs to the PASS and not to a table of phase vectors.
    // ---------------------------------------------------------------------
    struct EffectiveRS
    {
        uint32_t wm[4]{ 15, 15, 15, 15 };
        uint32_t blendEnable = 1, src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
        uint32_t op = D3DBLENDOP_ADD, sep = 0;
        uint32_t srcA = D3DBLEND_ONE, dstA = D3DBLEND_ZERO, opA = D3DBLENDOP_ADD;
        uint32_t alphaTest = 1, alphaFunc = D3DCMP_NOTEQUAL;
    };

    inline EffectiveRS ResolveRS(const Plan& p, const int* rsIdx,
                                 const PassInfo& pass, const StateVec& sv)
    {
        EffectiveRS e;
        auto get = [&](uint32_t rs, uint32_t fallback) {
            return rsIdx[rs] >= 0 ? p.defaultRS[rsIdx[rs]] : fallback;
        };
        e.wm[0] = get(D3DRS_COLORWRITEENABLE, 15);
        e.wm[1] = get(D3DRS_COLORWRITEENABLE1, 15);
        e.wm[2] = get(D3DRS_COLORWRITEENABLE2, 15);
        e.wm[3] = get(D3DRS_COLORWRITEENABLE3, 15);
        e.blendEnable = get(D3DRS_ALPHABLENDENABLE, 1);
        e.src  = get(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        e.dst  = get(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        e.op   = get(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        e.sep  = get(D3DRS_SEPARATEALPHABLENDENABLE, 0);
        e.srcA = get(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        e.dstA = get(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
        e.opA  = get(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        e.alphaTest = get(D3DRS_ALPHATESTENABLE, 1);
        e.alphaFunc = get(D3DRS_ALPHAFUNC, D3DCMP_NOTEQUAL);

        // the phase callback
        for (int i = 0; i < 4; i++) e.wm[i] = sv.cwe[i];
        e.blendEnable = sv.blendEnable ? 1u : 0u;
        e.alphaTest   = sv.alphaTest ? 1u : 0u;

        // the pass's own delta
        for (uint16_t s = 0; s < pass.stateCount; s++)
        {
            const uint32_t key = p.stateWords[pass.stateOff + s * 2];
            const uint32_t val = p.stateWords[pass.stateOff + s * 2 + 1];
            if (key >= kRageStates || !RSReaches(p.stateToRS[key])) continue;
            switch (p.stateToRS[key])
            {
            case D3DRS_COLORWRITEENABLE:  e.wm[0] = val; break;
            case D3DRS_COLORWRITEENABLE1: e.wm[1] = val; break;
            case D3DRS_COLORWRITEENABLE2: e.wm[2] = val; break;
            case D3DRS_COLORWRITEENABLE3: e.wm[3] = val; break;
            case D3DRS_ALPHABLENDENABLE:  e.blendEnable = val; break;
            case D3DRS_SRCBLEND:          e.src = val; break;
            case D3DRS_DESTBLEND:         e.dst = val; break;
            case D3DRS_BLENDOP:           e.op = val; break;
            case D3DRS_SEPARATEALPHABLENDENABLE: e.sep = val; break;
            case D3DRS_SRCBLENDALPHA:     e.srcA = val; break;
            case D3DRS_DESTBLENDALPHA:    e.dstA = val; break;
            case D3DRS_BLENDOPALPHA:      e.opA = val; break;
            case D3DRS_ALPHATESTENABLE:   e.alphaTest = val; break;
            case D3DRS_ALPHAFUNC:         e.alphaFunc = val; break;
            default: break;
            }
        }
        return e;
    }

    // Specialization constants 3 and 4: two bits per slot, 3 = null or unused.
    // DXVK folds nullOrUnusedMask = ~usedSamplerMask | ~usedTextureMask into
    // type 3 (d3d9_state.h updateSamplers), so an undeclared slot is 3 whatever
    // is bound and a declared slot with no texture is 3 as well.
    inline uint32_t SamplerSpec(const uint8_t* dim, int slots, bool bind, int nullSlot)
    {
        uint32_t v = 0;
        for (int s = 0; s < slots; s++)
        {
            uint32_t t = 3;
            if (bind && dim[s] && s != nullSlot)
                t = (dim[s] == 3) ? 2u : (dim[s] == 4 ? 1u : 0u);   // CUBE, VOLUME, else 2D
            v |= (t & 3u) << (2 * s);
        }
        return v;
    }

    // ---------------------------------------------------------------------
    //  Build the cross product that can actually occur.
    //
    //  One job = one (pass, projection, context, topology, PS sampler pattern,
    //  VS sampler pattern), and therefore one intended pipeline identity. The
    //  job CARRIES that identity, so dedup is over what DXVK will key on
    //  rather than over the axes we happened to vary -- and the same identity
    //  is what the emission log hands the containment scorer.
    // ---------------------------------------------------------------------
    inline void BuildJobs(Plan& p, int level)
    {
        if (level < 0) level = 0;
        if (level > kMaxLevel) level = kMaxLevel;
        const LevelSpec& L = kLevels[level];

        int rsIdx[256];
        for (int i = 0; i < 256; i++) rsIdx[i] = -1;
        for (uint32_t i = 0; i < kRageStates; i++)
            if (RSReaches(p.stateToRS[i]) && p.stateToRS[i] < 256)
                rsIdx[p.stateToRS[i]] = (int)i;

        // Which contexts have each kind. Ordered so that what the engine itself
        // declared comes before a shipped stand-in, and a set that carries
        // depth before one that does not -- most of the game's draws do. Two
        // per kind: one engine set and one stand-in is as far as a kind can
        // usefully go, and a third is always a duplicate format tuple.
        //
        // A CLIPPED SET AND AN UNCLIPPED ONE ARE NEVER ALTERNATIVES, though,
        // and that is why the fill is two passes rather than one.
        // ClassifyContext only consults `clip` in the fp16+depth arm (where it
        // yields kCtx_Reflect), so a clipped MIRROR_RT -- A8R8G8B8 with depth,
        // read from the engine, and every job at it carries kJF_ClipPlane --
        // classifies as plain kCtx_Ldr and competed with the unclipped LDR sets
        // for the same two slots. Whichever lost, its pipelines were never
        // built, and which one lost depended on registry order.
        // clipPlaneCount is in specialization constant 0, so they are different
        // pipelines for the same shader: take the first of each before taking a
        // second of either.
        std::vector<int> byKind[kCtx_KindCount];
        {
            std::vector<int> order(p.contexts.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const Context& A = p.contexts[(size_t)a];
                const Context& B = p.contexts[(size_t)b];
                if (A.fromEngine != B.fromEngine) return A.fromEngine;
                const bool ad = A.depth != D3DFMT_UNKNOWN, bd = B.depth != D3DFMT_UNKNOWN;
                if (ad != bd) return ad;
                return a < b;
            });
            for (int wantClip = 0; wantClip < 2; wantClip++)
                for (int i : order)
                {
                    const Context& C = p.contexts[(size_t)i];
                    if (C.clip != (wantClip != 0)) continue;
                    std::vector<int>& v = byKind[C.kind];
                    // First pass takes at most one of each clip-ness, so the
                    // other is guaranteed a slot; second pass fills what is left.
                    const size_t cap = (wantClip == 0) ? 1u : 2u;
                    if (v.size() < cap) v.push_back(i);
                }
            for (int i : order)
            {
                std::vector<int>& v = byKind[p.contexts[(size_t)i].kind];
                if (v.size() < 2 &&
                    std::find(v.begin(), v.end(), i) == v.end()) v.push_back(i);
            }
        }

        // Declaration ordering: most-drawn first, so a budget cut sheds the
        // rarest layouts rather than an arbitrary prefix.
        std::vector<uint32_t> declOrder(p.decls.size());
        for (uint32_t i = 0; i < declOrder.size(); i++) declOrder[i] = i;
        std::sort(declOrder.begin(), declOrder.end(), [&](uint32_t a, uint32_t b) {
            return p.decls[a].weight > p.decls[b].weight;
        });
        std::vector<uint32_t> declRank(p.decls.size(), 0);
        for (uint32_t i = 0; i < declOrder.size(); i++) declRank[declOrder[i]] = i;

        // Every declaration is a candidate at every level. Capping the
        // candidate list looked like a cheap knob and is not one: DXVK keys on
        // the declaration PROJECTED onto the VS input signature, so
        // declarations that agree on what a shader reads already collapse into
        // one job -- measured at 4.91 projections per pass over all 29
        // (07-geometry §9). A cap would not have saved 4.91x, it would only
        // have dropped real coordinates, starting with the instancing layout
        // and the blit quads, which rank low on draw count but are not
        // optional. What the level changes is the COORDINATE count.

        // A hard stop, so a plan that goes wrong cannot submit a million draws.
        // It STOPS the walk rather than skipping a push_back: the dedup set is
        // the thing that grows without bound otherwise, and in a 32-bit process
        // that already holds GTA IV's heap that matters more than the jobs do.
        // Jobs are ordered, so a cut sheds the speculative end.
        constexpr size_t kMaxJobs = 400000;
        bool capped = false;

        std::unordered_set<uint64_t> seen;
        seen.reserve(p.passes.size() * 16);
        uint64_t projTotal = 0;

        // Per-axis value sets, so the log can say what the enumeration varies
        // before a single draw is issued.
        std::unordered_set<uint64_t> axShaders, axProj, axRT, axBlend;
        std::unordered_set<uint32_t> axPrim, axSpec0, axPS, axVS, axClip;

        static const D3DPRIMITIVETYPE kTopoOf[3] =
            { D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN };

        for (uint32_t pi = 0; pi < p.passes.size() && !capped; pi++)
        {
            const PassInfo& pass = p.passes[pi];
            if (pass.group < kG_Count) p.census.groupPasses[pass.group]++;

            // Which declarations can feed this VS: one per distinct projection,
            // preferring the ones that satisfy the signature outright.
            const std::vector<std::pair<uint8_t, uint8_t>>* sig = pass.sig.empty() ? nullptr : &pass.sig;

            std::vector<uint32_t> useDecls;
            std::vector<uint64_t> useProj;
            if (sig && !sig->empty())
            {
                std::unordered_set<uint64_t> projSeen;
                uint32_t bestPartial = 0xFFFFFFFFu, bestMatched = 0;
                uint64_t bestProj = 0;
                for (uint32_t oi = 0; oi < declOrder.size(); oi++)
                {
                    const uint32_t di = declOrder[oi];
                    uint64_t proj = 0; uint32_t matched = 0;
                    const bool full = Project(*sig, p.decls[di], proj, matched);
                    if (!full)
                    {
                        if (matched > bestMatched)
                        { bestMatched = matched; bestPartial = di; bestProj = proj; }
                        continue;
                    }
                    if (projSeen.insert(proj).second)
                    { useDecls.push_back(di); useProj.push_back(proj); }
                }
                // One VS input signature in 47 is satisfied by no declaration
                // that exists (07-geometry's open question). Those shaders are
                // drawn with a null binding in gameplay, so the closest
                // declaration is still the right pipeline to warm.
                if (useDecls.empty() && bestPartial != 0xFFFFFFFFu)
                { useDecls.push_back(bestPartial); useProj.push_back(bestProj); }
            }
            if (useDecls.empty() && !declOrder.empty())
            {
                useDecls.push_back(declOrder[0]);
                useProj.push_back(DeclHash(p.decls[declOrder[0]].elems));
            }
            if (useDecls.empty()) continue;
            projTotal += useDecls.size();

            // --- the contexts this group's passes can be drawn at ---------
            const Tie& tie = kTie[pass.group < kG_Count ? pass.group : kG_Named];
            const int nRts = (std::min)((int)tie.nRts, (int)L.nRts);
            const int nSvs = (std::min)((int)tie.nSvs, (int)L.nSvs);
            std::vector<std::pair<int, uint8_t>> coords;
            for (int r = 0; r < nRts; r++)
                for (int ctx : byKind[tie.rts[r]])
                    for (int s = 0; s < nSvs; s++)
                        coords.emplace_back(ctx, tie.svs[s]);
            // The clipped context, for the groups a reflection or mirror phase
            // can draw. DXVK packs the number of ENABLED, NON-ZERO user clip
            // planes into specialization constant 0, so a vertex shader
            // compiled with one is a different pipeline from the same shader
            // compiled with none -- 75 route identities, all in the reflection
            // group and in named techniques of material effects. It is one
            // coordinate, not a crossed axis: crossing would double the whole
            // enumeration to reach 8 % of it.
            if (level >= 1 && (pass.group == kG_Forward || pass.group == kG_Named))
                for (int ctx : byKind[kCtx_Reflect])
                    coords.emplace_back(ctx, tie.svs[0]);
            if (coords.empty()) continue;

            // --- the material's sampler-binding family ---------------------
            std::vector<int> psNull{ -1 };          // -1 = every declared slot bound
            if (L.samplerVariants)
            {
                for (int s = 0; s < 16; s++) if (pass.psDim[s]) psNull.push_back(s);
                psNull.push_back(16);               // 16 = none bound at all
            }
            bool vsDeclares = false;
            for (int s = 0; s < 4; s++) if (pass.vsDim[s]) vsDeclares = true;

            // Topology bakes into the pipeline: VK_DYNAMIC_STATE_PRIMITIVE_
            // TOPOLOGY is not among the dynamic states DXVK 3.1.1 uses, so
            // list, strip and fan are three pipelines for one shader pair. A
            // GROUPED pass is entity geometry and RAGE draws it as an indexed
            // triangle list -- every grouped key in the route trace is one. A
            // named technique is a blit or a sprite batch and uses all three.
            const int topoN = (pass.group == kG_Named) ? 3 : 1;

            for (const auto& cs : coords)
            {
                const Context& C = p.contexts[(size_t)cs.first];
                const StateVec& SV = kStateVecs[cs.second];
                const EffectiveRS rs = ResolveRS(p, rsIdx, pass, SV);

                JobKey k{};
                k.vs = pass.vsHash; k.ps = pass.psHash;
                k.ds = (uint32_t)C.depth;
                k.clip = C.clip ? 1u : 0u;
                for (int i = 0; i < 4; i++)
                {
                    k.rt[i] = (uint32_t)C.color[i];
                    if (C.color[i] != D3DFMT_UNKNOWN)
                        k.blend[i] = NormaliseBlend(rs.blendEnable, rs.src, rs.dst, rs.op,
                                                    rs.sep, rs.srcA, rs.dstA, rs.opA, rs.wm[i]);
                }
                // The alpha compare op and the alpha PRECISION share
                // specialization constant 0, and the precision is a pure
                // function of the RT0 format -- tied to the context, never a
                // free axis.
                k.spec0 = (rs.alphaTest ? VkCompareOp(rs.alphaFunc) : 7u) |
                          (AlphaPrecision(C.color[0]) << 4);

                for (int tp = 0; tp < topoN && !capped; tp++)
                for (int vb = 0; vb < (vsDeclares ? 2 : 1) && !capped; vb++)
                for (int ni = 0; ni < (int)psNull.size() && !capped; ni++)
                {
                    k.prim = (uint32_t)kTopoOf[tp];
                    k.psTypes = SamplerSpec(pass.psDim, 16, psNull[ni] != 16, psNull[ni]);
                    k.vsTypes = SamplerSpec(pass.vsDim, 4, vb != 0, -1);

                    for (size_t d = 0; d < useDecls.size(); d++)
                    {
                        const uint32_t di = useDecls[d];
                        k.proj = useProj[d];
                        // Stream 1 of the instancing declaration carries
                        // per-instance data, and DXVK puts the input RATE in
                        // the key -- so it is part of the identity, not of the
                        // draw call only. Recorded the way the draw tracer and
                        // the recorded caches record it: 0 for a stream left at
                        // the default, and only stream 1's INSTANCEDATA where
                        // there is instancing. (The draw itself also has to put
                        // INDEXEDDATA on stream 0, or DXVK draws one instance
                        // and the pipeline is never the instanced one.)
                        k.freq[1] = p.decls[di].instanced ? (D3DSTREAMSOURCE_INSTANCEDATA | 1u) : 0u;

                        if (!seen.insert(pipelinekeys::Fnv1a(&k, sizeof(k))).second)
                        { p.census.jobsDeduped++; continue; }
                        if (p.jobs.size() >= kMaxJobs)
                        { p.census.jobsCapped++; capped = true; break; }
                        p.enumFingerprint = pipelinekeys::Fnv1a(&k, sizeof(k), p.enumFingerprint);

                        axShaders.insert(pipelinekeys::Fnv1a(&k.vs, 16));
                        axProj.insert(k.proj);
                        axRT.insert(pipelinekeys::Fnv1a(k.rt, sizeof(k.rt) + sizeof(k.ds)));
                        axBlend.insert(pipelinekeys::Fnv1a(k.blend, sizeof(k.blend)));
                        axPrim.insert(k.prim);
                        axSpec0.insert(k.spec0);
                        axPS.insert(k.psTypes);
                        axVS.insert(k.vsTypes);
                        axClip.insert(k.clip);

                        Job j{};
                        j.pass = pi;
                        j.decl = (uint16_t)di;
                        j.ctx = (uint8_t)cs.first;
                        j.state = cs.second;
                        j.flags = (uint8_t)((uint8_t)tp |
                                            (vb ? kJF_VSSampler : 0) |
                                            (C.clip ? kJF_ClipPlane : 0));
                        j.psVariant = (uint8_t)(psNull[ni] < 0 ? kPSV_AllBound
                                                               : (psNull[ni] == 16 ? kPSV_NoneBound
                                                                                   : psNull[ni] + 1));
                        // Ascending: the head of each group's context list
                        // first, then by how much the declaration is drawn,
                        // then the sampler and topology variants. A budget cut
                        // therefore sheds the speculative end rather than an
                        // arbitrary tail.
                        j.rank = ((uint32_t)((ni || tp || vb) ? 1u : 0u) << 30) |
                                 ((declRank[di] & 0x3FFu) << 20) |
                                 (((uint32_t)cs.first & 0x3Fu) << 14) |
                                 (((uint32_t)cs.second & 0xFu) << 10) |
                                 (pi & 0x3FFu);
                        p.jobs.push_back(j);
                        if (p.keepKeys) p.keys.push_back(k);
                    }
                }
                if (capped) break;
            }
        }
        p.census.jobsEmitted = (uint32_t)p.jobs.size();
        p.census.projectionsPerPass100 = p.passes.empty() ? 0
            : (uint32_t)((projTotal * 100) / p.passes.size());
        p.census.axisShaders = (uint32_t)axShaders.size();
        p.census.axisProj    = (uint32_t)axProj.size();
        p.census.axisRT      = (uint32_t)axRT.size();
        p.census.axisBlend   = (uint32_t)axBlend.size();
        p.census.axisPrim    = (uint32_t)axPrim.size();
        p.census.axisSpec0   = (uint32_t)axSpec0.size();
        p.census.axisPSTypes = (uint32_t)axPS.size();
        p.census.axisVSTypes = (uint32_t)axVS.size();
        p.census.axisClip    = (uint32_t)axClip.size();

        // Sort jobs and keys together: the key IS the job's identity and the
        // emission log pairs them by index.
        std::vector<uint32_t> ord(p.jobs.size());
        for (uint32_t i = 0; i < ord.size(); i++) ord[i] = i;
        std::stable_sort(ord.begin(), ord.end(),
                         [&](uint32_t a, uint32_t b) { return p.jobs[a].rank < p.jobs[b].rank; });
        std::vector<Job> sortedJobs(p.jobs.size());
        for (uint32_t i = 0; i < ord.size(); i++) sortedJobs[i] = p.jobs[ord[i]];
        p.jobs.swap(sortedJobs);
        if (p.keepKeys && p.keys.size() == ord.size())
        {
            std::vector<JobKey> sortedKeys(p.keys.size());
            for (uint32_t i = 0; i < ord.size(); i++) sortedKeys[i] = p.keys[ord[i]];
            p.keys.swap(sortedKeys);
        }
    }

    // Two calls, not one. Between them the caller reads every pass's shader
    // objects with GetFunction -- the VS input signature and the PS sampler
    // declarations -- and AddRefs the objects it will bind, so the walk never
    // depends on a .fxc join by array index and never holds a pointer it has
    // not proved is a live shader. See EngineWarmPass in shaderprecompile.ixx.
    inline void BuildTables(Plan& p, fxc_db* db,
                            const std::vector<bootgate::PhaseSet>& phases,
                            const std::vector<bootgate::Target>& registry,
                            D3DFORMAT backBuffer)
    {
        if (!ResolveTables(p)) return;
        EnumPasses(p, db);
        if (p.passes.empty()) { p.why = "RAGE's effect registry holds no usable pass"; return; }
        EnumContexts(p, phases, registry, backBuffer);
        if (p.contexts.empty()) { p.why = "the engine declares no render target"; return; }
        EnumDecls(p);
        if (p.decls.empty()) { p.why = "no vertex declaration could be built"; return; }
        p.ok = true;
    }

    inline void BuildPlanJobs(Plan& p, int level)
    {
        BuildJobs(p, level);
        p.ok = !p.jobs.empty();
        if (!p.ok) p.why = "the tie produced no job";
    }

    // ---------------------------------------------------------------------
    //  The emission log.
    //
    //  One line per intended pipeline identity, in exactly the fields
    //  re/rage-shader-precompile/tools/warmspec.py reduces a corpus to. That
    //  is the point: the containment gate can then be evaluated against what
    //  this code EMITS rather than against a Python model of what it is
    //  supposed to emit, and the two can be compared.
    //
    //  Off by default: the file is ~200 bytes a job.
    // ---------------------------------------------------------------------
    inline bool WriteEmissionLog(const Plan& p, const std::string& path, int level)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
        fprintf(f, "{\"kind\":\"enginewarm-emission\",\"level\":%d,\"jobs\":%zu,"
                   "\"passes\":%zu,\"contexts\":%zu,\"decls\":%zu}\n",
                level, p.keys.size(), p.passes.size(), p.contexts.size(), p.decls.size());
        for (size_t i = 0; i < p.keys.size() && i < p.jobs.size(); i++)
        {
            const JobKey& k = p.keys[i];
            const Job& j = p.jobs[i];
            fprintf(f, "{\"vs\":%llu,\"ps\":%llu,\"proj\":[",
                    (unsigned long long)k.vs, (unsigned long long)k.ps);
            // The projection in full, not as a hash: the scorer compares it
            // slot by slot against what the route trace's declaration produced
            // for the same signature.
            {
                const PassInfo& pass = p.passes[j.pass];
                ProjSlot slots[32]{};
                const size_t n = (std::min)(pass.sig.size(), (size_t)32);
                if (n && j.decl < p.decls.size())
                    ProjectSlots(pass.sig, p.decls[j.decl], slots, 32);
                for (size_t s = 0; s < n; s++)
                {
                    if (s) fputc(',', f);
                    if (slots[s].stream == 0xFF) fputs("null", f);
                    else fprintf(f, "[%u,%u,%u]", slots[s].stream, slots[s].type, slots[s].offset);
                }
            }
            fprintf(f, "],\"rt\":[%u,%u,%u,%u],\"ds\":%u,\"srgb\":%u,\"blend\":[",
                    k.rt[0], k.rt[1], k.rt[2], k.rt[3], k.ds, k.srgb);
            for (int i = 0; i < 4; i++)
            {
                if (i) fputc(',', f);
                if (!k.rt[i]) { fputs("null", f); continue; }
                const BlendKey& b = k.blend[i];
                fprintf(f, "[%u,%u,%u,%u,%u,%u,%u,%u]",
                        b.enable, b.cs, b.cd, b.co, b.sa, b.da, b.oa, b.wm);
            }
            fprintf(f, "],\"prim\":%u,\"freq\":[%u,%u,%u,%u],\"spec0\":%u,"
                       "\"psTypes\":%u,\"vsTypes\":%u,\"clip\":%u}\n",
                    k.prim, k.freq[0], k.freq[1], k.freq[2], k.freq[3],
                    k.spec0, k.psTypes, k.vsTypes, k.clip);
        }
        fclose(f);
        return true;
    }

    // The fingerprint of an enumeration: if this has not changed, and the
    // driver cache has not been thrown away, nothing the long pass would build
    // is missing and it does not have to run again. It covers everything that
    // decides WHICH pipelines the pass emits -- the level, the contexts, the
    // declarations, and every identity it produced.
    inline uint64_t EnumerationFingerprint(const Plan& p, int level)
    {
        uint64_t h = pipelinekeys::Fnv1a(&level, sizeof(level));
        for (const Context& c : p.contexts)
        {
            h = pipelinekeys::Fnv1a(c.color, sizeof(c.color), h);
            h = pipelinekeys::Fnv1a(&c.depth, sizeof(c.depth), h);
            h = pipelinekeys::Fnv1a(&c.msType, sizeof(c.msType), h);
            const uint32_t flags = (c.clip ? 1u : 0u) | (c.fromEngine ? 2u : 0u) | (c.kind << 2);
            h = pipelinekeys::Fnv1a(&flags, sizeof(flags), h);
        }
        return pipelinekeys::Fnv1a(&p.enumFingerprint, sizeof(p.enumFingerprint), h);
    }
}

