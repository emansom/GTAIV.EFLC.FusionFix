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
//   Draws issued by the loading-screen pass (its replay and overlay) are not
//   recorded: the file holds what this PC's game drew, nothing else.
//
// SHARING
//   Once per launch, before anything is recorded, each capture file is copied into
//   plugins\d3d9cache\ as FusionFix.<content hash>.bin (d3d9cache.h). That folder
//   is what a player copies to their other PCs; the replay reads it there. Files in
//   it are never merged into this capture.
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
#include "dxvk_d3d9_interfaces.h"
#include "pipelinekeys.h"
#include "d3d9cache.h"

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
        pipelinekeys::LogLine("[ShaderCapture] ", buf);
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
        // The loading-screen pass's own draws are not the game's (see PassThread).
        if (pipelinekeys::PassThread().load(std::memory_order_relaxed) == GetCurrentThreadId()) return;

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

        // Instancing. DXVK bakes each used stream's input rate and divisor into the
        // vertex-input state, so an instanced draw is a different pipeline from the
        // same shaders drawn per-vertex -- and a replay that never sets the frequency
        // warms only the per-vertex one. Only streams the declaration reads matter
        // (DXVK binds nothing else); the FVF path reads stream 0.
        {
            uint32_t streamMask = 1u;
            if (k.declIndex != 0xFFFFFFFFu)
            {
                streamMask = 0;
                for (const auto& e : declTable[k.declIndex])
                {
                    if (e.Stream == 0xFF) break;               // D3DDECL_END
                    if (e.Stream < pipelinekeys::kMaxStreams) streamMask |= 1u << e.Stream;
                }
            }
            for (uint32_t s = 0; s < pipelinekeys::kMaxStreams; s++)
            {
                if (!(streamMask & (1u << s))) continue;
                UINT setting = 1;
                if (SUCCEEDED(dev->GetStreamSourceFreq(s, &setting)))
                    k.streamFreq[s] = pipelinekeys::InstanceFreq(setting);
            }
        }

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

        // Inherit previous sessions' coverage before recording anything new. Keys and
        // the bytecode they name arrive together, from one file.
        LoadExisting();

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
    static inline std::string bundleBin;
    static inline uint32_t bundleFmt = 0;
    static inline int      bundleMsaa = 0;
    // Provenance, captured once and written into the file. See the container comment
    // in pipelinekeys.h: two captures are only mergeable if the game resolved the SAME
    // shader directory, so this is what a merge tool buckets on.
    static inline std::string metaShaderDir = "unknown", metaAdapter, metaDriver, metaOS, metaDxvk;
    static inline uint32_t metaVendorId = 0, metaDeviceId = 0, metaBackend = 0;

    // The shader directory GTA IV resolved (win32_30, win32_30_nv8, ...): what a
    // merge tool buckets on, and what the replay compares drop-ins against.
    // "unknown" rather than a plausible-looking lie if it cannot be read.
    static void ResolveShaderDir()
    {
        metaShaderDir = d3d9cache::ActiveShaderDir();
        Log("shader directory in use: %s", metaShaderDir.c_str());
    }

    static void ResolveProvenance(IDirect3DDevice9* d)
    {
        IDirect3D9* d3d = nullptr;
        if (d && SUCCEEDED(d->GetDirect3D(&d3d)) && d3d)
        {
            D3DADAPTER_IDENTIFIER9 id{};
            if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id)))
            {
                metaAdapter = id.Description;
                metaVendorId = id.VendorId;
                metaDeviceId = id.DeviceId;
                char drv[64];
                _snprintf_s(drv, sizeof(drv), _TRUNCATE, "%u.%u.%u.%u",
                            HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                            HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart));
                metaDriver = drv;
            }
            d3d->Release();
        }
        // DXVK is the backend that makes one shared cache plausible (it answers the
        // depth-format probe itself instead of the vendor driver), so record which one
        // produced this capture rather than assuming. Ask the device itself: DLL names
        // differ per install (vulkan.dll behind FusionFix's forwarder, d3d9.dll under
        // Proton) and misreported every Windows capture.
        FusionFixDxvkInfo dxvk;
        metaBackend = FusionFixQueryDxvk(d, &dxvk) ? 1 : 0;
        // Under DXVK the D3D9 driver version is a constant DXVK makes up, so replace it
        // with the Vulkan driver that actually compiles the pipelines.
        if (dxvk.haveDriver)
            metaDriver = std::string(dxvk.driverName) + " " + dxvk.driverInfo;
        if (metaBackend)
            metaDxvk = FusionFixDxvkBuild(d);
        metaOS = "windows";
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        {
            using PFN_WineVer = const char* (__cdecl*)(void);
            if (auto wv = (PFN_WineVer)GetProcAddress(nt, "wine_get_version"))
                metaOS = std::string("wine ") + (wv() ? wv() : "?");
        }
        Log("provenance: %s / %s / driver %s / backend %s%s%s",
            metaOS.c_str(), metaAdapter.c_str(), metaDriver.c_str(),
            metaBackend ? "DXVK" : "native D3D9",
            metaDxvk.empty() ? "" : " ", metaDxvk.c_str());
    }

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

        bundleBin  = pipelinekeys::CacheName(fmt, msaa);
        bundleFmt  = fmt;
        bundleMsaa = msaa;
        ResolveShaderDir();
        ResolveProvenance(d);
        // Log the back buffer this was derived from. It is NOT reliably the resolution
        // the session runs at -- a window left at 720p by a previous run made the game
        // start 720p and only later reset to 1080p -- which is exactly why the bundle
        // name no longer depends on it.
        Log("cache for this configuration: %s (back buffer %ux%u fmt %u msaa %d)",
            bundleBin.c_str(), w, h, fmt, msaa);
    }

    // One file: keys, the declarations they index, the bytecode they name, and the
    // provenance a merge tool needs. A player sends this and nothing else.
    static void WriteCacheFile()
    {
        pipelinekeys::CacheContents c;
        c.meta.numRS       = kNumRS;
        c.meta.numSamplers = kNumSamplers;
        c.meta.bbFormat    = bundleFmt;
        c.meta.msaa        = bundleMsaa;
        c.meta.frames      = (uint32_t)frameOrdinal;   // a session never reaches 2^32 frames
        c.meta.draws       = totalDraws;
        c.meta.backend     = metaBackend;
        c.meta.vendorId    = metaVendorId;
        c.meta.deviceId    = metaDeviceId;
        c.strings[pipelinekeys::kMetaShaderDir] = metaShaderDir;
        c.strings[pipelinekeys::kMetaAdapter]   = metaAdapter;
        c.strings[pipelinekeys::kMetaDriver]    = metaDriver;
        c.strings[pipelinekeys::kMetaOS]        = metaOS;
        c.strings[pipelinekeys::kMetaDxvk]      = metaDxvk;

        // Which render states the records carry, so a reader never has to guess.
        c.rsTypes.reserve(kNumRS);
        for (uint32_t i = 0; i < kNumRS; i++) c.rsTypes.push_back((uint32_t)kTrackedRS[i].rs);

        c.decls = declTable;
        c.keys  = records;
        // Bytecode only for what an install cannot supply itself; the replay
        // resolves every .fxc shader by hash from the install (see
        // pipelinekeys::InstalledFxcHashes). Keys are kept either way, so nothing
        // that was captured stops being warmed.
        size_t leftOut = 0;
        for (auto& [h, b] : shaderBlobs)
        {
            if (pipelinekeys::CarryBytecode(h)) c.shaders.emplace(h, b);
            else leftOut++;
        }
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            Log("cache carries bytecode for %zu shaders; %zu more are resolved from the install's "
                ".fxc files by hash (%zu known there)", c.shaders.size(), leftOut,
                pipelinekeys::InstalledFxcHashes().size());
        }

        // Write beside the target and rename, so a crash mid-write cannot leave a
        // contributor with a half-file that still has a valid header.
        const std::string finalPath = OutDir() + bundleBin;
        const std::string tmpPath   = finalPath + ".tmp";
        if (!pipelinekeys::WriteCache(tmpPath, c))
        {
            Log("could not write %s", tmpPath.c_str());
            return;
        }
        remove(finalPath.c_str());
        if (rename(tmpPath.c_str(), finalPath.c_str()) != 0)
            Log("could not replace %s", finalPath.c_str());
    }

    // Shader bytecode lives in the same container as the keys that name it. Only
    // shaders that actually appear in a DRAW are stored: a shader nobody draws with has
    // no key referencing it, so its bytecode would be dead weight -- which matters
    // here because the precompiler creates RAGE's whole 1734-shader database through
    // this same device, and capture is normally left on while it does. Of those, only
    // the ones no .fxc supplies are written out (WriteCacheFile).


    // There used to be a .txt summary written alongside the cache. A contribution has
    // to be ONE file, and every number it held (reduced key count, distinct shaders,
    // RT/depth combos, which render states vary, the heaviest keys, the declarations)
    // is derivable from the container by an external tool -- which is where it belongs,
    // since that tool has to read a thousand contributions anyway.

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
    //
    // A file that exists but cannot be merged -- a newer format, another state set,
    // another shader directory, a damaged container -- is MOVED ASIDE, never left in
    // place. Capture writes to this same path on its first flush, so "refuse and
    // start fresh" used to mean "refuse and then overwrite": every format bump
    // silently destroyed the accumulated capture it had just declined to read.
    //
    // It is read through the same validating reader as a drop-in (d3d9cache.h). A
    // key, declaration or shader that fails validation is dropped and the rest is
    // kept; the next flush writes the file without them. A v1 file is widened in
    // memory and carried on as v2.
    //
    // Drop-ins from the player's other PCs (plugins\d3d9cache\) are NOT merged in:
    // the replay reads them where they are, and this file stays this PC's own record
    // -- which is also what its snapshot in that folder shares.
    static void LoadBundle()
    {
        const std::string path = OutDir() + bundleBin;
        pipelinekeys::CacheContents c;
        std::unordered_map<uint64_t, std::string> seen;
        const d3d9cache::FileResult f = d3d9cache::LoadFile(d3d9cache::Wide(path), bundleBin, metaShaderDir, seen, c);
        if (f.verdict == d3d9cache::Verdict::Absent)
        {
            Log("no existing cache for this configuration - starting a new one");
            return;
        }
        pipelinekeys::LogLine("[ShaderCapture] ", ("existing cache " + bundleBin + ": " + d3d9cache::Describe(f, c)).c_str());
        if (f.verdict == d3d9cache::Verdict::Accepted)
        {
            MergeBundle(c);
            if (f.parse.badKeys || f.parse.badDecls || f.parse.badShaders || f.parse.version != pipelinekeys::kCacheVersion)
                dirty = true;   // write it back clean, and in the current version
            Log("merged %u keys from the existing cache (%zu unique, %zu declarations)",
                mergedIn, records.size(), declTable.size());
            return;
        }
        const std::string aside = path + ".unmerged";
        remove(aside.c_str());
        if (rename(path.c_str(), aside.c_str()) == 0)
            Log("existing cache could not be merged (the reason above) - kept it as %s.unmerged and "
                "started a new one", bundleBin.c_str());
        else
            Log("existing cache could not be merged NOR moved aside - capture disabled so it is "
                "not overwritten");
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) enabled = false;
    }

    static void LoadExisting()
    {
        LoadBundle();
    }

    // Merge one validated cache into the capture.
    static void MergeBundle(const pipelinekeys::CacheContents& c)
    {
        // Declaration indices are file-local, so they must be remapped into our
        // table before a record's key is hashed -- declIndex is part of the key.
        std::vector<uint32_t> remap(c.decls.size(), 0);
        for (size_t i = 0; i < c.decls.size(); i++)
        {
            uint64_t h = fnv1a(c.decls[i].data(), c.decls[i].size() * sizeof(D3DVERTEXELEMENT9));
            auto it = declIndexByHash.find(h);
            if (it != declIndexByHash.end())
            {
                remap[i] = it->second;
            }
            else
            {
                remap[i] = (uint32_t)declTable.size();
                declIndexByHash.emplace(h, remap[i]);
                declTable.push_back(c.decls[i]);
            }
        }

        for (auto& rec : c.keys)
        {
            KeyRecord k = rec;
            if (k.declIndex != 0xFFFFFFFFu)
                k.declIndex = remap[k.declIndex];   // in range: the reader checked it

            uint64_t h = fnv1a(&k, offsetof(KeyRecord, count));
            auto it = keyIndex.find(h);
            if (it != keyIndex.end())
                // Only reachable if the file holds the same key twice. It must NOT be
                // reachable from re-merging files that share history: `count` drives
                // the warm-most-used-first ordering, and re-adding an already-
                // accumulated file compounds it -- that is how this cache reached
                // 41.5e9 counted draws against 5,073,509 actually recorded.
                records[it->second].count += k.count;
            else
            {
                keyIndex.emplace(h, (uint32_t)records.size());
                records.push_back(k);
            }
            mergedIn++;
        }

        // The bytecode travels in the same file as the keys that name it, so a
        // contribution can never arrive with keys whose shaders are missing.
        for (auto& [h, b] : c.shaders)
            if (shaderBlobs.emplace(h, b).second) blobsMergedIn++;
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
        WriteCacheFile();
        blobsDirty = false;
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

            // Share this PC's capture(s) through plugins\d3d9cache\, whether or not
            // this session captures: now, before anything could be appended to them.
            d3d9cache::SnapshotOwnCaptures([](const char* line) { pipelinekeys::LogLine("[ShaderCapture] ", line); });

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
