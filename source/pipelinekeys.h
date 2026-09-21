// ===========================================================================
// pipelinekeys.h — on-disk format for captured D3D9 pipeline keys.
//
// Shared by the writer (shadercapture.ixx) and the reader (shaderprecompile.ixx's
// replay pass). It lives in one header on purpose: a silent layout drift between
// the two would produce a file that parses but replays the wrong state, which is
// indistinguishable from the synthetic pass's failure mode and just as expensive
// to diagnose. One definition, one truth.
// ===========================================================================
#pragma once

#include <d3d9.h>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include "fxc_parse.h"

namespace pipelinekeys
{
    constexpr uint32_t kMaxRT       = 4;

    // Vertex streams whose instancing is recorded. GTA IV's declarations only ever
    // use streams 0 and 1 (every capture so far: 29 declarations, max stream 1), and
    // the replay binds 0..3, so 4 covers the game with room to spare.
    constexpr uint32_t kMaxStreams  = 4;
    constexpr uint32_t kPSSamplers  = 16;       // s0..s15
    constexpr uint32_t kVSSamplers  = 4;        // D3DVERTEXTEXTURESAMPLER0..3
    constexpr uint32_t kNumSamplers = kPSSamplers + kVSSamplers;

    constexpr uint32_t kDeclNone = 0xFFFFFFFFu; // KeyRecord::declIndex when the FVF path was used

    // Sampler kinds stored in KeyRecord::samplerType.
    enum SamplerKind : uint8_t { kSamplerNone = 0, kSampler2D = 1, kSamplerCube = 2, kSamplerVolume = 3 };

    // How DXVK samples a slot, stored in KeyRecord::samplerMode. Same encoding as
    // dxvk 3.1.1's D3D9SamplerMode (d3d9_state.h), which is what lands in the
    // psSamplerModes / vsSamplerModes spec constants -- a different mode is a
    // different pipeline. Which one a slot gets is NOT a function of the texture's
    // resource type: it comes from the FORMAT of the texture bound there plus the
    // sampler state, neither of which the v2 key carried.
    //   Fetch4    D3DSAMP_MIPMAPLODBIAS == 'GET4' and MAGFILTER == POINT and the
    //             format is one of DXVK's single-channel set (INTZ, DF16, DF24,
    //             R16F, R32F, A8, L8, L16)
    //   Dref      the format is a depth format DXVK depth-compares; INTZ, DF16 and
    //             DF24 are explicitly BLACKLISTED from this
    //   DrefClamp Dref on a format DXVK had to emulate with D32F
    enum SamplerMode : uint8_t { kModeDefault = 0, kModeFetch4 = 1, kModeDref = 2, kModeDrefClamp = 3 };

    // One fixed-function texture stage, as DXVK reads it for D3D9SpecData's
    // stageOps/stageArgs (spec IDs 8..19). Only pipelines with NO pixel shader use
    // these -- a programmable shader never declares those spec constants, so DXVK
    // zeroes them and they split nothing. Recorded raw rather than pre-packed: DXVK
    // derives the packed form from the raw values together with the bound textures
    // and the render target, and the replay rebuilds all three, so letting DXVK do
    // the packing is the only way the two can never disagree.
    //
    // WHAT A REAL CAPTURE OF THIS INSTALL HOLDS, measured 2026-09-21 off the first
    // v3 recording (14,543 keys, 10.2 M draws, GTA IV Complete Edition + FusionFix
    // + Liberty City Plates under DXVK 3.1.1): 1180 keys have no pixel shader and
    // ALL 1180 carry ONE stage block, which is D3D9's own default -- exactly what
    // the replay had been binding before the field existed. 473 keys carry a clip
    // plane, every one of them count 1, which is what widening a pre-v3 record from
    // the popcount of the enable mask infers. Nothing carries a b# register, a
    // projected stage or a Fetch4/depth-compare sampler. The v3 capture and the v2
    // snapshot it replaced therefore produce the SAME partition -- 5445 replay
    // identities and 599 base identities offline either way -- so on this install
    // the fields cost nothing and prove the widening right rather than replacing
    // it. That is a fact about this install, not about the game: a mod that draws
    // its own 2D, or an episode that does, can still need a second stage block, and
    // the pass's own "of N keys that RECORDED it" line is how to check.
#pragma pack(push, 1)
    struct FFStage
    {
        uint8_t colorOp;        // D3DTSS_COLOROP
        uint8_t alphaOp;        // D3DTSS_ALPHAOP
        uint8_t resultArg;      // D3DTSS_RESULTARG
        uint8_t colorArg[3];    // D3DTSS_COLORARG0, 1, 2
        uint8_t alphaArg[3];    // D3DTSS_ALPHAARG0, 1, 2
    };
#pragma pack(pop)
    constexpr uint32_t kFFStages = 8;   // caps::TextureStageCount

    // D3D9's own defaults for the texture stages, which is what a device that has
    // never been told otherwise holds. Used for keys from a pre-v3 file: those
    // predate the field, and the default block is both the likeliest truth for a
    // shader-only engine and exactly what the replay used to draw them with, so an
    // old file keeps producing the pipelines it always did.
    inline void DefaultFFStages(FFStage* out)
    {
        for (uint32_t s = 0; s < kFFStages; s++)
        {
            out[s].colorOp   = (uint8_t)(s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
            out[s].alphaOp   = (uint8_t)(s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
            out[s].resultArg = (uint8_t)D3DTA_CURRENT;
            out[s].colorArg[0] = (uint8_t)D3DTA_CURRENT;
            out[s].colorArg[1] = (uint8_t)D3DTA_TEXTURE;
            out[s].colorArg[2] = (uint8_t)D3DTA_CURRENT;
            out[s].alphaArg[0] = (uint8_t)D3DTA_CURRENT;
            out[s].alphaArg[1] = (uint8_t)D3DTA_TEXTURE;
            out[s].alphaArg[2] = (uint8_t)D3DTA_CURRENT;
        }
    }

    // Which of COLORARG0/1/2 an op actually reads, exactly as
    // D3D9DeviceEx::GetTextureStageArgMask decides it (d3d9_device.cpp:8273).
    inline uint32_t FFArgMask(uint8_t op)
    {
        switch (op)
        {
        case D3DTOP_DISABLE:
        case D3DTOP_BUMPENVMAP:
        case D3DTOP_BUMPENVMAPLUMINANCE:  return 0b000u;
        case D3DTOP_SELECTARG1:
        case D3DTOP_PREMODULATE:          return 0b010u;
        case D3DTOP_SELECTARG2:           return 0b100u;
        case D3DTOP_MULTIPLYADD:
        case D3DTOP_LERP:                 return 0b111u;
        default:                          return 0b110u;
        }
    }

    // Put a stage block into the one form DXVK would derive from it, so that two
    // ways of saying the same thing hash the same.
    //
    // THIS IS NOT COSMETIC. The capture writes a disabled stage as all-zero with
    // both ops DISABLE, which is what D3D9SpecData::disableTextureStage stores
    // (d3d9_state.h:266); DefaultFFStages -- what a pre-v3 record is widened to --
    // writes D3D9's own defaults, resultArg = CURRENT and args CURRENT/TEXTURE/
    // CURRENT. Both mean "this stage is off", and without this they hash
    // differently, so a v3 capture replayed beside the shipped v2 baseline would
    // draw every no-pixel-shader identity twice for nothing.
    //
    // Two more rules, from the same place, which merge rather than split: DXVK
    // stops at the first stage whose COLOROP is DISABLE and calls
    // disableTextureStage for every stage from there on (d3d9_device.cpp:8471), and
    // it zeroes each argument the op does not consume (:8408-8417). Bytes DXVK
    // discards cannot make two pipelines, so keying on them only costs draws.
    inline void CanonicalFFStages(FFStage* st)
    {
        bool off = false;
        for (uint32_t s = 0; s < kFFStages; s++)
        {
            FFStage& f = st[s];
            if (off || f.colorOp == D3DTOP_DISABLE)
            {
                off = true;
                f = FFStage{};
                f.colorOp = (uint8_t)D3DTOP_DISABLE;
                f.alphaOp = (uint8_t)D3DTOP_DISABLE;
                continue;
            }
            const uint32_t cm = FFArgMask(f.colorOp);
            const uint32_t am = FFArgMask(f.alphaOp);
            for (uint32_t a = 0; a < 3; a++)
            {
                if (!(cm & (1u << a))) f.colorArg[a] = 0;
                if (!(am & (1u << a))) f.alphaArg[a] = 0;
            }
        }
    }

    // The render states we record. `pipeline` marks the ones that (as far as we can
    // tell from DXVK's d3d9 backend) are baked into the Vulkan pipeline rather than
    // set dynamically. Everything is recorded either way — the flag splits the two
    // histograms in the summary, so we can see how much of the key space is real
    // pipeline variation and how much is dynamic state a replay could ignore.
    struct RSDef { D3DRENDERSTATETYPE rs; const char* name; bool pipeline; };

    inline constexpr RSDef kTrackedRS[] = {
        { D3DRS_ZENABLE,                  "ZENABLE",                  true  },
        { D3DRS_ZWRITEENABLE,             "ZWRITEENABLE",             true  },
        { D3DRS_ZFUNC,                    "ZFUNC",                    true  },
        { D3DRS_ALPHATESTENABLE,          "ALPHATESTENABLE",          true  },
        { D3DRS_ALPHAFUNC,                "ALPHAFUNC",                true  },
        { D3DRS_ALPHAREF,                 "ALPHAREF",                 false },  // dynamic
        { D3DRS_ALPHABLENDENABLE,         "ALPHABLENDENABLE",         true  },
        { D3DRS_SRCBLEND,                 "SRCBLEND",                 true  },
        { D3DRS_DESTBLEND,                "DESTBLEND",                true  },
        { D3DRS_BLENDOP,                  "BLENDOP",                  true  },
        { D3DRS_SEPARATEALPHABLENDENABLE, "SEPARATEALPHABLENDENABLE", true  },
        { D3DRS_SRCBLENDALPHA,            "SRCBLENDALPHA",            true  },
        { D3DRS_DESTBLENDALPHA,           "DESTBLENDALPHA",           true  },
        { D3DRS_BLENDOPALPHA,             "BLENDOPALPHA",             true  },
        { D3DRS_COLORWRITEENABLE,         "COLORWRITEENABLE",         true  },
        { D3DRS_COLORWRITEENABLE1,        "COLORWRITEENABLE1",        true  },
        { D3DRS_COLORWRITEENABLE2,        "COLORWRITEENABLE2",        true  },
        { D3DRS_COLORWRITEENABLE3,        "COLORWRITEENABLE3",        true  },
        { D3DRS_CULLMODE,                 "CULLMODE",                 true  },
        { D3DRS_FILLMODE,                 "FILLMODE",                 true  },
        { D3DRS_SHADEMODE,                "SHADEMODE",                true  },
        { D3DRS_STENCILENABLE,            "STENCILENABLE",            true  },
        { D3DRS_TWOSIDEDSTENCILMODE,      "TWOSIDEDSTENCILMODE",      true  },
        { D3DRS_STENCILFUNC,              "STENCILFUNC",              true  },
        { D3DRS_STENCILFAIL,              "STENCILFAIL",              true  },
        { D3DRS_STENCILZFAIL,             "STENCILZFAIL",             true  },
        { D3DRS_STENCILPASS,              "STENCILPASS",              true  },
        { D3DRS_STENCILREF,               "STENCILREF",               false },  // dynamic
        { D3DRS_STENCILMASK,              "STENCILMASK",              false },  // dynamic
        { D3DRS_STENCILWRITEMASK,         "STENCILWRITEMASK",         false },  // dynamic
        { D3DRS_CCW_STENCILFUNC,          "CCW_STENCILFUNC",          true  },
        { D3DRS_CCW_STENCILFAIL,          "CCW_STENCILFAIL",          true  },
        { D3DRS_CCW_STENCILZFAIL,         "CCW_STENCILZFAIL",         true  },
        { D3DRS_CCW_STENCILPASS,          "CCW_STENCILPASS",          true  },
        { D3DRS_FOGENABLE,                "FOGENABLE",                true  },
        { D3DRS_FOGTABLEMODE,             "FOGTABLEMODE",             true  },
        { D3DRS_FOGVERTEXMODE,            "FOGVERTEXMODE",            true  },
        { D3DRS_RANGEFOGENABLE,           "RANGEFOGENABLE",           true  },
        { D3DRS_CLIPPLANEENABLE,          "CLIPPLANEENABLE",          true  },
        { D3DRS_CLIPPING,                 "CLIPPING",                 true  },
        { D3DRS_MULTISAMPLEANTIALIAS,     "MULTISAMPLEANTIALIAS",     true  },
        { D3DRS_MULTISAMPLEMASK,          "MULTISAMPLEMASK",          true  },
        { D3DRS_POINTSPRITEENABLE,        "POINTSPRITEENABLE",        true  },
        { D3DRS_POINTSCALEENABLE,         "POINTSCALEENABLE",         true  },
        { D3DRS_LIGHTING,                 "LIGHTING",                 true  },
        { D3DRS_COLORVERTEX,              "COLORVERTEX",              true  },
        { D3DRS_SPECULARENABLE,           "SPECULARENABLE",           true  },
        { D3DRS_NORMALIZENORMALS,         "NORMALIZENORMALS",         true  },
        { D3DRS_DIFFUSEMATERIALSOURCE,    "DIFFUSEMATERIALSOURCE",    true  },
        { D3DRS_SPECULARMATERIALSOURCE,   "SPECULARMATERIALSOURCE",   true  },
        { D3DRS_AMBIENTMATERIALSOURCE,    "AMBIENTMATERIALSOURCE",    true  },
        { D3DRS_EMISSIVEMATERIALSOURCE,   "EMISSIVEMATERIALSOURCE",   true  },
        { D3DRS_VERTEXBLEND,              "VERTEXBLEND",              true  },
        { D3DRS_INDEXEDVERTEXBLENDENABLE, "INDEXEDVERTEXBLENDENABLE", true  },
        { D3DRS_SRGBWRITEENABLE,          "SRGBWRITEENABLE",          true  },
        { D3DRS_DEPTHBIAS,                "DEPTHBIAS",                false },  // dynamic
        { D3DRS_SLOPESCALEDEPTHBIAS,      "SLOPESCALEDEPTHBIAS",      false },  // dynamic
        { D3DRS_SCISSORTESTENABLE,        "SCISSORTESTENABLE",        false },  // dynamic
    };
    constexpr uint32_t kNumRS = (uint32_t)(sizeof(kTrackedRS) / sizeof(kTrackedRS[0]));

    // One recorded draw state. Fully self-describing: replay must be able to rebuild
    // the state from this alone, so nothing here is a hash except the shaders (which
    // are matched back to the .fxc database by bytecode hash) and the declaration
    // (stored out-of-line in a table, indexed from here).
#pragma pack(push, 1)
    struct KeyRecord
    {
        uint64_t vsHash;                    // FNV-1a of VS bytecode, 0 = no VS bound
        uint64_t psHash;                    // FNV-1a of PS bytecode, 0 = no PS bound
        uint32_t declIndex;                 // index into the declaration table, or kDeclNone
        uint32_t fvf;                       // only meaningful when declIndex == kDeclNone
        uint32_t primType;                  // D3DPRIMITIVETYPE
        uint32_t upDraw;                    // 1 if this came from a Draw*PrimitiveUP
        uint32_t rtFmt[kMaxRT];             // D3DFORMAT per bound RT, 0 = unbound
        uint32_t dsFmt;                     // D3DFORMAT of the depth/stencil surface, 0 = none
        uint32_t msType;                    // D3DMULTISAMPLE_TYPE of the targets (v2+)
        uint32_t msQuality;                 // multisample quality level (v2+)
        uint32_t rs[kNumRS];                // values of kTrackedRS, in order
        uint8_t  samplerType[kNumSamplers]; // SamplerKind per sampler
        uint32_t streamFreq[kMaxStreams];   // InstanceFreq() per stream the declaration uses (v2+)
        // ---- v3: the rest of what DXVK specialises a D3D9 pipeline on ---------
        // Everything below is DEVICE state at the draw that v2 left to whatever
        // the last thing to touch the device happened to leave behind. It all
        // enters DxvkGraphicsPipelineStateInfo::sc through D3D9SpecData, so with
        // pipeline libraries off -- this rig -- each one costs a real compile.
        uint16_t vsBools;                   // b0..b15, vertex stage   (spec ID 3)
        uint16_t psBools;                   // b0..b15, pixel stage    (spec ID 6)
        // The number of clip planes DXVK counts, which is NOT the enable mask: it
        // walks the six planes and keeps those that are enabled AND whose
        // coefficients are not all zero (d3d9_device.cpp, UpdateClipPlanes). The
        // plane VALUES are therefore part of the key, and nothing recorded them.
        uint8_t  clipPlaneCount;            //                         (spec ID 0)
        // D3DTTFF_PROJECTED per texture stage 0..7. Every pixel shader that
        // samples a texture declares spec ID 2, so this is not fixed-function-only.
        uint8_t  projMask;                  //                         (spec ID 2)
        uint8_t  samplerMode[kNumSamplers]; // SamplerMode per slot    (spec IDs 3-5)
        FFStage  ffStage[kFFStages];        // no pixel shader only    (spec IDs 8-19)
        uint32_t count;                     // how many draws hit this key
        uint32_t firstFrame;                // frame ordinal of first sighting
    };

#pragma pack(pop)

    // The part of SetStreamSourceFreq that selects a Vulkan pipeline.
    //
    // DXVK turns each stream the declaration uses into a vertex binding whose input
    // rate and divisor come from this setting (d3d9_device.cpp, BindInputLayout):
    // INSTANCEDATA makes it a per-instance binding with divisor = the low 23 bits;
    // anything else is per-vertex. The instance COUNT (INDEXEDDATA | n on stream 0)
    // only reaches the draw call, not the pipeline, so recording it would split one
    // pipeline into a key per batch size. Keep exactly what DXVK keeps.
    inline uint32_t InstanceFreq(UINT setting)
    {
        return (setting & D3DSTREAMSOURCE_INSTANCEDATA)
            ? (D3DSTREAMSOURCE_INSTANCEDATA | (setting & 0x7FFFFFu))
            : 0u;
    }

    // Everything that identifies a key, excluding the bookkeeping tail.
    constexpr size_t kKeyHashBytes = offsetof(KeyRecord, count);

    // Cache bundles are per graphics configuration.
    //
    // Recorded keys carry render-target formats, so a cache captured at one
    // configuration both MISSES (nothing matches, no warming) and WASTES (builds
    // pipelines that configuration never uses) when replayed under another. Naming
    // the file after the configuration keeps them separate, lets a user accumulate
    // one bundle per setup, and makes a shipped set of bundles selectable rather
    // than a single file that is wrong for most people.
    //
    // Back-buffer FORMAT and the MSAA level are what actually move the formats in a
    // key. Resolution does NOT, and used to be in this name.
    //
    // Measured 2026-09-18 by capturing the same autoload scene at 1920x1080 and at
    // 1280x720 (driven at runtime through the game's own mode table + device reset):
    // both produced the SAME 14 render-target/depth/multisample combinations, down to
    // the rare tails matching on the exact draw count (191, 114, 3). A repeatability
    // control -- two captures at identical settings -- showed the combo set has no
    // run-to-run noise, so that null result is real rather than an insensitive test.
    //
    // It makes sense: a Vulkan pipeline bakes attachment formats and sample counts,
    // not framebuffer dimensions, and GTA IV's targets are fixed engine formats
    // (R32F shadows, the A8R8G8B8 x3 + R16F G-buffer, the fp16 scene). Bucketing by
    // resolution therefore fragmented the cache for nothing: it split one cache into
    // near-identical copies, reset a user's coverage whenever they changed
    // resolution, and would have forced us to ship a separate baseline per bucket.
    // One self-contained file per graphics configuration: keys, the declarations they
    // index, and the shader bytecode they name. This is what a player sends in.
    inline std::string CacheName(uint32_t bbFormat, int msaa)
    {
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "FusionFix.pipelinecache.f%u-ms%d.bin",
                    bbFormat, msaa);
        return std::string(buf);
    }

    // Log to a FILE as well as OutputDebugString.
    //
    // On Linux the debug strings are readable because Wine funnels them into its own
    // debug channel, which the harness greps. On WINDOWS nothing captures
    // OutputDebugString without a debugger attached, so a contributor could never see
    // this log at all -- and the crowd-test harness's Verify-Precompiler step hunts
    // for a log file the ASI never wrote, which is why it could never pass there.
    //
    // It also has to be a file for the log to survive a reboot: when the only way to
    // get results off a Windows install is to mount the partition from the other OS,
    // anything that lived only in a debug channel is gone.
    inline void LogLine(const char* tag, const char* msg)
    {
        OutputDebugStringA(tag);
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");

        static std::string path = []() -> std::string {
            char buf[MAX_PATH] = {};
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&LogLine), &self);
            if (!GetModuleFileNameA(self, buf, MAX_PATH)) return std::string();
            std::string s(buf);
            auto slash = s.find_last_of("\\/");
            if (slash == std::string::npos) return std::string();
            return s.substr(0, slash + 1) + "FusionFix.shaders.log";
        }();
        if (path.empty()) return;

        // Truncate once per process so the file is this run's log, not an unbounded
        // accumulation a contributor would have to figure out how to read.
        static bool opened = false;
        FILE* f = fopen(path.c_str(), opened ? "a" : "w");
        if (!f) return;
        opened = true;
        fprintf(f, "%s%s\n", tag, msg);
        fclose(f);
    }

    inline uint64_t Fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ull)
    {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    // The content hash that names and de-duplicates the files players share between
    // PCs (FusionFix.<h>.bin in plugins\d3d9cache\, FusionFix.<h>.foz in
    // plugins\pipelinecache\): 64-bit FNV-1a with the standard offset basis, over the
    // whole file. NOT Fnv1a above, whose basis is 1469598103934665603 -- the standard
    // one with its last digit lost -- and which cannot be fixed, because every shader
    // and key hash in every cache file is computed with it. Pass the previous result
    // as `h` to hash a file in pieces.
    inline uint64_t ContentHash(const void* data, size_t len, uint64_t h = 0xcbf29ce484222325ull)
    {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
        return h;
    }

    // A file in the wrong one of the two shared folders gets ONE line in the log,
    // however many of the readers (the D3D9 replay, vkcapture) come across it.
    enum Misplaced : uint32_t
    {
        kBinInPipelineCache = 1,   // a D3D9 cache file (.bin) in the Vulkan folder
        kFozInD3d9Cache     = 2,   // a Vulkan pipeline database (.foz) in the D3D9 folder
    };
    inline void HintMisplaced(const char* tag, Misplaced what, uint32_t n, const std::string& example)
    {
        static std::atomic<uint32_t> given{ 0 };
        if (!n || (given.fetch_or(what) & what)) return;
        const std::string line = what == kBinInPipelineCache
            ? "replay: pipelinecache\\ holds " + std::to_string(n) + " .bin file(s) (e.g. '" + example +
              "') - not loaded: D3D9 cache files belong in plugins\\d3d9cache\\, pipelinecache\\ is for Vulkan .foz files"
            : "replay: d3d9cache\\ holds " + std::to_string(n) + " .foz file(s) (e.g. '" + example +
              "') - not loaded: Vulkan pipeline databases belong in plugins\\pipelinecache\\";
        LogLine(tag, line.c_str());
    }

    // Every shader the PROCESS creates, indexed by the same bytecode hash the
    // capture records.
    //
    // Replay previously built this map only from RAGE's .fxc database, so any draw
    // using a shader FusionFix compiles itself -- SMAA, FXAA, sun shafts, gamma,
    // LOD lights -- had no handle to bind and was skipped. That was 178 of 5110
    // pipelines on the first capture. Filling this from the D3D9 CreateVertexShader
    // / CreatePixelShader hooks catches every shader regardless of origin, including
    // the ones the precompiler creates itself (they go through the same hooked
    // vtable). Populated by shadercapture.ixx, consumed by the replay pass.
    struct ShaderRegistry
    {
        std::unordered_map<uint64_t, IDirect3DVertexShader9*> vs;
        std::unordered_map<uint64_t, IDirect3DPixelShader9*>  ps;
    };

    inline ShaderRegistry& Registry()
    {
        static ShaderRegistry r;
        return r;
    }

    // The thread the loading-screen pass is running on, 0 when it is not running.
    // The capture ignores draws from it: they are the replay's and the overlay's,
    // not the game's. Recording them put every key the replay warms -- the shipped
    // baseline and other PCs' drop-ins included -- into this PC's own capture as if
    // this PC had drawn them, and from there into the snapshot it shares.
    inline std::atomic<unsigned long>& PassThread()
    {
        static std::atomic<unsigned long> id{ 0 };
        return id;
    }

    // The .fxc directory the replay loads: <gameroot>/update/common/shaders/win32_30,
    // the effective installed set (update/ overrides common/), else the base one.
    // One definition, so what cache files leave out and what the replay can resolve
    // are always the same set.
    inline std::filesystem::path InstalledShaderDir()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto root = std::filesystem::path(exe).parent_path();
        auto upd = root / "update" / "common" / "shaders" / "win32_30";
        std::error_code ec;
        if (std::filesystem::exists(upd, ec)) return upd;
        return root / "common" / "shaders" / "win32_30";
    }

    // Every shader in the install's own .fxc files, by the bytecode hash the capture
    // records.
    //
    // Cache files carry bytecode only for shaders the install CANNOT supply. The
    // replay resolves .fxc shaders by hash from the install itself
    // (IndexShadersByHash), so their bytecode in a file warms nothing extra: where
    // the install has the shader it is found there, and where it does not, the
    // pipeline would never be drawn on that install anyway. Storing it only put
    // Rockstar-derived bytecode -- and other mods' shaders, e.g. Liberty City Plates
    // -- into every file a player shares. What still needs carrying is what FusionFix
    // compiles at runtime (SMAA, FXAA, ...), which no .fxc holds.
    //
    // Parsed once, on first use, from whichever thread gets there first. Empty if the
    // directory cannot be parsed, which falls back to carrying all bytecode.
    //
    // The whole set goes to the log: a summary line with a digest of the sorted
    // hashes -- two installs with the same digest resolve exactly the same shaders --
    // then one line per effect naming its shaders, so a difference between two
    // installs (a mod's .fxc, a newer FusionFix build of one effect) can be traced to
    // the file it came from.
    inline const std::unordered_set<uint64_t>& InstalledFxcHashes()
    {
        static std::once_flag once;
        static std::unordered_set<uint64_t> hashes;
        std::call_once(once, [] {
            const std::string dir = InstalledShaderDir().string();
            fxc_db* db = fxc_load_all(dir.c_str());
            if (!db)
            {
                LogLine("[FxcHashes] ", (dir + ": could not be parsed - cache files will carry all bytecode").c_str());
                return;
            }
            const uint32_t n = fxc_unique_count(db);
            std::vector<uint64_t> byIndex(n, 0);
            uint32_t vs = 0, ps = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                const fxc_shader* s = fxc_unique_shader(db, i);
                if (!s || !s->bytecode || !s->size) continue;
                byIndex[i] = Fnv1a(s->bytecode, s->size);
                hashes.insert(byIndex[i]);
                (s->stage == FXC_STAGE_VS ? vs : ps)++;
            }

            std::vector<uint64_t> sorted(hashes.begin(), hashes.end());
            std::sort(sorted.begin(), sorted.end());
            const uint64_t digest = sorted.empty() ? 0 : Fnv1a(sorted.data(), sorted.size() * sizeof(uint64_t));
            char line[512];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%s: %u effects, %zu unique shaders (%u vs, %u ps), set digest %016llx",
                        dir.c_str(), fxc_effect_count(db), hashes.size(), vs, ps,
                        (unsigned long long)digest);
            LogLine("[FxcHashes] ", line);

            for (uint32_t e = 0, ne = fxc_effect_count(db); e < ne; e++)
            {
                const fxc_effect* fx = fxc_get_effect(db, e);
                if (!fx) continue;
                std::string out = std::string("  ") + (fx->name ? fx->name : "?") + ".fxc";
                auto list = [&](const char* label, const uint32_t* map, uint32_t count) {
                    std::vector<uint64_t> hs;
                    for (uint32_t i = 0; i < count; i++)
                        if (map[i] < n && byIndex[map[i]]) hs.push_back(byIndex[map[i]]);
                    std::sort(hs.begin(), hs.end());
                    hs.erase(std::unique(hs.begin(), hs.end()), hs.end());
                    out += std::string(" ") + label + "[" + std::to_string(hs.size()) + "]";
                    for (uint64_t h : hs)
                    {
                        char b[20];
                        _snprintf_s(b, sizeof(b), _TRUNCATE, " %016llx", (unsigned long long)h);
                        out += b;
                    }
                };
                list("vs", fx->vs_local_to_unique, fx->vs_count);
                list("ps", fx->ps_local_to_unique, fx->ps_count);
                LogLine("[FxcHashes] ", out.c_str());
            }
            fxc_free(db);
        });
        return hashes;
    }

    // The bytecode worth writing to a cache file: everything the install cannot
    // supply itself (see InstalledFxcHashes).
    inline bool CarryBytecode(uint64_t hash)
    {
        return InstalledFxcHashes().count(hash) == 0;
    }

    // Every Present on the device, counted at the vtable.
    //
    // The progress overlay redraws itself completely each frame -- full-screen
    // backdrop blit, bar, text -- so a frame it presents cannot be missing the bar.
    // If the bar still flickers, some OTHER present is reaching the screen between
    // ours, showing a frame we never drew. Comparing this against the overlay's own
    // frame count says whether that is happening, which no amount of staring at the
    // overlay code can.
    inline unsigned& DevicePresentCount()
    {
        static unsigned n = 0;
        return n;
    }

    // ---- the Vulkan replay and the loading-screen pass -----------------------
    //
    // vkcapture replays Fossilize databases on DXVK's device from threads of its own
    // and sees every pipeline DXVK creates; shaderprecompile owns the loading screen.
    // They meet here. On the loading screen, strictly in this order:
    //   1. the D3D9 pass (shaderprecompile), then a wait until DXVK is QUIET: nothing
    //      it started compiling -- on its own worker threads too -- is still going;
    //   2. the whole Vulkan replay: this PC's own recording, then other PCs' files
    //      and the player's Steam pre-cache, on most of the cores;
    //   3. another wait until DXVK is quiet, and only then is the loading screen let go.
    // An unwarmed pipeline is a stutter in gameplay; a longer load is not.
    //   * passPlanned / passDone: the loading-screen pass will run / step 1 is over.
    //     The replay waits for it -- and foreign entries are judged against what
    //     DXVK has used on this device, which that pass shows most of. Without the
    //     pass (PrecompileShaders = 0) nothing waits and nothing is held.
    //   * holding: the loading screen is held for the replay, so it may use most of
    //     the cores instead of background threads. PrecompileBudgetSeconds, if set,
    //     ends the hold; the replay then carries on in the background.
    //   * running, phase, done / total: whether the replay is still going, which
    //     part of it, and how many of the pipeline entries it found it has been
    //     through, for the bar.
    //   * watching, inFlight, creations, lastActivityUs: vkcapture's view of DXVK's
    //     own vkCreate*Pipelines / vkCreateShaderModule calls, from every thread --
    //     whether it wraps DXVK's device at all, how many are running right now, how
    //     many have started, and when one last started or finished (ClockUs). The
    //     replay's own creations bypass the wrappers and are not counted.
    //   * compilerCpuUs: the CPU time DXVK's compiler threads ("dxvk-shader-*" and
    //     the IR cache writer "dxvk-cache") have used so far, and how many there are.
    //     DXVK also works on them without any Vulkan call (turning D3D9 shaders into
    //     its IR, with or without pipeline libraries), so a create counter alone
    //     cannot say they are idle. Set once watching is; not free, ask sparingly.
    //     csUs, if asked for, gets the CS thread's ("dxvk-cs") CPU time: busy at
    //     every draw, so a reading that stays 0 there means this cannot be measured
    //     here at all, rather than that the compilers were idle.
    //   * recordPaused / notRecorded: while set, vkcapture records no pipelines, and
    //     counts the ones it leaves out. The D3D9 pass sets it for the keys only other
    //     PCs' files name (drawn last, after a wait until DXVK is quiet) and clears it
    //     once DXVK is quiet again: their pipelines are warmed every launch, but they
    //     are not this PC's, and must not end up in its own Vulkan recording -- nor,
    //     through its shared copy, on the other PCs.
    //   * epochUs / epochLocal: the session clock every "t=" in the log counts from
    //     (vkcapture touches this state when the ASI loads).
    // Quiet cannot see a driver compiling in threads of its own after the create call
    // has returned. RADV does not; it compiles inside the call.

    // Microseconds on the one clock every module shares.
    inline int64_t ClockUs()
    {
        static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (int64_t)f.QuadPart; }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (int64_t)(c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq);
    }

    // ---- the 32-bit address space -------------------------------------------
    //
    // GTA IV is a 32-bit process, and this Proton is an old-wow64 one (its wine
    // ships lib/wine/i386-unix), so the Vulkan loader, DXVK and the Mesa driver
    // all live in the SAME 4 GB the game does. Anything a warm pass leaves
    // behind is spent out of the game's own budget for the rest of the session,
    // and DXVK does not give a graphics pipeline back before its device goes
    // (dxvk_pipemanager.cpp: m_graphicsPipelines is only ever inserted into).
    // So every phase of the pass says here what it cost.
    //
    // ullAvailVirtual is the SUM of the free runs. `largest` is the biggest
    // single one, which is what an allocation actually has to fit in: the two
    // diverge under fragmentation, and a sum on its own cannot see that.
    struct VaStats
    {
        uint64_t avail = 0;     // bytes free in total
        uint64_t largest = 0;   // bytes in the largest single free run
        uint64_t commit = 0;    // bytes committed, private
        uint64_t image = 0;     // bytes committed, images (the PE modules)
        uint64_t mapped = 0;    // bytes committed, file mappings
        uint32_t freeRuns = 0;
        uint32_t regions = 0;
    };

    inline VaStats ReadVa()
    {
        VaStats v;
        MEMORYSTATUSEX ms{ sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms)) v.avail = ms.ullAvailVirtual;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        uintptr_t at = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        MEMORY_BASIC_INFORMATION mbi{};
        while (at < end && VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            const uint64_t size = (uint64_t)mbi.RegionSize;
            if (!size) break;
            v.regions++;
            if (mbi.State == MEM_FREE)
            {
                v.freeRuns++;
                if (size > v.largest) v.largest = size;
            }
            else if (mbi.State == MEM_COMMIT)
            {
                if (mbi.Type == MEM_IMAGE)       v.image  += size;
                else if (mbi.Type == MEM_MAPPED) v.mapped += size;
                else                             v.commit += size;
            }
            at += (uintptr_t)size;
        }
        return v;
    }

    // The cheap half of the above: no region walk, for a loop that has to ask
    // often. Megabytes free in total.
    inline uint64_t AvailVaMB()
    {
        MEMORYSTATUSEX ms{ sizeof(ms) };
        return GlobalMemoryStatusEx(&ms) ? (ms.ullAvailVirtual >> 20) : 0;
    }

    inline std::string VaLine()
    {
        const VaStats v = ReadVa();
        char b[224];
        _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "free %llu MB (largest run %llu MB, %u runs), committed %llu MB private + "
                    "%llu MB image + %llu MB mapped, %u regions",
                    (unsigned long long)(v.avail >> 20), (unsigned long long)(v.largest >> 20),
                    v.freeRuns, (unsigned long long)(v.commit >> 20),
                    (unsigned long long)(v.image >> 20), (unsigned long long)(v.mapped >> 20),
                    v.regions);
        return std::string(b);
    }

    struct VulkanReplayState
    {
        std::atomic<bool> passPlanned{ false }, passDone{ false }, holding{ false }, running{ false };
        std::atomic<uint32_t> done{ 0 }, total{ 0 };
        enum Phase : uint32_t { kPreparing = 0, kOwn = 1, kForeign = 2 };
        std::atomic<uint32_t> phase{ kPreparing };

        std::atomic<bool> watching{ false };
        std::atomic<int32_t> inFlight{ 0 };
        std::atomic<uint32_t> creations{ 0 };
        // Of those creations, the ones that took >= 5 ms, i.e. the ones the
        // driver really compiled rather than found in its own on-disk cache.
        // It is the only signal available in-process for "is the driver cache
        // warm for these pipelines", which is what lets a second launch skip a
        // pass whose whole output is already on disk.
        std::atomic<uint32_t> compiles{ 0 };
        std::atomic<int64_t> lastActivityUs{ 0 };
        uint64_t (*compilerCpuUs)(uint32_t* threads, uint64_t* csUs) = nullptr;
        std::atomic<bool> recordPaused{ false };
        std::atomic<uint32_t> notRecorded{ 0 };   // pipelines DXVK created meanwhile
        // Set while the MOD is creating a D3D9 device of its own (the engine
        // warm pass's throwaway device, ShaderPrecompiler::CreateWarmDevice), so
        // vkcapture can tell that VkDevice from the game's: it is wrapped for
        // the counters, but it must not record, replay or report.
        std::atomic<bool> modOwnedDevice{ false };
        // Whether the game's DXVK device enabled VK_EXT_graphics_pipeline_library
        // (vkcapture reads it off the VkDeviceCreateInfo). It decides whether
        // DXVK builds optimized pipelines inline on its CS thread or queues
        // them to worker threads a fence cannot see, which is why the engine
        // walk waits for DXVK to go QUIET and not merely for the GPU.
        std::atomic<bool> gplEnabled{ false };

        const int64_t epochUs = ClockUs();
        const SYSTEMTIME epochLocal = [] { SYSTEMTIME st{}; GetLocalTime(&st); return st; }();
        double Seconds(int64_t us) const { return (us - epochUs) / 1e6; }
        double Seconds() const { return Seconds(ClockUs()); }
    };
    inline VulkanReplayState& VulkanReplay()
    {
        static VulkanReplayState s;
        return s;
    }

    // ---- shader bytecode sidecar --------------------------------------------
    //
    // A recorded key names its shaders by bytecode hash, so replay has to turn that
    // hash back into a D3D9 shader object. It had two sources, and neither is
    // complete:
    //
    //   * RAGE's .fxc database — every shader the game ships, created by the
    //     precompiler itself. Misses anything FusionFix compiles.
    //   * the runtime Registry above — whatever the process happened to have created
    //     by the time the pass runs. On the last capture that was TWO shaders.
    //
    // The gap is not a hooking-order problem and cannot be fixed by hooking earlier:
    // FusionFix builds SMAA, FXAA, sun shafts, gamma and LOD-light shaders when each
    // effect first runs, which is long after the loading screen the pass runs on. The
    // shader genuinely does not exist yet. 202 of 2034 pipelines were skipped for
    // want of a handle.
    //
    // So the capture stores the bytecode as well, and replay creates the missing
    // shaders from it. That is sound because DXVK keys its shader modules on a hash
    // of the bytecode: a second object built from identical bytes resolves to the
    // same module and therefore warms the same pipeline. The .fxc path already
    // demonstrates this end to end — the precompiler's own 1734 shader objects warm
    // pipelines that the game's separate objects then reuse, which is the entire
    // reason replay measured as a win.
    //
    // The bytecode travels in the same container as the keys that name it, so a
    // contribution can never arrive with keys whose shaders are missing.
    enum ShaderStage : uint32_t { kStageVS = 0, kStagePS = 1 };

    // =======================================================================
    //  ONE-FILE CONTRIBUTION CONTAINER
    //
    //  A player's capture has to be a SINGLE artifact: they send one file, an
    //  external tool indexes and merges many of them, and the result ships back as
    //  one "golden" cache. Three files (keys, shader bytecode, a text summary) made
    //  that a packaging problem for every contributor and an ordering problem for
    //  the merge tool, because the keys are useless without the bytecode they name.
    //
    //  Sectioned rather than one fixed struct so the merge tool can seek to what it
    //  needs -- read the metadata of a thousand contributions to bucket them without
    //  parsing a thousand key tables -- and so a section can be added later without
    //  invalidating readers.
    //
    //  THE METADATA IS NOT DECORATION. Two captures are only mergeable if the game
    //  resolved the SAME shader directory: GTA IV picks between win32_30 and five
    //  vendor-specific variants by probing depth formats (RAWZ -> nv6, DF24 ->
    //  atidx10, INTZ -> nv8, else win32_30), and those directories hold genuinely
    //  different bytecode, so the shader hashes -- and every key naming them -- differ.
    //  Under DXVK that probe is answered by DXVK rather than the vendor driver, which
    //  is what makes one golden cache plausible at all, but DXVK's own format support
    //  can still depend on the Vulkan implementation. So record what was actually
    //  resolved and let the merge tool bucket on it, rather than assuming convergence.
    constexpr uint32_t kCacheMagic   = 0x43504646;   // 'FFPC'

    // The version exists so a stale or foreign file is never MISREAD. Bump this on
    // any layout change. The reader (d3d9cache.h) widens v1 in memory -- a player's
    // other PC may run an older build -- and skips anything newer than it knows.
    // Files are only ever written in this version. (A local capture that cannot be
    // read is moved aside, never overwritten: see the capture's LoadBundle.)
    //
    //   v1  first single-file container
    //   v2  KeyRecord::streamFreq -- instanced streams are a different pipeline
    //   v3  KeyRecord gains the rest of D3D9SpecData's device inputs: the b#
    //       registers per stage, DXVK's clip-plane COUNT, the projected-texture
    //       mask, the per-slot sampler mode and the fixed-function stage state
    constexpr uint32_t kCacheVersion = 3;

    enum CacheSectionId : uint32_t
    {
        kSecMeta    = 1,   // CacheMeta, then metaStrings length-prefixed UTF-8
        kSecRSTypes = 2,   // uint32_t rsType[numRS]
        kSecDecls   = 3,   // count x { uint32_t n; D3DVERTEXELEMENT9 elems[n] }
        kSecKeys    = 4,   // count x KeyRecord
        kSecShaders = 5,   // count x { ShaderBlobHeader; uint8_t code[size] }
    };

    // Fixed order of the strings in kSecMeta. shaderDir is the bucketing key.
    enum CacheMetaString : uint32_t
    {
        kMetaShaderDir = 0,   // "win32_30", "win32_30_nv8", ... or "unknown"
        kMetaAdapter   = 1,   // D3DADAPTER_IDENTIFIER9::Description
        kMetaDriver    = 2,   // under DXVK the Vulkan driver ("radv Mesa 25.2.3"), else the D3D9 driver version
        kMetaOS        = 3,   // "windows" / "wine <version>"
        // The DXVK build: "<module file> <size> fnv:<FNV-1a-64 of the file>". Part of
        // the bucket key for Vulkan-level (Fossilize) caches, whose SPIR-V and state
        // change with every DXVK build. Added within v2: readers take min(stringCount,
        // kMetaStringCount), so files with 4 strings read this as empty.
        kMetaDxvk      = 4,
        kMetaStringCount = 5,
    };

#pragma pack(push, 1)
    struct CacheHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t sectionCount;
        uint32_t reserved;
    };
    struct CacheSection
    {
        uint32_t id;
        uint32_t offset;     // from the start of the file
        uint32_t size;       // bytes
        uint32_t count;      // elements, where the section has them
    };
    struct CacheMeta
    {
        uint32_t numRS;          // reader rejects a mismatch rather than misreading
        uint32_t numSamplers;
        uint32_t bbFormat;       // D3DFORMAT of the back buffer
        int32_t  msaa;           // ReflectionMSAAQuality this capture ran with
        uint32_t frames;         // frames the capture observed
        uint64_t draws;          // draws recorded (this capture's own count)
        uint32_t backend;        // 0 = native D3D9, 1 = DXVK
        uint32_t vendorId;       // from D3DADAPTER_IDENTIFIER9
        uint32_t deviceId;
        uint32_t stringCount;    // == kMetaStringCount for this version
    };
#pragma pack(pop)

#pragma pack(push, 1)
    struct ShaderFileHeader { uint32_t magic; uint32_t version; uint32_t count; };
    struct ShaderBlobHeader { uint64_t hash; uint32_t stage; uint32_t size; };
#pragma pack(pop)

    struct ShaderBlob { uint32_t stage; std::vector<uint8_t> code; };
    using ShaderBlobMap = std::unordered_map<uint64_t, ShaderBlob>;

    // ---- container I/O ----------------------------------------------------
    // Everything a reader needs from one file, so no caller has to know the layout.
    struct CacheContents
    {
        CacheMeta                meta{};
        std::string              strings[kMetaStringCount];
        std::vector<uint32_t>    rsTypes;
        std::vector<std::vector<D3DVERTEXELEMENT9>> decls;
        std::vector<KeyRecord>   keys;
        ShaderBlobMap            shaders;
    };

    inline void WriteStr(FILE* f, const std::string& s)
    {
        uint32_t n = (uint32_t)s.size();
        fwrite(&n, 4, 1, f);
        if (n) fwrite(s.data(), 1, n, f);
    }

    // Reading is in d3d9cache.h (d3d9cache::LoadFile / Parse): every file is read
    // whole, bounded, and validated field by field before anything uses it, because
    // a cache file may come from another PC, another build, or a damaged copy.

    inline bool WriteCache(const std::string& path, const CacheContents& in)
    {
        // Two passes: reserve the section table, write the payload recording each
        // section's real offset, then rewrite the table. Simpler and less fragile than
        // computing sizes up front, which would have to track every element's encoding.
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;

        CacheSection secs[5] = {
            { kSecMeta, 0, 0, 1 },
            { kSecRSTypes, 0, 0, (uint32_t)in.rsTypes.size() },
            { kSecDecls, 0, 0, (uint32_t)in.decls.size() },
            { kSecKeys, 0, 0, (uint32_t)in.keys.size() },
            { kSecShaders, 0, 0, (uint32_t)in.shaders.size() },
        };
        const uint32_t nsec = 5;

        CacheHeader h{ kCacheMagic, kCacheVersion, nsec, 0 };
        fwrite(&h, sizeof(h), 1, f);
        const long tableAt = ftell(f);
        fwrite(secs, sizeof(CacheSection), nsec, f);

        auto mark = [&](uint32_t i) { secs[i].offset = (uint32_t)ftell(f); };
        auto seal = [&](uint32_t i) { secs[i].size = (uint32_t)ftell(f) - secs[i].offset; };

        mark(0);
        CacheMeta m = in.meta;
        m.stringCount = kMetaStringCount;
        fwrite(&m, sizeof(m), 1, f);
        for (uint32_t i = 0; i < kMetaStringCount; i++) WriteStr(f, in.strings[i]);
        seal(0);

        mark(1);
        if (!in.rsTypes.empty()) fwrite(in.rsTypes.data(), 4, in.rsTypes.size(), f);
        seal(1);

        mark(2);
        for (auto& d : in.decls)
        {
            uint32_t n = (uint32_t)d.size();
            fwrite(&n, 4, 1, f);
            fwrite(d.data(), sizeof(D3DVERTEXELEMENT9), n, f);
        }
        seal(2);

        mark(3);
        if (!in.keys.empty()) fwrite(in.keys.data(), sizeof(KeyRecord), in.keys.size(), f);
        seal(3);

        mark(4);
        for (auto& [hash, blob] : in.shaders)
        {
            ShaderBlobHeader bh{ hash, blob.stage, (uint32_t)blob.code.size() };
            fwrite(&bh, sizeof(bh), 1, f);
            fwrite(blob.code.data(), 1, blob.code.size(), f);
        }
        seal(4);

        fseek(f, tableAt, SEEK_SET);
        fwrite(secs, sizeof(CacheSection), nsec, f);
        // A full disk shows up here and nowhere else: fwrite and fclose both report it,
        // and a half-written file must never replace a whole one.
        const bool ok = !ferror(f);
        return fclose(f) == 0 && ok;
    }


    // Hash a shader the same way the capture does: from the bytes GetFunction
    // returns, never from the caller's pointer, so the two can never disagree.
    template <typename T>
    inline uint64_t HashShaderFunction(T* shader)
    {
        if (!shader) return 0;
        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) return 0;
        std::vector<uint8_t> code(size);
        if (FAILED(shader->GetFunction(code.data(), &size))) return 0;
        return Fnv1a(code.data(), size);
    }
}
