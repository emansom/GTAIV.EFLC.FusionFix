#pragma once
// ===========================================================================
//  warmmusic.h — what the warm-up SOUNDS like.
//
//  WHY THIS EXISTS. The warm pass holds the game's own loading screen open for
//  as long as the pipelines take, and on a cold cache that is tens of minutes.
//  The loading screen's music is six short layered stems (cello, organ and
//  strings, two each) that the engine starts once and lets loop, so a long hold
//  is the same forty seconds of music over and over.
//
//  WHAT IT DOES. It rotates through the music the game ALREADY HAS RESIDENT at
//  this point in the boot, using the engine's own audio entity and the engine's
//  own calls -- no files of ours, no decoder of ours, nothing mixed outside the
//  game's own mixer, so the player's music volume applies exactly as it does to
//  the loading music it replaces.
//
//  WHAT IS REACHABLE HERE, AND WHAT IS NOT (pc/audio/config/sounds.dat15):
//    * The LOADING wave bank is loaded for the loading screen and holds
//      LOADING_TUNE, MENU_STREAMED, the DEATH_ set, STARTING_TUNE,
//      END_CREDITS_THEME_TUNE and INTRO_CUTSCENE_TRACK. Every sound whose wave
//      lives there can be created by name at the gate.
//    * The frontend audio entity 0x01176888 owns both loading-music stem sets --
//      LOADING_MUSIC_* and INSTALL_MUSIC_* (the console install screen's, a
//      second full arrangement of the same theme that the PC build still ships)
//      -- through ONE engine call with a flag.
//    * Radio tracks and MENU_MUSIC_STREAMED are STREAMED sounds: they have no
//      wave until one is loaded through a STREAM wave slot, and the engine's
//      own way of doing that on this very screen is 0x008E47C0, which plays
//      LOADING_TUNE through the slot it looks up by the name "RADIO3_A".
//      RADIO_DANCE_MIX_FK is Electro-Choc's continuous François K club mix,
//      which is the closest thing the base game has to a "club track" that is
//      one sound rather than a radio station with a DJ, adverts and a schedule.
//    * What is NOT reachable: a radio STATION (it wants the world, a listener
//      and the retune machinery) and the episodes' club interiors. A name that
//      the engine will not make costs a log line and the next track.
//
//  RULES IT KEEPS
//    * Never leave anything of ours playing into gameplay: End() stops our
//      sound, which also gives the stream slot back, and it is called on every
//      path out of the gate including a faulting pass.
//    * Hand the screen back the way it was found: if the engine's stems were
//      playing when we took over, they are playing again when we leave, and the
//      game's own first frame stops them as it always does (0x008DA390 ->
//      0x008E5800 from BuildFrameRenderLists_FF).
//    * One ini key turns the whole thing off.
//
//  THREAD. Everything here runs on the MAIN thread, from the gate's hold loop --
//  the same thread on which the engine itself calls 0x008E55F0 (from
//  0x008DA400) and 0x008E5850 (from rageBoot_InitSession).
//
//  TIMING RULE. Every address below is above 0x004FB000, so it is outside the
//  SecuROM-encrypted range and safe to fingerprint at ASI load.
// ===========================================================================

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace warmmusic
{
    // ---------------------------------------------------------------------
    //  Addresses (GTA IV 1.2.0.59, image base 0x400000).
    // ---------------------------------------------------------------------
    constexpr uintptr_t kVA_Entity        = 0x01176888;  // the frontend audio entity
    constexpr uintptr_t kVA_StopStems     = 0x008E5800;  // __thiscall(entity)
    constexpr uintptr_t kVA_StartStems    = 0x008E55F0;  // __thiscall(entity, char bInstallSet)
    constexpr uintptr_t kVA_InitParams    = 0x008E3D50;  // __thiscall(audSoundInitParams*)
    constexpr uintptr_t kVA_LookupByName  = 0x00887DF0;  // __cdecl(const char*) -> id
    constexpr uintptr_t kVA_CreateAndPlay = 0x00E393D0;  // __thiscall(entity, name, &slot, params, -1,0,0)
    constexpr uintptr_t kVA_StopSound     = 0x00891850;  // __thiscall(sound, 0)
    constexpr uintptr_t kVA_OutputModeB   = 0x01030C0C;  // the byte the menu music copies into params+0x44
    // The commit. Creating a sound and playing it is not enough: it reaches the
    // mixer only when the sound manager walks its list, and every engine path
    // that touches this music ends with the same two globals and this call
    // (0x008E583A: PUSH [0x011618FC] / MOV ECX,[0x0115F848] / CALL 0x008AA790).
    // Without it a track is a live sound object that makes no sound at all --
    // measured twice, at -inf dB, before this line existed.
    constexpr uintptr_t kVA_Commit        = 0x008AA790;  // __thiscall(manager, u32)
    constexpr uintptr_t kVA_CommitThis    = 0x0115F848;
    constexpr uintptr_t kVA_CommitArg     = 0x011618FC;

    // The stream slot a named track is loaded through. "RADIO3_A" is what
    // 0x008E47C0 uses for LOADING_TUNE on this same loading screen, and
    // waveslots.xml says it is one of the six STREAM slots.
    constexpr const char* kStreamSlot = "RADIO3_A";

    // audSoundInitParams, as the engine fills it for the loading stems at
    // 0x008E5628..0x008E5687. 0x48 bytes; only these five fields are touched.
    constexpr uint32_t kIP_Size      = 0x48;
    constexpr uint32_t kIP_Volume    = 0x00;   // float, dB
    constexpr uint32_t kIP_Predelay  = 0x08;   // u32 ms
    // +0x18 is the WAVE SLOT the sound's wave is loaded through, not a mixer
    // category: 0x00E39230 hands it straight to audSound::Play 0x00891150,
    // which is where 0x008E47C0 puts the id it looks up for "RADIO3_A" when it
    // plays LOADING_TUNE -- a streamed track, on this same loading screen.
    constexpr uint32_t kIP_WaveSlot  = 0x18;
    constexpr uint32_t kIP_Fade      = 0x28;   // u32 ms
    constexpr uint32_t kIP_OutMode   = 0x44;   // u8, from 0x01030C0C
    constexpr uint32_t kIP_Flags46   = 0x46;   // u8, |= 0x10

    // The entity's own fields (0x008E55F0 / 0x008E5800).
    constexpr uint32_t kEnt_Index     = 0x04;   // i16; -1 means "not registered", every create refuses
    constexpr uint32_t kEnt_Stem0     = 0x420;  // six sound slots
    constexpr uint32_t kEnt_StemsOn   = 0x43C;  // u8, 1 while the stems are playing

    // ---------------------------------------------------------------------
    //  Fingerprints. If any of these is not this build's, the whole feature
    //  stays off: a wrong call into the audio engine is a crash on the main
    //  thread with the loading screen pinned.
    // ---------------------------------------------------------------------
    struct Check { uintptr_t va; const char* what; uint8_t bytes[24]; uint8_t n; };

    static const Check kChecks[] = {
        // PUSH EBX / MOV EBX,ECX / CMP byte[ebx+0x43C],0
        { kVA_StopStems,     "0x008E5800 stop loading stems",   { 0x53, 0x8B, 0xD9, 0x80, 0xBB, 0x3C }, 6 },
        // SUB ESP,0x48 / PUSH EBX / PUSH ESI / MOV ESI,ECX
        { kVA_StartStems,    "0x008E55F0 start loading stems",  { 0x83, 0xEC, 0x48, 0x53, 0x56, 0x8B }, 6 },
        // MOV [ECX],0 / MOV [ECX+4],0
        { kVA_InitParams,    "0x008E3D50 audSoundInitParams",   { 0xC7, 0x01, 0x00, 0x00, 0x00, 0x00 }, 6 },
        // LEA EAX,[ESP+4] / PUSH EAX / MOV ECX,...
        { kVA_LookupByName,  "0x00887DF0 lookup by name",       { 0x8D, 0x44, 0x24, 0x04, 0x50, 0xB9 }, 6 },
        // SUB ESP,0x48 / PUSH ESI / MOV ESI,ECX
        { kVA_CreateAndPlay, "0x00E393D0 create and play",      { 0x83, 0xEC, 0x48, 0x56, 0x8B, 0xF1 }, 6 },
        // PUSH EBX / PUSH ESI / MOV ESI,ECX / MOVZX EAX,byte[esi+4]
        { kVA_StopSound,     "0x00891850 stop sound",           { 0x53, 0x56, 0x8B, 0xF1, 0x0F, 0xB6 }, 6 },
        // THE COMMIT, and the one that has to be more than a prologue: this is
        // a __thiscall into the sound manager with `this` read out of a global,
        // made twice per track change on the main thread with the loading
        // screen pinned, so calling the wrong function here is the crash this
        // whole table exists to prevent -- and `PUSH EBP / MOV EBP,ESP /
        // AND ESP,-8` is the opening of thousands of functions in this build.
        // So it runs past the prologue to the frame size and the first field
        // test, whose displacement (+0x3230) is this build's:
        //   PUSH EBP / MOV EBP,ESP / AND ESP,-8 / SUB ESP,0x44 / PUSH EBX /
        //   PUSH ESI / PUSH EDI / MOV EDI,ECX / CMP byte[edi+0x3230],0
        { kVA_Commit,        "0x008AA790 commit sounds",
          { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x44, 0x53, 0x56, 0x57,
            0x8B, 0xF9, 0x80, 0xBF, 0x30, 0x32, 0x00, 0x00 }, 20 },
    };

    // ---------------------------------------------------------------------
    //  One track of the rotation.
    //
    //  kind == Stems  -> the engine's own six-stem loading music, `flag` picks
    //                    the LOADING_ (0) or INSTALL_ (1) set.
    //  kind == Named  -> one sound created by name. `seconds` 0 means "until it
    //                    ends by itself"; the engine nulls our slot pointer when
    //                    a sound finishes, which is how that is detected.
    // ---------------------------------------------------------------------
    enum class Kind { Stems, Named };

    struct Track
    {
        Kind        kind = Kind::Stems;
        int         flag = 0;
        std::string name;
        int         seconds = 0;
    };

    // The shipped rotation. It opens on what the game itself is already playing,
    // so the first minute of a warm-up sounds exactly like an ordinary load, and
    // only then brings in the rest.
    inline std::vector<Track> DefaultTracks()
    {
        return {
            { Kind::Stems, 0, "",                    150 },  // the loading music, as shipped
            { Kind::Named, 0, "MENU_MUSIC_STREAMED",   0 },  // the pause / frontend menu music
            { Kind::Stems, 0, "",                    120 },  // back to the loading music
            { Kind::Named, 0, "RADIO_DANCE_MIX_FK",  600 },  // Electro-Choc's club mix
            { Kind::Named, 0, "INTRO_MUSIC_TRACK",     0 },  // the intro cutscene track
            { Kind::Named, 0, "END_CREDITS_MUSIC",     0 },  // the end-credits theme
        };
        // NOT HERE, AND MEASURED: the INSTALL_MUSIC_ set. 0x008E55F0's flag
        // picks it and the sounds exist in sounds.dat15, but the PC build's
        // LOADING wave bank holds no INSTALL_ wave -- the bank is
        // LOADING_TUNE, MENU_STREAMED, the DEATH_ set, STARTING_TUNE,
        // END_CREDITS_THEME_TUNE and INTRO_CUTSCENE_TRACK -- and a run with it
        // in the rotation recorded -inf dB for its whole slot. Silence is
        // worse than repetition, so it is out.
    }

    // ---------------------------------------------------------------------
    //  State. One instance, owned by the gate.
    // ---------------------------------------------------------------------
    struct State
    {
        bool                validated = false;
        bool                armed = false;       // validated AND turned on in the ini
        bool                running = false;
        std::vector<Track>  tracks;
        size_t              index = 0;
        int64_t             trackStartUs = 0;
        int64_t             lastPollUs = 0;
        uintptr_t           sound = 0;           // OUR sound; the engine nulls the slot when it ends
        uintptr_t           slotAddr = 0;        // where the engine keeps that pointer
        bool                hadStems = false;
        uint32_t            played = 0, failed = 0;
    };

    inline State& S()
    {
        static State s;
        return s;
    }

    // Logging is the mod's; the gate hands us its own Log so this header does
    // not reach back into shaderprecompile.ixx.
    using LogFn = void(*)(const char*, ...);
    inline LogFn& Log()
    {
        static LogFn f = nullptr;
        return f;
    }
    inline void Say(const char* fmt, ...)
    {
        if (!Log()) return;
        char b[512];
        va_list ap; va_start(ap, fmt);
        _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
        va_end(ap);
        Log()("%s", b);
    }

    // ---------------------------------------------------------------------
    //  Safe reads, same rules as bootgate.h: nothing here may fault.
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
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
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

    inline uintptr_t Entity()
    {
        uintptr_t e = Rebase(kVA_Entity);
        // 0x00E39780 refuses every create when this is -1, and every engine
        // caller tests it first; so do we, rather than find out by crashing.
        int16_t idx = -1;
        if (!Peek(e + kEnt_Index, idx) || idx == -1) return 0;
        return e;
    }

    inline bool StemsPlaying()
    {
        uintptr_t e = Rebase(kVA_Entity);
        uint8_t on = 0;
        return Peek(e + kEnt_StemsOn, on) && on != 0;
    }

    // ---------------------------------------------------------------------
    //  Validation.
    // ---------------------------------------------------------------------
    inline bool Validate(std::string& why)
    {
        for (const Check& c : kChecks)
        {
            const uint8_t* at = (const uint8_t*)Rebase(c.va);
            if (!Readable(at, c.n) || memcmp(at, c.bytes, c.n) != 0)
            {
                why = std::string(c.what) + " is not this build's";
                return false;
            }
        }
        S().validated = true;
        return true;
    }

    // ---------------------------------------------------------------------
    //  The engine calls.
    // ---------------------------------------------------------------------
    inline void StopStems()
    {
        uintptr_t e = Entity();
        if (!e) return;
        ((void(__thiscall*)(void*))Rebase(kVA_StopStems))((void*)e);
    }

    inline void StartStems(int installSet)
    {
        uintptr_t e = Entity();
        if (!e) return;
        // 0x008E55F0 stops the six slots itself before it refills them.
        ((void(__thiscall*)(void*, int))Rebase(kVA_StartStems))((void*)e, installSet ? 1 : 0);
    }

    // audSoundInitParams, filled exactly the way 0x008E55F0 fills it for the
    // loading stems -- same volume, same category, same fade -- so anything we
    // play is mixed on the bus the loading music was already using and the
    // player's music volume applies to it unchanged.
    inline void FillParams(uint8_t* p, const char* waveSlot)
    {
        memset(p, 0, kIP_Size);
        ((void(__thiscall*)(void*))Rebase(kVA_InitParams))(p);
        const float vol = 6.0f;
        memcpy(p + kIP_Volume, &vol, 4);
        const uint32_t ms = 2000;
        memcpy(p + kIP_Predelay, &ms, 4);
        memcpy(p + kIP_Fade, &ms, 4);
        p[kIP_Flags46] = (uint8_t)(p[kIP_Flags46] | 0x10);
        uint32_t slot = ((uint32_t(__cdecl*)(const char*))Rebase(kVA_LookupByName))(waveSlot);
        memcpy(p + kIP_WaveSlot, &slot, 4);
        uint8_t mode = 0;
        if (Peek(Rebase(kVA_OutputModeB), mode)) p[kIP_OutMode] = mode;
    }

    // Hand what was just created or stopped to the sound manager. Every engine
    // path that plays this music ends with it; ours has to as well.
    inline void Commit()
    {
        uintptr_t mgr = 0;
        uint32_t arg = 0;
        if (!Peek(Rebase(kVA_CommitThis), mgr) || !mgr) return;
        Peek(Rebase(kVA_CommitArg), arg);
        ((void(__thiscall*)(void*, uint32_t))Rebase(kVA_Commit))((void*)mgr, arg);
    }

    // Stop OUR sound. The stream slot needs no releasing: it is the engine's
    // own named slot, and stopping the sound that is using it is what gives it
    // back -- exactly as the game does with LOADING_TUNE at 0x008E5850.
    inline void StopOurSound()
    {
        State& s = S();
        if (s.sound)
        {
            // The engine nulls the caller's slot when a sound ends on its own,
            // so re-read it: stopping a sound object the engine already
            // recycled is the one way this can take the process with it.
            uintptr_t live = 0;
            if (s.slotAddr) Peek(s.slotAddr, live);
            if (live == s.sound)
            {
                ((void(__thiscall*)(void*, int))Rebase(kVA_StopSound))((void*)s.sound, 0);
                Commit();
            }
        }
        s.sound = 0;
    }

    // Where the engine writes (and later clears) the pointer to our sound. One
    // slot, reused by every track: only one of ours ever plays at a time.
    inline uintptr_t* OurSlot()
    {
        static uintptr_t slot = 0;
        return &slot;
    }

    // Start one named sound. Returns false if the engine would not make it.
    //
    // THROUGH A STREAM SLOT, and that is a correction from a measured run.
    // Creating MENU_MUSIC_STREAMED and RADIO_DANCE_MIX_FK with the loading
    // stems' own parameters SUCCEEDS -- a live sound object comes back and
    // stays alive for as long as it is left alone -- and is completely silent:
    // the recording is at -inf dB for the whole of both tracks (2026-09-21).
    // The reason is the field at +0x18. It is not a mixer category, it is the
    // WAVE SLOT the wave is loaded through, and the stems' "GPS" is a BANK slot
    // (pc/audio/config/waveslots.xml) which holds no streamed music.
    //
    // The engine's own answer for a streamed track on this very screen is
    // 0x008E47C0: create LOADING_TUNE without playing it, look up the name
    // "RADIO3_A" -- one of the six STREAM slots -- and hand that id to
    // audSound::Play. Create-and-play does exactly the same thing in one call
    // when +0x18 carries the id, so that is what this does.
    inline bool StartNamed(const std::string& name)
    {
        State& s = S();
        uintptr_t e = Entity();
        if (!e) return false;
        StopOurSound();

        uintptr_t* slot = OurSlot();
        *slot = 0;
        s.slotAddr = (uintptr_t)slot;

        uint8_t params[kIP_Size];
        FillParams(params, kStreamSlot);
        uint32_t slotId = 0;
        memcpy(&slotId, params + kIP_WaveSlot, 4);

        ((void(__thiscall*)(void*, const char*, uintptr_t*, void*, int, int, int))
            Rebase(kVA_CreateAndPlay))((void*)e, name.c_str(), slot, params, -1, 0, 0);
        if (!*slot)
        {
            Say("warm music: the engine has no sound called %s", name.c_str());
            s.failed++;
            return false;
        }
        s.sound = *slot;
        s.played++;
        Commit();
        Say("warm music: playing %s (stream slot %s = %u)", name.c_str(), kStreamSlot, slotId);
        return true;
    }

    // ---------------------------------------------------------------------
    //  The rotation.
    // ---------------------------------------------------------------------
    // Start track i, or the first one after it that will actually play. A name
    // the audio data does not define, or one whose wave bank is not loaded at a
    // loading screen, must cost a line in the log and not a silent screen.
    inline void StartTrack(size_t i, int64_t nowUs)
    {
        State& s = S();
        if (s.tracks.empty()) return;
        for (size_t tries = 0; tries < s.tracks.size(); tries++)
        {
            s.index = (i + tries) % s.tracks.size();
            s.trackStartUs = nowUs;
            const Track& t = s.tracks[s.index];
            if (t.kind == Kind::Stems)
            {
                StopOurSound();
                // Restarting the set the engine is already playing would put a
                // seam in the music for no reason.
                if (!(t.flag == 0 && StemsPlaying()))
                {
                    StartStems(t.flag);
                    Say("warm music: the %s stems", t.flag ? "install" : "loading");
                }
                return;
            }
            StopStems();
            if (StartNamed(t.name)) return;
        }
        // Nothing in the list would play. Hand the screen back to the music the
        // game itself puts here rather than leaving it silent, and stop trying.
        StartStems(0);
        s.running = false;
        Say("warm music: nothing in the rotation would play - the game's own "
            "loading music is back on and the rotation is off for this load");
    }

    // ini: "LOADING", "INSTALL" or a sound name, each optionally ":seconds",
    // comma separated. Anything unparseable is skipped with a line in the log
    // rather than silently dropped.
    inline std::vector<Track> ParseTracks(const std::string& spec)
    {
        std::vector<Track> out;
        size_t at = 0;
        while (at <= spec.size())
        {
            size_t comma = spec.find(',', at);
            std::string item = spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            at = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
            // trim
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r'))
                item.pop_back();
            if (item.empty()) continue;
            int seconds = 0;
            size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                seconds = atoi(item.c_str() + colon + 1);
                item.erase(colon);
            }
            Track t;
            if (_stricmp(item.c_str(), "LOADING") == 0) { t.kind = Kind::Stems; t.flag = 0; }
            else if (_stricmp(item.c_str(), "INSTALL") == 0) { t.kind = Kind::Stems; t.flag = 1; }
            else { t.kind = Kind::Named; t.name = item; }
            t.seconds = seconds;
            out.push_back(std::move(t));
        }
        return out;
    }

    // Begin: the gate is open and the loading screen is pinned.
    inline void BeginImpl(int64_t nowUs, bool enabled, const std::string& spec)
    {
        State& s = S();
        s.running = false;
        if (!enabled) { Say("warm music: off (PrecompileWarmMusic = 0)"); return; }
        std::string why;
        if (!s.validated && !Validate(why))
        {
            Say("warm music: off - %s", why.c_str());
            return;
        }
        if (!Entity())
        {
            Say("warm music: off - the frontend audio entity is not registered at the gate");
            return;
        }
        s.tracks = spec.empty() ? DefaultTracks() : ParseTracks(spec);
        if (s.tracks.empty()) { Say("warm music: off - no tracks"); return; }
        s.hadStems = StemsPlaying();
        s.armed = true;
        s.running = true;
        s.played = s.failed = 0;
        Say("warm music: on, %zu tracks%s", s.tracks.size(),
            s.hadStems ? " (the engine's loading stems were playing)" : "");
        StartTrack(0, nowUs);
    }

    // Tick: from the gate's hold loop, on the main thread. Cheap and bounded --
    // it does nothing at all for 50 ms at a time.
    inline void TickImpl(int64_t nowUs)
    {
        State& s = S();
        if (!s.running) return;
        if (nowUs - s.lastPollUs < 50000) return;
        s.lastPollUs = nowUs;

        const Track& t = s.tracks[s.index];
        const int64_t elapsed = nowUs - s.trackStartUs;
        bool advance = false;
        if (t.kind == Kind::Named)
        {
            uintptr_t live = 0;
            if (s.slotAddr) Peek(s.slotAddr, live);
            if (!live) advance = true;                       // it ended by itself
            else if (t.seconds > 0 && elapsed > (int64_t)t.seconds * 1000000ll) advance = true;
        }
        else if (t.seconds > 0 && elapsed > (int64_t)t.seconds * 1000000ll)
        {
            advance = true;
        }
        if (advance) StartTrack(s.index + 1, nowUs);
    }

    // End: the pass is done and the loading screen is about to come down.
    // NOTHING of ours may still be audible after this.
    inline void EndImpl()
    {
        State& s = S();
        if (!s.running)
        {
            // Still sweep: Begin may have started something and then failed.
            StopOurSound();
            return;
        }
        s.running = false;
        StopOurSound();
        // Put the screen back the way it was found. The game's own first frame
        // stops the stems (0x005C20DF -> 0x008DA390 -> 0x008E5800), so this is
        // handing them back, not leaving them on.
        if (s.hadStems && !StemsPlaying()) StartStems(0);
        Say("warm music: off - %u tracks played, %u refused; ours stopped, the game's %s",
            s.played, s.failed, s.hadStems ? "loading music handed back" : "audio untouched");
    }

    // ---------------------------------------------------------------------
    //  CONTAINMENT. Everything above calls into the game's audio engine, on
    //  the MAIN thread, at a gate where the loading screen is pinned by a
    //  patched rageBoot_LoadscreenEnd -- so a fault here does not just lose the
    //  music, it ends the process on a loading screen that would never have
    //  come down anyway. Every other new engine call at this gate is behind an
    //  SEH frame (the phase build behind GuardedCall, the overlay's text inside
    //  DrawProgress, the pass on its own fiber); these three were the only ones
    //  that were not, and the fingerprints above cannot see a build where a
    //  STRUCTURE changed under a function that still starts with the right
    //  bytes.
    //
    //  MSVC refuses __try in a function that needs C++ unwinding, so the SEH
    //  frame lives in these trivial wrappers and the work stays in the *Impl
    //  above. A fault turns the feature off for the rest of the load and sweeps
    //  whatever it had playing, under an SEH frame of its own -- because the
    //  one thing worse than losing the music is a track of ours playing into
    //  gameplay.
    // ---------------------------------------------------------------------
    inline void SweepQuiet()
    {
        __try { StopOurSound(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { S().sound = 0; }
    }

    inline void Faulted(const char* doing)
    {
        S().running = false;
        S().armed = false;
        Say("warm music: FAULTED while %s - off for the rest of the load", doing);
    }

    inline void Begin(int64_t nowUs, bool enabled, const std::string& spec)
    {
        __try { BeginImpl(nowUs, enabled, spec); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Faulted("starting the rotation"); SweepQuiet(); }
    }

    inline void Tick(int64_t nowUs)
    {
        __try { TickImpl(nowUs); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Faulted("changing track"); SweepQuiet(); }
    }

    inline void End()
    {
        __try { EndImpl(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Faulted("stopping"); SweepQuiet(); }
    }
}
