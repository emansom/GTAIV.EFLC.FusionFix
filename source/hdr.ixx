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
    static inline float fSdrPaperWhiteNits = 100.0f; // BT.1886 SDR reference, used when HDR is off
    static inline float fUiPaperWhiteNits = 0.0f;    // 0 = match scene paper white; else absolute nits for the HUD/menu/map
    static inline int32_t iConsoleGamma = 0;         // 0 = off, 1 = Xenon (360), 2 = Cell (PS3); fused into the blit
    static inline float fPeakNits = 0.0f;           // 0 = probe the display
    static inline float fShoulderFraction = 0.5f;   // roll-off starts at 50% of peak

    // True once the swapchain is confirmed BT.2020 PQ. The container is fixed for
    // the life of the process - DXVK disables the swapchain upgrade entirely unless
    // dxvk.conf also names a colour space, and any colour space it names is written
    // back over the app's on every swapchain recreation. So the HDR toggle changes
    // the transform, not the container, exactly as Windows does for SDR content
    // while the desktop is in HDR mode.
    static bool IsContainerHdr() { return bContainerHdr; }

    // The UI paper-white boost must apply only to the 2D/HUD pass, never the 3D
    // world: gta_default and the other glyph shaders are shared with world/LOD
    // geometry, so a frame-global boost would also lift distant scenery whenever
    // UI Brightness is raised. grcViewport::mIsPerspective is true for the world,
    // reflection and cutscene passes and false for the orthographic 2D HUD/menu,
    // giving an exact, cheap pass discriminator. The viewport hook flips this.
    static void SetUiPass(bool ortho) { bUiPass = ortho; }
    static bool IsUiPass() { return bUiPass; }

private:
    static inline bool bContainerHdr = false;
    static inline bool bProbed = false;
    static inline bool bUiPass = false;                 // true only during the ortho 2D/HUD pass
    static inline const void* pLastSwapchain = nullptr; // identity of the swapchain we last negotiated

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

        // With the toggle off the container is still PQ, but white maps to the
        // SDR reference level instead of the HDR paper white - the user chose a
        // dimmer plain-SDR look over the Windows model of one unified white.
        const float white = (std::max)(bEnabled ? fPaperWhiteNits : fSdrPaperWhiteNits, 50.0f);

        // The shader rolls off in BOTH modes; only the target differs. HDR
        // compresses into the panel's peak, SDR-in-PQ into its own white point,
        // so the fp16 scene's overbright folds into the top of the SDR range
        // instead of clipping at saturate ("blown out lights").
        const float effPeak = bEnabled ? peak : white;
        const float effShoulder = bEnabled ? shoulder
                                           : effPeak * std::clamp(fShoulderFraction, 0.0f, 0.95f);

        const float params[4] =
        {
            bEnabled ? 0.0f : 1.0f,   // 0 = HDR, 1 = SDR-in-PQ (informational to the shader now)
            white / 10000.0f,
            effPeak / 10000.0f,
            effShoulder / 10000.0f,
        };
        pDevice->SetPixelShaderConstantF(kParamRegister, params, 1);

        // Console gamma fused into the blit (c51.x): the Xenon/Cell curve runs
        // before the decode inside the shader, so the console look is applied
        // correctly in PQ instead of the standalone ramp fighting the encode.
        // Applies in both HDR and SDR-in-PQ, since the blit owns the final image
        // in both. See HDR_PQ.hlsl for why the decode stays 2.2.
        const float consoleGamma[4] = { float(iConsoleGamma), 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(kParamRegister + 1, consoleGamma, 1);
    }

    // UI paper white: the HUD, pause menu, map and 2D overlays sit at their own
    // luminance, independent of the scene's paper white, so the interface can be
    // pushed brighter for legibility without dragging scene exposure with it -
    // the standard modern-HDR UI-brightness control. Applied per DRAW: the
    // gta_im / rage_im pixel shaders multiply their colour output by (1 + c207.y),
    // so UI draws land at the UI level while the world (which never passes through
    // those shaders) keeps the calibrated paper white. Only in HDR mode -
    // SDR-in-PQ has no headroom above its white point, so the UI rides the scene
    // there (factor 1). An unset or zero c207 is an exact identity, so stock
    // shaders, non-HDR sessions and "match scene" are untouched by construction.
    static void UploadUiPaperWhite(IDirect3DDevice9* pDevice)
    {
        const float scenePw = (std::max)(bEnabled ? fPaperWhiteNits : fSdrPaperWhiteNits, 50.0f);
        const float uiPw = (fUiPaperWhiteNits > 0.0f) ? fUiPaperWhiteNits : scenePw; // 0 = match scene
        const float factor = (bEnabled && bContainerHdr) ? (uiPw / scenePw) : 1.0f;

        // gain = factor^(1/2.2) - 1: the im shaders multiply in gamma space and
        // the final blit decodes with pow(2.2), so this lands as an exact uniform
        // luminance multiply and the UI ends at uiPw nits. z/w (floor/clamp) stay
        // zero - the old menu/splash boost used them; a plain UI level does not.
        const float gain = std::pow((std::max)(factor, 1e-4f), 1.0f / 2.2f) - 1.0f;

        // Gate the boost to the 2D/HUD pass. Identity (0) during the 3D world pass
        // so shaders shared between the HUD and world geometry (gta_default, etc.)
        // leave the world untouched; the full gain during the ortho HUD/menu pass.
        const float gatedGain = bUiPass ? gain : 0.0f;

        const float params[4] = { 0.0f, gatedGain, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(207, params, 1);

        // UI paper-white ceiling for gta_imPS3 (HUD glyphs). The per-draw wanted-star
        // boost multiplies the dim star glyphs up to paper white, but it also shares
        // the shader with already-bright glyphs (money/ammo) that would then overshoot
        // to ~2x paper white. Clamping the glyph output to (1 + gain) = the UI level
        // caps them: dim stars are still lifted up to it, bright text is held at it.
        // Large outside the HUD pass so in-world gta_im sprites keep their highlights.
        const float uiCeil[4] = { bUiPass ? (1.0f + gain) : 1.0e6f, 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(204, uiCeil, 1);
    }

    // The wanted-level stars draw through gta_imPS3 like the rest of the HUD and
    // do receive the c207 boost, but the game emits the star glyphs far darker at
    // source than the radar/text, so the uniform boost leaves them dim. This is the
    // c207.y to use just around the star draw so they land at UI paper white:
    // fStarBaseDeficit compensates for how much darker the source is. Zero outside
    // HDR so SDR is untouched. Tunable via [HDR] WantedStarDeficit.
    static inline float fStarBaseDeficit = 11.0f;
    static float StarBoostGain()
    {
        if (!(bEnabled && bContainerHdr)) return 0.0f;
        const float scenePw = (std::max)(fPaperWhiteNits, 50.0f);
        const float uiPw = (fUiPaperWhiteNits > 0.0f) ? fUiPaperWhiteNits : scenePw;
        const float factor = (std::max)(fStarBaseDeficit, 1.0f) * (uiPw / scenePw);
        return std::pow((std::max)(factor, 1e-4f), 1.0f / 2.2f) - 1.0f;
    }

public:
    // Establishes the HDR10 container once, then does nothing. NOT a runtime switch:
    // dxvk.conf pins the colour space (it has to - DXVK disables the swapchain
    // upgrade outright unless a colour space is named), and GetSurfaceFormat writes
    // that value back over m_colorspace on every swapchain recreation. Calling
    // SetColorSpace here is belt and braces for builds where the config route is
    // absent; the useful work is the EDID probe and the metadata.
    // Re-negotiate whenever the swapchain object changes, not just once. A device
    // reset (alt-tab, exclusive-fullscreen mode switch, resolution change) makes
    // DXVK build a fresh swapchain that reverts to the default SDR colour space;
    // our SetColorSpace / metadata / format-unlock live on the old one and would
    // otherwise never be re-applied, leaving the shaders PQ-encoding into an SDR
    // container. Keyed on the swapchain's identity so it also catches recreations
    // that do not surface as a RAGE device-reset callback; OnDeviceReset() below
    // is the belt-and-suspenders trigger for the reset that does.
    static void EnsureContainer(IDirect3DDevice9* pDevice)
    {
        if (!pDevice) return;

        IDirect3DSwapChain9* pSwapchain = nullptr;
        if (FAILED(pDevice->GetSwapChain(0, &pSwapchain)) || !pSwapchain) return;
        const void* scId = static_cast<void*>(pSwapchain);
        pSwapchain->Release(); // identity compare only; never dereferenced after this

        // Already HDR on this exact swapchain: nothing to do. (When NOT HDR we
        // still fall through every frame, so an OS-side HDR flip that recreates
        // the swapchain with ST2084 support flips us on and un-greys the row.)
        if (bContainerHdr && scId == pLastSwapchain) return;
        if (scId != pLastSwapchain) { pLastSwapchain = scId; bContainerHdr = false; bProbed = false; }

        auto* pExt = GetExtSwapchain(pDevice);
        if (!pExt)
        {
            // Not DXVK: no HDR container is possible. Grey the HDR row itself -
            // the toggle could not do anything on this output.
            bHdrLockHdrToggle = true;
            return;
        }

        if (!pExt->CheckColorSpaceSupport(VK_COLOR_SPACE_HDR10_ST2084_EXT))
        {
            // The surface offers no ST2084: an SDR display session. Same
            // treatment; re-checked every frame, so an OS-side HDR flip that
            // recreates the swapchain with support un-greys the row again.
            bHdrLockHdrToggle = true;
            pExt->Release();
            return;
        }

        bHdrLockHdrToggle = false;

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

    static float UiPaperWhiteFromIndex(int32_t i)
    {
        // 0 means "match the scene paper white" (identity); the rest are absolute
        // UI nits, brighter than the 203 scene default for HUD legibility.
        static constexpr float kNits[] = { 0.0f, 300.0f, 400.0f };
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
        static std::optional<std::reference_wrapper<int32_t>> pHdr, pPaperWhite, pPeak, pUiPaperWhite, pConsoleGamma;
        if (!pHdr)          pHdr          = FusionFixSettings.GetRef("PREF_HDR");
        if (!pPaperWhite)   pPaperWhite   = FusionFixSettings.GetRef("PREF_HDR_PAPERWHITE");
        if (!pPeak)         pPeak         = FusionFixSettings.GetRef("PREF_HDR_PEAK");
        if (!pUiPaperWhite) pUiPaperWhite = FusionFixSettings.GetRef("PREF_HDR_UIPAPERWHITE");
        if (!pConsoleGamma) pConsoleGamma = FusionFixSettings.GetRef("PREF_CONSOLE_GAMMA");

        int32_t hdr, paperWhite, peak, uiPaperWhite;
        if (pHdr && pPaperWhite && pPeak && pUiPaperWhite && pConsoleGamma)
        {
            hdr          = pHdr->get();
            paperWhite   = pPaperWhite->get();
            peak         = pPeak->get();
            uiPaperWhite = pUiPaperWhite->get();
            iConsoleGamma = pConsoleGamma->get();
        }
        else
        {
            // The settings table is not populated for the game's first frames -
            // the Rockstar legal splash and the episode intros - which is exactly
            // when correct encoding matters most, because those frames otherwise
            // hit the PQ container raw and present at its 10000 nit peak. Bridge
            // the gap straight from the ini; the menu-backed references above
            // take over the moment they resolve.
            static int32_t iniHdr = -1, iniPaperWhite = 1, iniPeak = 0, iniUiPaperWhite = 0, iniConsoleGamma = 0;
            if (iniHdr < 0)
            {
                CIniReader ini("");
                iniHdr          = ini.ReadInteger("HDR", "HDR", 0);
                iniPaperWhite   = ini.ReadInteger("HDR", "PaperWhiteLevel", 1);
                iniPeak         = ini.ReadInteger("HDR", "PeakLevel", 0);
                iniUiPaperWhite = ini.ReadInteger("HDR", "UIPaperWhiteLevel", 0);
                iniConsoleGamma = ini.ReadInteger("MISC", "ConsoleGamma", 0);
            }
            hdr          = iniHdr;
            paperWhite   = iniPaperWhite;
            peak         = iniPeak;
            uiPaperWhite = iniUiPaperWhite;
            iConsoleGamma = iniConsoleGamma;
        }

        bEnabled = hdr != 0;
        fPaperWhiteNits = PaperWhiteFromIndex(paperWhite);
        fUiPaperWhiteNits = UiPaperWhiteFromIndex(uiPaperWhite);

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
        // Same retry pattern as SyncFromSettings - looked up before the settings
        // table exists, and a latched nullopt would silently do nothing.
        static std::optional<std::reference_wrapper<int32_t>> pToneMapping;
        if (!pToneMapping)  pToneMapping  = FusionFixSettings.GetRef("PREF_TONEMAPPING");

        // Locks the rows the PQ output owns. Console gamma is now FUSED into the
        // blit (c51, applied before the encode), so it works correctly with PQ
        // and stays fully user-controllable - no lock, no forcing off. Only Tone
        // Mapping is locked, and only while HDR is on: its LUT+shoulder clamp the
        // scene to SDR, which cannot coexist with real HDR. In SDR-in-PQ the
        // game's tone map is legitimate and stays user-controlled.
        bHdrLockToneMapping = bEnabled;

        // Tone mapping is mode-dependent. In HDR the final blit's roll-off
        // replaces it, so it must be off. In SDR-in-PQ the game's own tone map
        // is load-bearing: it is what compresses the fp16 scene's overbright
        // into [0,1] before the blit's saturate - without it highlight detail
        // clips away and lights render blown out. On the HDR->SDR transition
        // the user's ini preference is handed back (the runtime value was
        // zeroed while HDR was on; the ini itself was never touched).
        static bool bWasHdr = true;
        if (bEnabled)
        {
            if (pToneMapping) pToneMapping->get() = 0;
        }
        else if (bWasHdr && pToneMapping)
        {
            CIniReader ini("");
            pToneMapping->get() = ini.ReadInteger("MISC", "ToneMapping", 0);
        }
        bWasHdr = bEnabled;
    }

    // Per-frame housekeeping: pick up menu changes and negotiate the container once.
    // No encoding happens here any more - that is entirely the final blit's job.
    static void BeginFrame(IDirect3DDevice9* pDevice)
    {
        if (!pDevice) return;
        SyncFromSettings();
        EnsureContainer(pDevice);
        bUiPass = false; // a frame starts with the 3D world; the viewport hook flips this for the ortho HUD
        UploadUiPaperWhite(pDevice);
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
            HDR::fSdrPaperWhiteNits = std::clamp(iniReader.ReadFloat("HDR", "SdrPaperWhite", 100.0f), 50.0f, 400.0f);
            HDR::fStarBaseDeficit = std::clamp(iniReader.ReadFloat("HDR", "WantedStarDeficit", 11.0f), 1.0f, 32.0f);
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
