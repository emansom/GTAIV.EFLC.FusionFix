#pragma once
// ===========================================================================
//  bootgate.h — WHERE in GTA IV's boot cycle the warm pass runs, and how it
//  shows itself inside the game's OWN loading screen.
//
//  WHY THIS EXISTS. The warm pass used to fire on the first loading-screen
//  render (0x005CC760). That is the LEGALS screen at t+2.2 s, before
//  CGame_InitSystems has finished: no render phase exists, no render target
//  has a format, and the pass drew the game's real shaders through a shipped
//  constant table instead of the engine's own frame. Its own log said so --
//  "engine: 0 render phases after waiting 2003 ms".
//
//  The measurement that moves the gate (re/rage-shader-precompile/08-boot-gate.md):
//
//    * ragePhase_CreateAllPhasesAndTargets 0x00B00B60 runs ONCE, at t+12.7 s,
//      0.7 s AFTER the game tore its own loading screen down and 0.03 s before
//      the first scene frame. No frame is built during the episode load at all,
//      so it is not reachable from any loading screen.
//    * But the viewport tick's guard is `word[viewport+0x404] == 0`, so a mod
//      may call rageBoot_CViewportGame_CreateRenderPhases 0x005D49D0 ITSELF at
//      a gate of its choosing; the engine then skips its own build two seconds
//      later. Verified over three boots: no 0x00B00B60 afterwards, and the
//      frame list still reaches 29 phases at the first DrawScene.
//    * The gate is the RETURN of rageBoot_InitSession 0x005C1360 -- one caller,
//      main thread, once per session start, after the frontend AND the
//      episode/DLC flow, with the game's own loading screen up. The very next
//      instruction is the CALL that ends that loading screen.
//    * While a loading screen is up the render thread FREE-RUNS it (450,511
//      ticks over a 60 s hold, artwork advancing, simulation frozen), so the
//      screen stays alive and animated for as long as the main thread is held
//      there -- and only there.
//
//  WHAT THIS HEADER OWNS
//    * every address the gate needs, and a fingerprint for each;
//    * the one-byte pin over rageBoot_LoadscreenEnd that keeps the game's own
//      loading screen up while we work (balanced: the function is
//      __fastcall(char) with no stack argument and a plain RET);
//    * asking the engine to build its render phases, and reading them back;
//    * the render-target registry the texture factory keeps, which is where the
//      format axis really lives;
//    * drawing progress INSIDE the game's loading screen with the loading
//      screen's OWN untextured quad -- CFont faults at this gate because the
//      font textures are unloaded between the frontend and the first gameplay
//      frame (bisected live, step 11 of 12).
//
//  IT WRITES EXACTLY ONE BYTE OF ENGINE MEMORY (the pin) and restores it under
//  a scope guard on every path. Everything else is a read or a call into the
//  engine's own code with the engine's own argument.
//
//  TIMING RULE. 0x00401000-0x004FB000 is SecuROM-encrypted until the game
//  unpacks itself, so nothing there may be read at ASI load. Every address
//  below is above 0x004FB000 and is safe to fingerprint at load time; the
//  DATA (viewport, registry, phases) is only meaningful at the gate.
// ===========================================================================

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace bootgate
{
    // ---------------------------------------------------------------------
    //  Addresses (GTA IV 1.2.0.59, image base 0x400000).
    // ---------------------------------------------------------------------
    constexpr uintptr_t kVA_InitSession      = 0x005C1360;  // THE GATE, __fastcall(char)
    constexpr uintptr_t kVA_InitSessionCall  = 0x005C1B4C;  // its one caller
    constexpr uintptr_t kVA_LoadscreenEnd    = 0x005CDE90;  // __fastcall(char), pinned with 0xC3
    constexpr uintptr_t kVA_LoadscreenTick   = 0x005CC760;  // render-thread loadscreen tick
    constexpr uintptr_t kVA_LoadscreenLayers = 0x005CC180;  // the art layers, twice per drawn frame
    constexpr uintptr_t kVA_CreatePhases     = 0x005D49D0;  // CViewportGame vtable slot 12
    constexpr uintptr_t kVA_ViewportVT       = 0x00FE0A44;  // that vtable
    constexpr uintptr_t kVA_PumpOsAndOverlay = 0x005C31F0;  // the OS/overlay pump the legals spin uses
    // The three bytes rageBoot_PumpOsAndOverlay's own message pump (FUN_00420E90)
    // tests before it decides to STOP RETURNING. Read, never written; see PumpOs.
    constexpr uintptr_t kVA_PumpSpinA        = 0x017ED8D1;
    constexpr uintptr_t kVA_PumpSpinB        = 0x0105B48F;
    constexpr uintptr_t kVA_AppActive        = 0x017ACCF8;
    constexpr uintptr_t kVA_SpriteSetTexture = 0x008D4CB0;
    constexpr uintptr_t kVA_SpriteQuad       = 0x008D4990;  // untextured quad, PIXEL coords
    constexpr uintptr_t kVA_SpriteFlush      = 0x008D3C20;

    // CFont, in the order rageBoot_LoadscreenDrawLegendText 0x005CD010 calls it.
    // Two of these were named wrongly by the first pass over that function and
    // the mistake matters: 0x00924610 is the WRAP BOX, not the scale, and
    // 0x00924500 is the scale, not an edge. 0x009222A0 settles it -- it reads
    // the pair at +0x38/+0x3C as the left and right bounds of the text box and
    // the align field at +0x28 decides which of them the x is measured from.
    constexpr uintptr_t kVA_FontSetStyle  = 0x00924390;  // __cdecl(int)  0 = font1, the UI face
    constexpr uintptr_t kVA_FontSetProp   = 0x009244E0;  // __cdecl(int)  proportional
    constexpr uintptr_t kVA_FontSetSlant  = 0x00924310;  // __cdecl(float) the legend passes 0
    constexpr uintptr_t kVA_FontSetAlign  = 0x009244A0;  // __cdecl(int) 0 centre 1 left 2 RIGHT 3 justify
    constexpr uintptr_t kVA_FontSetColour = 0x009241F0;  // __cdecl(u32 argb)
    constexpr uintptr_t kVA_FontSetBox    = 0x00924610;  // __cdecl(float left, float right), normalised
    constexpr uintptr_t kVA_FontSetScale  = 0x00924500;  // __cdecl(float sx, float sy)
    constexpr uintptr_t kVA_FontPrint     = 0x00921DA0;  // __cdecl(float x, float y, const wchar_t*, -1, -1)
    constexpr uintptr_t kVA_FontFlush     = 0x00923950;  // __cdecl()
    constexpr uintptr_t kVA_WidescreenFix = 0x008FC260;  // __cdecl(7, 0, float2*, 0) -- aspect-correct a scale
    // CFont's four resolved font textures (0x0091F0F0 / 0x00922BB0):
    // 0 font1, 1 font3, 2 font4 (Japanese builds only), 3 the streamed font2.
    // A null here is exactly what faulted the first attempt at native text at
    // this gate, so it is tested rather than assumed.
    constexpr uintptr_t kVA_FontTextures  = 0x011956B8;
    constexpr uint32_t  kFontStride       = 0x258;

    constexpr uintptr_t kVA_Viewports[3]     = { 0x0118D800, 0x0118D804, 0x0118D808 };
    constexpr uintptr_t kVA_ScreenInfo       = 0x017F5838;  // +0x2B0/+0x2B4 int x +0x288/+0x28C float
    constexpr uintptr_t kVA_LoadscreenShown  = 0x018B6F2E;
    constexpr uintptr_t kVA_AllRenderTargets = 0x018E0108;  // grcRenderTarget*[512]
    constexpr uintptr_t kVA_AllRenderTargetN = 0x018E0908;  // int count
    constexpr uintptr_t kVA_RageFormatToD3D  = 0x01111A28;  // u32[19]
    constexpr uintptr_t kVA_GBuffer3         = 0x0154E17C;  // _DEFERRED_GBUFFER_3_ (the depth)
    constexpr uintptr_t kVA_StencilBuffer    = 0x0154E180;  // _STENCIL_BUFFER_ (the REAL colour 3)

    // Viewport layout (08-boot-gate §2.3).
    constexpr uint32_t kVP_Phases = 0x400, kVP_PhaseCount = 0x404;
    // Render phase: the +0x890 block is four (colour, depth) pairs, stride 0x10.
    constexpr uint32_t kPh_Slot0 = 0x890, kPh_SlotStride = 0x10, kPh_DepthDelta = 0x08;
    // grcRenderTargetPC.
    constexpr uint32_t kRT_Name = 0x30, kRT_Tex = 0x34, kRT_W = 0x3C, kRT_H = 0x3E, kRT_Fmt = 0x40;

    constexpr uint32_t kRageFormats = 19;
    constexpr uint32_t FOURCC_INTZ = ('I' | ('N' << 8) | ('T' << 16) | ('Z' << 24));

    // The rage-format table as the exe holds it, used ONLY as a fingerprint --
    // the live table is what the code reads. Index 14 says D3DFMT_D24S8 (75) and
    // the texture behind it is always created as INTZ, so 14 is remapped; that
    // one substitution is load-bearing, because every depth-bearing pipeline key
    // is wrong without it.
    static const uint32_t kExpectedRageFormats[kRageFormats] = {
        0, 23, 21, 111, 114, 35, 113, 34, 112, 116, 113, 36, 50, 25, 75, 82, 22, 111, 115
    };

    // ---------------------------------------------------------------------
    //  Safe reads. Nothing here may fault: a gate that crashes the loading
    //  screen is far worse than one that gives up and says why.
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
    //  Fingerprints. Every one of these is code or static data above
    //  0x004FB000, so it is readable the moment the ASI loads -- which is what
    //  lets the feature refuse to arm on a build it does not know instead of
    //  writing 0xC3 into the middle of something else.
    // ---------------------------------------------------------------------
    inline bool BytesAre(uintptr_t va, const uint8_t* want, size_t n)
    {
        const uint8_t* at = (const uint8_t*)Rebase(va);
        if (!Readable(at, n)) return false;
        return memcmp(at, want, n) == 0;
    }

    // A relative CALL at `va` whose target is `target`.
    inline bool CallsTo(uintptr_t va, uintptr_t target)
    {
        uint8_t op = 0; int32_t rel = 0;
        if (!Peek(Rebase(va), op) || op != 0xE8) return false;
        if (!Peek(Rebase(va) + 1, rel)) return false;
        return (uintptr_t)((uintptr_t)Rebase(va) + 5 + (intptr_t)rel) == Rebase(target);
    }

    struct Validation
    {
        bool ok = false;
        std::string why;
    };

    // Everything the gate needs, checked before anything is hooked or written.
    inline Validation ValidateCode()
    {
        Validation v;

        // rageBoot_InitSession: PUSH ECX / PUSH EBX / MOV EBX,ECX.
        static const uint8_t kInit[4] = { 0x51, 0x53, 0x8B, 0xD9 };
        if (!BytesAre(kVA_InitSession, kInit, sizeof(kInit)))
        { v.why = "rageBoot_InitSession 0x005C1360 is not this build's"; return v; }

        // Its one caller, and the CALL to LoadscreenEnd 5 bytes later. This is
        // the check that says the gate really is "the last thing before the
        // game takes its own loading screen down".
        if (!CallsTo(kVA_InitSessionCall, kVA_InitSession))
        { v.why = "0x005C1B4C does not call rageBoot_InitSession"; return v; }
        static const uint8_t kXorCl[2] = { 0x32, 0xC9 };
        if (!BytesAre(kVA_InitSessionCall + 5, kXorCl, sizeof(kXorCl)) ||
            !CallsTo(kVA_InitSessionCall + 7, kVA_LoadscreenEnd))
        { v.why = "the instruction after the gate is not XOR CL,CL / CALL LoadscreenEnd"; return v; }

        // rageBoot_LoadscreenEnd: TEST CL,CL / JNZ -- __fastcall(char), no stack
        // argument, so one 0xC3 over the first byte is a balanced no-op.
        static const uint8_t kEnd[4] = { 0x84, 0xC9, 0x75, 0x0C };
        if (!BytesAre(kVA_LoadscreenEnd, kEnd, sizeof(kEnd)))
        { v.why = "rageBoot_LoadscreenEnd 0x005CDE90 is not this build's - refusing to pin it"; return v; }

        // The viewport's own phase builder: PUSH ESI / PUSH [ESP+8] / MOV ESI,ECX
        // / CALL ragePhase_CreateAllPhasesAndTargets.
        static const uint8_t kPhases[7] = { 0x56, 0xFF, 0x74, 0x24, 0x08, 0x8B, 0xF1 };
        if (!BytesAre(kVA_CreatePhases, kPhases, sizeof(kPhases)))
        { v.why = "rageBoot_CViewportGame_CreateRenderPhases 0x005D49D0 is not this build's"; return v; }

        // Slot 12 of the CViewportGame vtable must BE that function; the live
        // object is checked against the same value at the gate.
        uintptr_t slot12 = 0;
        if (!Peek(Rebase(kVA_ViewportVT) + 12 * 4, slot12) || slot12 != Rebase(kVA_CreatePhases))
        { v.why = "CViewportGame vtable slot 12 is not the phase builder"; return v; }

        // The rage-format table. Without it every depth-bearing key is a guess.
        const uint32_t* fmt = (const uint32_t*)Rebase(kVA_RageFormatToD3D);
        if (!Readable(fmt, kRageFormats * 4))
        { v.why = "the rage-format table 0x01111A28 is not readable"; return v; }
        int match = 0;
        for (uint32_t i = 0; i < kRageFormats; i++)
            if (fmt[i] == kExpectedRageFormats[i]) match++;
        if (match < (int)kRageFormats - 2)
        {
            char b[128];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "the rage-format table matches only %d of %u entries", match, kRageFormats);
            v.why = b;
            return v;
        }

        v.ok = true;
        return v;
    }

    // ---------------------------------------------------------------------
    //  The pin. One byte, restored by the guard on every path including a
    //  fault inside the warm pass -- if it is ever left in place the loading
    //  screen never comes down and the game is dead.
    // ---------------------------------------------------------------------
    class LoadscreenPin
    {
    public:
        LoadscreenPin() = default;
        ~LoadscreenPin() { Release(); }
        LoadscreenPin(const LoadscreenPin&) = delete;
        LoadscreenPin& operator=(const LoadscreenPin&) = delete;

        bool Acquire()
        {
            if (held) return true;
            uint8_t* at = (uint8_t*)Rebase(kVA_LoadscreenEnd);
            DWORD old = 0;
            if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
            saved = *at;
            *at = 0xC3;                     // RET
            VirtualProtect(at, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 1);
            held = true;
            return true;
        }

        void Release()
        {
            if (!held) return;
            uint8_t* at = (uint8_t*)Rebase(kVA_LoadscreenEnd);
            DWORD old = 0;
            if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old))
            {
                *at = saved;
                VirtualProtect(at, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, 1);
            }
            held = false;
        }

        bool Held() const { return held; }

    private:
        bool    held = false;
        uint8_t saved = 0;
    };

    // A bounded message pump of our own: it always returns.
    inline void PumpOwn()
    {
        MSG m;
        for (int i = 0; i < 128 && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE); i++)
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    // Would rageBoot_PumpOsAndOverlay fail to come back?
    //
    // Its message pump FUN_00420E90 does NOT just drain the queue. Once the
    // queue is empty it tests two flags, and while both are set it either calls
    // SetFocus on the game window and pumps again, or -- if the window is
    // iconic -- Sleep(100)s inside `do { } while(true)`. For the game's own
    // legals spin that is correct: it is a second or two and the point is to
    // get the focus back. For a hold measured in MINUTES it is not: it snatches
    // the focus from a player who alt-tabbed, and a minimised window stops the
    // gate's loop evaluating its own exit conditions at all.
    //
    // So read the same three bytes the engine reads and skip the call exactly
    // when it would spin. Unreadable, or in doubt, means call it: that is the
    // behaviour this replaces.
    inline bool PumpOsWouldSpin()
    {
        uint8_t a = 0, b = 0, active = 1;
        if (!Peek(Rebase(kVA_PumpSpinA), a) || !Peek(Rebase(kVA_PumpSpinB), b)) return false;
        if (a == 0 || b == 0) return false;              // it returns as soon as the queue is empty
        if (!Peek(Rebase(kVA_AppActive), active)) return false;
        return active == 0;                              // inactive: SetFocus spin, or Sleep(100) forever
    }

    // The OS / Steam-overlay pump the game's own legals spin calls. The main
    // thread is blocked at the gate for the whole pass, so nothing else runs
    // it; 60 s without it was fine under Proton, minutes is untested, and one
    // call per millisecond costs nothing.
    inline void PumpOs()
    {
        if (PumpOsWouldSpin()) { PumpOwn(); return; }
        ((void(__cdecl*)())Rebase(kVA_PumpOsAndOverlay))();
    }

    inline bool LoadscreenShown()
    {
        uint8_t v = 0;
        return Peek(Rebase(kVA_LoadscreenShown), v) && v != 0;
    }

    // ---------------------------------------------------------------------
    //  Ask the engine to build its render phases NOW.
    //
    //  This is the one real intervention, and it is engine code called with the
    //  engine's own argument on the engine's own object: viewport+0x404 goes
    //  0 -> 27 synchronously and rageBoot_ViewportManager_Tick's own
    //  `word[+0x404] == 0` guard then suppresses the engine's duplicate build
    //  two seconds later.
    // ---------------------------------------------------------------------
    inline void* FindGameViewport()
    {
        const uintptr_t want = Rebase(kVA_CreatePhases);
        for (uintptr_t va : kVA_Viewports)
        {
            uintptr_t obj = 0, vt = 0, slot = 0;
            if (!Peek(Rebase(va), obj) || !obj) continue;
            if (!Peek(obj, vt) || !vt) continue;
            if (!Peek(vt + 12 * 4, slot) || slot != want) continue;
            return (void*)obj;
        }
        return nullptr;
    }

    inline uint16_t PhaseCount(void* vp)
    {
        uint16_t n = 0;
        if (!vp || !Peek((uintptr_t)vp + kVP_PhaseCount, n)) return 0;
        return n > 512 ? 0 : n;
    }

    // Returns the phase count the viewport ended up with, 0 on failure.
    inline uint16_t BuildRenderPhases(void* vp)
    {
        if (!vp) return 0;
        const uint16_t before = PhaseCount(vp);
        if (before) return before;          // the engine got there first: leave it alone
        ((void(__thiscall*)(void*, int))Rebase(kVA_CreatePhases))(vp, 0);
        return PhaseCount(vp);
    }

    // ---------------------------------------------------------------------
    //  Reading the engine's render targets.
    //
    //  TWO SOURCES, and they answer different questions.
    //
    //  * The FACTORY REGISTRY (0x018E0108 / 0x018E0908) is every render target
    //    grcTextureFactoryPC::CreateRenderTarget has ever made, looked up by
    //    NAME so the pointers survive a device reset. It is the whole format
    //    axis: the scene pair, the post-FX chain, SMAA, SSAO, the cascade
    //    atlas, script_rt -- none of which any phase declares.
    //  * The PHASE LIST says which targets are bound TOGETHER, which is the
    //    only source for the MRT sets.
    //
    //  What is NOT available here is the IDirect3DTexture9 at +0x34: 0 of 36 at
    //  this gate, and still 0 after a render flush. So the D3DFORMAT comes from
    //  the rage byte at +0x40 through the live table, and the sample count
    //  cannot be read back at all (it comes from FusionFix's own setting).
    // ---------------------------------------------------------------------
    inline D3DFORMAT RageToD3D(uint8_t rageFmt)
    {
        if (rageFmt >= kRageFormats) return D3DFMT_UNKNOWN;
        // Index 14 claims D3DFMT_D24S8 and the engine creates the texture as
        // INTZ every time (09-phase-contexts §2.2). DXVK maps INTZ to
        // D32_SFLOAT_S8_UINT, which is in the pipeline key.
        if (rageFmt == 14) return (D3DFORMAT)FOURCC_INTZ;
        uint32_t f = 0;
        if (!Peek(Rebase(kVA_RageFormatToD3D) + rageFmt * 4, f)) return D3DFMT_UNKNOWN;
        return (D3DFORMAT)f;
    }

    struct Target
    {
        uintptr_t   ptr = 0;
        std::string name;
        uint16_t    w = 0, h = 0;
        uint8_t     rageFmt = 0;
        D3DFORMAT   fmt = D3DFMT_UNKNOWN;
        bool        hasTexture = false;     // 0 of 36 at this gate; recorded, not relied on
    };

    // Is this pointer plausibly a live COM object?
    //
    // Readable() proves four bytes are committed; it does not prove they are an
    // IDirect3DTexture9, and at this gate a half-built registry entry is a real
    // possibility (09-phase-contexts caught five phases with vtable 0x86150000
    // and 0xCDCD filler in exactly this window). A virtual call through one of
    // those is an access violation on the main thread, with the loading screen
    // pinned -- which this header exists not to do. So before dispatching:
    // the object is readable, its vtable is readable, and the first three slots
    // (IUnknown's) each point into committed, EXECUTABLE memory.
    inline bool LooksLikeCom(uintptr_t p)
    {
        uintptr_t vt = 0;
        if (!Peek(p, vt) || !vt) return false;
        if (!Readable((const void*)vt, 3 * sizeof(uintptr_t))) return false;
        for (int i = 0; i < 3; i++)
        {
            uintptr_t fn = 0;
            if (!Peek(vt + (uintptr_t)i * sizeof(uintptr_t), fn) || !fn) return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((const void*)fn, &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT) return false;
            const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & exec)) return false;
        }
        return true;
    }

    inline bool ReadTarget(uintptr_t rt, Target& out)
    {
        if (!rt || !Readable((const void*)rt, 0x50)) return false;
        uintptr_t nameP = 0;
        out.ptr = rt;
        if (Peek(rt + kRT_Name, nameP) && nameP) PeekName(nameP, out.name);
        Peek<uint16_t>(rt + kRT_W, out.w);
        Peek<uint16_t>(rt + kRT_H, out.h);
        Peek<uint8_t>(rt + kRT_Fmt, out.rageFmt);
        uintptr_t tex = 0;
        out.hasTexture = Peek(rt + kRT_Tex, tex) && tex != 0;
        out.fmt = RageToD3D(out.rageFmt);
        // A live texture is authoritative when there IS one: it is what DXVK
        // keys on, and it is the only thing that can disagree with the rage
        // byte (rage 5 claims A2R10G10B10 on rigs where no such target exists).
        if (out.hasTexture && !LooksLikeCom(tex)) out.hasTexture = false;
        if (out.hasTexture)
        {
            auto* t = (IDirect3DTexture9*)tex;
            D3DSURFACE_DESC sd{};
            IDirect3DSurface9* surf = nullptr;
            if (SUCCEEDED(t->GetSurfaceLevel(0, &surf)) && surf)
            {
                if (SUCCEEDED(surf->GetDesc(&sd)) && sd.Format) out.fmt = sd.Format;
                surf->Release();
            }
        }
        return out.fmt != D3DFMT_UNKNOWN;
    }

    // The factory's registry, whole.
    inline void ReadTargetRegistry(std::vector<Target>& out)
    {
        int32_t n = 0;
        if (!Peek(Rebase(kVA_AllRenderTargetN), n) || n <= 0 || n > 512) return;
        for (int32_t i = 0; i < n; i++)
        {
            uintptr_t rt = 0;
            if (!Peek(Rebase(kVA_AllRenderTargets) + (uintptr_t)i * 4, rt) || !rt) continue;
            Target t;
            if (ReadTarget(rt, t)) out.push_back(std::move(t));
        }
    }

    // One render phase's bound set: up to four colour slots and one depth.
    struct PhaseSet
    {
        uint16_t    phase = 0;
        std::string name;                   // the first colour target's name
        Target      colour[4];
        int         mrt = 0;
        Target      depth;
        bool        hasDepth = false;
    };

    inline void ReadPhaseSets(void* vp, std::vector<PhaseSet>& out)
    {
        const uint16_t n = PhaseCount(vp);
        if (!n) return;
        uintptr_t arr = 0;
        if (!Peek((uintptr_t)vp + kVP_Phases, arr) || !arr) return;

        // The MRT3 trap: the G-buffer phase struct lists _DEFERRED_GBUFFER_3_
        // (the INTZ depth) in COLOUR slot 3, but the phase's own callback
        // ragePhase_GBuffer_RT_PreDrawList_CB 0x00AD2280 binds _STENCIL_BUFFER_
        // there. Keyed on the target POINTER, so it survives a reorder.
        uintptr_t gbuf3 = 0, stencil = 0;
        Peek(Rebase(kVA_GBuffer3), gbuf3);
        Peek(Rebase(kVA_StencilBuffer), stencil);

        for (uint16_t i = 0; i < n; i++)
        {
            uintptr_t ph = 0;
            if (!Peek(arr + (uintptr_t)i * 4, ph) || !ph) continue;
            if (!Readable((const void*)(ph + kPh_Slot0), 4 * kPh_SlotStride)) continue;

            PhaseSet s;
            s.phase = i;
            for (int k = 0; k < 4; k++)
            {
                uintptr_t col = 0, dep = 0;
                Peek(ph + kPh_Slot0 + k * kPh_SlotStride, col);
                Peek(ph + kPh_Slot0 + k * kPh_SlotStride + kPh_DepthDelta, dep);
                if (k == 3 && col && gbuf3 && stencil && col == gbuf3) col = stencil;
                if (col && ReadTarget(col, s.colour[k]))
                {
                    s.mrt = k + 1;
                    if (s.name.empty()) s.name = s.colour[k].name;
                }
                if (!s.hasDepth && dep && ReadTarget(dep, s.depth)) s.hasDepth = true;
            }
            if (s.mrt) out.push_back(std::move(s));
        }
    }

    // ---------------------------------------------------------------------
    //  Progress, drawn INSIDE the game's own loading screen.
    //
    //  The loading screen's own fallback primitive: bind no texture, draw an
    //  untextured quad in PIXEL coordinates, flush. rageBoot_LoadscreenRenderTick
    //  uses exactly this sequence for its fade quads, which is why it is the one
    //  that is certain to work at a point where CFont faults.
    //
    //  Called from the leave of rageBoot_LoadscreenDrawLayers 0x005CC180 -- the
    //  last thing in a frame the loading screen actually drew. NOT from the
    //  tick's own leave: that is called ~7 kHz and 99.9 % of those calls are
    //  outside a drawn frame (and after the frame has been handed on).
    // ---------------------------------------------------------------------
    inline void SpriteSetTexture(void* tex, char which)
    {
        ((void(__cdecl*)(void*, char))Rebase(kVA_SpriteSetTexture))(tex, which);
    }
    inline void SpriteQuad(float x0, float y0, float x1, float y1, float z, uint32_t argb)
    {
        ((void(__cdecl*)(float, float, float, float, float, const uint32_t*))
            Rebase(kVA_SpriteQuad))(x0, y0, x1, y1, z, &argb);
    }
    inline void SpriteFlush()
    {
        ((void(__cdecl*)())Rebase(kVA_SpriteFlush))();
    }

    // The loading screen's own idea of the screen, in pixels.
    inline bool ScreenSize(float& w, float& h)
    {
        uintptr_t info = 0;
        if (!Peek(Rebase(kVA_ScreenInfo), info) || !info) return false;
        int32_t iw = 0, ih = 0; float fw = 0.0f, fh = 0.0f;
        if (!Peek(info + 0x2B0, iw) || !Peek(info + 0x2B4, ih) ||
            !Peek(info + 0x288, fw) || !Peek(info + 0x28C, fh)) return false;
        w = (float)(int)(iw * fw);
        h = (float)(int)(ih * fh);
        return w > 16.0f && h > 16.0f;
    }

    // A 5x7 column-major glyph set, because there is no text engine here: CFont
    // faults (its textures are unloaded at this gate) and an ID3DXFont draws
    // through the device immediately, which is not where this frame is being
    // recorded. Each byte is one column, bit 0 the top row. Unknown characters
    // draw blank rather than garbage.
    struct Glyph { char c; uint8_t col[5]; };
    static const Glyph kGlyphs[] = {
        { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
        { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } }, { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
        { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } }, { '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
        { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } }, { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
        { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } }, { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
        { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } }, { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
        { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } }, { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
        { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } }, { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
        { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } }, { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
        { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } }, { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
        { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } }, { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
        { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } }, { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
        { 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } }, { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
        { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } }, { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
        { 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } }, { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
        { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } }, { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
        { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } }, { 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
        { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } }, { 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
        { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } }, { 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
        { '%', { 0x23, 0x13, 0x08, 0x64, 0x62 } }, { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
        { ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } }, { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
        { '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } }, { '+', { 0x08, 0x08, 0x3E, 0x08, 0x08 } },
        { ',', { 0x00, 0x50, 0x30, 0x00, 0x00 } }, { '(', { 0x00, 0x1C, 0x22, 0x41, 0x00 } },
        { ')', { 0x00, 0x41, 0x22, 0x1C, 0x00 } },
    };

    inline const uint8_t* GlyphFor(char c)
    {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for (const Glyph& g : kGlyphs) if (g.c == c) return g.col;
        return nullptr;
    }

    // ---------------------------------------------------------------------
    //  The band, and where every number in it comes from.
    //
    //  The player asked for what the menus do -- "all black with separation bar
    //  between the art and the black bottom bar" -- so the overlay is a solid
    //  black band across the bottom with a rule between it and the artwork, and
    //  nothing of ours is ever drawn on the art itself. That is what makes it
    //  readable over EVERY loading screen in the rotation instead of over most
    //  of them.
    //
    //  The colours are the game's own: kTextARGB is the exact value
    //  rageBoot_LoadscreenDrawLegendText 0x005CD010 passes to CFont for the
    //  legal notice on this same screen family (0xFFE1E1E1), and the band is
    //  the flat black the loading screen itself clears to and falls back to
    //  when an artwork layer has no texture (0xFF000000, 0x005CC760 /
    //  0x005CC180). The proportions are ours: enough band for two rows of text
    //  and the bar with the game's own safe-area inset, checked on screen
    //  against the artwork rotation rather than derived from anything.
    // ---------------------------------------------------------------------
    //  AND THE FIRST TWO ARE THE MENU'S OWN, read from the frontend's tables
    //  rather than guessed. CRenderPhaseFrontEnd_RenderPauseMenu_CB draws the
    //  pause menu's bottom bar as
    //      rect{0, H*layout[0x16].x, W, H}                      black, alpha only
    //      rect{0, H*layout[0x16].x, W, H*(layout[0x16].x+0.002)}  colour[0x41]
    //  where layout is the float2 array at *0x011609C8 (index * 8) and colour
    //  is the u32 ARGB array at 0x0118DD98. So the band's height, its rule's
    //  height and the rule's colour all come from the same numbers the pause
    //  menu uses, and the constants below are only the fallback for a boot
    //  where those tables are not populated.
    //  AND THE TEXT COLOUR IS THE SAME ONE. The bottom-right legend the player
    //  pointed at -- "the next stage after this loading screen has some text in
    //  the right bottom corner" -- is drawn by 0x008B6C80, which is the pause
    //  menu's help line: font style 0, ALIGN 2 (right) against a text box whose
    //  right edge is the layout anchor, colour[0x41], and a drop shadow in
    //  colour[0x02]. So the band's text uses colour[0x41] too, and the right
    //  column is right-aligned the same way. Same index as the rule, which is
    //  not a coincidence: in the pause menu they are the same colour.
    constexpr uintptr_t kVA_MenuLayout   = 0x011609C8;  // float2[], index * 8
    constexpr uintptr_t kVA_MenuColours  = 0x0118DD98;  // u32 ARGB[]
    constexpr int       kMenuBottomBar   = 0x16;        // the bottom bar's top edge
    constexpr int       kMenuRuleColour  = 0x41;        // the rule, and the legend's text
    constexpr float     kMenuRuleFrac    = 0.002f;      // the frontend's own literal

    constexpr float    kBandFrac     = 0.125f;   // band height / screen height
    constexpr float    kMarginFrac   = 0.075f;   // left and right inset / screen width
    constexpr float    kRow1Frac     = 0.11f;    // headline row, fraction into the band
    constexpr float    kBarFrac      = 0.46f;    // bar top, fraction into the band
    constexpr float    kBarHFrac     = 0.070f;   // bar height, fraction of the band
    constexpr float    kRow2Frac     = 0.62f;    // detail row, fraction into the band
    constexpr uint32_t kBandARGB     = 0xFF000000u;
    constexpr uint32_t kSepARGB      = 0xFFB4B4B4u;
    constexpr uint32_t kBarTrackARGB = 0x40FFFFFFu;
    // The bar's fill has no constant of its own: it draws in the same colour as
    // the headline (the pause menu's colour[0x41] when that table was readable,
    // kTextARGB when it was not), so the band is one palette and not two.
    constexpr uint32_t kTextARGB     = 0xFFE1E1E1u;   // the legend's own colour
    constexpr uint32_t kDetailARGB   = 0xFF9A9A9Au;
    constexpr int      kFontStyle    = 0;            // font1, the face the menus and HUD use
    constexpr float    kHeadScaleX   = 0.38f, kHeadScaleY   = 0.55f;
    constexpr float    kDetailScaleX = 0.26f, kDetailScaleY = 0.38f;

    // ---------------------------------------------------------------------
    //  The game's own text.
    //
    //  08-boot-gate §4.4 concluded that CFont cannot be used here, because a
    //  live bisect faulted at the flush with a null font texture. That is one
    //  measurement of ONE boot, and the code says it should not be so:
    //  rageBoot_InitSession itself calls CFont::Reload 0x00922BB0 at 0x005C161A,
    //  a handful of calls before the return we gate on, and that function ends
    //  by resolving font1/font3 out of the freshly loaded "fonts" texture
    //  dictionary. So the font may well be resident by the time we look.
    //
    //  Rather than decide the question in a comment: TEST IT, every session,
    //  one pointer read -- and keep the 5x7 glyph set as the fallback for the
    //  boots where it is null. The log says which one drew.
    // ---------------------------------------------------------------------
    inline void FontSetStyle(int s)  { ((void(__cdecl*)(int))Rebase(kVA_FontSetStyle))(s); }
    inline void FontSetProp(int p)   { ((void(__cdecl*)(int))Rebase(kVA_FontSetProp))(p); }
    inline void FontSetSlant(float f){ ((void(__cdecl*)(float))Rebase(kVA_FontSetSlant))(f); }
    inline void FontSetAlign(int a)  { ((void(__cdecl*)(int))Rebase(kVA_FontSetAlign))(a); }
    inline void FontSetColour(uint32_t c) { ((void(__cdecl*)(uint32_t))Rebase(kVA_FontSetColour))(c); }
    inline void FontSetBox(float l, float r) { ((void(__cdecl*)(float, float))Rebase(kVA_FontSetBox))(l, r); }
    inline void FontSetScale(float x, float y) { ((void(__cdecl*)(float, float))Rebase(kVA_FontSetScale))(x, y); }
    inline void FontPrint(float x, float y, const wchar_t* s)
    { ((void(__cdecl*)(float, float, const wchar_t*, int, int))Rebase(kVA_FontPrint))(x, y, s, -1, -1); }
    inline void FontFlush()          { ((void(__cdecl*)())Rebase(kVA_FontFlush))(); }
    inline void WidescreenFix(float& sx, float& sy)
    {
        float v[2] = { sx, sy };
        ((int(__cdecl*)(int, void*, float*, void*))Rebase(kVA_WidescreenFix))(7, nullptr, v, nullptr);
        sx = v[0]; sy = v[1];
    }

    // Is style `s` backed by a resident texture? This is the exact pointer
    // rageBoot_Sprite_SetTexture is handed from 0x00923200, and a null one is
    // the "access violation accessing 0x280" the first attempt hit.
    inline bool FontStyleResident(int s)
    {
        if (s < 0 || s > 3) return false;
        uintptr_t tex = 0;
        if (!Peek(Rebase(kVA_FontTextures) + (uintptr_t)s * kFontStride, tex) || !tex) return false;
        return Readable((const void*)tex, 4);
    }

    // font1 is the face the menus and the HUD use and is what we want; font3 is
    // the game's other resident face and is worth having rather than falling
    // back to a 5x7 bitmap. font4 is Japanese-only and 3 is the streamed font,
    // which is exactly the one that may not be there. -1 means "no native text".
    inline int ResolveFontStyle()
    {
        if (FontStyleResident(0)) return 0;
        if (FontStyleResident(1)) return 1;
        return -1;
    }

    // The CFont entry points, fingerprinted. A mismatch costs the native font
    // and nothing else -- the glyph set draws instead -- so this is separate
    // from ValidateCode, which decides whether the gate runs at all.
    // Most of these carry a relative CALL displacement in their first six
    // bytes, which is this build's and nobody else's. Two did not and had to be
    // lengthened: a bare `PUSH EBX / PUSH ESI / CALL` and the universal
    // `PUSH EBP / MOV EBP,ESP / AND ESP,-8` prologue match thousands of
    // functions each, so they validated nothing. Both now run far enough to
    // reach an absolute or relative operand -- the E8 displacement in
    // FontSetStyle, and in FontPrint the security cookie's own address
    // (MOV EAX,[0x01057FB4]), which cannot be anything but this image.
    inline bool ValidateFont()
    {
        struct { uintptr_t va; const uint8_t b[20]; uint8_t n; } k[] = {
            // PUSH EBX / PUSH ESI / CALL 0x0091C150 / MOV EBX,[ESP+0xC] / PUSH EBX
            { kVA_FontSetStyle,  { 0x53, 0x56, 0xE8, 0xB9, 0x7D, 0xFF, 0xFF, 0x8B,
                                   0x5C, 0x24, 0x0C, 0x53, 0x8D, 0x34, 0xC0, 0xE8 }, 16 },
            { kVA_FontSetProp,   { 0xE8, 0x6B, 0x7C, 0xFF, 0xFF, 0x8D },             6 },
            { kVA_FontSetSlant,  { 0xE8, 0x3B, 0x7E, 0xFF, 0xFF, 0xF3 },             6 },
            { kVA_FontSetAlign,  { 0xE8, 0xAB, 0x7C, 0xFF, 0xFF, 0x8D },             6 },
            { kVA_FontSetColour, { 0xE8, 0x5B, 0x7F, 0xFF, 0xFF, 0xF3 },             6 },
            { kVA_FontSetBox,    { 0xE8, 0x3B, 0x7B, 0xFF, 0xFF, 0xF3 },             6 },
            { kVA_FontSetScale,  { 0x83, 0xEC, 0x08, 0x56, 0xE8, 0x47 },             6 },
            // PUSH EBP / MOV EBP,ESP / AND ESP,-8 / SUB ESP,0x58 /
            // MOV EAX,[0x01057FB4] / XOR EAX,ESP / MOV [ESP+0x54],EAX
            { kVA_FontPrint,     { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x58, 0xA1,
                                   0xB4, 0x7F, 0x05, 0x01, 0x33, 0xC4, 0x89, 0x44, 0x24, 0x54 }, 20 },
            { kVA_FontFlush,     { 0x56, 0x57, 0xE8, 0xF9, 0x87, 0xFF },             6 },
            { kVA_WidescreenFix, { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x85 },             6 },
        };
        for (const auto& e : k)
            if (!BytesAre(e.va, e.b, e.n)) return false;
        return true;
    }

    // ---------------------------------------------------------------------
    //  What the pass tells the loading screen to show.
    //
    //  Four strings, laid out like the game's own menus: a headline and a
    //  percentage on one row, the bar, then the detail and the time estimate
    //  on a smaller row. Written by the pass thread, read by the render thread;
    //  nothing here is worth a lock, because a torn line is one frame of a
    //  caption and every buffer is always NUL-terminated.
    // ---------------------------------------------------------------------
    struct Progress
    {
        volatile bool   active = false;
        volatile float  fraction = 0.0f;
        char            head[64] = {};      // left, top row
        char            pct[24] = {};       // right, top row
        char            detail[96] = {};    // left, bottom row
        char            eta[48] = {};       // right, bottom row
        volatile long   lineSeq = 0;
        // Filled on the first draw so the log can say which text engine drew.
        volatile bool   nativeFont = false;
        volatile bool   nativeChecked = false;
        volatile int    faultStep = 0;
        volatile int    fontStyle = kFontStyle;
        // The band's geometry, resolved once from the frontend's own tables.
        // TWO reads, and they fail independently -- the layout float2[] and the
        // colour u32[] are different tables -- so the log says which of them
        // answered rather than one flag for both. ruleFrac is in neither: the
        // frontend hard-codes 0.002 * H, so it is ours and always the literal.
        volatile float    bandFrac = kBandFrac;
        volatile float    ruleFrac = kMenuRuleFrac;
        volatile uint32_t ruleARGB = kSepARGB;
        volatile uint32_t textARGB = kTextARGB;
        volatile uint32_t detailARGB = kDetailARGB;
        volatile bool     layoutFromGame = false;
        volatile bool     colourFromGame = false;
    };
    inline Progress& Prog()
    {
        static Progress p;
        return p;
    }

    inline void CopyLine(char* dst, size_t cap, const char* src)
    {
        if (!src) { dst[0] = 0; return; }
        strncpy_s(dst, cap, src, _TRUNCATE);
        // '~' opens a token in the game's text system (~r~, ~n~ and friends).
        // Our strings are built from counters, not from GXT, so the safe thing
        // is to make sure one can never be mistaken for a token.
        for (char* c = dst; *c; c++) if (*c == '~') *c = '-';
    }

    inline void SetProgress(float frac, const char* head, const char* pct,
                            const char* detail, const char* eta)
    {
        Progress& p = Prog();
        p.fraction = (frac < 0.0f) ? 0.0f : (frac > 1.0f ? 1.0f : frac);
        if (head)   CopyLine(p.head, sizeof(p.head), head);
        if (pct)    CopyLine(p.pct, sizeof(p.pct), pct);
        if (detail) CopyLine(p.detail, sizeof(p.detail), detail);
        if (eta)    CopyLine(p.eta, sizeof(p.eta), eta);
        InterlockedIncrement(&p.lineSeq);
    }

    // Take the band's proportions and its rule's colour from the pause menu's
    // own tables. Both are filled by the frontend, which has finished by the
    // time this gate opens; both are range-checked, because a table that has
    // been freed reads as plausible-looking rubbish rather than as zero.
    inline void ResolveMenuStyle()
    {
        Progress& p = Prog();
        uintptr_t tbl = 0;
        float top = 0.0f;
        if (Peek(Rebase(kVA_MenuLayout), tbl) && tbl &&
            Peek(tbl + (uintptr_t)kMenuBottomBar * 8, top) &&
            top > 0.70f && top < 0.96f)
        {
            p.bandFrac = 1.0f - top;
            p.layoutFromGame = true;
        }
        uint32_t c = 0;
        if (Peek(Rebase(kVA_MenuColours) + (uintptr_t)kMenuRuleColour * 4, c) && (c >> 24) >= 0x40)
        {
            p.colourFromGame = true;
            p.ruleARGB = c;
            p.textARGB = c;
            // The second row is ours: the same colour at about two thirds, so
            // the detail reads as subordinate to the headline without
            // introducing a colour the game does not use.
            const uint32_t r = ((c >> 16) & 0xFF) * 2 / 3;
            const uint32_t g = ((c >> 8) & 0xFF) * 2 / 3;
            const uint32_t b = (c & 0xFF) * 2 / 3;
            p.detailARGB = (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }

    // ---------------------------------------------------------------------
    //  The two text engines.
    // ---------------------------------------------------------------------

    // One quad per vertical run of lit pixels in a glyph column: a 24-character
    // line costs on the order of a hundred quads, not 5x7x24.
    inline float GlyphTextWidth(const char* s, float px) { return (float)strlen(s) * px * 6.0f; }

    inline int DrawGlyphText(const char* s, float x, float y, float px, uint32_t argb)
    {
        int quads = 0;
        for (const char* c = s; *c && quads < 512; c++, x += px * 6.0f)
        {
            const uint8_t* g = GlyphFor(*c);
            if (!g) continue;
            for (int col = 0; col < 5; col++)
            {
                uint8_t bits = g[col];
                int row = 0;
                while (row < 7)
                {
                    if (!(bits & (1 << row))) { row++; continue; }
                    int end = row;
                    while (end < 7 && (bits & (1 << end))) end++;
                    SpriteQuad(x + col * px, y + row * px,
                               x + (col + 1) * px, y + end * px, 0.0f, argb);
                    quads++;
                    row = end;
                }
            }
        }
        return quads;
    }

    inline void ToWide(const char* s, wchar_t* out, size_t cap)
    {
        size_t i = 0;
        for (; s[i] && i + 1 < cap; i++)
        {
            unsigned char c = (unsigned char)s[i];
            out[i] = (c >= 0x20 && c <= 0x7E) ? (wchar_t)c : L' ';
        }
        out[i] = 0;
    }

    // The four lines, in the game's own font, through the same sequence
    // rageBoot_LoadscreenDrawLegendText uses on the legal screens.
    //
    // The step number goes into Prog().faultStep, which is volatile, because
    // that is the only way a value written inside a __try is guaranteed to be
    // readable in its __except -- and on a fault the step is the whole
    // diagnosis. It is cleared to 0 on the way out.
    inline void DrawNativeText(float bandY, float bandH, float w, float h)
    {
        Progress& p = Prog();
        volatile int& step = p.faultStep;
        char head[64], pct[24], detail[96], eta[48];
        strncpy_s(head, sizeof(head), p.head, _TRUNCATE);
        strncpy_s(pct, sizeof(pct), p.pct, _TRUNCATE);
        strncpy_s(detail, sizeof(detail), p.detail, _TRUNCATE);
        strncpy_s(eta, sizeof(eta), p.eta, _TRUNCATE);

        wchar_t wbuf[96];
        const float left  = kMarginFrac;
        const float right = 1.0f - kMarginFrac;
        // A COLUMN EACH. Both prints on a row used to share one box spanning
        // the whole band, so a long left-hand string could wrap at the far
        // margin -- and a wrapped line is drawn BELOW its row, which on the top
        // row lands on the bar. The longest headline this can carry is a settle
        // ("WAITING FOR THE DRIVER") plus its clock in the other column, so the
        // split is where the two columns can never reach each other.
        const float split = left + (right - left) * 0.62f;
        const float y1 = (bandY + bandH * kRow1Frac) / h;
        const float y2 = (bandY + bandH * kRow2Frac) / h;

        step = 1;  FontSetStyle(p.fontStyle);
        step = 2;  FontSetProp(1);
        step = 3;  FontSetSlant(0.0f);
        step = 4;  FontSetColour(p.textARGB);

        float sx = kHeadScaleX, sy = kHeadScaleY;
        step = 6;  WidescreenFix(sx, sy);
        step = 7;  FontSetScale(sx, sy);
        if (head[0])
        {
            step = 5;  FontSetBox(left, split);
            step = 8;  FontSetAlign(1);
            ToWide(head, wbuf, 96);
            step = 9;  FontPrint(left, y1, wbuf);
        }
        if (pct[0])
        {
            // RIGHT-ALIGNED TEXT IS PRINTED AT THE BOX'S LEFT EDGE, not its
            // right. 0x009222A0 takes `x - boxLeft` as an INDENT and subtracts
            // it from the width it may wrap in, so printing at the right edge
            // leaves nothing to fit and every line breaks after its first word
            // (seen on screen: "ABOUT" / "1:08 LEFT IN THIS STAGE"). Align 2
            // ignores the x and measures from the box's right edge anyway,
            // which is what 0x008B6C80 relies on when it prints the pause
            // menu's help line at x = 0.
            step = 5;  FontSetBox(split, right);
            step = 10; FontSetAlign(2);
            ToWide(pct, wbuf, 96);
            step = 11; FontPrint(split, y1, wbuf);
        }

        sx = kDetailScaleX; sy = kDetailScaleY;
        step = 12; WidescreenFix(sx, sy);
        step = 13; FontSetScale(sx, sy);
        step = 14; FontSetColour(p.detailARGB);
        // The bottom row's own split. The detail is the longest string in the
        // band ("STAGE 2 OF 3   ENGINE PIPELINE 27739 / 61191   0:52") and the
        // estimate beside it is short, so it sits further right than the top
        // row's.
        const float split2 = left + (right - left) * 0.72f;
        if (detail[0])
        {
            step = 15; FontSetBox(left, split2); FontSetAlign(1);
            ToWide(detail, wbuf, 96);
            step = 16; FontPrint(left, y2, wbuf);
        }
        if (eta[0])
        {
            step = 17; FontSetBox(split2, right); FontSetAlign(2);
            ToWide(eta, wbuf, 96);
            step = 18; FontPrint(split2, y2, wbuf);
        }
        step = 19; FontFlush();
        step = 0;
        (void)w;
    }

    // ---------------------------------------------------------------------
    //  The overlay.
    //
    //  The menus' own device, which is what the player asked for: a solid black
    //  band across the bottom with a separator rule between it and the artwork,
    //  and everything of ours inside the band. Nothing of ours is ever drawn
    //  over the art itself, so it reads the same over every loading screen in
    //  the rotation -- bright TBoGT neon or a dark TLAD alley.
    //
    //  Still the loading screen's own primitive (bind no texture, untextured
    //  quad in PIXEL coordinates, flush), from the CL == 0 leave of
    //  rageBoot_LoadscreenDrawLayers, so the artwork keeps animating
    //  underneath and the mod presents no frame of its own.
    // ---------------------------------------------------------------------
    inline void DrawProgress()
    {
        Progress& p = Prog();
        if (!p.active) return;

        float w = 0.0f, h = 0.0f;
        if (!ScreenSize(w, h)) return;

        // Decide once per session what this band looks like and what can draw
        // its text, and let the log say so.
        if (!p.nativeChecked)
        {
            ResolveMenuStyle();
            const int style = ResolveFontStyle();
            p.fontStyle = style < 0 ? kFontStyle : style;
            p.nativeFont = style >= 0 && ValidateFont();
            p.nativeChecked = true;
        }

        const float bandH = (float)(int)((std::max)(56.0f, h * p.bandFrac));
        const float bandY = (float)(int)(h - bandH);
        const float sepH  = (std::max)(2.0f, (float)(int)(h * p.ruleFrac));
        const float mx    = (float)(int)(w * kMarginFrac);

        SpriteSetTexture(nullptr, 0);
        SpriteQuad(0.0f, bandY, w, h, 0.0f, kBandARGB);                   // the band
        // The rule sits INSIDE the band along its top edge, which is where the
        // pause menu puts its own (0x005A9F18: H*c .. H*(c+0.002)).
        SpriteQuad(0.0f, bandY, w, bandY + sepH, 0.0f, p.ruleARGB);

        const float barY = (float)(int)(bandY + bandH * kBarFrac);
        const float barH = (std::max)(4.0f, (float)(int)(bandH * kBarHFrac));
        SpriteQuad(mx, barY, w - mx, barY + barH, 0.0f, kBarTrackARGB);
        const float fillW = (w - 2.0f * mx) * p.fraction;
        if (fillW >= 1.0f)
            SpriteQuad(mx, barY, mx + fillW, barY + barH, 0.0f, p.textARGB);
        SpriteFlush();

        if (p.nativeFont)
        {
            __try { DrawNativeText(bandY, bandH, w, h); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (p.faultStep == 0) p.faultStep = -1;
            }
            if (p.faultStep != 0)
                p.nativeFont = false;       // the glyph set draws for the rest of the load
            else
                return;
        }

        // Fallback: the 5x7 set, laid out on the same two rows.
        char head[64], pct[24], detail[96], eta[48];
        strncpy_s(head, sizeof(head), p.head, _TRUNCATE);
        strncpy_s(pct, sizeof(pct), p.pct, _TRUNCATE);
        strncpy_s(detail, sizeof(detail), p.detail, _TRUNCATE);
        strncpy_s(eta, sizeof(eta), p.eta, _TRUNCATE);

        const float px1 = (std::max)(2.0f, (float)(int)(bandH / 26.0f));
        const float px2 = (std::max)(1.0f, (float)(int)(bandH / 40.0f));
        const float y1  = bandY + bandH * kRow1Frac;
        const float y2  = bandY + bandH * kRow2Frac;

        SpriteSetTexture(nullptr, 0);
        if (head[0])   DrawGlyphText(head, mx, y1, px1, p.textARGB);
        if (pct[0])    DrawGlyphText(pct, w - mx - GlyphTextWidth(pct, px1), y1, px1, p.textARGB);
        if (detail[0]) DrawGlyphText(detail, mx, y2, px2, p.detailARGB);
        if (eta[0])    DrawGlyphText(eta, w - mx - GlyphTextWidth(eta, px2), y2, px2, p.detailARGB);
        SpriteFlush();
    }
}
