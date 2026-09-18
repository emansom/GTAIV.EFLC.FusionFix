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

    // v1 -> v2 added the render targets' multisample type/quality. Vulkan bakes the
    // sample count into the pipeline, so a capture taken with ReflectionMSAAQuality
    // set would otherwise replay as non-multisampled pipelines that gameplay never
    // asks for -- the same wrong-key failure this whole design exists to avoid.
    // v1 files are still readable: they were all recorded with MSAA off, so they
    // migrate by filling in D3DMULTISAMPLE_NONE.
    constexpr uint32_t kVersion   = 2;
    constexpr uint32_t kVersionV1 = 1;

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

    // The v1 record, kept solely so existing caches can be migrated rather than
    // discarded -- they represent real play time that cannot be recovered cheaply.
    struct KeyRecordV1
    {
        uint64_t vsHash;
        uint64_t psHash;
        uint32_t declIndex;
        uint32_t fvf;
        uint32_t primType;
        uint32_t upDraw;
        uint32_t rtFmt[kMaxRT];
        uint32_t dsFmt;
        uint32_t rs[kNumRS];
        uint8_t  samplerType[kNumSamplers];
        uint32_t count;
        uint32_t firstFrame;
    };

    inline void MigrateV1(const KeyRecordV1& in, KeyRecord& out)
    {
        out = KeyRecord{};
        out.vsHash = in.vsHash;   out.psHash = in.psHash;
        out.declIndex = in.declIndex; out.fvf = in.fvf;
        out.primType = in.primType;   out.upDraw = in.upDraw;
        for (uint32_t i = 0; i < kMaxRT; i++) out.rtFmt[i] = in.rtFmt[i];
        out.dsFmt = in.dsFmt;
        out.msType = 0;      // D3DMULTISAMPLE_NONE - true for every v1 capture
        out.msQuality = 0;
        for (uint32_t i = 0; i < kNumRS; i++) out.rs[i] = in.rs[i];
        for (uint32_t i = 0; i < kNumSamplers; i++) out.samplerType[i] = in.samplerType[i];
        out.count = in.count; out.firstFrame = in.firstFrame;
    }

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
    // Back-buffer size and format plus the MSAA level are what actually move the
    // formats in the key; they are cheap to read and stable across a session.
    inline std::string BundleName(uint32_t width, uint32_t height, uint32_t bbFormat,
                                  int msaa, const char* ext)
    {
        char buf[128];
        // Bucket the resolution: an off-by-a-few-pixels borderless size must not
        // fragment the cache, but 1080p and 4K must not share one.
        const char* cls = (height >= 2000) ? "4k" : (height >= 1300) ? "1440p"
                        : (height >= 1000) ? "1080p" : (height >= 700) ? "720p" : "low";
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "FusionFix.pipelinekeys.%s-f%u-ms%d.%s",
                    cls, bbFormat, msaa, ext);
        (void)width;
        return std::string(buf);
    }

    // The pre-bundle file name, kept so an existing cache is not orphaned.
    inline const char* LegacyBundleName(const char* ext)
    {
        return (ext && ext[0] == 'b') ? "FusionFix.pipelinekeys.bin"
                                      : "FusionFix.pipelinekeys.txt";
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
