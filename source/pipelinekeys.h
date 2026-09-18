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
#include <vector>
#include <string>
#include <cstdio>

namespace pipelinekeys
{
    constexpr uint32_t kMagic   = 0x4B504646;   // 'FFPK'

    // The on-disk format carries a version purely so a stale or foreign file is
    // REJECTED rather than misread -- there is deliberately no migration path. This
    // feature is not upstream yet, so no user has a cache worth preserving, and
    // carrying readers for formats nobody has costs more than re-capturing does.
    // Bump this on any layout change; add migration only once upstream ships a
    // release whose caches must survive.
    constexpr uint32_t kVersion = 2;

    constexpr uint32_t kMaxRT       = 4;
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
        uint32_t count;                     // how many draws hit this key
        uint32_t firstFrame;                // frame ordinal of first sighting
    };

    // File header, immediately followed by:
    //   uint32_t rsTypes[numRS]
    //   declCount x { uint32_t n; D3DVERTEXELEMENT9 elems[n] }
    //   recCount  x KeyRecord
    struct FileHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t numRS;
        uint32_t numSamplers;
        uint32_t declCount;
        uint32_t recCount;
    };
#pragma pack(pop)

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
    inline std::string BundleName(uint32_t bbFormat, int msaa, const char* ext)
    {
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "FusionFix.pipelinekeys.f%u-ms%d.%s",
                    bbFormat, msaa, ext);
        return std::string(buf);
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
    // Bytecode does not vary with the graphics configuration, so unlike the keys this
    // is ONE file shared by every bundle.
    constexpr uint32_t kShaderMagic   = 0x53504646;   // 'FFPS'
    constexpr uint32_t kShaderVersion = 1;

    enum ShaderStage : uint32_t { kStageVS = 0, kStagePS = 1 };

#pragma pack(push, 1)
    struct ShaderFileHeader { uint32_t magic; uint32_t version; uint32_t count; };
    struct ShaderBlobHeader { uint64_t hash; uint32_t stage; uint32_t size; };
#pragma pack(pop)

    struct ShaderBlob { uint32_t stage; std::vector<uint8_t> code; };
    using ShaderBlobMap = std::unordered_map<uint64_t, ShaderBlob>;

    inline const char* ShaderSidecarName() { return "FusionFix.pipelineshaders.bin"; }

    // Merges into `out`; an entry already present wins, so a caller can load several
    // files (or load over what it captured) without losing anything.
    inline bool ReadShaderSidecar(const std::string& path, ShaderBlobMap& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;

        ShaderFileHeader hdr{};
        if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
            hdr.magic != kShaderMagic || hdr.version != kShaderVersion)
        {
            fclose(f);
            return false;
        }

        for (uint32_t i = 0; i < hdr.count; i++)
        {
            ShaderBlobHeader bh{};
            if (fread(&bh, sizeof(bh), 1, f) != 1) break;    // short file: keep what is whole
            // A shader token stream is DWORDs and far smaller than this; the bound
            // only exists so a corrupt length cannot ask for a gigabyte.
            if (bh.size == 0 || bh.size > (1u << 20) || (bh.size & 3u)) break;

            std::vector<uint8_t> code(bh.size);
            if (fread(code.data(), 1, bh.size, f) != bh.size) break;
            if (bh.stage != kStageVS && bh.stage != kStagePS) continue;

            out.emplace(bh.hash, ShaderBlob{ bh.stage, std::move(code) });
        }
        fclose(f);
        return true;
    }

    inline bool WriteShaderSidecar(const std::string& path, const ShaderBlobMap& in)
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;

        ShaderFileHeader hdr{ kShaderMagic, kShaderVersion, (uint32_t)in.size() };
        fwrite(&hdr, sizeof(hdr), 1, f);

        for (auto& [hash, blob] : in)
        {
            ShaderBlobHeader bh{ hash, blob.stage, (uint32_t)blob.code.size() };
            fwrite(&bh, sizeof(bh), 1, f);
            fwrite(blob.code.data(), 1, blob.code.size(), f);
        }
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
