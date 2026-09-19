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

    // plugins\pipelinecache\, beside the ASI: where a player drops cache files from
    // their OTHER devices. Capture merges them into this device's file and the replay
    // warms them (see the capture's LoadImports). Empty if the ASI path is unknown.
    inline std::string ImportDir()
    {
        char buf[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ImportDir), &self);
        if (!GetModuleFileNameA(self, buf, MAX_PATH)) return std::string();
        std::string s(buf);
        auto slash = s.find_last_of("\\/");
        if (slash == std::string::npos) return std::string();
        return s.substr(0, slash + 1) + "pipelinecache\\";
    }

    inline uint64_t Fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ull)
    {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
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

    // The version exists purely so a stale or foreign file is REJECTED rather than
    // misread -- there is deliberately no migration path in the ASI. This feature is
    // not upstream yet, so no user has a cache worth preserving, and carrying readers
    // for formats nobody has costs more than re-capturing does. Bump this on any
    // layout change; add migration only once upstream ships a release whose caches
    // must survive. (A rejected file is moved aside, never overwritten: see the
    // capture's LoadExisting.)
    //
    //   v1  first single-file container
    //   v2  KeyRecord::streamFreq -- instanced streams are a different pipeline
    constexpr uint32_t kCacheVersion = 2;

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

    inline bool ReadStr(FILE* f, std::string& out)
    {
        uint32_t n = 0;
        if (fread(&n, 4, 1, f) != 1 || n > (1u << 16)) return false;
        out.assign(n, '\0');
        return n == 0 || fread(&out[0], 1, n, f) == n;
    }

    inline void WriteStr(FILE* f, const std::string& s)
    {
        uint32_t n = (uint32_t)s.size();
        fwrite(&n, 4, 1, f);
        if (n) fwrite(s.data(), 1, n, f);
    }

    // `want` selects sections, so the merge tool can read a thousand files' metadata
    // without touching their key tables. Sections absent from the file are simply
    // left empty -- a reader asks for what it needs and checks what it got.
    inline bool ReadCache(const std::string& path, CacheContents& out, uint32_t wantMask = ~0u)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;

        CacheHeader h{};
        if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != kCacheMagic ||
            h.version != kCacheVersion || h.sectionCount == 0 || h.sectionCount > 32)
        {
            fclose(f);
            return false;
        }

        std::vector<CacheSection> secs(h.sectionCount);
        if (fread(secs.data(), sizeof(CacheSection), h.sectionCount, f) != h.sectionCount)
        {
            fclose(f);
            return false;
        }

        bool ok = true;
        for (auto& s : secs)
        {
            if (!(wantMask & (1u << s.id))) continue;
            if (fseek(f, (long)s.offset, SEEK_SET) != 0) { ok = false; break; }

            if (s.id == kSecMeta)
            {
                if (fread(&out.meta, sizeof(CacheMeta), 1, f) != 1) { ok = false; break; }
                for (uint32_t i = 0; i < out.meta.stringCount && i < kMetaStringCount; i++)
                    if (!ReadStr(f, out.strings[i])) { ok = false; break; }
            }
            else if (s.id == kSecRSTypes)
            {
                out.rsTypes.resize(s.count);
                if (s.count && fread(out.rsTypes.data(), 4, s.count, f) != s.count) { ok = false; break; }
            }
            else if (s.id == kSecDecls)
            {
                out.decls.resize(s.count);
                for (uint32_t i = 0; i < s.count; i++)
                {
                    uint32_t n = 0;
                    if (fread(&n, 4, 1, f) != 1 || n == 0 || n > MAXD3DDECLLENGTH + 1) { ok = false; break; }
                    out.decls[i].resize(n);
                    if (fread(out.decls[i].data(), sizeof(D3DVERTEXELEMENT9), n, f) != n) { ok = false; break; }
                }
            }
            else if (s.id == kSecKeys)
            {
                out.keys.resize(s.count);
                if (s.count && fread(out.keys.data(), sizeof(KeyRecord), s.count, f) != s.count)
                {
                    out.keys.clear();
                    ok = false;
                    break;
                }
            }
            else if (s.id == kSecShaders)
            {
                for (uint32_t i = 0; i < s.count; i++)
                {
                    ShaderBlobHeader bh{};
                    if (fread(&bh, sizeof(bh), 1, f) != 1 || bh.size == 0 || bh.size > (1u << 20)) { ok = false; break; }
                    ShaderBlob b{ bh.stage, std::vector<uint8_t>(bh.size) };
                    if (fread(b.code.data(), 1, bh.size, f) != bh.size) { ok = false; break; }
                    out.shaders.emplace(bh.hash, std::move(b));
                }
            }
        }
        fclose(f);
        return ok;
    }

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
        fclose(f);
        return true;
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
