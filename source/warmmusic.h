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
//  AND IT ROTATES THE EPISODE'S OWN MUSIC. Which episode is loading is read at
//  the gate from the engine's own episode index (comvars' _dwCurrentEpisode,
//  which FusionFix already finds by pattern and which every other episodic fix
//  in this mod tests: 0 = IV, 1 = TLAD, 2 = TBoGT), never from a path or a
//  file listing. Each episode gets a list drawn only from what THAT episode's
//  audio data defines -- Vladivostok FM for Niko, Liberty Rock Radio's TLAD
//  half for Johnny, Electro-Choc's club mixes and K109's TBoGT disco for Luis
//  -- plus that episode's own loading and menu music. The candidate names were
//  read out of the shipped tables (pc/audio/config/sounds.dat15 for the base
//  game, TLAD/pc/audio/config/EP1_*.DAT15 and TBoGT/.../EP2_*.DAT15 for the
//  episodes, whose entries are [u32][u32][u8 length][name]) and then every one
//  of them was PLAYED AT THIS GATE AND RECORDED off the system mix, because
//  the tables are not the last word: see EVERY NAME HERE WAS HEARD below.
//
//  WHY THE LISTS ARE PER EPISODE, measured rather than assumed (2026-09-21).
//  An episode-foreign name does NOT fail the same way in every episode:
//    * on an IV boot, E2_RADIO_DANCE_MIX_CROOKERS_MIX is REFUSED outright --
//      "the engine has no sound called ...", which is the safe failure;
//    * on a TLAD boot, the same name is CREATED and then plays absolute
//      digital silence, -120 dBFS for its whole slot, which is the failure
//      this file exists to avoid;
//    * on a TBoGT boot, TLAD's E1_RADIO_LIBERTY_ROCK_HAIROFTHEDOG_PH plays
//      perfectly, byte-identical in the capture to the way it plays in TLAD.
//  So "the engine made the sound" is not evidence that it will be audible,
//  and the only safe rule is the one this file follows: play an episode's
//  names only in that episode.
//
//  LOADING_TUNE does the episode work for free: all three tables define it,
//  each pointing at its own wave (EP1_SFX\LOADINGTUNE_1, EP2_SFX\LOADING_TUNE),
//  and the three captures have three different spectra -- centroid 3090 Hz on
//  IV, 1641 on TLAD, 3443 on TBoGT -- so the override is real.
//
//  EVERY NAME HERE WAS HEARD, and some obvious ones did not survive it. Each
//  list below was played at this gate with PrecompileWarmMusicTracks* at
//  ~12-25 s an entry while the system mix was recorded, and each slot's RMS
//  was measured. Struck out by measurement:
//    * MENU_MUSIC_STREAMED -- SILENT in ALL THREE episodes, including a 25 s
//      slot and a second pass on an IV boot. This is the name the previous
//      rotation used for "the pause / frontend menu music", and the run that
//      "spent ten minutes on it" spent ten minutes on silence. The frontend
//      music that IS audible here is MENU_MUSIC_1 (-24 dBFS on IV, -23 on
//      TBoGT, each episode's own arrangement) -- but not on TLAD, where
//      MENU_MUSIC_1, MENU_MUSIC_DULCIMER and MENU_MUSIC_PERCUSSION are all
//      silent, so TLAD's list carries no menu music and leans on the
//      episode's own loading tune and intro theme instead.
//    * INTRO_MUSIC_TRACK -- silent on an IV boot, although the episode's own
//      EP1_INTRO_MUSIC_TRACK is fine on TLAD.
//    * the INSTALL_MUSIC_ set -- silent, measured earlier and again here.
//  Kept because they were heard: the six-stem loading set, LOADING_TUNE,
//  STARTING_TUNE, END_CREDITS_MUSIC, MENU_MUSIC_1 (IV and TBoGT), all twelve
//  Vladivostok FM songs, all fourteen of TLAD's Liberty Rock records and four
//  of the base station's, EP1_INTRO_MUSIC_TRACK, E1_END_CREDITS_FIRST_TRACK,
//  both Electro-Choc mixes, the TBoGT eurobeat mix, ten K109 records and the
//  Hercules dance-floor mix.
//
//  WHAT IS REACHABLE HERE, AND WHAT IS NOT:
//    * The frontend audio entity 0x01176888 owns both loading-music stem sets --
//      LOADING_MUSIC_* and INSTALL_MUSIC_* (the console install screen's, a
//      second full arrangement of the same theme that the PC build still ships)
//      -- through ONE engine call with a flag. The LOADING_ set is audible in
//      IV and in TLAD; the INSTALL_ set is silent (its waves are not in the
//      PC build's LOADING bank).
//    * Radio tracks are STREAMED sounds: they have no wave until one is loaded
//      through a STREAM wave slot, and the engine's own way of doing that on
//      this very screen is 0x008E47C0, which plays LOADING_TUNE through the
//      slot it looks up by the name "RADIO3_A". That is the slot every Named
//      track here goes through, and the streaming is not instant: a track
//      given a 12 s slot in the probe runs was occasionally silent on its
//      first play and audible on its second. The shipped slots are minutes.
//    * What is NOT reachable: a radio STATION (it wants the world, a listener
//      and the retune machinery). A name that the engine will not make costs a
//      log line and the next track.
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

    // ---------------------------------------------------------------------
    //  Which episode is loading.
    //
    //  THE GLOBAL, not a path. comvars resolves _dwCurrentEpisode by pattern
    //  ("83 3D ? ? ? ? ? 75 0F 6A 02") and the whole mod reads it: cheats.ixx
    //  tests it for the TLAD and TBoGT cheat sets, cutscenecam.ixx for TBoGT,
    //  altdialogue.ixx indexes an array with it, and currentEpisodePath() maps
    //  it through { "", "TLAD", "TBoGT" } -- so 0/1/2 is the mod's own settled
    //  reading of it and this file does not get a second opinion.
    //
    //  The pointer arrives from the gate (this header is in the global module
    //  fragment and cannot import comvars). It is validated the same way every
    //  other engine read here is: readable, and holding a value this build's
    //  episode index can actually be. Anything else means "do not know", and
    //  "do not know" plays the mixed rotation rather than an episode's names.
    // ---------------------------------------------------------------------
    enum class Episode { Unknown = -1, IV = 0, TLAD = 1, TBoGT = 2 };

    inline const char* EpisodeName(Episode e)
    {
        switch (e)
        {
        case Episode::IV:    return "GTA IV";
        case Episode::TLAD:  return "The Lost and Damned";
        case Episode::TBoGT: return "The Ballad of Gay Tony";
        default:             return "an episode it could not name";
        }
    }

    // ResolveEpisode is further down, with the other engine reads: it needs the
    // guarded Peek, and nothing in this file may fault.

    // The shipped rotation for the base game. It opens on what the game itself
    // is already playing, so the first minute of a warm-up sounds exactly like
    // an ordinary load, and only then brings in the rest.
    inline std::vector<Track> DefaultTracksIV()
    {
        // EVERY ENTRY HAS A DURATION, and that is a correction from a measured
        // run. "0 seconds" means "until the sound ends by itself", which is
        // detected by the engine nulling our slot -- and a track that LOOPS
        // never does. On the first long hold of the merged build the rotation
        // reached MENU_MUSIC_STREAMED at 2 minutes and was still on it ten
        // minutes later, which is the repetition this feature exists to fix,
        // in a new place. So the shipped rotation is timed end to end, and
        // Tick has a hard cap over the top of it for anything the ini asks for
        // at 0.
        //
        // NIKO'S EPISODE IS VLADIVOSTOK FM. Its twelve songs are the Russian
        // pop of the base game, they are single sounds of exactly the shape
        // that was proven to play at this gate, and twelve of them at five
        // minutes is over an hour before the list can come round. Each name is
        // a flat sound in pc/audio/config/sounds.dat15 (RADIO_VLADIVOSTOK\X
        // gives the wave, RADIO_VLADIVOSTOK_X the sound the engine creates);
        // the station's DJ links (RADIO_VLADIVOSTOK_SOLO_*) and idents
        // (_ID_*) are deliberately not here, because a warm-up wants music and
        // not Ruslana reading the news.
        //
        // The durations are a CAP, not a length. A radio track ends by itself
        // at three to five minutes and the engine nulls our slot, which is how
        // the rotation moves on; 330 seconds is only there so a track that
        // turns out to loop cannot hold the screen. (That is the correction
        // the first long hold forced: "0 seconds" means "until it ends by
        // itself", and a track that loops never does, so one held the rotation
        // for ten minutes.)
        //
        // A name the engine will not make costs a log line and the next entry,
        // so a track whose wave will not stream at this gate is not a risk --
        // it is a line in FusionFix.shaders.log.
        //
        // Every entry below was played at this gate and recorded: -21.8 to
        // -31.0 dBFS for the twelve songs, -42.5 for the stems, -33.2 for the
        // loading tune, -24.3 for the menu music, -28.0 for the starting tune
        // and -24.3 for the credits. Nothing here is silent.
        return {
            // THE OPENER IS SHORT ON PURPOSE. Once the driver's cache is warm this
            // gate is about 126 seconds, so anything past the second entry is never
            // heard on the shipped path -- the first measured default run played one
            // 126-second loading tune and nothing else. The episode's own music is
            // therefore entry two, and the scene-setting material follows it.
            { Kind::Stems, 0, "",                                  45 },  // the loading music, as shipped
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_CHIKI",          330 },
            { Kind::Named, 0, "MENU_MUSIC_1",                     240 },  // the pause / frontend menu music
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_REPREZENTY",     330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_ADD_SPEED",      330 },
            { Kind::Stems, 0, "",                                  90 },  // back to the loading music
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_MON_AMI",        330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_HOT_SUMMER",     330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_UNDERGROUND",    330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_NOCHJU",         330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_LIBERTYCITY",    330 },
            { Kind::Named, 0, "LOADING_TUNE",                     150 },  // the game's own loading tune
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_PLAY",           330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_KARAOKE",        330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_BEGO_RANCHO_SONG", 330 },
            { Kind::Named, 0, "RADIO_VLADIVOSTOK_RIFFMASTER_TONY",  330 },
            { Kind::Named, 0, "STARTING_TUNE",                    120 },
            { Kind::Named, 0, "END_CREDITS_MUSIC",                300 },  // the end-credits theme
        };
        // NOT HERE, AND MEASURED: MENU_MUSIC_STREAMED and INTRO_MUSIC_TRACK,
        // both -120 dBFS for their whole slot on an IV boot (the first of them
        // twice, once in a slot of 25 s), and the INSTALL_MUSIC_ set, silent
        // for the same reason -- the PC build's LOADING bank holds none of
        // those waves. Silence is worse than repetition, so they are out; the
        // header has the whole measurement.
        //
        // ALSO NOT HERE ANY MORE: the four continuous DJ mixes (Electro-Choc's
        // Francois K, Massive B Soundsystem, The Classics, RamJam FM). They
        // are still the right shape and still play, but only one of them is
        // Niko's music by genre and Electro-Choc belongs to Luis's list now.
        // PrecompileWarmMusicTracksIV is how to have them back.
    }

    // TLAD: Liberty Rock Radio. Johnny's episode re-scores LRR with fourteen
    // more hard-rock records of its own (E1_RADIO_LIBERTY_ROCK_*_PH in
    // TLAD/pc/audio/config/EP1_RADIO_SOUNDS.DAT15, waves in EP1_SFX.rpf, which
    // e1_radio.xml registers as a wavepack) on top of the base station's
    // seventeen, and it redefines LOADING_TUNE to its own loading music -- so
    // that name, unchanged, is already the episode's. The four base LRR
    // records at the end are the hardest of the base station's, kept so the
    // list is over an hour and a half.
    //
    // NO MENU MUSIC IN THIS ONE, and that is measured rather than an
    // oversight: on a TLAD boot MENU_MUSIC_STREAMED, MENU_MUSIC_1,
    // MENU_MUSIC_DULCIMER and MENU_MUSIC_PERCUSSION are all -120 dBFS for
    // their whole slot, so there is no name through which the episode's
    // frontend music is audible at this gate. Its loading tune (-32.3) and
    // its intro theme (-28.7) carry the non-radio part of the list instead.
    inline std::vector<Track> DefaultTracksTLAD()
    {
        // NO STEMS IN THIS LIST EITHER. The six-stem loading set
        // (LOADING_MUSIC_*) is base-game-only -- neither episode table
        // redefines it -- and it IS audible on a TLAD boot (-38.3 dBFS,
        // centroid 505 Hz), which is the problem: it is Niko's cellos, on
        // Johnny's loading screen. The episode's own LOADING_TUNE opens
        // instead, and the engine's stems, if they were the thing playing when
        // the gate opened, are handed back untouched at the end as before.
        return {
            // Short opener, hard rock second: a warm-cache gate is about 126 seconds
            // and only the first two entries are ever reached on the shipped path.
            { Kind::Named, 0, "LOADING_TUNE",                             45 },  // EP1_SFX\LOADINGTUNE_1
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_00_HIGHWAYSTAR_PH", 330 },
            { Kind::Named, 0, "EP1_INTRO_MUSIC_TRACK",                   180 },  // the episode's intro theme
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_HAIROFTHEDOG_PH",   330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_WHEELOFSTEEL_PH",   330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_LORDOFTHETHIGHS_PH",330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_RENEGADE_PH",       330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_WILDSIDE_PH",       330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_SATURDAYNIGHTSPECIAL_PH", 330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_GOTOHELL_PH",       330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_FUNKNUMBER49_PH",   330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_FREERIDE_PH",       330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_DEADORALIVE_PH",    330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_CHINAGROVE_PH",     330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_EVERYPICTURETELLS_PH", 330 },
            { Kind::Named, 0, "E1_RADIO_LIBERTY_ROCK_DRIVINWHEEL_PH",    330 },
            { Kind::Named, 0, "RADIO_LIBERTY_ROCK_IWANNABEYOURDOG",      330 },
            { Kind::Named, 0, "RADIO_LIBERTY_ROCK_ROCKYMOUNTAINWAY",     330 },
            { Kind::Named, 0, "RADIO_LIBERTY_ROCK_JAILBREAK",            330 },
            { Kind::Named, 0, "RADIO_LIBERTY_ROCK_HERSTRUT",             330 },
            { Kind::Named, 0, "E1_END_CREDITS_FIRST_TRACK",              300 },  // the episode's credits
        };
        // All twenty were played at this gate and recorded: -23.0 to -37.1
        // dBFS. The one that needed a second look is
        // RADIO_LIBERTY_ROCK_IWANNABEYOURDOG, silent on its first 12-second
        // probe slot and fine (-28.0) when the rotation came round to it --
        // the stream slot had not finished loading it. With a slot of five
        // and a half minutes that cannot happen.
    }

    // TBoGT: the club. Luis's episode gives Electro-Choc a second continuous
    // club mix (the Crookers one, E2_RADIO_DANCE_MIX_CROOKERS_MIX), turns
    // Vladivostok FM into a eurobeat mix, adds ten disco records to K109 The
    // Studio, and redefines LOADING_TUNE and the MENU_MUSIC_ set to its own --
    // all in TBoGT/pc/audio/config/EP2_*.DAT15 with the waves in EP2_SFX.rpf,
    // which both e2_audio.xml and e2_radio.xml register. The base game's own
    // Electro-Choc mix is still loaded under the episode and is still the same
    // club, so it is in this list and not Niko's.
    //
    // Every entry was played at this gate and recorded: -23.4 to -32.0 dBFS,
    // sixteen of sixteen audible, none refused.
    inline std::vector<Track> DefaultTracksTBoGT()
    {
        // MENU_MUSIC_1, not MENU_MUSIC_STREAMED. MENU_MUSIC_STREAMED is
        // silent at this gate in every episode (see the header); MENU_MUSIC_1
        // here is the EP2_SFX\MENU_MUSIC set that TBoGT's own table puts
        // behind it, and it is the loudest thing in the rotation at -23.4.
        return {
            // Short opener, the club second: a warm-cache gate is about 126 seconds
            // and only the first two entries are ever reached on the shipped path.
            { Kind::Named, 0, "LOADING_TUNE",                             45 },  // EP2_SFX\LOADING_TUNE
            { Kind::Named, 0, "E2_RADIO_DANCE_MIX_CROOKERS_MIX",         600 },  // Electro-Choc, the Crookers mix
            { Kind::Named, 0, "MENU_MUSIC_1",                            240 },  // EP2_SFX\MENU_MUSIC
            { Kind::Named, 0, "E2_RADIO_VLADIVOSTOK_EUROBEAT_MIX",       600 },  // Vladivostok FM, the eurobeat mix
            { Kind::Named, 0, "RADIO_DANCE_MIX_FK",                      420 },  // Electro-Choc, the Francois K mix
            { Kind::Named, 0, "DANCING_HERCULES_MIX",                    300 },  // the Hercules dance floor
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_DISCOINFERNO",   330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_EVERYBODYDANCE", 330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_BOOGIEOOGIE",    330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_RELIGHTMYFIRE",  330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_GREATESTDANCER", 330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_YOUNGHEARTSRUNFREE", 330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_SHAKEYOURGROOVETHING", 330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_MENERGY",        330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_PUTURBODYINIT",  330 },
            { Kind::Named, 0, "E2_RADIO_K109_THE_STUDIO_BUSSTOP",        330 },
        };
        // DANCING_HERCULES_MIX (EP2_SFX\HERCULES_CLUB_MIX) is in the list
        // because it was HEARD, which is not what was expected of it: it is
        // the club interior's own mix rather than a radio track, so a loading
        // screen with no listener looked like the wrong place for it. It
        // played at -32.0 dBFS, six decibels under the radio tracks but
        // nowhere near silence, so it stays -- the dance floor of the episode
        // the player is about to load.
    }

    inline std::vector<Track> DefaultTracks(Episode e)
    {
        switch (e)
        {
        case Episode::TLAD:  return DefaultTracksTLAD();
        case Episode::TBoGT: return DefaultTracksTBoGT();
        case Episode::IV:    return DefaultTracksIV();
        default: break;
        }
        // The episode could not be read, so play the base game's list. Every
        // name in it is a BASE name, and a base name is the only kind that is
        // defined whichever episode this turns out to be -- the measurement
        // that settles it is RADIO_VLADIVOSTOK_CHIKI, played and recorded in
        // all three (-23.0 dBFS in each, the same spectrum every time), and
        // the base loading stems and LOADING_TUNE, audible in each as well.
        // An episode's OWN list would be a gamble here; this one is not.
        return DefaultTracksIV();
    }

    // ---------------------------------------------------------------------
    //  What the gate hands us: the ini, and where to read the episode from.
    //  One struct rather than five arguments, because the ini gained a key per
    //  episode and the call site should stay one line.
    // ---------------------------------------------------------------------
    struct Options
    {
        bool           enabled = true;       // PrecompileWarmMusic
        std::string    tracks;               // PrecompileWarmMusicTracks, every episode
        std::string    tracksIV;             // PrecompileWarmMusicTracksIV
        std::string    tracksTLAD;           // PrecompileWarmMusicTracksTLAD
        std::string    tracksTBoGT;          // PrecompileWarmMusicTracksTBoGT
        const int32_t* episode = nullptr;    // comvars' _dwCurrentEpisode
    };

    // ---------------------------------------------------------------------
    //  State. One instance, owned by the gate.
    // ---------------------------------------------------------------------
    struct State
    {
        bool                validated = false;
        bool                armed = false;       // validated AND turned on in the ini
        bool                running = false;
        Episode             episode = Episode::Unknown;
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

    // The episode index, validated. See the Episode enum above for why this is
    // the global the whole mod already reads rather than a path or a file list.
    inline Episode ResolveEpisode(const int32_t* global, std::string& why)
    {
        if (!global)
        {
            why = "FusionFix never found the episode global in this build";
            return Episode::Unknown;
        }
        int32_t v = -1;
        if (!Peek((uintptr_t)global, v))
        {
            why = "the episode global is not readable at the gate";
            return Episode::Unknown;
        }
        if (v < 0 || v > 2)
        {
            char b[96];
            _snprintf_s(b, sizeof(b), _TRUNCATE,
                        "the episode global reads %d, which is not 0, 1 or 2", v);
            why = b;
            return Episode::Unknown;
        }
        return (Episode)v;
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

    // Which ini key wins for this episode. The per-episode key is the specific
    // one, so it beats the all-episodes key; with neither set the episode's own
    // shipped list plays. Named so the log line can say which was used.
    inline const std::string& SpecFor(const Options& o, Episode e, const char*& key)
    {
        const std::string* per = nullptr;
        switch (e)
        {
        case Episode::IV:    per = &o.tracksIV;    key = "PrecompileWarmMusicTracksIV";    break;
        case Episode::TLAD:  per = &o.tracksTLAD;  key = "PrecompileWarmMusicTracksTLAD";  break;
        case Episode::TBoGT: per = &o.tracksTBoGT; key = "PrecompileWarmMusicTracksTBoGT"; break;
        default: break;
        }
        if (per && !per->empty()) return *per;
        key = "PrecompileWarmMusicTracks";
        return o.tracks;
    }

    // Begin: the gate is open and the loading screen is pinned.
    inline void BeginImpl(int64_t nowUs, const Options& o)
    {
        State& s = S();
        s.running = false;
        if (!o.enabled) { Say("warm music: off (PrecompileWarmMusic = 0)"); return; }
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

        // The episode, before the list: a list is only correct for the episode
        // whose audio data defines its names.
        std::string noEpisode;
        s.episode = ResolveEpisode(o.episode, noEpisode);
        if (s.episode == Episode::Unknown)
            Say("warm music: %s - playing the tracks every episode has", noEpisode.c_str());

        const char* key = nullptr;
        const std::string& spec = SpecFor(o, s.episode, key);
        s.tracks = spec.empty() ? DefaultTracks(s.episode) : ParseTracks(spec);
        if (s.tracks.empty()) { Say("warm music: off - no tracks"); return; }
        s.hadStems = StemsPlaying();
        s.armed = true;
        s.running = true;
        s.played = s.failed = 0;
        Say("warm music: on for %s, %zu tracks from %s%s", EpisodeName(s.episode), s.tracks.size(),
            spec.empty() ? "the shipped rotation" : key,
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
        // Nothing holds the rotation for more than a quarter of an hour, ini or
        // not: "until it ends by itself" cannot end a track that loops, and a
        // twenty-minute hold on one loop is the thing this feature is for.
        constexpr int64_t kHardCapUs = 900ll * 1000000ll;
        bool advance = elapsed > kHardCapUs;
        if (advance)
        {
            Say("warm music: %s has had its quarter of an hour - moving on",
                t.kind == Kind::Named ? t.name.c_str() : "the loading stems");
        }
        else if (t.kind == Kind::Named)
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

    inline void Begin(int64_t nowUs, const Options& o)
    {
        __try { BeginImpl(nowUs, o); }
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
