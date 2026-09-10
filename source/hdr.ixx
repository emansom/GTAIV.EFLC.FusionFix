module;

#include <common.hxx>
#include <d3d9.h>

export module hdr;

import common;
import comvars;
import settings;

// ---------------------------------------------------------------------------
// Native HDR10 output for GTA IV.
//
// The shader half lives in rage_postfx / gta_im / rage_im (see
// tools/apply-hdr-shader-patch.py): the composite pass decodes the game's
// gamma-2.2 output back to scene-linear, rolls highlights off to the display
// peak, converts BT.709 -> BT.2020 and encodes PQ. This module is the other
// half - it puts the swapchain into an HDR10 colour space, tells the compositor
// what luminance range the content covers, and feeds the shaders their
// parameters.
//
// Shader constant contract (see the patch script for the matching asm):
//
//   c207.x  encode enable   0 = passthrough, 1 = encode to PQ
//   c207.y  paper white nits / 10000
//   c207.z  peak nits       / 10000
//   c207.w  roll-off shoulder nits / 10000
//
// c207 is free in rage_postfx, gta_im and rage_im simultaneously - note gta_im
// binds c22..c27 and c208, so neighbouring registers are NOT safe.
//
// Why one flag drives both the scene and the UI: D3D9 pixel shader constants are
// device state, so there is a single c207 at any moment. Within a frame the game
// draws (1) the world, including in-world gta_im sprites, (2) the postfx
// composite, then (3) the HUD, again through gta_im. Setting the flag just before
// postfx therefore leaves in-world sprites untouched while covering both the
// composite and the HUD that follows it. The shader cannot make that distinction
// itself, which is why the 10000-nit HUD could not be fixed in asm alone.
// ---------------------------------------------------------------------------

// DXVK's D3D9 vendor extension. Declared here rather than pulled from dxvk
// headers so the mod builds against a stock SDK. Layout must match
// src/d3d9/d3d9_interfaces.h exactly - the vtable order is load-bearing.
using VkColorSpaceKHR = int32_t;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_SRGB_NONLINEAR_KHR       = 0;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT = 1000104002;
constexpr VkColorSpaceKHR VK_COLOR_SPACE_HDR10_ST2084_EXT         = 1000104008;

constexpr int32_t VK_STRUCTURE_TYPE_HDR_METADATA_EXT = 1000105000;

struct VkXYColorEXT { float x, y; };

struct VkHdrMetadataEXT
{
    int32_t      sType;
    const void*  pNext;
    VkXYColorEXT displayPrimaryRed;
    VkXYColorEXT displayPrimaryGreen;
    VkXYColorEXT displayPrimaryBlue;
    VkXYColorEXT whitePoint;
    float        maxLuminance;
    float        minLuminance;
    float        maxContentLightLevel;
    float        maxFrameAverageLightLevel;
};

// NOTE: primaries are float[2], not VkXYColorEXT - verified against dxvk source.
struct D3D9VkExtOutputMetadata
{
    float RedPrimary[2];
    float GreenPrimary[2];
    float BluePrimary[2];
    float WhitePoint[2];
    float MinLuminance;
    float MaxLuminance;
    float MaxFullFrameLuminance;
};

struct __declspec(uuid("13776e93-4aa9-430a-a4ec-fe9e281181d5")) ID3D9VkExtSwapchain : public IUnknown
{
    virtual BOOL    STDMETHODCALLTYPE CheckColorSpaceSupport(VkColorSpaceKHR ColorSpace) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace(VkColorSpaceKHR ColorSpace) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(const VkHdrMetadataEXT* pHDRMetadata) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentOutputDesc(D3D9VkExtOutputMetadata* pOutputDesc) = 0;
    virtual void    STDMETHODCALLTYPE UnlockAdditionalFormats() = 0;
};

export class HDR
{
public:
    // Single constant register feeding the final HDR blit shader:
    //   x = mode (0 HDR / 1 SDR), y = paper white/10000, z = peak/10000,
    //   w = roll-off shoulder/10000. Matches HDR_PQ.hlsl's hdrParams : register(c50).
    static inline constexpr uint32_t kParamRegister = 50;

    static inline bool  bEnabled = false;
    static inline float fPaperWhiteNits = 203.0f;   // ITU reference white
    static inline float fPeakNits = 0.0f;           // 0 = probe the display
    static inline float fShoulderFraction = 0.5f;   // roll-off starts at 50% of peak

    // True once the swapchain is confirmed BT.2020 PQ. The container is fixed for
    // the life of the process - DXVK disables the swapchain upgrade entirely unless
    // dxvk.conf also names a colour space, and any colour space it names is written
    // back over the app's on every swapchain recreation. So the HDR toggle changes
    // the transform, not the container, exactly as Windows does for SDR content
    // while the desktop is in HDR mode.
    static bool IsContainerHdr() { return bContainerHdr; }

private:
    static inline bool bContainerHdr = false;
    static inline bool bProbed = false;

    // Reported to the compositor as the mastering display. Overwritten by the
    // real panel's EDID when GetCurrentOutputDesc() succeeds.
    static inline D3D9VkExtOutputMetadata sOutput =
    {
        { 0.640f, 0.330f }, { 0.300f, 0.600f }, { 0.150f, 0.060f }, { 0.3127f, 0.3290f },
        0.0f, 400.0f, 100.0f
    };

    static ID3D9VkExtSwapchain* GetExtSwapchain(IDirect3DDevice9* pDevice)
    {
        if (!pDevice) return nullptr;
        IDirect3DSwapChain9* pSwapchain = nullptr;
        if (FAILED(pDevice->GetSwapChain(0, &pSwapchain)) || !pSwapchain) return nullptr;
        ID3D9VkExtSwapchain* pExt = nullptr;
        pSwapchain->QueryInterface(__uuidof(ID3D9VkExtSwapchain), reinterpret_cast<void**>(&pExt));
        pSwapchain->Release();
        return pExt;   // null on non-DXVK D3D9 - HDR then stays off, which is correct
    }

    // Ask the swapchain for the panel's real capabilities so peak nits does not
    // have to be guessed. DXVK fills this from the monitor's EDID.
    static void ProbeDisplay(ID3D9VkExtSwapchain* pExt)
    {
        D3D9VkExtOutputMetadata desc = {};
        if (FAILED(pExt->GetCurrentOutputDesc(&desc))) return;
        if (desc.MaxLuminance <= 0.0f) return;
        sOutput = desc;
        if (fPeakNits <= 0.0f)
            fPeakNits = desc.MaxLuminance;
    }

    static void PublishMetadata(ID3D9VkExtSwapchain* pExt)
    {
        // Mesa's Wayland WSI silently DROPS the whole metadata block unless
        // maxFALL <= maxCLL <= mastering max, so clamp rather than trust inputs.
        const float maxLum = sOutput.MaxLuminance > 0.0f ? sOutput.MaxLuminance : fPeakNits;
        const float maxCLL = (std::min)(fPeakNits, maxLum);
        const float maxFALL = (std::min)(sOutput.MaxFullFrameLuminance > 0.0f
                                            ? sOutput.MaxFullFrameLuminance : fPaperWhiteNits,
                                         maxCLL);

        VkHdrMetadataEXT meta = {};
        meta.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
        meta.displayPrimaryRed   = { sOutput.RedPrimary[0],   sOutput.RedPrimary[1] };
        meta.displayPrimaryGreen = { sOutput.GreenPrimary[0], sOutput.GreenPrimary[1] };
        meta.displayPrimaryBlue  = { sOutput.BluePrimary[0],  sOutput.BluePrimary[1] };
        meta.whitePoint          = { sOutput.WhitePoint[0],   sOutput.WhitePoint[1] };
        meta.maxLuminance = maxLum;
        meta.minLuminance = sOutput.MinLuminance;
        meta.maxContentLightLevel = maxCLL;
        meta.maxFrameAverageLightLevel = maxFALL;
        pExt->SetHDRMetaData(&meta);
    }

public:
    // Feeds the final full-screen HDR blit (see consolegamma.ixx). Nothing else
    // encodes any more: the game keeps writing display-referred gamma 2.2, all 2D
    // content blends against it in gamma space as its artists intended, and this
    // one pass converts the finished frame to BT.2020 PQ.
    static void UploadBlitConstants(IDirect3DDevice9* pDevice)
    {
        // Never let peak collapse to zero or meet the shoulder. The shader's
        // roll-off divides by (peak - shoulder); at zero the recombination
        // collapses and the frame is multiplied by 0 - a black screen. The shader
        // guards this too: it is a silent, total failure rather than a glitch.
        const float peak = (std::max)(fPeakNits, 100.0f);
        const float shoulder = peak * std::clamp(fShoulderFraction, 0.0f, 0.95f);

        const float params[4] =
        {
            bEnabled ? 0.0f : 1.0f,   // 0 = HDR, 1 = SDR-in-PQ
            (std::max)(fPaperWhiteNits, 50.0f) / 10000.0f,
            peak / 10000.0f,
            shoulder / 10000.0f,
        };
        pDevice->SetPixelShaderConstantF(kParamRegister, params, 1);
    }

public:
    // Establishes the HDR10 container once, then does nothing. NOT a runtime switch:
    // dxvk.conf pins the colour space (it has to - DXVK disables the swapchain
    // upgrade outright unless a colour space is named), and GetSurfaceFormat writes
    // that value back over m_colorspace on every swapchain recreation. Calling
    // SetColorSpace here is belt and braces for builds where the config route is
    // absent; the useful work is the EDID probe and the metadata.
    static void EnsureContainer(IDirect3DDevice9* pDevice)
    {
        if (bContainerHdr) return;

        auto* pExt = GetExtSwapchain(pDevice);
        if (!pExt) return;   // not DXVK: no HDR container, shaders stay in passthrough

        if (!pExt->CheckColorSpaceSupport(VK_COLOR_SPACE_HDR10_ST2084_EXT))
        {
            pExt->Release();
            return;
        }

        if (!bProbed) { ProbeDisplay(pExt); bProbed = true; }
        if (fPeakNits <= 0.0f) fPeakNits = 400.0f;   // last-resort default

        // Lets the game create 10-bit / fp16 backbuffers that plain D3D9 forbids.
        pExt->UnlockAdditionalFormats();

        pExt->SetColorSpace(VK_COLOR_SPACE_HDR10_ST2084_EXT);
        PublishMetadata(pExt);
        bContainerHdr = true;
        pExt->Release();
    }

    // Menu index -> nits. A short list of named presets rather than a fine-grained
    // slider: GTA IV can only show a bar for a slider, with no numeric readout, so
    // a three-entry option list with real nits figures is both simpler and far
    // more useful for calibration.
    //
    //   paper white: 100 / 203 / 300 nits        (default index 1 = 203,
    //                                             ITU-R BT.2408 reference white)
    //   peak:        Auto / 400 / 1000 nits      (default index 0 = Auto)
    //
    // The rows borrow the dormant MENU_DISPLAY_EXTRA_1 / EXTRA_2 enums and their
    // option strings live in the <menupc> blocks of frontend_menus.xml - keep
    // these indices in step with those blocks. Borrowing a LIVE stock enum would show
    // that enum's own fixed strings, which is why the option text lives there rather
    // than in text/americanFF.txt.
    static float PaperWhiteFromIndex(int32_t i)
    {
        static constexpr float kNits[] = { 100.0f, 203.0f, 300.0f };
        return kNits[std::clamp(i, 0, 2)];
    }

    static float PeakFromIndex(int32_t i)
    {
        // 0 means "ask the display" - resolved from EDID in SyncFromSettings.
        static constexpr float kNits[] = { 0.0f, 400.0f, 1000.0f };
        return kNits[std::clamp(i, 0, 2)];
    }

    // Re-read the live menu values every frame so the sliders and the toggle take
    // effect immediately, without a restart.
    static void SyncFromSettings()
    {
        // Retry the lookups until they resolve. This runs from grcSetup_BeginDraw,
        // which fires before the settings table is populated, so caching the result
        // of the first call in a plain `static auto` latches nullopt forever - the
        // menu values are then never read, bEnabled stays false, and the mod runs in
        // SDR mode at the hardcoded default paper white no matter what the UI says.
        static std::optional<std::reference_wrapper<int32_t>> pHdr, pPaperWhite, pPeak;
        if (!pHdr)        pHdr        = FusionFixSettings.GetRef("PREF_HDR");
        if (!pPaperWhite) pPaperWhite = FusionFixSettings.GetRef("PREF_HDR_PAPERWHITE");
        if (!pPeak)       pPeak       = FusionFixSettings.GetRef("PREF_HDR_PEAK");

        int32_t hdr, paperWhite, peak;
        if (pHdr && pPaperWhite && pPeak)
        {
            hdr        = pHdr->get();
            paperWhite = pPaperWhite->get();
            peak       = pPeak->get();
        }
        else
        {
            // The settings table is not populated for the game's first frames -
            // the Rockstar legal splash and the episode intros - which is exactly
            // when correct encoding matters most, because those frames otherwise
            // hit the PQ container raw and present at its 10000 nit peak. Bridge
            // the gap straight from the ini; the menu-backed references above
            // take over the moment they resolve.
            static int32_t iniHdr = -1, iniPaperWhite = 1, iniPeak = 0;
            if (iniHdr < 0)
            {
                CIniReader ini("");
                iniHdr        = ini.ReadInteger("HDR", "HDR", 0);
                iniPaperWhite = ini.ReadInteger("HDR", "PaperWhiteLevel", 1);
                iniPeak       = ini.ReadInteger("HDR", "PeakLevel", 0);
            }
            hdr        = iniHdr;
            paperWhite = iniPaperWhite;
            peak       = iniPeak;
        }

        bEnabled = hdr != 0;
        fPaperWhiteNits = PaperWhiteFromIndex(paperWhite);

        // Auto (index 0) resolves to the panel's reported peak, but the display is
        // not probed until the colour space is first negotiated. Fall back to a sane
        // figure meanwhile rather than leaving the peak at zero, which would collapse
        // the shader's roll-off.
        const float requested = PeakFromIndex(peak);
        if (requested > 0.0f)
            fPeakNits = requested;
        else if (bProbed && sOutput.MaxLuminance > 0.0f)
            fPeakNits = sOutput.MaxLuminance;
        else
            fPeakNits = 400.0f;
    }

    // The game's own SDR tone map and console gamma ramp would both run on top of
    // our PQ output. Neither can coexist with it, so they are held off while HDR is
    // active rather than left as a way to silently corrupt the image.
    //
    // Deliberately written through GetRef rather than FusionFixSettings::Set():
    // Set() calls WriteToIni(), which rewrites the whole .ini file. This runs every
    // frame, so using Set() here means rewriting the ini ~60 times a second for as
    // long as HDR is on - which stalls the game and leaves writes pending at exit.
    // Applies whenever the container is PQ, in both HDR and SDR modes: the game's
    // own tone map and console gamma ramp would run on top of our encode either way.
    static void EnforceConflictingSettings()
    {
        if (!IsContainerHdr()) return;
        // Same retry pattern as SyncFromSettings - these are looked up before the
        // settings table exists, and a latched nullopt would silently do nothing.
        static std::optional<std::reference_wrapper<int32_t>> pToneMapping, pConsoleGamma;
        if (!pToneMapping)  pToneMapping  = FusionFixSettings.GetRef("PREF_TONEMAPPING");
        if (!pConsoleGamma) pConsoleGamma = FusionFixSettings.GetRef("PREF_CONSOLE_GAMMA");
        if (pToneMapping)  pToneMapping->get()  = 0;
        if (pConsoleGamma) pConsoleGamma->get() = 0;
    }

    // Per-frame housekeeping: pick up menu changes and negotiate the container once.
    // No encoding happens here any more - that is entirely the final blit's job.
    static void BeginFrame(IDirect3DDevice9* pDevice)
    {
        if (!pDevice) return;
        SyncFromSettings();
        EnsureContainer(pDevice);
    }
};

class HDRConfig
{
public:
    HDRConfig()
    {
        FusionFix::onInitEvent() += []()
        {
            // HDR, PaperWhiteLevel and PeakLevel are menu-backed, so the settings
            // system already loads them from [HDR] and writes them back on change.
            // Only the roll-off shape is ini-only - it is a tuning knob, not
            // something worth a menu row.
            CIniReader iniReader("");
            HDR::fShoulderFraction = std::clamp(iniReader.ReadFloat("HDR", "ShoulderFraction", 0.5f), 0.0f, 1.0f);
        };

        // The game's SDR tone map and console gamma ramp cannot coexist with PQ
        // output. Re-assert every frame rather than once, because the pause menu
        // lets the player change them mid-session.
        FusionFix::onEndScene() += []()
        {
            HDR::EnforceConflictingSettings();
        };
    }
} HDRConfig;
