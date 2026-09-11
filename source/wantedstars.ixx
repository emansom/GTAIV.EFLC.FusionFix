module;

#include <common.hxx>
#include <map>
#include <mutex>

export module wantedstars;

import common;
import comvars;
import hdr;
import shaders; // GetFusionShaderID

// The wanted-level stars are gta_imPS3 (FusionShader 284) glyph quads drawn in the
// top-right. They already receive the c207 UI paper-white boost, but the game emits
// the star glyphs far darker at source than the radar and text, so the uniform boost
// leaves them dim. They cannot be lifted from CFont::PrintString: the glyphs are
// flushed later inside a render-bucket flush whose viewport updates re-run
// UploadUiPaperWhite and reset c207 back to the ordinary level. So we set the star
// gain on c207.y immediately around the actual DrawPrimitive - nothing runs between
// the constant upload and the draw - and restore it right after. Scoped to gta_imPS3
// draws in the top-right screen region, so only the stars (and adjacent money/clock)
// are lifted, never the world. Identity outside HDR, so SDR is byte-for-byte unchanged.

static bool IsStarShader(IDirect3DDevice9* dev)
{
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetPixelShader(&ps);
    if (!ps) return false;
    static std::map<IDirect3DPixelShader9*, bool>& cache = *new std::map<IDirect3DPixelShader9*, bool>();
    bool star;
    auto it = cache.find(ps);
    if (it != cache.end()) star = it->second;
    else { star = (GetFusionShaderID(ps) == 284); cache[ps] = star; }
    ps->Release();
    return star;
}

// True if stream-0's first drawn vertex sits in the top-right screen region.
static bool IsTopRight(IDirect3DDevice9* dev, int baseVtx)
{
    IDirect3DVertexBuffer9* vb = nullptr; unsigned off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return false;
    bool tr = false; void* ptr = nullptr;
    if (SUCCEEDED(vb->Lock(0, 0, &ptr, D3DLOCK_READONLY)) && ptr)
    {
        __try {
            const float* v = (const float*)((const char*)ptr + off + (size_t)baseVtx * stride);
            tr = (v[0] > 800.0f && v[1] < 320.0f); // screen-space, top-right corner
        } __except (EXCEPTION_EXECUTE_HANDLER) { tr = false; }
        vb->Unlock();
    }
    vb->Release();
    return tr;
}

SafetyHookInline shDP{};
HRESULT __stdcall DP_detour(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT startVtx, UINT primCount)
{
    const float sg = HDR::StarBoostGain();
    if (sg > 0.0f && HDR::IsUiPass() && IsStarShader(self) && IsTopRight(self, (int)startVtx))
    {
        float saved[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        self->GetPixelShaderConstantF(207, saved, 1);
        const float boosted[4] = { 0.0f, sg, saved[2], saved[3] };
        self->SetPixelShaderConstantF(207, boosted, 1);
        const HRESULT hr = shDP.stdcall<HRESULT>(self, type, startVtx, primCount);
        self->SetPixelShaderConstantF(207, saved, 1);
        return hr;
    }
    return shDP.stdcall<HRESULT>(self, type, startVtx, primCount);
}

SafetyHookInline shDIP{};
HRESULT __stdcall DIP_detour(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT baseVtx,
                             UINT minVtx, UINT numVtx, UINT startIdx, UINT primCount)
{
    const float sg = HDR::StarBoostGain();
    if (sg > 0.0f && HDR::IsUiPass() && IsStarShader(self) && IsTopRight(self, baseVtx))
    {
        float saved[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        self->GetPixelShaderConstantF(207, saved, 1);
        const float boosted[4] = { 0.0f, sg, saved[2], saved[3] };
        self->SetPixelShaderConstantF(207, boosted, 1);
        const HRESULT hr = shDIP.stdcall<HRESULT>(self, type, baseVtx, minVtx, numVtx, startIdx, primCount);
        self->SetPixelShaderConstantF(207, saved, 1);
        return hr;
    }
    return shDIP.stdcall<HRESULT>(self, type, baseVtx, minVtx, numVtx, startIdx, primCount);
}

class WantedStars
{
public:
    WantedStars()
    {
        FusionFix::onEndScene() += []()
        {
            static std::once_flag once;
            std::call_once(once, []
            {
                auto dev = rage::grcDevice::GetD3DDevice();
                if (!dev) return;
                void** vtbl = *reinterpret_cast<void***>(dev);
                shDP  = safetyhook::create_inline(vtbl[81], reinterpret_cast<void*>(DP_detour));
                shDIP = safetyhook::create_inline(vtbl[82], reinterpret_cast<void*>(DIP_detour));
            });
        };
    }
} WantedStars;
