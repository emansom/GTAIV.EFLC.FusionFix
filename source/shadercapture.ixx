module;

// ===========================================================================
// shadercapture.ixx  —  D3D9 draw-call pipeline-key CAPTURE
//
// WHY THIS EXISTS
//   The launch-time precompiler (shaderprecompile.ixx) issues SYNTHETIC draws:
//   a position-only vertex declaration into its own scratch render targets with
//   a hand-written matrix of blend/spec variants. Measured on Linux/DXVK with
//   graphics-pipeline-library disabled, that bought nothing (59 isolated frame
//   spikes with it ON vs 55 with it OFF) because a pipeline is keyed on the FULL
//   state vector, and our synthetic state matches the game's in almost none of
//   those fields. The pipelines we built were different OBJECTS from the ones
//   gameplay needs. See re/shader-precompile/RESULTS-linux-ab.md.
//
//   So: stop guessing the keys, record them. This module hooks the four D3D9
//   Draw* entry points on the REAL device — deliberately below RAGE, because
//   this is exactly the boundary DXVK (and native D3D9) key their pipelines on —
//   and writes every distinct key it sees to a cache file. A later replay pass
//   rebuilds those exact states before gameplay, so the warm-up draws produce the
//   SAME pipeline keys as the game rather than lookalikes.
//
// WHAT A KEY IS
//   vertex + pixel shader bytecode hash · vertex declaration (or FVF) ·
//   primitive topology · render-target formats 0..3 + depth format ·
//   the pipeline-relevant render-state block · the TYPE of texture bound to
//   each sampler (2D/CUBE/VOLUME — DXVK folds sampler dimensions into SPIR-V
//   specialisation constants, so a mismatch there is a different pipeline).
//
// COST / CORRECTNESS TRADE
//   Every field is read back from the device on every draw rather than shadowed
//   from Set* hooks. Shadowing is much cheaper but silently drifts the moment a
//   state block is Applied (RAGE uses them), and a wrong key here would recreate
//   the exact failure this work exists to escape. Capture is an opt-in authoring
//   mode, not a shipping per-frame cost, so it buys correctness with frame time.
//
// USAGE  (plugins/GTAIV.EFLC.FusionFix.ini)
//   [SHADERS]
//   CaptureDrawKeys = 1      ; record keys while you play, then quit normally
//   Leave PrecompileShaders = 0 during a capture run, or the precompiler's own
//   synthetic draws land in the cache as if the game had issued them.
//
// OUTPUT (next to the .asi)
//   FusionFix.pipelinekeys.<config>.bin  versioned binary, for the replay pass
//   FusionFix.pipelinekeys.<config>.txt  human-readable summary + histograms
//   FusionFix.pipelineshaders.bin        bytecode of every shader the keys name,
//                                        so replay can create the ones that do not
//                                        exist yet when it runs (see pipelinekeys.h)
// ===========================================================================

#include <common.hxx>
#include <d3d9.h>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include "pipelinekeys.h"

export module shadercapture;

import common;
import comvars;

// ---------------------------------------------------------------------------
// Key layout — ONE definition, in pipelinekeys.h, shared with the replay reader.
//
// This used to be duplicated here with static_asserts pinning the two together.
// That was a stopgap taken to avoid rewriting a file while it was in use; keeping
// two copies in step through a format change is exactly the drift the asserts were
// guarding against, so the copy is gone.
// ---------------------------------------------------------------------------
using namespace pipelinekeys;

// ---------------------------------------------------------------------------
class ShaderCapture
{
    // ---- config -------------------------------------------------------
    static inline bool enabled = false;
    static inline int  flushSeconds = 10;

    // ---- hook state ---------------------------------------------------
    using PFN_DrawPrimitive          = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using PFN_DrawIndexedPrimitive   = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using PFN_DrawPrimitiveUP        = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using PFN_DrawIndexedPrimitiveUP = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

    static inline PFN_DrawPrimitive          origDP   = nullptr;
    static inline PFN_DrawIndexedPrimitive   origDIP  = nullptr;
    static inline PFN_DrawPrimitiveUP        origDPUP = nullptr;
    static inline PFN_DrawIndexedPrimitiveUP origDIPUP = nullptr;

    static inline IDirect3DDevice9* dev = nullptr;
    static inline bool installed = false;

    // A D3D9 device created without D3DCREATE_MULTITHREADED is not free-threaded, so
    // the game has to serialise its own draws — but GTA IV does run a separate
    // loading-screen thread (Ghidra: the pump at 0x008da240 has its own thread entry
    // at 0x008da370), and a torn read of `records`/`keyIndex` would crash rather than
    // just lose a key. An uncontended critical section costs far less than the ~60
    // interface calls each RecordDraw already makes, so guard rather than assume.
    static inline CRITICAL_SECTION lock{};
    static inline bool lockReady = false;

    // ---- capture state ------------------------------------------------
    static inline std::vector<KeyRecord> records;
    static inline std::unordered_map<uint64_t, uint32_t> keyIndex;   // strict key hash -> record index

    // declaration table: each entry is the raw D3DVERTEXELEMENT9 array incl. the END marker
    static inline std::vector<std::vector<D3DVERTEXELEMENT9>> declTable;
    static inline std::unordered_map<uint64_t, uint32_t> declIndexByHash;

    // memoised bytecode hashes. Keyed on {pointer, byte size} rather than pointer
    // alone so a freed shader whose address is reused cannot silently inherit the
    // previous shader's hash (sizes would have to match too).
    static inline std::unordered_map<uint64_t, uint64_t> shaderHashCache;

    // Bytecode of every shader we have seen, by the same hash the keys carry, so
    // replay can create the ones it cannot find any other way. Shared by every
    // bundle: bytecode does not vary with the graphics configuration.
    static inline pipelinekeys::ShaderBlobMap shaderBlobs;
    static inline bool blobsDirty = false;
    static inline size_t blobsMergedIn = 0;

    static inline uint32_t mergedIn = 0;     // keys inherited from a previous session
    static inline uint64_t totalDraws = 0;
    static inline uint64_t frameOrdinal = 0;
    static inline uint32_t lastReportedUnique = 0;
    static inline bool dirty = false;
    static inline std::chrono::steady_clock::time_point tLastFlush;

    static inline HMODULE hSelf = nullptr;

    // -------------------------------------------------------------------
    static void Log(const char* fmt, ...)
    {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        OutputDebugStringA("[ShaderCapture] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }

    static inline uint64_t fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ull)
    {
        auto p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    // ---- device acquisition (same ladder the precompiler uses) --------
    static IDirect3DDevice9* AcquireDevice()
    {
        if (RageDirect3DDevice9::m_pRealDevice && *RageDirect3DDevice9::m_pRealDevice)
            return *RageDirect3DDevice9::m_pRealDevice;
        if (rage::grcDevice::ms_pD3DDevice && *rage::grcDevice::ms_pD3DDevice)
            return *rage::grcDevice::ms_pD3DDevice;
        return nullptr;
    }

    // ---- bytecode / declaration identity ------------------------------
    // Both are cached: hashing a shader means copying its whole token stream, and
    // GTA IV re-binds the same few thousand shaders tens of thousands of times a
    // frame. GetFunction/GetDeclaration with a null buffer only returns the size,
    // which is a trivial read on both native D3D9 and DXVK.
    template <typename T>
    static uint64_t ShaderHash(T* shader, uint32_t stage)
    {
        if (!shader) return 0;

        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) return 0;

        uint64_t cacheKey = ((uint64_t)(uintptr_t)shader) ^ ((uint64_t)size << 48);
        auto it = shaderHashCache.find(cacheKey);
        if (it != shaderHashCache.end()) return it->second;

        std::vector<uint8_t> code(size);
        if (FAILED(shader->GetFunction(code.data(), &size))) return 0;

        uint64_t h = fnv1a(code.data(), size);
        shaderHashCache.emplace(cacheKey, h);

        // Keep the bytes. Replay resolves a hash to a shader object, and the two
        // sources it had (the .fxc database, the runtime registry) between them miss
        // every shader FusionFix compiles lazily -- those keys were skipped outright.
        // See the sidecar notes in pipelinekeys.h. This is the only place we hold the
        // bytecode, so it costs one move on FIRST sight of a shader and nothing after.
        if (shaderBlobs.find(h) == shaderBlobs.end())
        {
            shaderBlobs.emplace(h, pipelinekeys::ShaderBlob{ stage, std::move(code) });
            blobsDirty = true;
        }
        return h;
    }

    static uint32_t DeclarationIndex(IDirect3DVertexDeclaration9* decl)
    {
        if (!decl) return 0xFFFFFFFFu;

        UINT count = 0;
        if (FAILED(decl->GetDeclaration(nullptr, &count)) || count == 0) return 0xFFFFFFFFu;

        std::vector<D3DVERTEXELEMENT9> elems(count);
        if (FAILED(decl->GetDeclaration(elems.data(), &count))) return 0xFFFFFFFFu;
        elems.resize(count);

        uint64_t h = fnv1a(elems.data(), elems.size() * sizeof(D3DVERTEXELEMENT9));
        auto it = declIndexByHash.find(h);
        if (it != declIndexByHash.end()) return it->second;

        uint32_t idx = (uint32_t)declTable.size();
        declTable.push_back(std::move(elems));
        declIndexByHash.emplace(h, idx);
        return idx;
    }

    static uint32_t SurfaceFormat(IDirect3DSurface9* surf)
    {
        if (!surf) return 0;
        D3DSURFACE_DESC d{};
        if (FAILED(surf->GetDesc(&d))) return 0;
        return (uint32_t)d.Format;
    }

    // Sample count is baked into the Vulkan pipeline, so it belongs in the key.
    // D3D9 requires every bound target to share a multisample type, so RT0's is
    // representative.
    static void SurfaceMultisample(IDirect3DSurface9* surf, uint32_t& type, uint32_t& quality)
    {
        type = 0; quality = 0;
        if (!surf) return;
        D3DSURFACE_DESC d{};
        if (FAILED(surf->GetDesc(&d))) return;
        type = (uint32_t)d.MultiSampleType;
        quality = (uint32_t)d.MultiSampleQuality;
    }

    static uint8_t SamplerKind(DWORD stage)
    {
        IDirect3DBaseTexture9* tex = nullptr;
        if (FAILED(dev->GetTexture(stage, &tex)) || !tex) return 0;
        D3DRESOURCETYPE t = tex->GetType();
        tex->Release();
        switch (t)
        {
        case D3DRTYPE_TEXTURE:       return 1;
        case D3DRTYPE_CUBETEXTURE:   return 2;
        case D3DRTYPE_VOLUMETEXTURE: return 3;
        default:                     return 0;
        }
    }

    // ---- the actual capture -------------------------------------------
    static void RecordDraw(D3DPRIMITIVETYPE primType, bool up)
    {
        if (!dev || !lockReady) return;

        // Held across the whole body: the shader/declaration memo tables are mutated
        // during key construction, not just at insert time.
        EnterCriticalSection(&lock);

        KeyRecord k{};
        k.primType = (uint32_t)primType;
        k.upDraw   = up ? 1u : 0u;

        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9*  ps = nullptr;
        if (SUCCEEDED(dev->GetVertexShader(&vs)) && vs) { k.vsHash = ShaderHash(vs, pipelinekeys::kStageVS); vs->Release(); }
        if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps)  { k.psHash = ShaderHash(ps, pipelinekeys::kStagePS); ps->Release(); }

        IDirect3DVertexDeclaration9* decl = nullptr;
        if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl)
        {
            k.declIndex = DeclarationIndex(decl);
            decl->Release();
        }
        else
        {
            k.declIndex = 0xFFFFFFFFu;
        }
        if (k.declIndex == 0xFFFFFFFFu)
            dev->GetFVF((DWORD*)&k.fvf);

        for (uint32_t i = 0; i < kMaxRT; i++)
        {
            IDirect3DSurface9* rt = nullptr;
            // An unbound RT index legitimately fails; that is what rtFmt == 0 means.
            if (SUCCEEDED(dev->GetRenderTarget(i, &rt)) && rt)
            {
                k.rtFmt[i] = SurfaceFormat(rt);
                if (i == 0) SurfaceMultisample(rt, k.msType, k.msQuality);
                rt->Release();
            }
        }
        {
            IDirect3DSurface9* ds = nullptr;
            if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { k.dsFmt = SurfaceFormat(ds); ds->Release(); }
        }

        for (uint32_t i = 0; i < kNumRS; i++)
        {
            DWORD v = 0;
            dev->GetRenderState(kTrackedRS[i].rs, &v);
            k.rs[i] = (uint32_t)v;
        }

        for (uint32_t i = 0; i < kPSSamplers; i++)
            k.samplerType[i] = SamplerKind(i);
        for (uint32_t i = 0; i < kVSSamplers; i++)
            k.samplerType[kPSSamplers + i] = SamplerKind(D3DVERTEXTEXTURESAMPLER0 + i);

        // Hash everything except the bookkeeping tail.
        uint64_t h = fnv1a(&k, offsetof(KeyRecord, count));

        auto it = keyIndex.find(h);
        if (it != keyIndex.end())
        {
            records[it->second].count++;
        }
        else
        {
            k.count = 1;
            k.firstFrame = (uint32_t)frameOrdinal;
            keyIndex.emplace(h, (uint32_t)records.size());
            records.push_back(k);
            dirty = true;
        }

        totalDraws++;
        LeaveCriticalSection(&lock);
    }

    // ---- hooks ---------------------------------------------------------
    static HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE pt, UINT startVertex, UINT primCount)
    {
        RecordDraw(pt, false);
        return origDP(self, pt, startVertex, primCount);
    }

    static HRESULT WINAPI Hook_DrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE pt, INT baseVertexIndex,
                                                    UINT minVertexIndex, UINT numVertices, UINT startIndex, UINT primCount)
    {
        RecordDraw(pt, false);
        return origDIP(self, pt, baseVertexIndex, minVertexIndex, numVertices, startIndex, primCount);
    }

    static HRESULT WINAPI Hook_DrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE pt, UINT primCount,
                                               const void* vtxData, UINT vtxStride)
    {
        RecordDraw(pt, true);
        return origDPUP(self, pt, primCount, vtxData, vtxStride);
    }

    static HRESULT WINAPI Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE pt, UINT minVertexIndex,
                                                      UINT numVertices, UINT primCount, const void* idxData,
                                                      D3DFORMAT idxFmt, const void* vtxData, UINT vtxStride)
    {
        RecordDraw(pt, true);
        return origDIPUP(self, pt, minVertexIndex, numVertices, primCount, idxData, idxFmt, vtxData, vtxStride);
    }

    // ---- shader registry -----------------------------------------------
    // These hooks are installed even when capture is OFF: the replay pass needs
    // them to bind shaders FusionFix compiled itself, which are absent from RAGE's
    // .fxc database. Hashing from GetFunction (not the caller's pointer) keeps the
    // hash identical to the one RecordDraw computes.
    using PFN_CreateVertexShader = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
    using PFN_CreatePixelShader  = HRESULT(WINAPI*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

    static inline PFN_CreateVertexShader origCreateVS = nullptr;
    static inline PFN_CreatePixelShader  origCreatePS = nullptr;
    static inline bool shaderHooksInstalled = false;

    // Counted so the precompiler can tell whether anything other than its own
    // overlay is presenting during the warming pass.
    using PFN_Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    static inline PFN_Present origPresent = nullptr;

    static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty)
    {
        pipelinekeys::DevicePresentCount()++;
        return origPresent(self, src, dst, wnd, dirty);
    }

    static HRESULT WINAPI Hook_CreateVertexShader(IDirect3DDevice9* self, const DWORD* fn, IDirect3DVertexShader9** out)
    {
        HRESULT hr = origCreateVS(self, fn, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            uint64_t h = pipelinekeys::HashShaderFunction(*out);
            if (h) pipelinekeys::Registry().vs[h] = *out;
        }
        return hr;
    }

    static HRESULT WINAPI Hook_CreatePixelShader(IDirect3DDevice9* self, const DWORD* fn, IDirect3DPixelShader9** out)
    {
        HRESULT hr = origCreatePS(self, fn, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            uint64_t h = pipelinekeys::HashShaderFunction(*out);
            if (h) pipelinekeys::Registry().ps[h] = *out;
        }
        return hr;
    }

    // IDirect3DDevice9 vtable slots (COM layout, fixed by the interface).
    static constexpr int kVT_DrawPrimitive          = 81;
    static constexpr int kVT_DrawIndexedPrimitive   = 82;
    static constexpr int kVT_DrawPrimitiveUP        = 83;
    static constexpr int kVT_DrawIndexedPrimitiveUP = 84;
    static constexpr int kVT_CreateVertexShader     = 91;
    static constexpr int kVT_CreatePixelShader      = 106;
    static constexpr int kVT_Present                = 17;

    static bool PatchSlot(void** vtbl, int index, void* fn, void** outOrig)
    {
        DWORD prot = 0;
        if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &prot)) return false;
        *outOrig = vtbl[index];
        vtbl[index] = fn;
        VirtualProtect(&vtbl[index], sizeof(void*), prot, &prot);
        return true;
    }

    // Installed on the first EndScene whether or not capture is enabled, because
    // the replay pass depends on the registry it fills.
    static void InstallShaderHooks(IDirect3DDevice9* d)
    {
        if (shaderHooksInstalled) return;
        auto vtbl = *reinterpret_cast<void***>(d);
        bool ok = PatchSlot(vtbl, kVT_CreateVertexShader, (void*)&Hook_CreateVertexShader, (void**)&origCreateVS)
               && PatchSlot(vtbl, kVT_CreatePixelShader,  (void*)&Hook_CreatePixelShader,  (void**)&origCreatePS)
               && PatchSlot(vtbl, kVT_Present,            (void*)&Hook_Present,            (void**)&origPresent);
        shaderHooksInstalled = ok;
        Log(ok ? "shader registry armed on device %p" : "FAILED to patch the shader-creation slots on %p", d);
    }

    static void Install(IDirect3DDevice9* d)
    {
        if (!lockReady) { InitializeCriticalSection(&lock); lockReady = true; }

        // Which bundle this configuration reads and writes.
        ResolveBundle(d);

        // Inherit previous sessions' coverage before recording anything new.
        LoadExisting();
        LoadShaderBlobs();

        auto vtbl = *reinterpret_cast<void***>(d);
        bool ok = true;
        ok &= PatchSlot(vtbl, kVT_DrawPrimitive,          (void*)&Hook_DrawPrimitive,          (void**)&origDP);
        ok &= PatchSlot(vtbl, kVT_DrawIndexedPrimitive,   (void*)&Hook_DrawIndexedPrimitive,   (void**)&origDIP);
        ok &= PatchSlot(vtbl, kVT_DrawPrimitiveUP,        (void*)&Hook_DrawPrimitiveUP,        (void**)&origDPUP);
        ok &= PatchSlot(vtbl, kVT_DrawIndexedPrimitiveUP, (void*)&Hook_DrawIndexedPrimitiveUP, (void**)&origDIPUP);

        if (!ok) { Log("FAILED to patch the Draw* vtable slots on device %p", d); return; }

        dev = d;
        installed = true;
        tLastFlush = std::chrono::steady_clock::now();
        Log("capturing draw keys on device %p (vtable %p), EndScene thread = %lu",
            d, vtbl, GetCurrentThreadId());
    }

    // ---- output --------------------------------------------------------
    static std::string OutDir()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(hSelf, path, MAX_PATH);
        std::string s(path);
        auto slash = s.find_last_of("\\/");
        return (slash == std::string::npos) ? std::string() : s.substr(0, slash + 1);
    }

    static std::string OutPath(const char* leaf) { return OutDir() + leaf; }

    // Cache file for THIS graphics configuration. Recorded keys carry render-target
    // formats, so one captured at another resolution or MSAA level is both useless
    // (nothing matches) and harmful (warms pipelines this setup never uses).
    static inline std::string bundleBin, bundleTxt;
    // Kept so LoadExisting can build every predecessor name for this configuration.
    static inline uint32_t bundleFmt = 0;
    static inline int      bundleMsaa = 0;

    static void ResolveBundle(IDirect3DDevice9* d)
    {
        uint32_t w = 1920, h = 1080, fmt = (uint32_t)D3DFMT_A8R8G8B8;
        IDirect3DSwapChain9* sc = nullptr;
        if (d && SUCCEEDED(d->GetSwapChain(0, &sc)) && sc)
        {
            D3DPRESENT_PARAMETERS pp{};
            if (SUCCEEDED(sc->GetPresentParameters(&pp)) && pp.BackBufferWidth)
            {
                w = pp.BackBufferWidth; h = pp.BackBufferHeight;
                fmt = (uint32_t)pp.BackBufferFormat;
            }
            sc->Release();
        }
        CIniReader ini("");
        int msaa = ini.ReadInteger("EXPERIMENTAL", "ReflectionMSAAQuality", 0);

        bundleBin = pipelinekeys::BundleName(fmt, msaa, "bin");
        bundleTxt = pipelinekeys::BundleName(fmt, msaa, "txt");
        bundleFmt  = fmt;
        bundleMsaa = msaa;
        // Log the back buffer this was derived from. It is NOT reliably the resolution
        // the session runs at -- a window left at 720p by a previous run made the game
        // start 720p and only later reset to 1080p -- which is exactly why the bundle
        // name no longer depends on it.
        Log("cache bundle for this configuration: %s (back buffer %ux%u fmt %u msaa %d)",
            bundleBin.c_str(), w, h, fmt, msaa);
    }

    static void WriteBinary()
    {
        FILE* f = fopen((OutDir() + bundleBin).c_str(), "wb");
        if (!f) return;

        const uint32_t magic = pipelinekeys::kMagic;
        const uint32_t version = pipelinekeys::kVersion;
        const uint32_t recCount = (uint32_t)records.size();
        const uint32_t declCount = (uint32_t)declTable.size();
        const uint32_t numRS = kNumRS;
        const uint32_t numSamplers = kNumSamplers;

        fwrite(&magic, 4, 1, f);
        fwrite(&version, 4, 1, f);
        fwrite(&numRS, 4, 1, f);
        fwrite(&numSamplers, 4, 1, f);
        fwrite(&declCount, 4, 1, f);
        fwrite(&recCount, 4, 1, f);

        // Which render states the records carry, so a reader never has to guess.
        for (uint32_t i = 0; i < kNumRS; i++)
        {
            uint32_t rs = (uint32_t)kTrackedRS[i].rs;
            fwrite(&rs, 4, 1, f);
        }

        for (auto& d : declTable)
        {
            uint32_t n = (uint32_t)d.size();
            fwrite(&n, 4, 1, f);
            fwrite(d.data(), sizeof(D3DVERTEXELEMENT9), n, f);
        }

        if (recCount) fwrite(records.data(), sizeof(KeyRecord), recCount, f);
        fclose(f);
    }

    // ---- shader bytecode sidecar ---------------------------------------
    // Only shaders that actually appear in a DRAW are stored. A shader nobody draws
    // with has no key referencing it, so its bytecode would be dead weight -- which
    // matters here because the precompiler creates RAGE's whole 1734-shader database
    // through this same device, and capture is normally left on while it does.
    static std::string ShaderSidecarPath() { return OutPath(pipelinekeys::ShaderSidecarName()); }

    static void LoadShaderBlobs()
    {
        pipelinekeys::ShaderBlobMap existing;
        if (!pipelinekeys::ReadShaderSidecar(ShaderSidecarPath(), existing))
        {
            Log("no shader bytecode sidecar yet - starting a new one");
            return;
        }
        blobsMergedIn = existing.size();
        for (auto& [h, b] : existing) shaderBlobs.emplace(h, std::move(b));
        Log("merged %zu shaders from the existing bytecode sidecar", blobsMergedIn);
    }

    static void WriteShaderBlobs()
    {
        if (!blobsDirty) return;

        size_t bytes = 0;
        for (auto& [h, b] : shaderBlobs) bytes += b.code.size();
        if (!pipelinekeys::WriteShaderSidecar(ShaderSidecarPath(), shaderBlobs)) return;

        blobsDirty = false;
        Log("wrote %zu shaders (%zu KiB of bytecode) to %s",
            shaderBlobs.size(), bytes / 1024, pipelinekeys::ShaderSidecarName());
    }

    // Count distinct keys under a reduced field set, so we can tell genuine
    // pipeline variation apart from dynamic state that inflates the strict key.
    static uint32_t CountReducedKeys()
    {
        std::vector<uint64_t> hashes;
        hashes.reserve(records.size());
        for (auto& r : records)
        {
            KeyRecord t = r;
            for (uint32_t i = 0; i < kNumRS; i++)
                if (!kTrackedRS[i].pipeline) t.rs[i] = 0;
            t.upDraw = 0;
            t.count = 0; t.firstFrame = 0;
            hashes.push_back(fnv1a(&t, offsetof(KeyRecord, count)));
        }
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        return (uint32_t)hashes.size();
    }

    static void WriteSummary()
    {
        FILE* f = fopen((OutDir() + bundleTxt).c_str(), "w");
        if (!f) return;

        fprintf(f, "FusionFix D3D9 pipeline-key capture\n");
        fprintf(f, "===================================\n\n");
        fprintf(f, "draws recorded      : %llu\n", (unsigned long long)totalDraws);
        fprintf(f, "frames              : %llu\n", (unsigned long long)frameOrdinal);
        fprintf(f, "unique strict keys  : %u   (every recorded field)\n", (uint32_t)records.size());
        fprintf(f, "unique pipeline keys: %u   (dynamic state folded out)\n", CountReducedKeys());
        fprintf(f, "vertex declarations : %u\n", (uint32_t)declTable.size());

        // distinct shader pairs, and distinct shaders
        {
            std::vector<uint64_t> pairs, vss, pss;
            for (auto& r : records)
            {
                pairs.push_back(r.vsHash ^ (r.psHash * 1099511628211ull));
                vss.push_back(r.vsHash);
                pss.push_back(r.psHash);
            }
            auto uniq = [](std::vector<uint64_t>& v) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
                return (uint32_t)v.size();
            };
            fprintf(f, "distinct VS         : %u\n", uniq(vss));
            fprintf(f, "distinct PS         : %u\n", uniq(pss));
            fprintf(f, "distinct VS/PS pairs: %u\n", uniq(pairs));
        }

        // Replay can only build a key whose shaders it can resolve, so say how many
        // of them we are carrying the bytecode for.
        {
            size_t bytes = 0;
            for (auto& [h, b] : shaderBlobs) bytes += b.code.size();
            fprintf(f, "shader bytecode kept : %zu shaders, %zu KiB (%s)\n",
                    shaderBlobs.size(), bytes / 1024, pipelinekeys::ShaderSidecarName());
        }

        // render-target format combinations
        {
            std::vector<uint64_t> combos;
            for (auto& r : records)
            {
                uint64_t h = fnv1a(r.rtFmt, sizeof(r.rtFmt));
                h = fnv1a(&r.dsFmt, sizeof(r.dsFmt), h);
                combos.push_back(h);
            }
            std::sort(combos.begin(), combos.end());
            combos.erase(std::unique(combos.begin(), combos.end()), combos.end());
            fprintf(f, "RT/depth combos     : %u\n", (uint32_t)combos.size());
        }

        fprintf(f, "\n-- how many render states actually vary --\n");
        for (uint32_t i = 0; i < kNumRS; i++)
        {
            std::vector<uint32_t> vals;
            for (auto& r : records) vals.push_back(r.rs[i]);
            std::sort(vals.begin(), vals.end());
            vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
            if (vals.size() > 1)
            {
                fprintf(f, "  %-26s %2u values :", kTrackedRS[i].name, (uint32_t)vals.size());
                for (size_t v = 0; v < vals.size() && v < 12; v++) fprintf(f, " %u", vals[v]);
                if (vals.size() > 12) fprintf(f, " ...");
                fprintf(f, "%s\n", kTrackedRS[i].pipeline ? "" : "   [dynamic]");
            }
        }

        fprintf(f, "\n-- top 40 keys by draw count --\n");
        std::vector<uint32_t> order(records.size());
        for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return records[a].count > records[b].count; });
        for (size_t i = 0; i < order.size() && i < 40; i++)
        {
            auto& r = records[order[i]];
            fprintf(f, "  %8u draws  vs=%016llx ps=%016llx decl=%d fvf=%u prim=%u rt0=%u ds=%u frame=%u\n",
                    r.count, (unsigned long long)r.vsHash, (unsigned long long)r.psHash,
                    (int)r.declIndex, r.fvf, r.primType, r.rtFmt[0], r.dsFmt, r.firstFrame);
        }

        fprintf(f, "\n-- vertex declarations --\n");
        for (size_t i = 0; i < declTable.size(); i++)
        {
            fprintf(f, "  [%zu] %zu elements:", i, declTable[i].size());
            for (auto& e : declTable[i])
            {
                if (e.Stream == 0xFF) { fprintf(f, " END"); break; }
                fprintf(f, " (s%u+%u t%u u%u%u)", e.Stream, e.Offset, e.Type, e.Usage, e.UsageIndex);
            }
            fprintf(f, "\n");
        }

        fclose(f);
    }

    // Merge an existing cache in at startup.
    //
    // Without this, every session starts from an empty set and throws the last one
    // away, so coverage can never exceed what ONE playthrough happens to touch --
    // and GTA IV is far too large for that to be most of the pipeline space. With
    // it, the file grows monotonically across sessions (and, later, across
    // contributors: merging two players' caches is the same operation).
    // Load this configuration's bundle.
    //
    // This used to merge a list of predecessor names too (per-resolution buckets and a
    // pre-bundle file) so that no accumulated capture was orphaned by a rename. That
    // mattered while the on-disk name kept changing under us; it does not now. The
    // feature has never shipped, so there is no user cache in the wild to rescue, and
    // every extra candidate is another file whose counts get merged in AGAIN on every
    // run -- which is exactly how this file's draw counts reached 41.5e9 against
    // 5,073,509 draws actually recorded. One bundle per (format, MSAA), nothing else.
    static void LoadExisting()
    {
        if (MergeBundleFile(OutDir() + bundleBin, bundleBin.c_str()))
            Log("merged %u keys from the existing cache (%zu unique, %zu declarations)",
                mergedIn, records.size(), declTable.size());
        else
            Log("no existing cache for this configuration - starting a new one");
    }

    // Merge one bundle file. Returns false if it is absent or unusable, so the caller
    // can simply try every candidate.
    static bool MergeBundleFile(const std::string& path, const char* label)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        // Records READ and keys the file actually ADDED are different numbers, and
        // only the second says whether a file was worth merging: the pre-bundle
        // contributed 11234 records and zero new keys, being a strict subset.
        const size_t uniqueBefore = records.size();

        uint32_t magic = 0, version = 0, numRS = 0, numSamplers = 0, declCount = 0, recCount = 0;
        if (fread(&magic, 4, 1, f) != 1 || magic != 0x4B504646u) { fclose(f); return false; }
        if (fread(&version, 4, 1, f) != 1 || fread(&numRS, 4, 1, f) != 1 ||
            fread(&numSamplers, 4, 1, f) != 1 || fread(&declCount, 4, 1, f) != 1 ||
            fread(&recCount, 4, 1, f) != 1) { fclose(f); return false; }

        // A cache recorded against a different version or state set cannot be merged
        // field for field. Keep it rather than silently corrupting it: bail and start
        // fresh. There is deliberately no migration path (see kVersion).
        if (version != pipelinekeys::kVersion || numRS != kNumRS || numSamplers != kNumSamplers)
        {
            Log("%s is v%u tracking %u states / %u samplers (this build: v%u, %u / %u) - not merging",
                label, version, numRS, numSamplers, pipelinekeys::kVersion, kNumRS, kNumSamplers);
            fclose(f);
            return false;
        }

        std::vector<uint32_t> rsTypes(numRS);
        if (fread(rsTypes.data(), 4, numRS, f) != numRS) { fclose(f); return false; }
        for (uint32_t i = 0; i < numRS; i++)
            if (rsTypes[i] != (uint32_t)kTrackedRS[i].rs) { fclose(f); return false; }

        // Declaration indices are file-local, so they must be remapped into our
        // table before a record's key is hashed -- declIndex is part of the key.
        std::vector<uint32_t> remap(declCount, 0);
        for (uint32_t i = 0; i < declCount; i++)
        {
            uint32_t n = 0;
            if (fread(&n, 4, 1, f) != 1 || n == 0 || n > MAXD3DDECLLENGTH + 1) { fclose(f); return false; }
            std::vector<D3DVERTEXELEMENT9> elems(n);
            if (fread(elems.data(), sizeof(D3DVERTEXELEMENT9), n, f) != n) { fclose(f); return false; }

            uint64_t h = fnv1a(elems.data(), elems.size() * sizeof(D3DVERTEXELEMENT9));
            auto it = declIndexByHash.find(h);
            if (it != declIndexByHash.end())
            {
                remap[i] = it->second;
            }
            else
            {
                remap[i] = (uint32_t)declTable.size();
                declIndexByHash.emplace(h, remap[i]);
                declTable.push_back(std::move(elems));
            }
        }

        for (uint32_t i = 0; i < recCount; i++)
        {
            KeyRecord k{};
            if (fread(&k, sizeof(k), 1, f) != 1)
                break;   // short file: keep what is whole
            if (k.declIndex != 0xFFFFFFFFu)
            {
                if (k.declIndex >= declCount) continue;
                k.declIndex = remap[k.declIndex];
            }

            uint64_t h = fnv1a(&k, offsetof(KeyRecord, count));
            auto it = keyIndex.find(h);
            if (it != keyIndex.end())
                // Only reachable if a single file holds the same key twice, or if a
                // second cache is ever merged in deliberately (two players pooling
                // coverage), where summing is what you want. It must NOT be reachable
                // from re-merging files that share history: `count` drives the
                // warm-most-used-first ordering, and re-adding an already-accumulated
                // file compounds it -- that is how this cache reached 41.5e9 counted
                // draws against 5,073,509 actually recorded.
                records[it->second].count += k.count;
            else
            {
                keyIndex.emplace(h, (uint32_t)records.size());
                records.push_back(k);
            }
            mergedIn++;
        }
        fclose(f);

        Log("  %s: %u records read, %zu keys new", label, recCount, records.size() - uniqueBefore);
        return true;
    }

    static void Flush(bool force)
    {
        if (!enabled || (!dirty && !force)) return;

        auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration_cast<std::chrono::seconds>(now - tLastFlush).count() < flushSeconds)
            return;

        // Writing walks the same tables RecordDraw mutates, and runs on a different
        // thread (EndScene) than the loading-screen draws.
        if (lockReady) EnterCriticalSection(&lock);
        WriteBinary();
        WriteSummary();
        WriteShaderBlobs();
        if (lockReady) LeaveCriticalSection(&lock);

        tLastFlush = now;
        dirty = false;

        if ((uint32_t)records.size() != lastReportedUnique)
        {
            Log("%llu draws, %u unique keys, %u declarations",
                (unsigned long long)totalDraws, (uint32_t)records.size(), (uint32_t)declTable.size());
            lastReportedUnique = (uint32_t)records.size();
        }
    }

    static void ReadConfig()
    {
        CIniReader ini("");
        enabled      = ini.ReadInteger("SHADERS", "CaptureDrawKeys", 0) != 0;
        flushSeconds = ini.ReadInteger("SHADERS", "CaptureFlushSeconds", 10);
        if (flushSeconds < 1) flushSeconds = 1;
    }

public:
    ShaderCapture()
    {
        FusionFix::onInitEvent() += []()
        {
            ReadConfig();
            if (!enabled) return;

            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&ReadConfig, &hSelf);

            records.reserve(8192);
            Log("armed (flush every %ds) - waiting for the device", flushSeconds);
        };

        // Install lazily on the render thread: by the first EndScene the device
        // exists and nothing is mid-draw, so swapping vtable slots is safe.
        FusionFix::onEndScene() += []()
        {
            // The shader registry is needed by the replay pass even when capture is
            // disabled, so arm it unconditionally and as early as a device exists.
            if (!shaderHooksInstalled)
                if (auto d = AcquireDevice()) InstallShaderHooks(d);

            if (!enabled) return;

            frameOrdinal++;

            if (!installed)
            {
                if (auto d = AcquireDevice()) Install(d);
                return;
            }

            // A device recreation would leave us hooked to a dead vtable. GTA IV
            // resets rather than recreates, but check cheaply rather than assume.
            if (auto d = AcquireDevice(); d && d != dev)
            {
                Log("device changed %p -> %p, re-installing", dev, d);
                installed = false;
                return;
            }

            Flush(false);
        };

        FusionFix::onShutdownEvent() += []()
        {
            if (!enabled) return;
            Flush(true);
            Log("final: %llu draws, %u unique keys", (unsigned long long)totalDraws, (uint32_t)records.size());
        };
    }
} ShaderCapture;
