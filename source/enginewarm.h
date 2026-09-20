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
//  All four are enumerable from the engine at the loading-screen gate, and
//  this header is that enumeration. The evidence is in
//  re/rage-shader-precompile/{06-phases,07-geometry}.md; every address below
//  is named there and every one is validated at run time before it is used.
//
//  THE TIMING RULE. Everything in 0x00401000-0x004FB000 is SecuROM-encrypted
//  at rest, so nothing here may be touched at ASI load. Resolve() must be
//  called from the loading-screen gate (inside RunBlocking), where 04-poc
//  proved the region is plaintext. Nothing in this header is a static
//  initializer for that reason.
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
    constexpr uintptr_t kVA_PhaseList      = 0x0118D7F0;  // +0x38 CRenderPhase**, +0x3C u16 count
    constexpr uintptr_t kVA_GBuffer3       = 0x0154E17C;  // _DEFERRED_GBUFFER_3_ (INTZ)
    constexpr uintptr_t kVA_StencilBuffer  = 0x0154E180;  // _STENCIL_BUFFER_ -- the REAL MRT3
    // Named engine render targets the PHASE LIST does not reach. The post-FX
    // phases bind theirs inside DrawList and the shadow phases only declare the
    // point pair, so walking +0x890 alone misses the single-colour sets that
    // carry a quarter of the recorded draws. These are the creators' own
    // globals: ragePhase_CreateShadowTargets 0x00925430 and
    // ragePhase_CreateGBufferTargets 0x00AD1410.
    constexpr uintptr_t kVA_NamedTargets[] = {
        0x0119CFE8,  // SHADOW_MAP_COLOUR_POINT
        0x0119CFEC,  // SHADOW_MAP_DEPTH_POINT
        0x0119CFF0,  // SHADOW_MAP_COLOUR_CACHE
        0x0119CFF4,  // SHADOW_MAP_COLOUR_HEMI_CUBE
        0x0154E170,  // _DEFERRED_GBUFFER_0_
        0x0154E174,  // _DEFERRED_GBUFFER_1_
        0x0154E178,  // _DEFERRED_GBUFFER_2_
        0x0154E17C,  // _DEFERRED_GBUFFER_3_ (INTZ)
        0x0154E180,  // _STENCIL_BUFFER_
        0x0154E184,  // _BACK_ZBUFFER_
    };
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
    constexpr uint32_t kPhase_Slot0     = 0x890, kPhase_SlotStride = 0x10, kPhase_DepthDelta = 0x08;
    constexpr uint32_t kRT_Name         = 0x30, kRT_Tex = 0x34, kRT_W = 0x3C, kRT_H = 0x3E, kRT_Fmt = 0x40;
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

    // ---------------------------------------------------------------------
    //  Safe reads. Nothing here may fault: an enumeration that crashes the
    //  loading screen is far worse than one that gives up.
    // ---------------------------------------------------------------------
    inline uintptr_t Rebase(uintptr_t va)
    {
        static uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        return va - 0x400000u + base;
    }

    inline bool Readable(const void* p, size_t n)
    {
        if (!p || n == 0) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        auto at = (const uint8_t*)p;
        const uint8_t* end = at + n;
        while (at < end)
        {
            if (!VirtualQuery(at, &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT) return false;
            const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
            if (mbi.Protect & bad) return false;
            const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & ok)) return false;
            at = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        }
        return true;
    }

    template <typename T> inline bool Peek(uintptr_t p, T& out)
    {
        if (!Readable((const void*)p, sizeof(T))) return false;
        memcpy(&out, (const void*)p, sizeof(T));
        return true;
    }

    // A C string that is actually a name: printable, bounded, NUL-terminated.
    inline bool PeekName(uintptr_t p, std::string& out, size_t cap = 64)
    {
        if (!Readable((const void*)p, 1)) return false;
        auto s = (const char*)p;
        out.clear();
        for (size_t i = 0; i < cap; i++)
        {
            if (!Readable(s + i, 1)) return false;
            char c = s[i];
            if (c == 0) return !out.empty();
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
            out.push_back(c);
        }
        return false;
    }

    // ---------------------------------------------------------------------
    //  Axis 1 — render-target / depth / sample-count contexts, read from the
    //  live render-phase list (06-phases §5.1).
    // ---------------------------------------------------------------------
    struct Context
    {
        std::string name;           // for the log: the phase target that produced it
        D3DFORMAT color[4]{};       // D3DFMT_UNKNOWN = slot unbound
        int       mrt = 0;
        D3DFORMAT depth = D3DFMT_UNKNOWN;
        uint32_t  msType = 0, msQuality = 0;
        uint32_t  weight = 0;       // draw-order rank; lower is warmed first
        bool      fromPhase = false;
    };

    // How many render phases the list holds right now. FusionFix's gate fires
    // about three seconds into the loading screen, which is EARLIER than
    // ragePhase_CreateAllPhasesAndTargets (0x00B00B60) runs: measured live, the
    // list is empty there and exactly one named target exists, although
    // 06-phases' probe found 29 phases and every target already created on the
    // same loading screen thirty seconds later -- and holding the screen for a
    // further twenty seconds did not make them appear. So the walk probes for
    // the list rather than assuming it, and falls back to the shipped sets
    // when it is not there, saying which in the log.
    inline uint16_t PhaseCount()
    {
        uintptr_t listBase = 0, arr = 0; uint16_t n = 0;
        if (!Peek(Rebase(kVA_PhaseList), listBase) || !listBase) return 0;
        if (!Peek(listBase + 0x38, arr) || !arr) return 0;
        if (!Peek<uint16_t>(listBase + 0x3C, n) || n > 256) return 0;
        return n;
    }

    // ---------------------------------------------------------------------
    //  Axis 2 — the blend / colour-write / depth-stencil vector a render
    //  phase establishes before it emits any bucket draw.
    //
    //  These are the six render-thread callbacks 06-phases §3.4 decoded, in
    //  the D3D9 terms they reach the device in. They are engine CODE, not
    //  engine data, so they cannot be read at run time -- they are written out
    //  here with their addresses, and the live vector at 0x017F5848 is logged
    //  beside them so a drift shows up as a number rather than a guess.
    //
    //  The measured marginal cost of each, at a fixed target set over 526
    //  passes (06-phases §5.2): none 403 new pipelines, ForwardAllChannels 9,
    //  ForwardStencil 0, GBufferStandard 0, GBufferAlphaClip 85,
    //  DepthOnlyAlphaTest 85. ForwardStencil and GBufferStandard collapse onto
    //  what is already built -- DXVK treats the stencil enable/ref/func this
    //  engine sets as dynamic state -- so they are not emitted separately.
    // ---------------------------------------------------------------------
    struct StateVec
    {
        const char* name;
        DWORD  cwe[4];          // COLORWRITEENABLE 0..3
        BOOL   blendEnable;
        DWORD  srcBlend, dstBlend, blendOp;
        BOOL   alphaTest;
        BOOL   zWrite;
    };

    // 0x00AD1980 State_ForwardAllChannels — CW F/F/F/F, blend off.
    // 0x00AD2510 State_GBufferStandard    — CW 7/7/7/F, blend off, alpha test off.
    // 0x00AD2430 State_GBufferAlphaClip   — CW 7/0/3/F, blend ON, depth write OFF.
    // 0x00AD23F0 State_DepthOnlyAlphaTest — CW 7 on RT0, alpha test ON.
    // 0x00AD1890 State_ForwardRT0Only     — CW F on RT0, the rest untouched.
    // The three blend presets below GBufferAlphaClip's are preset 0
    // (SRCALPHA/INVSRCALPHA/ADD), 1 (ONE/ONE/ADD) and 7 (SRCALPHA/ONE/ADD) of
    // the 14 the engine's SetState id 2 selects -- the transparency, additive
    // and glow vectors a forward phase reaches.
    //
    // WHY EVERY SLOT THE PHASE DOES NOT TOUCH IS 15, NOT 0. D3D9's four
    // COLORWRITEENABLE states are sticky device state, and a phase that renders
    // to one target only sets the one it cares about; the other three keep
    // whatever the last MRT phase left, which for this engine is 15. DXVK does
    // NOT normalise that away: BindBlendState writes omBlend[0..3] from all four
    // render states unconditionally (d3d9_device.cpp:6995) and the pipeline
    // lookup is a bcmpeq over the whole state struct (dxvk_graphics_state.h:748),
    // so a 0 where gameplay has 15 is a DIFFERENT pipeline. Measured over this
    // rig's 14543-key recording: COLORWRITEENABLE3 is 15 in 100 % of keys, and
    // the 20 vectors that occur are exactly the masks below with 15 in every
    // slot the phase left alone. The first version of this table wrote 0 there
    // and every one of its level-2 coordinates was therefore unreachable.
    static const StateVec kStateVecs[] = {
        { "forward",        { 15, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_std",       {  7,  7,  7, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_alphaclip", {  7,  0,  3, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE },
        { "depthonly",      {  7,  7,  7, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, TRUE,  TRUE  },
        { "forward_rt0",    { 15,  0,  0, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "fwd_blend",      { 15, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE },
        { "fwd_add",        { 15, 15, 15, 15 }, TRUE,  D3DBLEND_ONE,      D3DBLEND_ONE,         D3DBLENDOP_ADD, FALSE, FALSE },
        { "fwd_glow",       { 15, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_ONE,         D3DBLENDOP_ADD, FALSE, FALSE },
        // ---- level-2 extras ------------------------------------------------
        // Every one of these is a (colour-write vector, blend preset) pair from
        // the statically-mined domain of all 514 grcState::SetState call sites
        // (README §3.1: COLORWRITEENABLE0 {0,5,7,8,0xF}, 1 {0,7,0xF}, 2
        // {0,3,7,0xF}, 3 {0xF}; blend presets 0,1,2,4,5,6,8 of the 14). Note the
        // mined domain of slot 3 is {0xF} alone, which is the same thing the
        // recording says and the reason every vector here ends in 15.
        //
        // The colour-write vectors are now the 20 the 14543-key recording holds,
        // exactly: the seven gbuf_* are its MRT vectors and the mask_* / rt0_*
        // its single-target ones. Tied below: the MRT ones only to the G-buffer
        // coordinate, the single-channel ones only to the passes the engine
        // reaches by name.
        { "gbuf_v1",        {  7,  7, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v2",        {  7,  7,  3, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v3",        {  7, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v4",        {  7,  7,  7, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v5",        {  0,  7,  0, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v6",        {  8,  0,  0, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        // The five (15, ...) MRT vectors the recording holds that the mined
        // domain also allows. gbuf_v7 used to be {15,15,0,0}, which occurs in
        // none of the 14543 keys -- slot 3 is never masked off.
        { "gbuf_v7",        { 15,  7, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v8",        { 15,  7,  3, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v9",        { 15,  7,  7, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v10",       { 15,  7,  0, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "gbuf_v11",       { 15,  0,  3, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_ra",        {  5, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_ra_blend",  {  5, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_none",      {  0, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_r",         {  1, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_r_blend",   {  1, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_g",         {  2, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_b_blend",   {  4, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_a",         {  8, 15, 15, 15 }, FALSE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "mask_a_blend",   {  8, 15, 15, 15 }, TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, TRUE  },
        { "rt0_destalpha",  {  7, 15, 15, 15 }, TRUE,  D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLENDOP_ADD, FALSE, TRUE },
        { "rt0_destone",    {  7, 15, 15, 15 }, TRUE,  D3DBLEND_DESTALPHA, D3DBLEND_ONE,        D3DBLENDOP_ADD, FALSE, TRUE  },
        { "fwd_destalpha",  { 15, 15, 15, 15 }, TRUE,  D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLENDOP_ADD, FALSE, FALSE },
        { "fwd_destone",    { 15, 15, 15, 15 }, TRUE,  D3DBLEND_DESTALPHA, D3DBLEND_ONE,        D3DBLENDOP_ADD, FALSE, FALSE },
        { "fwd_oneinv",     { 15, 15, 15, 15 }, TRUE,  D3DBLEND_ONE,      D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE },
    };
    // mask_a_opaque is gone: it was mask_a's colour-write vector with blending
    // ON and ONE/ZERO/ADD, and DxvkBlendMode::normalize turns exactly that into
    // blending OFF (dxvk_constant_state.cpp:93-117), so it was the same pipeline
    // drawn twice. BuildJobs now normalises the blend triple into the job
    // identity the same way, so any future duplicate of that shape dedups away
    // instead of costing a draw.
    enum { kSV_Forward = 0, kSV_GBufStd, kSV_GBufClip, kSV_DepthOnly, kSV_ForwardRT0,
           kSV_FwdBlend, kSV_FwdAdd, kSV_FwdGlow,
           kSV_GBufV1, kSV_GBufV2, kSV_GBufV3, kSV_GBufV4, kSV_GBufV5, kSV_GBufV6,
           kSV_GBufV7, kSV_GBufV8, kSV_GBufV9, kSV_GBufV10, kSV_GBufV11,
           kSV_MaskRA, kSV_MaskRABlend, kSV_MaskNone, kSV_MaskR, kSV_MaskRBlend, kSV_MaskG,
           kSV_MaskBBlend, kSV_MaskA, kSV_MaskABlend,
           kSV_RT0DestAlpha, kSV_RT0DestOne, kSV_FwdDestAlpha, kSV_FwdDestOne, kSV_FwdOneInv,
           kSV_Count };

    // Level-2 extras, per group, all emitted at that group's FIRST coordinate
    // only, so they add a bounded number of jobs instead of multiplying the
    // whole walk.
    static const uint8_t kExtraDeferred[] = { kSV_GBufV1, kSV_GBufV2, kSV_GBufV3, kSV_GBufV4,
                                              kSV_GBufV5, kSV_GBufV6, kSV_GBufV7, kSV_GBufV8,
                                              kSV_GBufV9, kSV_GBufV10, kSV_GBufV11 };
    static const uint8_t kExtraNamed[]    = { kSV_MaskRA, kSV_MaskRABlend, kSV_MaskNone, kSV_MaskR,
                                              kSV_MaskRBlend, kSV_MaskG, kSV_MaskBBlend, kSV_MaskA,
                                              kSV_MaskABlend, kSV_RT0DestAlpha, kSV_RT0DestOne };
    static const uint8_t kExtraForward[]  = { kSV_FwdDestAlpha, kSV_FwdDestOne, kSV_FwdOneInv };

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
        kG_Deferred = 0,     // deferred, deferredbs, imposterdeferred
        kG_DeferredClip,     // deferredalphaclip
        kG_Shadow,           // paraboloid + the named shadow techniques
        kG_Reflection,       // reflection
        kG_Forward,          // wd*, lightweight*, imposter, bs, default, unlit
        kG_Named,            // everything reached only by LookupTechnique: post-fx, lighting, blits
        kG_Count
    };

    inline GroupClass ClassifyTechnique(const char* name)
    {
        if (!name) return kG_Named;
        auto is = [&](const char* g) {
            size_t n = strlen(g);
            if (strncmp(name, g, n) != 0) return false;
            const char* rest = name + n;
            return strcmp(rest, "_draw") == 0 || strcmp(rest, "_drawskinned") == 0 ||
                   strcmp(rest, "_drawblit") == 0;
        };
        if (strcmp(name, "draw") == 0 || strcmp(name, "drawskinned") == 0 || strcmp(name, "drawblit") == 0)
            return kG_Forward;                       // the group literally named "default"
        if (is("deferredalphaclip"))                  return kG_DeferredClip;
        if (is("deferred") || is("deferredbs") || is("imposterdeferred")) return kG_Deferred;
        if (is("paraboloid"))                         return kG_Shadow;
        if (is("reflection"))                         return kG_Reflection;
        if (is("wd") || is("wd_masked") || is("wd_local") || is("wd_local_masked") ||
            is("lightweight0") || is("lightweight4") || is("imposter") || is("bs") || is("unlit"))
            return kG_Forward;
        // Instancing rides the forward scene set: DrawInstancedBatch forces no group.
        if (strcmp(name, "draw_inst") == 0 || strcmp(name, "unlit_draw_inst") == 0) return kG_Forward;
        return kG_Named;
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
        // projected onto, and the sampler slots the PS declares with their
        // texture dimension (0 = not declared).
        std::vector<std::pair<uint8_t, uint8_t>> sig;
        uint8_t  psDim[16]{};
        uint8_t  group = kG_Named;
        uint16_t effect = 0;                // index into effectNames
    };

    // Job::flags
    enum : uint8_t
    {
        kJF_AlphaTest = 0x01,   // cross D3DRS_ALPHATESTENABLE on
        kJF_TopoMask  = 0x06,   // 0 = list, 1 = strip, 2 = fan (shifted by 1)
        kJF_TopoShift = 1,
        kJF_ClipPlane = 0x08,   // one user clip plane enabled (DXVK spec constant 0)
    };

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
        uint32_t rank;      // ascending draw order
    };

    struct Census
    {
        uint32_t effects = 0, techniques = 0, passes = 0, vs = 0, ps = 0;
        uint32_t effectsJoined = 0, techniquesJoined = 0;
        uint32_t techByIndex = 0, techByHash = 0, techUnmatched = 0;
        uint32_t phases = 0, phaseTargets = 0, namedTargets = 0;
        uint32_t contextsFromPhases = 0, contextsDerived = 0, contextsShipped = 0;
        uint32_t phaseWaitMs = 0;
        uint32_t declsShipped = 0, declsLive = 0, declsTotal = 0;
        uint32_t liveRegistryEntries = 0;
        uint32_t jobsEmitted = 0, jobsDeduped = 0, jobsCapped = 0;
        uint32_t shaderObjects = 0, shaderRead = 0, shaderFailed = 0, passesDropped = 0;
        uint32_t projectionsPerPass100 = 0;   // mean x100
    };

    struct Plan
    {
        bool ok = false;
        std::string why;                 // why it is off, when it is
        std::vector<PassInfo> passes;
        std::vector<Context>  contexts;
        std::vector<Decl>     decls;
        std::vector<Job>      jobs;
        std::vector<uint32_t> stateWords;   // (key, value) pairs, PassInfo::stateOff
        std::vector<std::string> effectNames;
        uint32_t stateToRS[kRageStates]{};
        uint32_t defaultRS[kRageStates]{};
        uint32_t liveState[0x25]{};
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
    //  Axis 1 — walk the render-phase list for every target set that exists.
    // ---------------------------------------------------------------------
    inline D3DFORMAT TargetFormat(uintptr_t rt, uint32_t& msType, uint32_t& msQuality, std::string& name)
    {
        msType = msQuality = 0;
        if (!rt) return D3DFMT_UNKNOWN;
        uintptr_t nameP = 0;
        if (Peek(rt + kRT_Name, nameP) && nameP) PeekName(nameP, name);

        // The authoritative format is the texture's, NOT the rage format byte:
        // rage 14 says D24S8 but the texture is always created as INTZ.
        IDirect3DTexture9* tex = nullptr;
        if (!Peek(rt + kRT_Tex, tex) || !tex) return D3DFMT_UNKNOWN;
        D3DSURFACE_DESC sd{};
        IDirect3DSurface9* surf = nullptr;
        if (FAILED(tex->GetSurfaceLevel(0, &surf)) || !surf) return D3DFMT_UNKNOWN;
        HRESULT hr = surf->GetDesc(&sd);
        surf->Release();
        if (FAILED(hr)) return D3DFMT_UNKNOWN;
        msType = (uint32_t)sd.MultiSampleType;
        msQuality = sd.MultiSampleQuality;
        return sd.Format;
    }

    inline bool AddContext(Plan& p, std::unordered_set<uint64_t>& seen, const Context& c)
    {
        if (c.mrt == 0) return false;
        uint64_t key = pipelinekeys::Fnv1a(c.color, sizeof(c.color));
        key = pipelinekeys::Fnv1a(&c.depth, sizeof(c.depth), key);
        key = pipelinekeys::Fnv1a(&c.msType, sizeof(c.msType), key);
        if (!seen.insert(key).second) return false;
        p.contexts.push_back(c);
        return true;
    }

    inline void EnumContexts(Plan& p, D3DFORMAT backBuffer)
    {
        std::unordered_set<uint64_t> seen;
        std::vector<D3DFORMAT> singleColour;   // every colour format the engine renders to
        D3DFORMAT engineDepth = D3DFMT_UNKNOWN;

        auto noteColour = [&](D3DFORMAT f) {
            if (f == D3DFMT_UNKNOWN) return;
            if (std::find(singleColour.begin(), singleColour.end(), f) == singleColour.end())
                singleColour.push_back(f);
        };

        // The MRT3 trap: the G-buffer phase struct lists _DEFERRED_GBUFFER_3_
        // (INTZ) in COLOUR slot 3, but ragePhase_GBuffer_RT_PreDrawList_CB
        // (0x00AD2280) binds _STENCIL_BUFFER_ there. Taking the struct at face
        // value emits a coordinate the game never draws; this substitution is
        // load-bearing and is keyed on the target POINTER, not on a vtable, so
        // it survives a phase-list reorder.
        uintptr_t gbuf3 = 0, stencil = 0;
        Peek(Rebase(kVA_GBuffer3), gbuf3);
        Peek(Rebase(kVA_StencilBuffer), stencil);

        // A phase list we cannot read is not fatal: the named targets below
        // still give the single-colour sets, which is most of the coverage.
        uintptr_t listBase = 0, arr = 0;
        uint16_t count = 0;
        if (Peek(Rebase(kVA_PhaseList), listBase) && listBase)
        {
            uintptr_t a = 0; uint16_t n = 0;
            if (Peek(listBase + 0x38, a) && Peek<uint16_t>(listBase + 0x3C, n) && a && n && n <= 256)
            { arr = a; count = n; }
        }
        p.census.phases = count;

        for (uint16_t i = 0; i < count; i++)
        {
            uintptr_t phase = 0;
            if (!Peek(arr + i * 4, phase) || !phase) continue;
            if (!Readable((const void*)(phase + kPhase_Slot0), 4 * kPhase_SlotStride)) continue;

            Context c;
            for (int s = 0; s < 4; s++)
            {
                uintptr_t col = 0, dep = 0;
                Peek(phase + kPhase_Slot0 + s * kPhase_SlotStride, col);
                Peek(phase + kPhase_Slot0 + s * kPhase_SlotStride + kPhase_DepthDelta, dep);
                if (col && gbuf3 && stencil && col == gbuf3 && s == 3) col = stencil;

                uint32_t mt = 0, mq = 0; std::string n;
                D3DFORMAT f = TargetFormat(col, mt, mq, n);
                if (f != D3DFMT_UNKNOWN)
                {
                    c.color[s] = f;
                    if (s + 1 > c.mrt) c.mrt = s + 1;
                    if (c.name.empty()) c.name = n;
                    if (mt) { c.msType = mt; c.msQuality = mq; }
                    noteColour(f);
                    p.census.phaseTargets++;
                }
                uint32_t dmt = 0, dmq = 0; std::string dn;
                D3DFORMAT df = TargetFormat(dep, dmt, dmq, dn);
                if (df != D3DFMT_UNKNOWN && c.depth == D3DFMT_UNKNOWN)
                {
                    c.depth = df;
                    if (engineDepth == D3DFMT_UNKNOWN) engineDepth = df;
                    if (dmt) { c.msType = dmt; c.msQuality = dmq; }
                    p.census.phaseTargets++;
                }
            }
            c.fromPhase = true;
            AddContext(p, seen, c);
        }
        p.census.contextsFromPhases = (uint32_t)p.contexts.size();

        // The phase list is not the whole picture: the post-fx phases bind
        // their targets inside DrawList, and the shadow phases declare only the
        // point pair. So also read the creators' own named globals, and form
        // the single-colour sets from every colour format the engine renders
        // to -- with and without depth, since a post-fx step usually has none.
        //
        // Measured against this rig's own 14543-key recording: the phase walk
        // alone reaches the two dominant sets (10718 keys, 74 %); adding these
        // reaches 11 of the 14 sets the recording holds (~96 % of its keys).
        // What it still misses is fp16+A8R8G8B8 and A32B32G32R32F x2, both
        // post-fx MRT pairs that only the DrawList-bound targets would give.
        for (uintptr_t va : kVA_NamedTargets)
        {
            uintptr_t rt = 0;
            if (!Peek(Rebase(va), rt) || !rt) continue;
            uint32_t mt = 0, mq = 0; std::string n;
            D3DFORMAT f = TargetFormat(rt, mt, mq, n);
            if (f == D3DFMT_UNKNOWN) continue;
            p.census.namedTargets++;
            if (f == (D3DFORMAT)FOURCC_INTZ_EW || f == D3DFMT_D24S8)
            {
                if (engineDepth == D3DFMT_UNKNOWN) engineDepth = f;
                continue;   // a depth target is not a colour format
            }
            noteColour(f);
        }
        if (backBuffer != D3DFMT_UNKNOWN) noteColour(backBuffer);

        if (engineDepth == D3DFMT_UNKNOWN) engineDepth = (D3DFORMAT)FOURCC_INTZ_EW;
        for (D3DFORMAT f : singleColour)
        {
            Context a; a.mrt = 1; a.color[0] = f; a.depth = engineDepth; a.name = "derived+depth";
            AddContext(p, seen, a);
            Context b; b.mrt = 1; b.color[0] = f; b.depth = D3DFMT_UNKNOWN; b.name = "derived";
            AddContext(p, seen, b);
        }
        p.census.contextsDerived = (uint32_t)p.contexts.size() - p.census.contextsFromPhases;

        // The fallback layer. These are SHIPPED CONSTANTS, not engine reads,
        // and the log says so: they exist because the gate can be earlier than
        // phase creation, and a walk that only ever saw the back-buffer format
        // would warm one target set out of the eleven the game uses. They are
        // the same sets FusionFix's own coverage table has always carried,
        // cross-checked against this rig's 14543-key recording: the phase walk
        // plus these reach 11 of the 14 sets it holds, 99 % of its keys. Any
        // set the engine really did declare is already in the list above and is
        // not duplicated here.
        struct FB { int mrt; D3DFORMAT c0, c1, c2, c3, d; };
        static const FB kFallback[] = {
            { 1, D3DFMT_A16B16G16R16F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, (D3DFORMAT)FOURCC_INTZ_EW },
            { 4, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R16F,       (D3DFORMAT)FOURCC_INTZ_EW },
            { 1, D3DFMT_G16R16F,       D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, (D3DFORMAT)FOURCC_INTZ_EW },
            { 1, D3DFMT_R32F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, (D3DFORMAT)FOURCC_INTZ_EW },
            { 1, D3DFMT_R16F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, (D3DFORMAT)FOURCC_INTZ_EW },
            { 1, D3DFMT_A16B16G16R16F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 1, D3DFMT_R32F,          D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 1, D3DFMT_G16R16F,       D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 1, D3DFMT_L8,            D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 1, D3DFMT_A2B10G10R10,   D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 1, D3DFMT_A8B8G8R8,      D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN },
            { 2, D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_UNKNOWN, D3DFMT_UNKNOWN, (D3DFORMAT)FOURCC_INTZ_EW },
        };
        for (const FB& f : kFallback)
        {
            Context c;
            c.mrt = f.mrt; c.color[0] = f.c0; c.color[1] = f.c1; c.color[2] = f.c2; c.color[3] = f.c3;
            c.depth = f.d; c.name = "shipped";
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
    inline bool Project(const std::vector<std::pair<uint8_t, uint8_t>>& sig, const Decl& d,
                        uint64_t& out, uint32_t& matched)
    {
        struct Slot { uint8_t stream, type; uint16_t offset; } slots[32]{};
        matched = 0;
        const size_t n = (std::min)(sig.size(), (size_t)32);
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
        out = pipelinekeys::Fnv1a(slots, n * sizeof(Slot));
        return matched == n;
    }

    // ---------------------------------------------------------------------
    //  The coordinate tie: which (context, state) pairs a group can be drawn
    //  at. Index 0 is the primary; the rest are level-2 extras.
    // ---------------------------------------------------------------------
    struct Coord { int ctxKind; uint8_t state; };
    // ctxKind selects a context by shape rather than by index, because which
    // phases exist depends on the game state at the gate. kCtxAllSingle is not
    // a shape but an instruction: emit at EVERY single-colour context, which is
    // what the post-fx / lighting / blit passes need -- their target is
    // whatever step of the chain they are, and the chain visits most of the
    // engine's formats.
    enum { kCtxGBuffer = 0, kCtxScene, kCtxShadow, kCtxWaterSurface, kCtxOther };
    constexpr int kCtxAllSingle = -2;

    inline int ContextKind(const Context& c)
    {
        if (c.mrt >= 3) return kCtxGBuffer;
        if (c.mrt == 2 && c.depth == D3DFMT_UNKNOWN) return kCtxWaterSurface;
        if (c.mrt == 1 && c.color[0] == D3DFMT_A16B16G16R16F) return kCtxScene;
        if (c.mrt == 1 && (c.color[0] == D3DFMT_G16R16F || c.color[0] == D3DFMT_R32F ||
                           c.color[0] == D3DFMT_R16F))
            return kCtxShadow;
        return kCtxOther;
    }

    // Which (context kind, state vector) pairs each group is emitted at. The
    // first entry of a row is the primary coordinate -- the one gameplay
    // reaches on the common path -- and is emitted at every level; the rest
    // are the level-2 extras (the depth-only prepass, the RT0-only forward
    // pass, and the three transparency blend vectors).
    static const Coord kTie[kG_Count][4] = {
        /* deferred      */ { { kCtxGBuffer, kSV_GBufStd   }, { kCtxShadow, kSV_DepthOnly }, { kCtxGBuffer, kSV_DepthOnly }, { -1, 0 } },
        /* deferredclip  */ { { kCtxGBuffer, kSV_GBufClip  }, { kCtxShadow, kSV_DepthOnly }, { -1, 0 },                      { -1, 0 } },
        /* shadow        */ { { kCtxShadow,  kSV_DepthOnly }, { kCtxShadow, kSV_ForwardRT0 }, { -1, 0 },                     { -1, 0 } },
        /* reflection    */ { { kCtxScene,   kSV_Forward   }, { kCtxScene,  kSV_FwdBlend  }, { -1, 0 },                      { -1, 0 } },
        /* forward       */ { { kCtxScene,   kSV_Forward   }, { kCtxScene,  kSV_FwdBlend  }, { kCtxScene, kSV_FwdAdd },      { kCtxScene, kSV_ForwardRT0 } },
        /* named         */ { { kCtxAllSingle, kSV_Forward }, { kCtxAllSingle, kSV_FwdBlend }, { kCtxScene, kSV_Forward }, { -1, 0 } },
    };

    // Render states a pass's own PASS_VALUE delta can move that DXVK bakes
    // into the pipeline. Used only to fingerprint the delta for dedup -- the
    // draw applies every entry of the delta, exactly as grcEffectPass::Apply
    // does (01-effects §4.3).
    inline bool IsPipelineRS(uint32_t rs)
    {
        switch (rs)
        {
        case D3DRS_ZWRITEENABLE: case D3DRS_ALPHATESTENABLE: case D3DRS_SRCBLEND:
        case D3DRS_DESTBLEND: case D3DRS_ALPHAREF: case D3DRS_ALPHAFUNC:
        case D3DRS_ALPHABLENDENABLE: case D3DRS_BLENDOP: case D3DRS_COLORWRITEENABLE:
        case D3DRS_COLORWRITEENABLE1: case D3DRS_COLORWRITEENABLE2: case D3DRS_COLORWRITEENABLE3:
        case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA:
        case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
        case D3DRS_FOGENABLE: case D3DRS_CLIPPLANEENABLE:
            return true;
        default:
            return false;
        }
    }

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
        uintptr_t extra = 0;
        if (Peek(Rebase(kVA_EffectExtra), extra) && extra &&
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
    //  Build the cross product that can actually occur.
    // ---------------------------------------------------------------------
    inline void BuildJobs(Plan& p, int level)
    {
        // Resolve each context kind to the contexts that HAVE that shape --
        // plural, because keeping only the first silently dropped the second
        // shadow format. kCtxShadow matches G16R16F, R32F and R16F alike, and
        // R32F+INTZ is 767 keys and 48.2 M draws in this rig's recording, the
        // second-heaviest set in the game; with one context per kind the
        // cascade-shadow techniques were never warmed at it at all.
        //
        // Ordered so a cap sheds the least likely: what a render phase itself
        // declared, then what was derived from a live engine target, then the
        // shipped fallbacks, and within each the ones that carry depth (most of
        // the game's draws do). A kind with no context of its own falls back to
        // the scene set, which every install has.
        const size_t ctxPerKind = (level >= 2) ? 6 : 3;
        std::vector<int> byKind[kCtxOther + 1];
        {
            std::vector<int> order(p.contexts.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const Context& A = p.contexts[(size_t)a];
                const Context& B = p.contexts[(size_t)b];
                if (A.fromPhase != B.fromPhase) return A.fromPhase;
                const bool as = A.name == "shipped", bs = B.name == "shipped";
                if (as != bs) return bs;
                const bool ad = A.depth != D3DFMT_UNKNOWN, bd = B.depth != D3DFMT_UNKNOWN;
                if (ad != bd) return ad;
                return a < b;
            });
            for (int i : order)
            {
                std::vector<int>& v = byKind[ContextKind(p.contexts[(size_t)i])];
                if (v.size() < ctxPerKind) v.push_back(i);
            }
            if (byKind[kCtxScene].empty() && !order.empty()) byKind[kCtxScene].push_back(order[0]);
            for (int k = 0; k <= kCtxOther; k++)
                if (byKind[k].empty()) byKind[k] = byKind[kCtxScene];
        }

        // The single-colour contexts a post-fx / lighting / blit pass is drawn
        // at, taken from the per-kind lists so every kind is represented before
        // any kind gets a second, and capped so neither level can multiply the
        // whole walk by however many formats happen to exist. Eight covers the
        // eight heaviest single-colour sets this rig's recording holds.
        const size_t singleCap = (level >= 2) ? 12 : 8;
        std::vector<int> singles;
        for (size_t round = 0; round < ctxPerKind; round++)
            for (int k = 0; k <= kCtxOther; k++)
                if (round < byKind[k].size())
                {
                    const int i = byKind[k][round];
                    if (p.contexts[(size_t)i].mrt == 1 &&
                        std::find(singles.begin(), singles.end(), i) == singles.end() &&
                        singles.size() < singleCap)
                        singles.push_back(i);
                }
        if (singles.empty() && !p.contexts.empty()) singles.push_back(byKind[kCtxScene][0]);

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
        const uint32_t declCap = (uint32_t)p.decls.size();
        const int coordCap = (level >= 2) ? 4 : 1;

        // A hard stop, so a plan that goes wrong cannot submit a million draws.
        // It STOPS the walk rather than skipping a push_back: the dedup set is
        // the thing that grows without bound otherwise, and in a 32-bit process
        // that already holds GTA IV's heap that matters more than the jobs do.
        // Jobs are ordered, so a cut sheds the speculative end.
        constexpr size_t kMaxJobs = 150000;
        bool capped = false;

        std::unordered_set<uint64_t> seen;
        seen.reserve(p.passes.size() * 8);
        uint64_t projTotal = 0;

        // The coordinates one pass may be drawn at, reused. `extra` carries the
        // axes that are added AS a coordinate rather than crossed with every
        // other one, because crossing them would multiply the whole walk.
        struct CS { int ctx; uint8_t sv; uint8_t extra; bool primary; };
        std::vector<CS> coords;

        for (uint32_t pi = 0; pi < p.passes.size() && !capped; pi++)
        {
            const PassInfo& pass = p.passes[pi];

            // The pass's own state delta forks the pipeline too, so it is part
            // of the identity even though it is not part of the coordinate.
            uint64_t deltaHash = 1469598103934665603ull;
            for (uint16_t s = 0; s < pass.stateCount; s++)
            {
                const uint32_t key = p.stateWords[pass.stateOff + s * 2];
                const uint32_t val = p.stateWords[pass.stateOff + s * 2 + 1];
                if (key < kRageStates && RSReaches(p.stateToRS[key]) && IsPipelineRS(p.stateToRS[key]))
                {
                    deltaHash = pipelinekeys::Fnv1a(&p.stateToRS[key], 4, deltaHash);
                    deltaHash = pipelinekeys::Fnv1a(&val, 4, deltaHash);
                }
            }

            // Which declarations can feed this VS: one per distinct projection,
            // preferring the ones that satisfy the signature outright.
            const std::vector<std::pair<uint8_t, uint8_t>>* sig = pass.sig.empty() ? nullptr : &pass.sig;

            std::vector<uint32_t> useDecls;
            if (sig && !sig->empty())
            {
                std::unordered_set<uint64_t> projSeen;
                uint32_t bestPartial = 0xFFFFFFFFu, bestMatched = 0;
                for (uint32_t oi = 0; oi < declCap; oi++)
                {
                    const uint32_t di = declOrder[oi];
                    uint64_t proj = 0; uint32_t matched = 0;
                    const bool full = Project(*sig, p.decls[di], proj, matched);
                    if (!full)
                    {
                        if (matched > bestMatched) { bestMatched = matched; bestPartial = di; }
                        continue;
                    }
                    if (projSeen.insert(proj).second) useDecls.push_back(di);
                }
                // One VS input signature in 47 is satisfied by no declaration
                // that exists (07-geometry's open question). Those shaders are
                // drawn with a null binding in gameplay, so the closest
                // declaration is still the right pipeline to warm.
                if (useDecls.empty() && bestPartial != 0xFFFFFFFFu) useDecls.push_back(bestPartial);
            }
            if (useDecls.empty())
                useDecls.push_back(declOrder.empty() ? 0u : declOrder[0]);
            projTotal += useDecls.size();

            coords.clear();
            const Coord* row = kTie[pass.group];
            for (int ci = 0; ci < coordCap; ci++)
            {
                if (row[ci].ctxKind == -1) break;

                // A post-fx / lighting / blit pass is emitted at every
                // single-colour context; everything else at every context that
                // has its group's shape.
                if (row[ci].ctxKind == kCtxAllSingle)
                    for (int q : singles) coords.push_back({ q, row[ci].state, 0, ci == 0 });
                else
                    for (int q : byKind[row[ci].ctxKind]) coords.push_back({ q, row[ci].state, 0, ci == 0 });
            }
            if (coords.empty()) continue;

            // The mined write-mask/blend extras, at the first coordinate only.
            if (level >= 2)
            {
                const uint8_t* ex = nullptr; size_t exN = 0;
                if (pass.group == kG_Deferred || pass.group == kG_DeferredClip)
                { ex = kExtraDeferred; exN = sizeof(kExtraDeferred); }
                else if (pass.group == kG_Named)
                { ex = kExtraNamed; exN = sizeof(kExtraNamed); }
                else if (pass.group == kG_Forward || pass.group == kG_Reflection)
                { ex = kExtraForward; exN = sizeof(kExtraForward); }
                const int at = coords[0].ctx;
                for (size_t e = 0; e < exN; e++) coords.push_back({ at, ex[e], 0, false });
            }

            // One user clip plane. DXVK packs the number of ENABLED, non-zero
            // clip planes into spec constant 0 (UpdateClipPlanes ->
            // setClipPlaneCount), so a vertex shader compiled with none is a
            // different pipeline from the same shader compiled with one. The
            // reflection, mirror and water phases run with one in gameplay:
            // 473 of this rig's 14543 keys and 4.75 M draws carry
            // CLIPPLANEENABLE = 1, 443 of them at the scene set. Added as one
            // extra coordinate at the primary rather than crossed with
            // everything, because it is one bit of a spec constant.
            if (pass.group == kG_Forward || pass.group == kG_Reflection || pass.group == kG_Named)
                coords.push_back({ coords[0].ctx, coords[0].sv, kJF_ClipPlane, false });

            for (const CS& cs : coords)
            {
                const int ctx = cs.ctx;
                const Context& C = p.contexts[(size_t)ctx];
                const StateVec& SV = kStateVecs[cs.sv];

                // Level 2 crosses alpha test 0/1: 724 passes set it, but the
                // material overrides the pass's value in 13.5 % of draws, so
                // the pass's own value is not a reliable read (README §3.1).
                const int alphaN = (level >= 2 && cs.sv != kSV_DepthOnly) ? 2 : 1;
                // Topology bakes into the pipeline: VK_DYNAMIC_STATE_PRIMITIVE_
                // TOPOLOGY is not among the dynamic states DXVK 3.1.1 uses
                // (dxvk_graphics.cpp:774-808), so list, strip and fan are three
                // pipelines for one shader pair. 1537 of this rig's keys are
                // strips and 121 fans -- 494 of its 5445 replay identities, and
                // only 27 identities occur in more than one topology, so these
                // really are pipelines nothing else warms. The blit / post-fx /
                // lighting passes the engine reaches only by name are where
                // they live (04-poc §3.5), so those get all three at every
                // coordinate and everything else gets the second only at level 2.
                const int topoN = (pass.group == kG_Named) ? 3 : (level >= 2 ? 2 : 1);
                for (int a = 0; a < alphaN; a++)
                for (int tp = 0; tp < topoN; tp++)
                {
                    for (uint32_t di : useDecls)
                    {
                        uint64_t proj = 0; uint32_t matched = 0;
                        if (sig && !sig->empty()) Project(*sig, p.decls[di], proj, matched);
                        else proj = DeclHash(p.decls[di].elems);

                        struct Id
                        {
                            void* vs; void* ps; uint64_t proj, delta;
                            uint32_t color[4], depth, ms, msq;
                            DWORD cwe[4]; uint32_t blend[4];
                            uint32_t alpha, zw, topo, inst, clip;
                        } id;
                        memset(&id, 0, sizeof(id));
                        id.vs = pass.vs; id.ps = pass.ps; id.proj = proj; id.delta = deltaHash;
                        id.topo = (uint32_t)tp; id.inst = p.decls[di].instanced ? 1u : 0u;
                        id.clip = (cs.extra & kJF_ClipPlane) ? 1u : 0u;
                        id.depth = (uint32_t)C.depth; id.ms = C.msType; id.msq = C.msQuality;
                        id.alpha = (a ? 1u : (SV.alphaTest ? 1u : 0u)); id.zw = SV.zWrite;
                        for (int k = 0; k < 4; k++)
                        {
                            id.color[k] = (uint32_t)C.color[k];
                            id.cwe[k]   = SV.cwe[k];
                            // Normalise the blend triple the way DxvkBlendMode::
                            // normalize does before it reaches the pipeline key:
                            // blending that passes the source through, or that
                            // writes no channel, is recorded as no blending at
                            // all. Without this, two vectors that DXVK compiles
                            // to one pipeline are two jobs and one wasted draw.
                            const bool passthrough = SV.srcBlend == D3DBLEND_ONE &&
                                                     SV.dstBlend == D3DBLEND_ZERO &&
                                                     SV.blendOp  == D3DBLENDOP_ADD;
                            if (SV.blendEnable && !passthrough && (SV.cwe[k] & 0xF))
                                id.blend[k] = 1u | ((uint32_t)SV.srcBlend << 1) |
                                              ((uint32_t)SV.dstBlend << 9) |
                                              ((uint32_t)SV.blendOp  << 17);
                        }

                        if (!seen.insert(pipelinekeys::Fnv1a(&id, sizeof(id))).second) { p.census.jobsDeduped++; continue; }
                        if (p.jobs.size() >= kMaxJobs) { p.census.jobsCapped++; capped = true; break; }

                        Job j;
                        j.pass = pi;
                        j.decl = (uint16_t)di;
                        j.ctx = (uint8_t)ctx;
                        j.state = cs.sv;
                        j.flags = (uint8_t)((a ? kJF_AlphaTest : 0) |
                                            ((uint8_t)tp << kJF_TopoShift) | cs.extra);
                        // Ascending: primary coordinates first, then by how
                        // much the declaration is drawn, then by context and
                        // state. A budget cut therefore sheds the speculative
                        // end rather than an arbitrary tail.
                        j.rank = ((uint32_t)((cs.primary && !tp && !cs.extra) ? 0u : 1u) << 30) |
                                 ((declRank[di] & 0x3FFu) << 20) |
                                 (((uint32_t)ctx & 0x1Fu) << 15) |
                                 (((uint32_t)cs.sv & 0x1Fu) << 10) |
                                 ((uint32_t)a << 9) |
                                 (pi & 0x1FFu);
                        p.jobs.push_back(j);
                    }
                    if (capped) break;
                }
                if (capped) break;
            }
        }
        p.census.jobsEmitted = (uint32_t)p.jobs.size();
        p.census.projectionsPerPass100 = p.passes.empty() ? 0
            : (uint32_t)((projTotal * 100) / p.passes.size());

        std::stable_sort(p.jobs.begin(), p.jobs.end(), [](const Job& a, const Job& b) { return a.rank < b.rank; });
    }

    // Two calls, not one. Between them the caller reads every pass's shader
    // objects with GetFunction -- the VS input signature and the PS sampler
    // declarations -- and AddRefs the objects it will bind, so the walk never
    // depends on a .fxc join by array index and never holds a pointer it has
    // not proved is a live shader. See EngineWarmPass in shaderprecompile.ixx.
    inline void BuildTables(Plan& p, fxc_db* db, D3DFORMAT backBuffer)
    {
        if (!ResolveTables(p)) return;
        EnumPasses(p, db);
        if (p.passes.empty()) { p.why = "RAGE's effect registry holds no usable pass"; return; }
        EnumContexts(p, backBuffer);
        if (p.contexts.empty()) { p.why = "the render-phase list declares no render target"; return; }
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
}

