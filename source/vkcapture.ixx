module;

#include <common.hxx>
#include "vulkan/vulkan.h"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "layer/utils.hpp"          // Fossilize's LOG* macros, which fossilize_errors.hpp requires
#include "fossilize_errors.hpp"     // set_thread_log_callback
#include "cli/fossilize_feature_filter.hpp"
#define RAPIDJSON_HAS_STDSTRING 1       // as fossilize.cpp includes it
#include "rapidjson/document.h"
#include "pipelinekeys.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <unordered_set>

export module vkcapture;

import common;
import comvars;

// ===========================================================================
//  VkCapture -- record the Vulkan pipelines DXVK actually creates, in-process.
//
//  The D3D9 pipeline cache (shadercapture/shaderprecompile) is portable because
//  every machine's own DXVK turns the same D3D9 keys into its own Vulkan state. What
//  it cannot see is anything below D3D9: DXVK's internal pipelines, the exact spec
//  constants, whatever the driver compiles on first use. A Fossilize database of the
//  real Vulkan create-infos covers that, per machine (see the golden-Fossilize plan
//  in the test harness, state/2026-09-19-linux-golden-fossilize-plan.md).
//
//  HOW IT HOOKS
//    DXVK finds Vulkan with LoadLibraryA("winevulkan.dll" / "vulkan-1.dll") and then
//    GetProcAddress(..., "vkGetInstanceProcAddr") (src/vulkan/vulkan_loader.cpp).
//    A DLL-load notification fires inside that LoadLibraryA, before the lookup, and
//    inline-hooks the library's vkGetInstanceProcAddr. From there vkCreateInstance,
//    vkCreateDevice and vkGetDeviceProcAddr are wrapped, and vkGetDeviceProcAddr
//    hands DXVK recording wrappers for the object-creation calls.
//
//    This works with stock, latest DXVK on Windows and Proton alike: no Vulkan layer
//    to ship, no environment variables, nothing to load inside Steam's runtime
//    container. On Wine only winevulkan.dll is hooked (DXVK prefers it and Wine's
//    vulkan-1.dll forwards to it -- hooking both would record twice); on Windows,
//    vulkan-1.dll.
//
//  WHAT IT RECORDS
//    What Fossilize's own layer records in normal mode (layer/dispatch.cpp):
//    graphics and compute pipelines (libraries and links included), shader modules,
//    pipeline and descriptor-set layouts, samplers and render passes, plus the
//    application info and enabled device features they were created against.
//    Everything goes to one append-mode stream archive beside the ASI.
//
//  REPLAY (ReplayVulkanPipelines = 1)
//    Fossilize databases are replayed on DXVK's own device -- through the real,
//    unwrapped entry points, so nothing replayed is recorded again -- to warm the
//    DRIVER's pipeline cache before gameplay asks for the same pipelines. Each
//    pipeline is destroyed as soon as it exists: the goal is the driver cache, not
//    the objects, and GTA IV is a 32-bit process.
//    The replay thread starts with DXVK's device and counts what it will go through
//    while the game loads. It replays nothing until the loading-screen pass
//    (shaderprecompile) has run AND everything DXVK compiled for it has finished
//    (see VulkanReplayState in pipelinekeys.h); then, with the loading screen held
//    and on most of the cores:
//      1. this PC's own recording,
//      2. everything else: other PCs' .foz files in plugins\pipelinecache\ and
//         the player's own Steam pre-cache.
//    The loading screen then waits once more for DXVK to go quiet. An unwarmed
//    pipeline is a stutter in gameplay, a longer first load is not.
//    Without the loading-screen pass (PrecompileShaders = 0) nothing is held: the
//    own recording replays right away on a few lowest-priority threads while the
//    game loads, and the foreign files on one, once DXVK has shown enough of its
//    own pipelines to judge them by (see ReplayAll).
//
//  SHARING BETWEEN PCs (plugins\pipelinecache\)
//    Once per launch, before this session appends anything, the own recording is
//    copied to plugins\pipelinecache\FusionFix.<h>.foz, <h> being the 64-bit FNV-1a
//    of its bytes: the player copies that folder's contents to their other PCs, and
//    never renames anything. The copy this PC wrote before (remembered in
//    FusionFix.pipelinecache.state beside the ASI) is deleted; no other file there
//    is ever touched. The folder is read flat -- .foz files at its top level, any
//    name -- and every input is used once per content, so the own copy, or the same
//    file under two names, is not replayed twice.
//
//    Replaying on the game's own device is what makes it cache-effective on every
//    driver (NVIDIA keys its cache on the process); it is also why nothing invalid
//    may reach it. A driver does not validate: RADV creates a pipeline whose depth
//    format it cannot render to, and dereferences a missing vertex shader. And a
//    fault in the driver cannot be caught here -- under Wine it happens on the Unix
//    side, and winevulkan answers it with ExitProcess(3). So every object passes
//    three checks before it is created:
//      * RELEVANT: every pNext structure, pipeline flag, dynamic state, SPIR-V
//        capability and extension it uses is one DXVK itself used on this device
//        (learned from DXVK's live calls through the wrappers above and from the
//        trusted entries). Anything else was built by another DXVK build or
//        configuration and would never be asked for again.
//      * SUPPORTED: Fossilize's own feature filter -- the check fossilize-replay,
//        and so Steam's pre-caching, runs before creating anything -- set up with
//        exactly what DXVK enabled on this device: its extensions and feature
//        chain, the physical device's properties, formats and set-layout limits.
//      * COMPLETE: every shader module and pipeline library the replayer resolved
//        exists (again what fossilize-replay checks). The replayer cannot be
//        trusted with that: an object it failed to create stays in its handle map
//        as VK_NULL_HANDLE, and the next pipeline using it gets the null handle.
//    TRUSTED entries -- this machine's own recording, linked to exactly this
//    session's application + feature hash (same DXVK build, same features) -- skip
//    the first check and teach it instead. Everything else -- older own entries,
//    other PCs' files in plugins\pipelinecache\, the player's own Steam pre-cache
//    buckets (read only, never written) -- needs all three.
//    Steam's top-level mixed database (every DXVK since 1.7, ~2 GB) is not read:
//    it matched 0.3% of what DXVK 3.1.1 builds, against 85.8% for its DXVK-3 bucket.
//
//  WATCHING DXVK COMPILE (whenever DXVK creates its device, whatever the settings)
//    What DXVK 3.1.1 does after the D3D9 calls have returned:
//      * every D3D9 shader created is queued to its "dxvk-shader-n/-l" workers
//        (registerShader, src/dxvk/dxvk_pipemanager.cpp), which turn it into DXVK's
//        IR and SPIR-V and, with pipeline libraries, build its library;
//      * binding a shader not compiled yet queues it again at high priority
//        (requestCompileShader), picked up by any worker;
//      * every draw is executed later on its CS thread ("dxvk-cs"), which creates a
//        pipeline the first time a state is drawn: the full compile with pipeline
//        libraries off (as on this rig), a fast link with them on -- and then the
//        optimized pipeline is queued to the "dxvk-shader-l" workers;
//      * new shaders are written to DXVK's IR cache by "dxvk-cache", which converts
//        whatever the workers have not yet.
//    So quiet is: none of DXVK's vkCreateGraphicsPipelines / vkCreateComputePipelines
//    / vkCreateShaderModule calls in flight, from any thread, none started or ended
//    for a while, and no CPU time used by those worker threads meanwhile (the IR
//    work calls no Vulkan at all). The wrappers count the calls; CompilerCpuUs reads
//    the CPU time; shaderprecompile does the waiting (see VulkanReplayState in
//    pipelinekeys.h). A driver may compile further in threads of its own after
//    vkCreate*Pipelines has returned, and nothing here can see that; RADV does not,
//    it compiles inside the call.
//
//  GAMEPLAY METRICS (whenever DXVK creates its device, whatever the settings)
//    Every DXVK pipeline creation is timed, and every vkQueuePresentKHR on its device
//    makes a frame time. Gameplay is while no loading screen has been up for 2 s,
//    once the first one has gone and the loading-screen pass (if one runs) is done;
//    what DXVK creates in those 2 s counts as gameplay too, unless a loading screen
//    comes back (PhaseAt). Reported every 15 s of gameplay and once more at exit; the lines
//    are described at ReportGameplay. Costs a clock read per creation and per
//    present: it stays on with everything else off, to measure the unwarmed game
//    against.
// ===========================================================================

class VkCapture
{
    static inline bool enabled = true;          // CaptureVulkanPipelines
    static inline bool replayEnabled = true;    // ReplayVulkanPipelines
    // ReplayVulkanPipelinesTrace: log every foreign entry just before it is created.
    // The log is flushed per line, so if creating one takes the process down -- in
    // the driver's native code, where no Windows exception is raised -- the last
    // line names it.
    static inline bool replayTrace = false;
    // ReplayVulkanPipelinesForeign: also replay databases this PC did not record with
    // this exact DXVK build (other PCs, Steam's buckets). It once took the game down
    // within seconds: a shader module rejected as not relevant left VK_NULL_HANDLE in
    // the replayer's handle map, a later pipeline of the same batch was created with
    // it, and RADV dereferenced the missing vertex shader
    // (radv_pipeline_init_vertex_input_state). The three checks above close that.
    static inline bool replayForeign = true;
    static inline SafetyHookInline shGIPA{};
    static inline std::string hookedLib;

    static inline PFN_vkCreateInstance      realCreateInstance = nullptr;
    static inline PFN_vkCreateDevice        realCreateDevice   = nullptr;
    static inline PFN_vkGetDeviceProcAddr   realGDPA           = nullptr;
    static inline VkInstance                instance           = VK_NULL_HANDLE;

    // The application info DXVK creates its instance with ("GTAIV.exe" / "DXVK"),
    // copied because it must outlive vkCreateInstance.
    static inline std::string appName, engineName;
    static inline VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    static inline bool haveAppInfo = false;

    struct Device
    {
        PFN_vkCreateGraphicsPipelines     CreateGraphicsPipelines = nullptr;
        PFN_vkCreateComputePipelines      CreateComputePipelines = nullptr;
        PFN_vkCreateShaderModule          CreateShaderModule = nullptr;
        PFN_vkCreatePipelineLayout        CreatePipelineLayout = nullptr;
        PFN_vkCreateDescriptorSetLayout   CreateDescriptorSetLayout = nullptr;
        PFN_vkCreateSampler               CreateSampler = nullptr;
        PFN_vkCreateRenderPass            CreateRenderPass = nullptr;
        PFN_vkCreateRenderPass2           CreateRenderPass2 = nullptr;
        PFN_vkCreateRenderPass2           CreateRenderPass2KHR = nullptr;
        PFN_vkDestroyDevice               DestroyDevice = nullptr;
        PFN_vkGetShaderModuleIdentifierEXT           GetShaderModuleIdentifierEXT = nullptr;
        PFN_vkGetShaderModuleCreateInfoIdentifierEXT GetShaderModuleCreateInfoIdentifierEXT = nullptr;
        bool usesIdentifiers = false;
        // The FIRST DXVK device seen, which is the game's. Second devices --
        // Proton's own d3d11 DXVK, and the mod's warm device -- are wrapped for
        // the pipeline counters and nothing else. In particular they must not
        // get the present wrapper: it counts frames into the gameplay report
        // and calls a function pointer loaded from this device.
        bool primary = false;

        // Used only by the replay, to get rid of what it creates.
        PFN_vkDestroyPipeline             DestroyPipeline = nullptr;
        PFN_vkDestroyShaderModule         DestroyShaderModule = nullptr;
        PFN_vkDestroySampler              DestroySampler = nullptr;
        PFN_vkDestroyDescriptorSetLayout  DestroyDescriptorSetLayout = nullptr;
        PFN_vkDestroyPipelineLayout       DestroyPipelineLayout = nullptr;
        PFN_vkDestroyRenderPass           DestroyRenderPass = nullptr;

        // What the feature filter asks of the device beyond what DXVK enabled: format
        // support, set-layout limits, and physical-device features (for pipeline
        // robustness). The same answers fossilize-replay's own device gives it.
        struct Query : Fossilize::DeviceQueryInterface
        {
            VkPhysicalDevice gpu = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            PFN_vkGetPhysicalDeviceFormatProperties FormatProperties = nullptr;
            PFN_vkGetPhysicalDeviceFeatures2        Features2 = nullptr;
            PFN_vkGetDescriptorSetLayoutSupport     SetLayoutSupport = nullptr;

            bool format_is_supported(VkFormat format, VkFormatFeatureFlags features) override
            {
                VkFormatProperties p{};
                FormatProperties(gpu, format, &p);
                return (features & (p.linearTilingFeatures | p.optimalTilingFeatures | p.bufferFeatures)) == features;
            }
            bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo* info) override
            {
                VkDescriptorSetLayoutSupport s{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
                SetLayoutSupport(device, info, &s);
                return s.supported == VK_TRUE;
            }
            void physical_device_feature_query(VkPhysicalDeviceFeatures2* pdf2) override { Features2(gpu, pdf2); }
        } query;
        Fossilize::FeatureFilter filter;
        bool haveFilter = false;

        std::unique_ptr<Fossilize::DatabaseInterface> db;
        std::unique_ptr<Fossilize::StateRecorder>     recorder;
        std::string path;

        // This session's application + feature hash, the one Fossilize links every
        // blob to: equal means same DXVK build and same enabled features.
        Fossilize::Hash appHash = 0;
        std::thread replay, metrics;
        std::atomic<bool> stop{ false };

        std::atomic<uint32_t> graphics{ 0 }, compute{ 0 }, modules{ 0 }, other{ 0 }, failed{ 0 };

        // DestroyDevice joins both threads first. The only other way a Device goes is
        // with `devices` at process exit, as DXVK rarely destroys its device: the
        // threads are gone by then, and a std::thread still joinable when destroyed
        // calls std::terminate -- a fail-fast crash on every quit, before the C
        // runtime has flushed the recording.
        ~Device()
        {
            if (replay.joinable()) replay.detach();
            if (metrics.joinable()) metrics.detach();
        }
    };
    static inline std::shared_mutex devLock;
    static inline std::unordered_map<VkDevice, std::unique_ptr<Device>> devices;
    // The wrapped device's present, for the frame times. Queues carry no device, and
    // only one device is ever wrapped.
    static inline PFN_vkQueuePresentKHR realQueuePresent = nullptr;

    static void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list va; va_start(va, fmt);
        vsnprintf(buf, sizeof(buf), fmt, va);
        va_end(va);
        pipelinekeys::LogLine("[VkCapture] ", buf);
    }

    static std::string PluginsDir()
    {
        char buf[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PluginsDir), &self);
        if (!GetModuleFileNameA(self, buf, MAX_PATH)) return std::string();
        std::string s(buf);
        auto slash = s.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
    }

    static Device* Get(VkDevice device)
    {
        std::shared_lock lock(devLock);
        auto it = devices.find(device);
        return it == devices.end() ? nullptr : it->second.get();
    }

    static const void* FindPNext(const void* chain, VkStructureType type)
    {
        for (auto* s = static_cast<const VkBaseInStructure*>(chain); s; s = s->pNext)
            if (s->sType == type) return s;
        return nullptr;
    }

    // ---- what DXVK itself uses on this device ------------------------------
    //
    // A foreign pipeline is safe to create here only if nothing in it needs a
    // feature or extension this device did not enable. Rather than re-deriving
    // Vulkan's dependency rules, learn the answer from the one source that is
    // valid by construction: what DXVK actually created on this device. Anything
    // outside that is skipped.
    struct Uses
    {
        std::vector<uint32_t> sTypes, dynamicStates, spirvCaps;
        std::vector<std::string> spirvExts;
        uint64_t flags = 0;
        // A stage naming its shader only by module identifier: the identifier is
        // an opaque key into the RECORDING driver's cache, meaningless to any other.
        bool identifierOnly = false;
    };

    struct Learned
    {
        std::mutex m;
        std::unordered_set<uint32_t> sTypes, dynamicStates, spirvCaps;
        std::unordered_set<std::string> spirvExts;
        uint64_t flags = 0;
        std::atomic<uint32_t> pipelines{ 0 };
    };
    static inline Learned learned;

    static uint64_t PipelineFlags(const void* pNext, VkPipelineCreateFlags flags)
    {
        auto* f2 = static_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(
            FindPNext(pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR));
        return f2 ? f2->flags : flags;
    }

    // Capabilities and extensions come before OpMemoryModel in every module.
    static void ScanSpirv(const uint32_t* code, size_t bytes, Uses& u)
    {
        const size_t n = bytes / 4;
        if (!code || n < 5 || code[0] != 0x07230203u) return;
        for (size_t i = 5; i < n;)
        {
            const uint32_t op = code[i] & 0xFFFFu, wc = code[i] >> 16;
            if (wc == 0 || i + wc > n) break;
            if (op == 17 && wc >= 2) u.spirvCaps.push_back(code[i + 1]);                       // OpCapability
            else if (op == 10 && wc >= 2)                                                     // OpExtension
                u.spirvExts.emplace_back(reinterpret_cast<const char*>(&code[i + 1]),
                                         strnlen(reinterpret_cast<const char*>(&code[i + 1]), (wc - 1) * 4));
            else if (op == 14) break;                                                         // OpMemoryModel
            i += wc;
        }
    }

    static void CollectChain(const void* chain, Uses& u)
    {
        for (auto* s = static_cast<const VkBaseInStructure*>(chain); s; s = s->pNext)
        {
            u.sTypes.push_back((uint32_t)s->sType);
            // Inline SPIR-V (a module create info chained into a stage) is checked
            // like a module object.
            if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
            {
                auto* m = reinterpret_cast<const VkShaderModuleCreateInfo*>(s);
                ScanSpirv(m->pCode, m->codeSize, u);
            }
        }
    }

    static void CollectStages(const VkPipelineShaderStageCreateInfo* stages, uint32_t count, Uses& u)
    {
        for (uint32_t i = 0; stages && i < count; i++)
        {
            CollectChain(stages[i].pNext, u);
            if (stages[i].module == VK_NULL_HANDLE &&
                FindPNext(stages[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT) &&
                !FindPNext(stages[i].pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO))
                u.identifierOnly = true;
        }
    }

    static Uses CollectGraphics(const VkGraphicsPipelineCreateInfo& c)
    {
        Uses u;
        u.flags = PipelineFlags(c.pNext, c.flags);
        CollectChain(c.pNext, u);
        CollectStages(c.pStages, c.stageCount, u);
        const void* states[] = {
            c.pVertexInputState, c.pInputAssemblyState, c.pTessellationState, c.pViewportState,
            c.pRasterizationState, c.pMultisampleState, c.pDepthStencilState, c.pColorBlendState,
            c.pDynamicState };
        for (const void* s : states)
            if (s) CollectChain(static_cast<const VkBaseInStructure*>(s)->pNext, u);
        if (c.pDynamicState)
            for (uint32_t i = 0; i < c.pDynamicState->dynamicStateCount; i++)
                u.dynamicStates.push_back((uint32_t)c.pDynamicState->pDynamicStates[i]);
        return u;
    }

    static Uses CollectCompute(const VkComputePipelineCreateInfo& c)
    {
        Uses u;
        u.flags = PipelineFlags(c.pNext, c.flags);
        CollectChain(c.pNext, u);
        CollectStages(&c.stage, 1, u);
        return u;
    }

    static Uses CollectModule(const VkShaderModuleCreateInfo& m)
    {
        Uses u;
        CollectChain(m.pNext, u);
        ScanSpirv(m.pCode, m.codeSize, u);
        return u;
    }

    template <typename T>
    static Uses CollectPlain(const T& info)
    {
        Uses u;
        CollectChain(info.pNext, u);
        return u;
    }

    static void Learn(const Uses& u, bool isPipeline)
    {
        std::lock_guard lock(learned.m);
        learned.sTypes.insert(u.sTypes.begin(), u.sTypes.end());
        learned.dynamicStates.insert(u.dynamicStates.begin(), u.dynamicStates.end());
        learned.spirvCaps.insert(u.spirvCaps.begin(), u.spirvCaps.end());
        learned.spirvExts.insert(u.spirvExts.begin(), u.spirvExts.end());
        learned.flags |= u.flags;
        if (isPipeline) learned.pipelines++;
    }

    static bool Accepts(const Uses& u)
    {
        if (u.identifierOnly) return false;   // another driver's identifier: never valid here
        std::lock_guard lock(learned.m);
        if (u.flags & ~learned.flags) return false;
        for (auto s : u.sTypes)        if (!learned.sTypes.count(s)) return false;
        for (auto d : u.dynamicStates) if (!learned.dynamicStates.count(d)) return false;
        for (auto c : u.spirvCaps)     if (!learned.spirvCaps.count(c)) return false;
        for (auto& e : u.spirvExts)    if (!learned.spirvExts.count(e)) return false;
        return true;
    }

    // ---- watching DXVK compile, and the gameplay metrics ---------------------

    // Gameplay: the first loading screen has come and gone, none has been up for the
    // last kClearUs, and the loading-screen pass is done (or none is coming).
    // Everything else -- startup screens, loading, the pass and its hold -- is
    // loading, and the kClearUs after a loading screen are SETTLING. The wait
    // matters: GTA IV's loading goes through phases, and between two of them a
    // present can find no loading screen up (seen once, 15 s before the game was
    // playable). But what DXVK compiles while the world fades in after a load is
    // exactly the stutter this work is about, so creations there are held back
    // (Settle) until it is known which it was: gameplay, if no loading screen comes
    // back within kClearUs, else loading. Frames there are counted as neither. The
    // flags are only looked at here, on presents and creations.
    static inline std::atomic<bool> seenLoading{ false };
    static inline std::atomic<int64_t> clearSinceUs{ 0 };   // first seen with no loading screen up, 0 = one is up
    static constexpr int64_t kClearUs = 2000000;

    enum class Phase { Loading, Settling, Gameplay };

    static Phase PhaseAt(int64_t now)
    {
        auto& vr = pipelinekeys::VulkanReplay();
        if ((CMenuManager::bLoadscreenShown && *CMenuManager::bLoadscreenShown) || bLoadingShown)
        {
            seenLoading = true;
            clearSinceUs = 0;
            Settled(false);
            return Phase::Loading;
        }
        if (!seenLoading || vr.holding || (vr.passPlanned && !vr.passDone)) return Phase::Loading;
        int64_t since = clearSinceUs.load();
        if (!since && clearSinceUs.compare_exchange_strong(since, now)) since = now;
        if (now - since < kClearUs) return Phase::Settling;
        Settled(true);
        return Phase::Gameplay;
    }

    static bool InGameplay(int64_t now) { return PhaseAt(now) == Phase::Gameplay; }

    // One of DXVK's creations in flight (see VulkanReplayState). The finish is
    // stamped before the count drops, so whoever sees nothing in flight also sees
    // when the last one ended.
    struct InFlight
    {
        InFlight()
        {
            auto& vr = pipelinekeys::VulkanReplay();
            vr.inFlight++;
            vr.creations++;
            vr.lastActivityUs = pipelinekeys::ClockUs();
        }
        ~InFlight()
        {
            auto& vr = pipelinekeys::VulkanReplay();
            vr.lastActivityUs = pipelinekeys::ClockUs();
            vr.inFlight--;
        }
    };

    // CPU time used so far by DXVK's compiler threads -- the workers it names
    // "dxvk-shader-h", "-n" and "-l" (src/dxvk/dxvk_pipemanager.cpp) and its IR cache
    // writer "dxvk-cache" (src/dxvk/dxvk_shader_cache.cpp) -- and how many there are.
    // An idle one blocks on a condition variable and adds nothing; under Wine the
    // time of another thread comes from /proc, in 10 ms steps. csUs, if given, gets
    // the CS thread's ("dxvk-cs") -- not a compiler, but busy at every draw, so it
    // shows whether this can be measured at all. Only asked while the loading screen
    // waits for quiet, and once as gameplay starts: a snapshot of the process's
    // threads is not free.
    static uint64_t CompilerCpuUs(uint32_t* threads, uint64_t* csUs)
    {
        using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto getDescription = [] {
            auto p = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
            if (!p) p = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription");
            return reinterpret_cast<GetDescription>(p);
        }();
        uint64_t sum = 0, cs = 0;
        uint32_t n = 0;
        HANDLE snap = getDescription ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
        if (snap != INVALID_HANDLE_VALUE)
        {
            const DWORD pid = GetCurrentProcessId();
            THREADENTRY32 te{ sizeof(te) };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                PWSTR name = nullptr;
                FILETIME created, exited, kernel, user;
                if (SUCCEEDED(getDescription(h, &name)) && name)
                {
                    const bool compiler = wcsncmp(name, L"dxvk-shader-", 12) == 0 || wcscmp(name, L"dxvk-cache") == 0;
                    if ((compiler || wcscmp(name, L"dxvk-cs") == 0) && GetThreadTimes(h, &created, &exited, &kernel, &user))
                    {
                        const uint64_t us = ((((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                                             (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) / 10;
                        if (compiler) { sum += us; n++; }
                        else cs += us;
                    }
                }
                if (name) LocalFree(name);
                CloseHandle(h);
            }
            CloseHandle(snap);
        }
        if (threads) *threads = n;
        if (csUs) *csUs = cs;
        return sum;
    }

    // How long DXVK's own pipeline creations take, split by phase (PhaseAt). A
    // driver-cache hit costs well under a millisecond, a compile
    // several to tens. Whatever still compiles once gameplay has started is what the
    // warming missed: the number that says whether it worked, and what stutters.
    struct CreateTimes
    {
        std::atomic<uint32_t> n{ 0 }, libraries{ 0 }, linked{ 0 }, over1{ 0 }, over5{ 0 }, over20{ 0 }, slowLibraries{ 0 };
        std::atomic<uint64_t> worstUs{ 0 };

        void Add(uint64_t us, bool library, bool link)
        {
            n++;
            if (library) libraries++;
            if (link) linked++;
            if (us >= 1000) over1++;
            if (us >= 5000)
            {
                over5++;
                if (library) slowLibraries++;
                // Shared, so the warm pass can ask "did the driver actually
                // compile these, or did it already have them?" without
                // reaching into this module's own counters.
                pipelinekeys::VulkanReplay().compiles++;
            }
            if (us >= 20000) over20++;
            uint64_t w = worstUs.load();
            while (us > w && !worstUs.compare_exchange_weak(w, us)) {}
        }
        void Merge(const CreateTimes& o)
        {
            n += o.n.load(); libraries += o.libraries.load(); linked += o.linked.load(); over1 += o.over1.load();
            over5 += o.over5.load(); over20 += o.over20.load(); slowLibraries += o.slowLibraries.load();
            uint64_t w = worstUs.load();
            const uint64_t us = o.worstUs.load();
            while (us > w && !worstUs.compare_exchange_weak(w, us)) {}
        }
        void Reset() { n = 0; libraries = 0; linked = 0; over1 = 0; over5 = 0; over20 = 0; slowLibraries = 0; worstUs = 0; }
        std::string Describe() const
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "%u pipelines (%u libraries, %u linked); >=1 ms: %u, >=5 ms (compiled): %u "
                     "(%u libraries), >=20 ms: %u; worst %.1f ms", n.load(), libraries.load(), linked.load(), over1.load(),
                     over5.load(), slowLibraries.load(), over20.load(), worstUs.load() / 1000.0);
            return buf;
        }
    };
    // Before gameplay first starts, and on every loading screen after that.
    static inline CreateTimes loadingTimes, laterLoadingTimes, gameplayTimes;
    static inline std::atomic<uint32_t> slowLogged{ 0 };
    // Lines logged per kind of gameplay event (slow creation, long frame); the counts
    // in the 15 s reports go on past it.
    static constexpr uint32_t kDetailLines = 2000;

    static CreateTimes& LoadingTimes() { return frameTimes.startUs.load() ? laterLoadingTimes : loadingTimes; }

    // One "gameplay compile" line: every gameplay creation of 20 ms or more.
    static void LogSlow(int64_t t0, uint64_t us, const char* what, bool settling)
    {
        const uint32_t k = slowLogged++;
        if (k < kDetailLines)
            Log("gameplay compile t=%.3f: DXVK spent %.1f ms creating a %s%s", pipelinekeys::VulkanReplay().Seconds(t0),
                us / 1000.0, what, settling ? " (just after a loading screen)" : "");
        else if (k == kDetailLines)
            Log("gameplay compile: more than %u - no more of these lines, the reports still count them", kDetailLines);
    }

    // Creations while settling (see PhaseAt), until Settled says where they belong.
    struct Settle
    {
        std::mutex m;
        std::atomic<bool> any{ false };
        CreateTimes times;
        struct Slow { int64_t t0; uint64_t us; const char* what; };
        std::vector<Slow> slow;
    };
    static inline Settle settle;

    static void Settled(bool gameplay)
    {
        if (!settle.any.load()) return;
        std::lock_guard lock(settle.m);
        if (!settle.any.load()) return;
        (gameplay ? gameplayTimes : LoadingTimes()).Merge(settle.times);
        if (gameplay)
            for (const auto& s : settle.slow) LogSlow(s.t0, s.us, s.what, true);
        settle.times.Reset();
        settle.slow.clear();
        settle.any = false;
    }

    // t0 is when the creation started (ClockUs), which is also the t= of its line,
    // so it can be laid against the frame it landed in.
    static void TimeCreate(int64_t t0, uint64_t flags, const void* pNext)
    {
        const int64_t now = pipelinekeys::ClockUs();
        const uint64_t us = (uint64_t)(now - t0);
        const bool library = (flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;
        auto* libs = static_cast<const VkPipelineLibraryCreateInfoKHR*>(FindPNext(pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR));
        const bool link = libs && libs->libraryCount;
        const char* what = library ? "pipeline library" : link ? "linked pipeline" : "pipeline";
        const Phase phase = PhaseAt(now);
        if (phase == Phase::Settling)
        {
            std::lock_guard lock(settle.m);
            settle.times.Add(us, library, link);
            if (us >= 20000) settle.slow.push_back({ t0, us, what });
            settle.any = true;
            return;
        }
        (phase == Phase::Gameplay ? gameplayTimes : LoadingTimes()).Add(us, library, link);
        if (phase == Phase::Gameplay && us >= 20000) LogSlow(t0, us, what, false);
    }

    // Gameplay frame times: the gap between two vkQueuePresentKHR calls on DXVK's
    // device, both made in gameplay. DXVK presents from its submission thread once
    // its CS thread has worked through the frame, so a compile on the CS thread shows
    // up here as a long frame. Percentiles come from a histogram of 0.1 ms bins (the
    // last one takes everything from 1 s up), so reading them needs no lock: the
    // final report runs at process exit, when a thread holding one may be gone.
    struct FrameTimes
    {
        static constexpr uint32_t kBinUs = 100, kBins = 10000;
        std::atomic<uint32_t> bins[kBins]{};
        std::atomic<uint32_t> n{ 0 }, spikes{ 0 }, over50{ 0 };
        std::atomic<uint64_t> sumUs{ 0 }, maxUs{ 0 };
        std::atomic<int64_t> startUs{ 0 };     // gameplay's first present (ClockUs), 0 = none yet

        // The presenting thread's own state, under `m`: the previous present, and the
        // rolling window a spike is measured against.
        std::mutex m;
        int64_t lastUs = 0;
        bool lastGameplay = false;
        static constexpr uint32_t kWindow = 64, kWindowMin = 16;
        uint32_t window[kWindow] = {};
        uint32_t windowN = 0, windowAt = 0;
        uint32_t longLogged = 0;

        void Add(uint32_t us)
        {
            bins[(std::min)(us / kBinUs, kBins - 1)]++;   // before n, so a reader never finds fewer
            n++;
            sumUs += us;
            uint64_t w = maxUs.load();
            while (us > w && !maxUs.compare_exchange_weak(w, us)) {}
        }
        // The pct-th percentile: the upper edge of the bin holding the frame of that
        // rank (nearest-rank), or the longest frame if it is in the last bin.
        double PercentileMs(uint32_t pct) const
        {
            const uint64_t total = n.load();
            if (!total) return 0.0;
            const uint64_t rank = (std::max)(1ull, (total * pct + 99) / 100);
            uint64_t seen = 0;
            for (uint32_t i = 0; i + 1 < kBins; i++)
                if ((seen += bins[i].load(std::memory_order_relaxed)) >= rank)
                    return (i + 1) * kBinUs / 1000.0;
            return maxUs.load() / 1000.0;
        }
    };
    static inline FrameTimes frameTimes;

    // Once per present. A frame is the time from one gameplay present to the next.
    // A SPIKE is a frame longer than twice the median of the 64 gameplay frames
    // before it AND longer than that median + 8 ms (no verdict until 16 frames are
    // in; with an even count the median is the upper middle one): at 60 fps that is
    // anything over ~33.3 ms, at 144 fps over ~14.9 ms. Every frame over 50 ms is
    // logged with its t= (the present that ended it), whether or not it is a spike.
    static void CountFrame()
    {
        const int64_t now = pipelinekeys::ClockUs();
        const bool gameplay = InGameplay(now);
        auto& f = frameTimes;
        std::lock_guard lock(f.m);
        const bool both = gameplay && f.lastGameplay && f.lastUs;
        const uint32_t us = both ? (uint32_t)(std::min)(now - f.lastUs, (int64_t)UINT32_MAX) : 0u;
        f.lastUs = now;
        f.lastGameplay = gameplay;
        if (gameplay && !f.startUs)
        {
            f.startUs = now;
            Log("metrics: gameplay starts at t=%.3f", pipelinekeys::VulkanReplay().Seconds(now));
        }
        if (!both) return;

        uint32_t median = 0;
        if (f.windowN >= f.kWindowMin)
        {
            uint32_t sorted[FrameTimes::kWindow];
            std::copy_n(f.window, f.windowN, sorted);
            std::nth_element(sorted, sorted + f.windowN / 2, sorted + f.windowN);
            median = sorted[f.windowN / 2];
        }
        const bool spike = f.windowN >= f.kWindowMin && us > 2 * median && us > median + 8000;
        f.window[f.windowAt] = us;
        f.windowAt = (f.windowAt + 1) % f.kWindow;
        if (f.windowN < f.kWindow) f.windowN++;

        f.Add(us);
        if (spike) f.spikes++;
        if (us > 50000)
        {
            f.over50++;
            const uint32_t k = f.longLogged++;
            if (k < kDetailLines)
                Log("gameplay long frame t=%.3f: %.1f ms, median %.2f ms, spike %s",
                    pipelinekeys::VulkanReplay().Seconds(now), us / 1000.0, median / 1000.0, spike ? "yes" : "no");
            else if (k == kDetailLines)
                Log("gameplay long frame: more than %u - no more of these lines, the reports still count them", kDetailLines);
        }
    }

    // The gameplay report, every 15 s of gameplay and once at exit ("final"):
    //   gameplay frames[ final] t=<s> play=<s>s: frames <n>, avg <fps> fps, p50 <ms> ms,
    //     p95 <ms> ms, p99 <ms> ms, max <ms> ms, spikes <n>, >50 ms <n>
    //   created by DXVK in gameplay so far: <CreateTimes::Describe>   ("in gameplay:" at exit)
    //   created by DXVK on later loading screens so far: <CreateTimes::Describe>
    // t is the session clock (seconds since the ASI loaded), play the time since
    // gameplay's first present. Every figure covers the whole of gameplay so far, not
    // just the last 15 s: frames and fps (frames / their summed time), percentiles
    // (nearest rank, the upper edge of a 0.1 ms bin), the longest frame, spikes and
    // frames over 50 ms as defined at CountFrame. Frames under a later loading screen,
    // or in the kClearUs after one, are not gameplay and not counted; creations in
    // those kClearUs count as gameplay unless another loading screen follows (see
    // PhaseAt), and the ones on a loading screen after gameplay first started go to
    // the second line.
    static void ReportGameplay(bool final)
    {
        auto& f = frameTimes;
        const int64_t now = pipelinekeys::ClockUs(), start = f.startUs.load();
        const uint32_t n = f.n.load();
        const uint64_t sum = f.sumUs.load();
        Log("gameplay frames%s t=%.3f play=%.1fs: frames %u, avg %.1f fps, p50 %.2f ms, p95 %.2f ms, p99 %.2f ms, "
            "max %.1f ms, spikes %u, >50 ms %u", final ? " final" : "", pipelinekeys::VulkanReplay().Seconds(now),
            start ? (now - start) / 1e6 : 0.0, n, sum ? n * 1e6 / (double)sum : 0.0, f.PercentileMs(50),
            f.PercentileMs(95), f.PercentileMs(99), f.maxUs.load() / 1000.0, f.spikes.load(), f.over50.load());
        Log(final ? "created by DXVK in gameplay: %s" : "created by DXVK in gameplay so far: %s",
            gameplayTimes.Describe().c_str());
        Log(final ? "created by DXVK on later loading screens: %s" : "created by DXVK on later loading screens so far: %s",
            laterLoadingTimes.Describe().c_str());
        // The 32-bit address space, at the same cadence. A warm pass leaves live
        // DXVK pipelines behind it and the game keeps streaming into what is
        // left, so the number that matters is not what the pass cost at the gate
        // but what gameplay still has -- and whether it is still falling.
        Log("memory in gameplay: %s", pipelinekeys::VaLine().c_str());
    }

    // Runs as long as the device: the 15 s gameplay reports. The final one comes from
    // the shutdown event (ReportFinal), since DXVK rarely destroys its device.
    static inline std::atomic<bool> loadingReported{ false };

    // cpu: also what DXVK's threads have used by then (CompilerCpuUs); not at exit,
    // when they are gone.
    static void ReportLoading(bool cpu)
    {
        if (loadingReported.exchange(true)) return;
        Log("created by DXVK before gameplay: %s", loadingTimes.Describe().c_str());
        if (!cpu) return;
        uint32_t threads = 0;
        uint64_t cs = 0;
        const uint64_t us = CompilerCpuUs(&threads, &cs);
        Log("CPU used by DXVK before gameplay: %.2fs by its %u compiler threads, %.2fs by its CS thread", us / 1e6,
            threads, cs / 1e6);
    }

    static void ReportMetrics(Device* d)
    {
        constexpr int64_t kEveryUs = 15000000;
        int64_t next = 0;
        while (!d->stop)
        {
            Sleep(250);
            const int64_t start = frameTimes.startUs.load();
            if (!start) continue;
            if (!next)
            {
                next = start + kEveryUs;
                ReportLoading(true);
            }
            const int64_t now = pipelinekeys::ClockUs();
            if (now < next) continue;
            while (next <= now) next += kEveryUs;
            ReportGameplay(false);
        }
    }

    // At exit, from the shutdown event, if DXVK's device was ever watched.
    static inline std::atomic<bool> metricsOn{ false };

    static void ReportFinal()
    {
        if (!metricsOn) return;
        ReportLoading(false);
        ReportGameplay(true);
    }

    // ---- device-level wrappers (Fossilize layer/dispatch.cpp, normal mode) ----

    static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* out)
    {
        Device* d = Get(device);
        const int64_t t0 = pipelinekeys::ClockUs();
        VkResult res;
        {
            InFlight busy;
            res = d->CreateGraphicsPipelines(device, cache, count, infos, alloc, out);
        }
        if (count) TimeCreate(t0, PipelineFlags(infos[0].pNext, infos[0].flags), infos[0].pNext);
        // Recorded only on success: a VK_PIPELINE_COMPILE_REQUIRED probe creates
        // nothing, and DXVK records the pipeline when it then compiles it for real.
        // Nor while the D3D9 pass warms other PCs' keys (VulkanReplayState::recordPaused).
        if (res != VK_SUCCESS) return res;
        const bool paused = pipelinekeys::VulkanReplay().recordPaused;
        if (paused && d->recorder) pipelinekeys::VulkanReplay().notRecorded += count;
        for (uint32_t i = 0; i < count; i++)
        {
            if (replayEnabled) Learn(CollectGraphics(infos[i]), true);
            if (!d->recorder || paused) continue;
            if (d->recorder->record_graphics_pipeline(out[i], infos[i], out, count, 0, device,
                    d->usesIdentifiers ? d->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
                d->graphics++;
            else
                d->failed++;
        }
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* out)
    {
        Device* d = Get(device);
        const int64_t t0 = pipelinekeys::ClockUs();
        VkResult res;
        {
            InFlight busy;
            res = d->CreateComputePipelines(device, cache, count, infos, alloc, out);
        }
        if (count) TimeCreate(t0, PipelineFlags(infos[0].pNext, infos[0].flags), infos[0].pNext);
        if (res != VK_SUCCESS) return res;
        const bool paused = pipelinekeys::VulkanReplay().recordPaused;
        if (paused && d->recorder) pipelinekeys::VulkanReplay().notRecorded += count;
        for (uint32_t i = 0; i < count; i++)
        {
            if (replayEnabled) Learn(CollectCompute(infos[i]), true);
            if (!d->recorder || paused) continue;
            if (d->recorder->record_compute_pipeline(out[i], infos[i], out, count, 0, device,
                    d->usesIdentifiers ? d->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
                d->compute++;
            else
                d->failed++;
        }
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkShaderModule* out)
    {
        Device* d = Get(device);
        *out = VK_NULL_HANDLE;
        VkResult res;
        {
            InFlight busy;
            res = d->CreateShaderModule(device, info, alloc, out);
        }
        if (res != VK_SUCCESS) return res;
        if (replayEnabled) Learn(CollectModule(*info), false);
        if (!d->recorder) return res;

        // Carry the module identifier along, as the Fossilize layer does, so
        // pipelines created from identifiers alone can still be resolved later.
        VkPipelineShaderStageModuleIdentifierCreateInfoEXT idInfo =
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT };
        VkShaderModuleIdentifierEXT id = { VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
        VkShaderModuleCreateInfo tmp;
        if (d->usesIdentifiers && d->GetShaderModuleIdentifierEXT)
        {
            d->GetShaderModuleIdentifierEXT(device, *out, &id);
            idInfo.pIdentifier = id.identifier;
            idInfo.identifierSize = id.identifierSize;
            tmp = *info;
            idInfo.pNext = tmp.pNext;
            tmp.pNext = &idInfo;
            info = &tmp;
        }
        if (d->recorder->record_shader_module(*out, *info)) d->modules++;
        else d->failed++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkPipelineLayout* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreatePipelineLayout(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_pipeline_layout(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateDescriptorSetLayout(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        // A host-only layout can never appear in a pipeline layout.
        if (res == VK_SUCCESS && d->recorder && !(info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT))
            (d->recorder->record_descriptor_set_layout(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkSampler* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateSampler(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        // A custom border color index cannot affect compilation (same exception as
        // the Fossilize layer, which also avoids its "unsupported pNext" noise).
        if (res == VK_SUCCESS && d->recorder &&
            !FindPNext(info->pNext, VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT))
            (d->recorder->record_sampler(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkRenderPass* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateRenderPass(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_render_pass(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* alloc, VkRenderPass* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateRenderPass2(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_render_pass2(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* alloc, VkRenderPass* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateRenderPass2KHR(device, info, alloc, out);
        if (res == VK_SUCCESS && replayEnabled) Learn(CollectPlain(*info), false);
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_render_pass2(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
    {
        CountFrame();
        return realQueuePresent(queue, info);
    }

    // Flush and stop the recorder before the device goes; then forget the device.
    //
    // DXVK usually does NOT destroy its device when GTA IV quits -- the process just
    // exits -- so this rarely runs. That is fine: the recording thread writes each
    // entry as it comes, the C runtime flushes the stream at exit, and the archive
    // format tolerates a truncated last entry. Flushing from DLL detach instead
    // would risk a deadlock on a FILE lock held by the already-terminated writer.
    static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc)
    {
        std::unique_ptr<Device> d;
        {
            std::unique_lock lock(devLock);
            auto it = devices.find(device);
            if (it != devices.end()) { d = std::move(it->second); devices.erase(it); }
        }
        if (!d) return;
        // Only when the LAST one goes. The mod's own warm device is destroyed in
        // the middle of the loading screen, and clearing this there would leave
        // every later "wait until DXVK is quiet" with nothing to watch.
        {
            std::shared_lock lock(devLock);
            if (devices.empty()) pipelinekeys::VulkanReplay().watching = false;
        }
        // The replay creates objects on this device: it must be finished first.
        d->stop = true;
        if (d->replay.joinable()) d->replay.join();
        if (d->metrics.joinable()) d->metrics.join();
        if (d->recorder)
        {
            d->recorder->tear_down_recording_thread();
            if (d->db) d->db->flush();
            Log("device destroyed: recorded %u graphics + %u compute pipelines, %u shader modules, %u other objects "
                "(%u failed) to %s", d->graphics.load(), d->compute.load(), d->modules.load(), d->other.load(),
                d->failed.load(), d->path.c_str());
        }
        d->DestroyDevice(device, alloc);
    }

    // ---- replay ---------------------------------------------------------------

    // Which recorders (application + feature hash) produced each blob, from the
    // database's link entries. Creates nothing.
    struct LinkReader : Fossilize::StateCreatorInterface
    {
        std::unordered_map<Fossilize::Hash, std::vector<Fossilize::Hash>> appsOf;
        void notify_application_info_link(Fossilize::Hash, Fossilize::Hash app, Fossilize::ResourceTag,
                                          Fossilize::Hash blob) override { appsOf[blob].push_back(app); }
        bool enqueue_create_sampler(Fossilize::Hash, const VkSamplerCreateInfo*, VkSampler*) override { return false; }
        bool enqueue_create_descriptor_set_layout(Fossilize::Hash, const VkDescriptorSetLayoutCreateInfo*, VkDescriptorSetLayout*) override { return false; }
        bool enqueue_create_pipeline_layout(Fossilize::Hash, const VkPipelineLayoutCreateInfo*, VkPipelineLayout*) override { return false; }
        bool enqueue_create_shader_module(Fossilize::Hash, const VkShaderModuleCreateInfo*, VkShaderModule*) override { return false; }
        bool enqueue_create_render_pass(Fossilize::Hash, const VkRenderPassCreateInfo*, VkRenderPass*) override { return false; }
        bool enqueue_create_render_pass2(Fossilize::Hash, const VkRenderPassCreateInfo2*, VkRenderPass*) override { return false; }
        bool enqueue_create_compute_pipeline(Fossilize::Hash, const VkComputePipelineCreateInfo*, VkPipeline*) override { return false; }
        bool enqueue_create_graphics_pipeline(Fossilize::Hash, const VkGraphicsPipelineCreateInfo*, VkPipeline*) override { return false; }
        bool enqueue_create_raytracing_pipeline(Fossilize::Hash, const VkRayTracingPipelineCreateInfoKHR*, VkPipeline*) override { return false; }
    };

    // Why a replayed entry was not created: the first check that failed while the
    // replayer resolved it and its dependencies (see the top of this file).
    enum class Skip { None, Irrelevant, Unsupported, Incomplete, Failed };

    // Creates what the replayer parses on DXVK's device, through the real entry
    // points, once it passes the checks at the top of this file. Trusted entries skip
    // the relevance check and teach it instead. Pipelines are destroyed at once
    // (libraries only when the batch is released, since links reference them).
    struct ReplayCreator : Fossilize::StateCreatorInterface
    {
        VkDevice device;
        Device* d;
        bool trusted = false;
        // The entry being replayed, and how it went.
        Fossilize::Hash entry = 0;
        bool entryCreated = false;
        Skip entrySkip = Skip::None;
        uint32_t pipelines = 0;     // every pipeline created, libraries it needed included
        uint32_t created = 0, createdTrusted = 0, irrelevant = 0, unsupported = 0, incomplete = 0, failed = 0, earlier = 0;
        std::vector<VkSampler> samplers;
        std::vector<VkDescriptorSetLayout> setLayouts;
        std::vector<VkPipelineLayout> layouts;
        std::vector<VkShaderModule> modules;
        std::vector<VkRenderPass> passes;
        std::vector<VkPipeline> libraries;

        ReplayCreator(VkDevice dev, Device* dd) : device(dev), d(dd) {}
        ~ReplayCreator() { Release(); }

        // Everything kept alive for later pipelines to reference. Released every few
        // dozen pipelines (and the replayer told to forget the handles, so it simply
        // re-creates a dependency the next time one is needed): a Steam bucket holds
        // ~11k shader modules, and keeping them all alive at once exhausted GTA IV's
        // 32-bit address space mid-load.
        void Release()
        {
            for (auto p : libraries)  d->DestroyPipeline(device, p, nullptr);
            for (auto m : modules)    { d->filter.unregister_shader_module_info(m); d->DestroyShaderModule(device, m, nullptr); }
            for (auto l : layouts)    d->DestroyPipelineLayout(device, l, nullptr);
            for (auto s : setLayouts) d->DestroyDescriptorSetLayout(device, s, nullptr);
            for (auto s : samplers)   d->DestroySampler(device, s, nullptr);
            for (auto r : passes)     d->DestroyRenderPass(device, r, nullptr);
            libraries.clear(); modules.clear(); layouts.clear();
            setLayouts.clear(); samplers.clear(); passes.clear();
        }

        bool Reject(Skip why)
        {
            if (entrySkip == Skip::None) entrySkip = why;
            return false;
        }

        // Settle the entry just parsed. `parsed` is the replayer's own verdict: false
        // when it could not resolve something itself (a pipeline layout it never saw,
        // a module missing from the archive).
        void Settle(bool parsed)
        {
            if (entryCreated) { created++; if (trusted) createdTrusted++; return; }
            switch (entrySkip)
            {
            case Skip::Irrelevant:  irrelevant++;  break;
            case Skip::Unsupported: unsupported++; break;
            case Skip::Failed:      failed++;      break;
            case Skip::Incomplete:  incomplete++;  break;
            // Nothing rejected: already resolved in this batch (as a library another
            // entry linked), or the replayer itself found a dependency missing.
            case Skip::None:        (parsed ? earlier : incomplete)++; break;
            }
            if (replayTrace && !trusted && entrySkip != Skip::None)
                Log("trace:   not created: %s", entrySkip == Skip::Irrelevant ? "not relevant" :
                    entrySkip == Skip::Unsupported ? "not supported" : entrySkip == Skip::Failed ? "failed" : "incomplete");
        }

        // RELEVANT, then SUPPORTED: `supported` is the feature filter's verdict, only
        // asked once the object is relevant (it parses whole SPIR-V modules).
        template <typename Supported>
        bool Allow(const Uses& u, bool isPipeline, Supported supported)
        {
            if (!trusted && !Accepts(u)) return Reject(Skip::Irrelevant);
            if (d->haveFilter && !supported()) return Reject(Skip::Unsupported);
            if (trusted) Learn(u, isPipeline);
            return true;
        }

        template <typename Info, typename Handle, typename Fn, typename Supported>
        bool Make(const Info* ci, Handle* out, Fn create, std::vector<Handle>& keep, const Uses& u, Supported supported)
        {
            *out = VK_NULL_HANDLE;
            if (!Allow(u, false, supported)) return false;
            if (create(device, ci, nullptr, out) != VK_SUCCESS) { *out = VK_NULL_HANDLE; return Reject(Skip::Failed); }
            keep.push_back(*out);
            return true;
        }

        bool enqueue_create_sampler(Fossilize::Hash, const VkSamplerCreateInfo* ci, VkSampler* out) override
        { return Make(ci, out, d->CreateSampler, samplers, CollectPlain(*ci), [&] { return d->filter.sampler_is_supported(ci); }); }
        bool enqueue_create_descriptor_set_layout(Fossilize::Hash, const VkDescriptorSetLayoutCreateInfo* ci, VkDescriptorSetLayout* out) override
        {
            return Make(ci, out, d->CreateDescriptorSetLayout, setLayouts, CollectPlain(*ci),
                        [&] { return d->filter.descriptor_set_layout_is_supported(ci); });
        }
        bool enqueue_create_pipeline_layout(Fossilize::Hash, const VkPipelineLayoutCreateInfo* ci, VkPipelineLayout* out) override
        { return Make(ci, out, d->CreatePipelineLayout, layouts, CollectPlain(*ci), [&] { return d->filter.pipeline_layout_is_supported(ci); }); }
        bool enqueue_create_shader_module(Fossilize::Hash, const VkShaderModuleCreateInfo* ci, VkShaderModule* out) override
        {
            if (!Make(ci, out, d->CreateShaderModule, modules, CollectModule(*ci), [&] { return d->filter.shader_module_is_supported(ci); }))
                return false;
            // What the filter checks pipelines against (entry points, workgroup sizes).
            d->filter.register_shader_module_info(*out, ci);
            return true;
        }
        bool enqueue_create_render_pass(Fossilize::Hash, const VkRenderPassCreateInfo* ci, VkRenderPass* out) override
        {
            return d->CreateRenderPass &&
                   Make(ci, out, d->CreateRenderPass, passes, CollectPlain(*ci), [&] { return d->filter.render_pass_is_supported(ci); });
        }
        bool enqueue_create_render_pass2(Fossilize::Hash, const VkRenderPassCreateInfo2* ci, VkRenderPass* out) override
        {
            auto fn = d->CreateRenderPass2 ? d->CreateRenderPass2 : d->CreateRenderPass2KHR;
            return fn && Make(ci, out, fn, passes, CollectPlain(*ci), [&] { return d->filter.render_pass2_is_supported(ci); });
        }

        // COMPLETE: every shader module and library the replayer resolved exists, as
        // fossilize-replay checks in enqueue_pipeline() before it creates anything.
        // The replayer's handle maps keep a failed object as VK_NULL_HANDLE
        // (parse_shader_modules inserts the hash before it asks us to create it), so a
        // module rejected for one pipeline comes back as a null handle to the next one
        // in the same batch -- and RADV dereferences the missing vertex shader. A stage
        // may carry its SPIR-V inline instead; a module identifier alone never counts
        // (it names another driver's cache).
        static bool Complete(const VkPipelineShaderStageCreateInfo& s)
        {
            return s.module != VK_NULL_HANDLE || FindPNext(s.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        }
        static bool Complete(const VkGraphicsPipelineCreateInfo& c)
        {
            for (uint32_t i = 0; i < c.stageCount; i++)
                if (!Complete(c.pStages[i])) return false;
            if (auto* l = static_cast<const VkPipelineLibraryCreateInfoKHR*>(FindPNext(c.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)))
                for (uint32_t i = 0; i < l->libraryCount; i++)
                    if (l->pLibraries[i] == VK_NULL_HANDLE) return false;
            // Base pipelines are destroyed as soon as they exist.
            return !(PipelineFlags(c.pNext, c.flags) & VK_PIPELINE_CREATE_DERIVATIVE_BIT) || c.basePipelineHandle;
        }
        static bool Complete(const VkComputePipelineCreateInfo& c)
        {
            return Complete(c.stage) && (!(PipelineFlags(c.pNext, c.flags) & VK_PIPELINE_CREATE_DERIVATIVE_BIT) || c.basePipelineHandle);
        }

        static void Describe(const VkGraphicsPipelineCreateInfo& c, char* buf, size_t n)
        {
            uint32_t nullStages = 0, libs = 0, nullLibs = 0, depth = 0, stencil = 0, color0 = 0;
            for (uint32_t i = 0; i < c.stageCount; i++) if (c.pStages[i].module == VK_NULL_HANDLE) nullStages++;
            if (auto* l = static_cast<const VkPipelineLibraryCreateInfoKHR*>(FindPNext(c.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)))
                for (uint32_t i = 0; i < l->libraryCount; i++) { libs++; if (!l->pLibraries[i]) nullLibs++; }
            if (auto* r = static_cast<const VkPipelineRenderingCreateInfo*>(FindPNext(c.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)))
            { depth = r->depthAttachmentFormat; stencil = r->stencilAttachmentFormat; color0 = r->colorAttachmentCount ? r->pColorAttachmentFormats[0] : 0; }
            snprintf(buf, n, "flags %llx, %u stages (%u null module), %u libraries (%u null), layout %s, render pass %s, "
                     "formats color0 %u depth %u stencil %u", (unsigned long long)PipelineFlags(c.pNext, c.flags), c.stageCount,
                     nullStages, libs, nullLibs, c.layout ? "set" : "none", c.renderPass ? "set" : "none", color0, depth, stencil);
        }
        static void Describe(const VkComputePipelineCreateInfo& c, char* buf, size_t n)
        {
            snprintf(buf, n, "compute, flags %llx, module %s", (unsigned long long)PipelineFlags(c.pNext, c.flags),
                     c.stage.module ? "set" : "NULL");
        }

        template <typename Info, typename Fn, typename Supported>
        bool MakePipeline(Fossilize::Hash h, const Info* ci, VkPipeline* out, Fn create, const Uses& u, Supported supported)
        {
            *out = VK_NULL_HANDLE;
            if (!Complete(*ci)) return Reject(Skip::Incomplete);
            if (!Allow(u, true, supported)) return false;
            char what[256];
            if (replayTrace && !trusted) { Describe(*ci, what, sizeof(what)); Log("trace:   create %s", what); }
            const VkResult r = create(device, VK_NULL_HANDLE, 1, ci, nullptr, out);
            if (replayTrace && !trusted) Log("trace:   -> %d", (int)r);
            if (r != VK_SUCCESS) { *out = VK_NULL_HANDLE; return Reject(Skip::Failed); }
            pipelines++;
            if (h == entry) entryCreated = true;
            if (u.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) libraries.push_back(*out);
            else { d->DestroyPipeline(device, *out, nullptr); *out = VK_NULL_HANDLE; }
            return true;
        }

        bool enqueue_create_graphics_pipeline(Fossilize::Hash h, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* out) override
        {
            return MakePipeline(h, ci, out, d->CreateGraphicsPipelines, CollectGraphics(*ci),
                                [&] { return d->filter.graphics_pipeline_is_supported(ci); });
        }
        bool enqueue_create_compute_pipeline(Fossilize::Hash h, const VkComputePipelineCreateInfo* ci, VkPipeline* out) override
        {
            return MakePipeline(h, ci, out, d->CreateComputePipelines, CollectCompute(*ci),
                                [&] { return d->filter.compute_pipeline_is_supported(ci); });
        }
        bool enqueue_create_raytracing_pipeline(Fossilize::Hash, const VkRayTracingPipelineCreateInfoKHR*, VkPipeline*) override
        { return false; }
    };

    // Entries by outcome (`pipelines` counts every pipeline created, libraries included).
    struct Totals
    {
        uint32_t files = 0, pipelines = 0, created = 0, trusted = 0, irrelevant = 0, unsupported = 0, incomplete = 0,
                 failed = 0, already = 0, stale = 0, refused = 0;
        bool lowVA = false;

        void Add(const Totals& o)
        {
            files += o.files; pipelines += o.pipelines; created += o.created; trusted += o.trusted;
            irrelevant += o.irrelevant; unsupported += o.unsupported; incomplete += o.incomplete; failed += o.failed;
            already += o.already; stale += o.stale; refused += o.refused; lowVA |= o.lowVA;
        }
        // What was created and why the rest was not, for the log. `refused`: reads
        // CheckedDatabase turned down, the entries depending on them are incomplete.
        std::string Outcome() const
        {
            char buf[448];
            snprintf(buf, sizeof(buf), "%u entries created (%u trusted, %u pipelines in all); skipped: %u not relevant "
                     "(another DXVK build or configuration), %u not supported by this device, %u incomplete (a dependency "
                     "missing or skipped), %u failed; %u already done", created, trusted, pipelines, irrelevant, unsupported,
                     incomplete, failed, already);
            std::string s = buf;
            if (refused) s += "; " + std::to_string(refused) + " damaged entries not read";
            return s;
        }
        std::string Stale() const
        {
            return stale ? "; " + std::to_string(stale) + " from another DXVK build/feature set not replayed (foreign replay off)"
                         : std::string();
        }
    };

    // Free virtual address space. GTA IV is a 32-bit process that needs most of its
    // 4 GB itself; the replay must never be what runs it out.
    static uint64_t AvailableVA()
    {
        MEMORYSTATUSEX ms{ sizeof(ms) };
        return GlobalMemoryStatusEx(&ms) ? ms.ullAvailVirtual : 0;
    }
    static constexpr uint64_t kMinVA = 768ull << 20;

    // Pipeline entries in a database, for the loading screen's bar.
    static uint32_t CountPipelines(const std::string& path)
    {
        try
        {
            std::unique_ptr<Fossilize::DatabaseInterface> db(
                Fossilize::create_stream_archive_database(path.c_str(), Fossilize::DatabaseMode::ReadOnly));
            if (!db || !db->prepare()) return 0;
            size_t sum = 0;
            for (auto tag : { Fossilize::RESOURCE_GRAPHICS_PIPELINE, Fossilize::RESOURCE_COMPUTE_PIPELINE })
            {
                size_t n = 0;
                if (db->get_hash_list_for_resource_tag(tag, &n, nullptr)) sum += n;
            }
            return (uint32_t)sum;
        }
        catch (const std::exception&) { return 0; }   // ReplayFile then says it is damaged
    }

    // Every read the replay makes from a database goes through this: its own, and the
    // replayer's, which resolves what a pipeline needs through the database it is
    // given. Fossilize trusts what it reads. An entry header's size is not covered by
    // the checksum and sizes a buffer as it is, so one damaged byte there ended the
    // process (std::length_error, never caught); a shader module's codeSize sizes the
    // buffer its SPIR-V is decoded into, and a crafted one wraps that size and writes
    // past it. So an entry larger than kMaxEntryBytes is not there, and neither is a
    // shader module whose sizes do not fit the entry (ModuleOk). What else a crafted
    // entry's JSON may do inside Fossilize's parser is not checked here.
    static constexpr size_t kMaxEntryBytes = 16u << 20;

    // `{"version":N,"shaderModules":{"<hash>":{"flags":F,"codeSize":S,
    // "varintOffset":O,"varintSize":V}}}`, a NUL, then the varint-coded SPIR-V: only
    // that form (what Fossilize has written for years, and what the recorder here
    // writes), with every number inside the entry. The older base64 form is decoded
    // without checking the string against codeSize, so it is not accepted.
    static bool ModuleOk(const uint8_t* p, size_t n)
    {
        const auto* nul = static_cast<const uint8_t*>(memchr(p, 0, n));
        if (!nul) return false;
        const size_t json = (size_t)(nul - p), varint = n - json - 1;
        rapidjson::Document doc;
        doc.Parse(reinterpret_cast<const char*>(p), json);
        if (doc.HasParseError() || !doc.IsObject()) return false;
        const auto mods = doc.FindMember("shaderModules");
        if (mods == doc.MemberEnd()) return true;
        if (!mods->value.IsObject()) return false;
        for (auto m = mods->value.MemberBegin(); m != mods->value.MemberEnd(); ++m)
        {
            const auto& o = m->value;
            if (!o.IsObject()) return false;
            const auto code = o.FindMember("codeSize"), at = o.FindMember("varintOffset"), size = o.FindMember("varintSize");
            if (code == o.MemberEnd() || at == o.MemberEnd() || size == o.MemberEnd() ||
                !code->value.IsUint64() || !at->value.IsUint64() || !size->value.IsUint64())
                return false;
            const uint64_t c = code->value.GetUint64(), a = at->value.GetUint64(), s = size->value.GetUint64();
            if (!c || (c & 3) || c > kMaxEntryBytes || a > varint || s > varint - a) return false;
        }
        return true;
    }

    struct CheckedDatabase : Fossilize::DatabaseInterface
    {
        Fossilize::DatabaseInterface& db;
        std::atomic<uint32_t> refused{ 0 };
        explicit CheckedDatabase(Fossilize::DatabaseInterface& inner)
            : Fossilize::DatabaseInterface(Fossilize::DatabaseMode::ReadOnly), db(inner) {}

        bool prepare() override { return true; }   // the inner one is prepared already
        bool read_entry(Fossilize::ResourceTag tag, Fossilize::Hash hash, size_t* size, void* buffer,
                        Fossilize::PayloadReadFlags flags) override
        {
            size_t n = 0;
            if (!size || !db.read_entry(tag, hash, &n, nullptr, flags)) return false;
            if (n > kMaxEntryBytes) { refused++; return false; }
            if (!buffer) { *size = n; return true; }
            if (!db.read_entry(tag, hash, size, buffer, flags)) return false;
            if (tag == Fossilize::RESOURCE_SHADER_MODULE && !ModuleOk(static_cast<const uint8_t*>(buffer), *size))
            {
                refused++;
                return false;
            }
            return true;
        }
        bool write_entry(Fossilize::ResourceTag, Fossilize::Hash, const void*, size_t, Fossilize::PayloadWriteFlags) override
        { return false; }
        bool has_entry(Fossilize::ResourceTag tag, Fossilize::Hash hash) override { return db.has_entry(tag, hash); }
        bool get_hash_list_for_resource_tag(Fossilize::ResourceTag tag, size_t* count, Fossilize::Hash* hashes) override
        { return db.get_hash_list_for_resource_tag(tag, count, hashes); }
        void flush() override {}
        const char* get_db_path_for_hash(Fossilize::ResourceTag tag, Fossilize::Hash hash) override
        { return db.get_db_path_for_hash(tag, hash); }
    };

    // `part` of `parts`: parallel workers each take every parts-th pipeline entry of
    // the same file, through their own database handle and replayer. `prior` holds
    // what earlier files already replayed (read only while workers run); `seen`
    // collects this worker's.
    static void ReplayFile(const std::string& path, VkDevice dev, Device* d, bool own,
                           const std::unordered_set<Fossilize::Hash>& prior, std::unordered_set<Fossilize::Hash>& seen,
                           Totals& t, uint32_t part, uint32_t parts, const std::atomic<bool>& damaged)
    {
        auto& vr = pipelinekeys::VulkanReplay();
        std::unique_ptr<Fossilize::DatabaseInterface> raw(
            Fossilize::create_stream_archive_database(path.c_str(), Fossilize::DatabaseMode::ReadOnly));
        if (!raw || !raw->prepare()) { if (part == 0) Log("replay: cannot read %s", path.c_str()); return; }
        CheckedDatabase checked(*raw);
        Fossilize::DatabaseInterface* db = &checked;
        currentFile = &path;

        auto readAll = [&](Fossilize::ResourceTag tag, auto&& fn) {
            size_t count = 0;
            if (!db->get_hash_list_for_resource_tag(tag, &count, nullptr) || !count) return;
            std::vector<Fossilize::Hash> hashes(count);
            if (!db->get_hash_list_for_resource_tag(tag, &count, hashes.data())) return;
            std::vector<uint8_t> buf;
            for (auto h : hashes)
            {
                if (d->stop || damaged) return;
                size_t size = 0;
                if (!db->read_entry(tag, h, &size, nullptr, Fossilize::PAYLOAD_READ_NO_FLAGS) || !size) continue;
                buf.resize(size);
                if (!db->read_entry(tag, h, &size, buf.data(), Fossilize::PAYLOAD_READ_NO_FLAGS)) continue;
                fn(h, buf.data(), size);
            }
        };

        LinkReader links;
        {
            Fossilize::StateReplayer lr;
            readAll(Fossilize::RESOURCE_APPLICATION_BLOB_LINK,
                    [&](Fossilize::Hash, const uint8_t* p, size_t n) { (void)lr.parse(links, db, p, n); });
        }

        ReplayCreator creator(dev, d);
        Fossilize::StateReplayer replayer;
        uint32_t already = 0, parsed = 0, stale = 0, index = 0;
        for (auto tag : { Fossilize::RESOURCE_GRAPHICS_PIPELINE, Fossilize::RESOURCE_COMPUTE_PIPELINE })
        {
            readAll(tag, [&](Fossilize::Hash h, const uint8_t* p, size_t n) {
                if (t.lowVA) return;
                if ((index++ % parts) != part) return;
                vr.done++;
                if (prior.count(h) || !seen.insert(h).second) { already++; return; }
                if (++parsed % 64 == 0)
                {
                    creator.Release();
                    replayer.forget_handle_references();
                    // Full speed only while the loading screen waits for us.
                    SetThreadPriority(GetCurrentThread(), vr.holding ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
                    const uint64_t va = AvailableVA();
                    if (va < kMinVA)
                    {
                        t.lowVA = true;
                        Log("replay: stopping - only %llu MB of address space left", (unsigned long long)(va >> 20));
                        return;
                    }
                    if (parsed % 2048 == 0)
                        Log("replay: %s: %u entries, %u created, %llu MB address space free",
                            path.c_str(), parsed, creator.created, (unsigned long long)(va >> 20));
                }
                currentEntry = h;
                creator.trusted = false;
                if (own)
                    if (auto it = links.appsOf.find(h); it != links.appsOf.end())
                        for (auto app : it->second)
                            if (app == d->appHash) creator.trusted = true;
                // An own entry from another DXVK build or feature set is as foreign
                // as anyone else's: only created when foreign replay is enabled.
                if (!creator.trusted && !replayForeign) { stale++; return; }
                if (replayTrace && !creator.trusted)
                    Log("trace: %s entry %016llx (#%u)", tag == Fossilize::RESOURCE_GRAPHICS_PIPELINE ? "graphics" : "compute",
                        (unsigned long long)h, parsed);
                creator.entry = h;
                creator.entryCreated = false;
                creator.entrySkip = Skip::None;
                creator.Settle(replayer.parse(creator, db, p, n));
                // The parsed create-infos are dead once created; keep memory flat
                // in a 32-bit process, as fossilize-replay does between batches.
                replayer.get_allocator().reset();
            });
        }

        Totals f;
        f.files = 1; f.pipelines = creator.pipelines; f.created = creator.created; f.trusted = creator.createdTrusted;
        f.irrelevant = creator.irrelevant; f.unsupported = creator.unsupported; f.incomplete = creator.incomplete;
        f.failed = creator.failed; f.already = already + creator.earlier; f.stale = stale;
        f.refused = checked.refused;
        t.Add(f);
    }

    // One database, split across `parts` workers. They share nothing but the device
    // and the feature filter: Vulkan object creation is thread-safe without a
    // pipeline cache, and the filter locks the module table it keeps.
    static void ReplayParallel(const std::string& path, VkDevice dev, Device* d, bool own, uint32_t parts,
                               std::unordered_set<Fossilize::Hash>& done, Totals& t)
    {
        std::vector<Totals> part(parts);
        std::vector<std::unordered_set<Fossilize::Hash>> seen(parts);
        std::vector<std::thread> workers;
        // Anything Fossilize throws on a damaged file ends that file, with a line in
        // the log, and never the game: an exception leaving a std::thread is
        // std::terminate. The other workers on the file stop at their next entry.
        std::atomic<bool> damaged{ false };
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < parts; k++)
            workers.emplace_back([&, k] {
                SetThreadPriority(GetCurrentThread(),
                                  pipelinekeys::VulkanReplay().holding ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
                Fossilize::set_thread_log_callback(&FossilizeLog, nullptr);
                onReplayThread = true;
                try { ReplayFile(path, dev, d, own, done, seen[k], part[k], k, parts, damaged); }
                catch (const std::exception& e)
                {
                    if (!damaged.exchange(true))
                        Log("replay: %s is damaged (%s) - the rest of it is skipped", path.c_str(), e.what());
                }
            });
        for (auto& w : workers) w.join();
        Totals sum;
        for (uint32_t k = 0; k < parts; k++)
        {
            sum.Add(part[k]);
            done.insert(seen[k].begin(), seen[k].end());
        }
        sum.files = 1;
        t.Add(sum);
        Log("replay: %s: on %u threads in %.1fs: %s%s", path.c_str(), parts,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), sum.Outcome().c_str(),
            sum.Stale().c_str());
    }

    // ---- sharing between PCs (plugins\pipelinecache\) -------------------------

    static std::string PipelineCacheDir() { return PluginsDir() + "pipelinecache\\"; }

    // The content hash of a file (pipelinekeys::ContentHash, the same one the D3D9
    // side names its files with): the <h> in its shared name, and what tells two
    // inputs apart. False if it cannot be read to the end.
    static bool HashFile(const std::string& path, uint64_t& out)
    {
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        std::vector<uint8_t> buf(1u << 20);
        uint64_t h = pipelinekeys::ContentHash(nullptr, 0);
        DWORD n = 0;
        BOOL ok;
        while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr)) && n)
            h = pipelinekeys::ContentHash(buf.data(), n, h);
        CloseHandle(f);
        if (ok) out = h;
        return ok != FALSE;
    }

    // The content hash of this PC's recording as the launch found it, before this
    // session appended anything: the name of its copy in plugins\pipelinecache\, and
    // how the replay knows that copy when it meets it there.
    static inline uint64_t ownHash = 0;
    static inline bool haveOwnHash = false;

    // "FusionFix.<16 lowercase hex digits>.foz": the only names ShareOwnRecording ever
    // deletes, and only those its state file lists.
    static bool IsSharedName(const std::string& n)
    {
        if (n.size() != 30 || n.compare(0, 10, "FusionFix.") != 0 || n.compare(26, 4, ".foz") != 0) return false;
        for (size_t i = 10; i < 26; i++)
            if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return false;
        return true;
    }

    // Once per launch, before the recorder opens the file for appending: copy this
    // PC's recording to plugins\pipelinecache\FusionFix.<h>.foz, unless that name is
    // there already. Once it is in place, delete the copies earlier launches wrote --
    // the names FusionFix.pipelinecache.state lists, nothing else -- and remember the
    // new one. The copy is made beside the ASI and renamed into the folder, so a
    // crash never leaves a stray temporary in the folder players copy around, and the
    // state file names it BEFORE the rename: a crash or a failed update can never
    // leave a copy of ours that nothing remembers, and so nothing ever deletes.
    static void ShareOwnRecording(const std::string& own)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExA(own.c_str(), GetFileExInfoStandard, &fa) || (!fa.nFileSizeHigh && !fa.nFileSizeLow))
            return;   // nothing recorded yet
        if (!HashFile(own, ownHash)) { Log("share: cannot read %s", own.c_str()); return; }
        haveOwnHash = true;

        const std::string dir = PipelineCacheDir(), state = PluginsDir() + "FusionFix.pipelinecache.state";
        char name[32];
        snprintf(name, sizeof(name), "FusionFix.%016llx.foz", (unsigned long long)ownHash);
        const std::string target = dir + name;

        std::vector<std::string> wrote;
        std::string before;
        if (FILE* f = fopen(state.c_str(), "r"))
        {
            char line[MAX_PATH];
            while (fgets(line, sizeof(line), f))
            {
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                if (IsSharedName(s)) { wrote.push_back(s); before += s + "\n"; }
            }
            fclose(f);
        }
        // The state file's new contents, or no file for none.
        auto writeState = [&](const std::string& text) {
            if (text.empty()) return DeleteFileA(state.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
            const std::string tmp = state + ".tmp";
            FILE* f = fopen(tmp.c_str(), "w");
            if (!f) return false;
            const bool ok = fputs(text.c_str(), f) >= 0;
            return fclose(f) == 0 && ok && MoveFileExA(tmp.c_str(), state.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        };

        bool ours = std::find(wrote.begin(), wrote.end(), name) != wrote.end();
        std::string listed = before;
        if (GetFileAttributesA(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            Log(ours ? "share: %s%s is there since an earlier launch - the recording has not changed"
                     : "share: %s%s is there already - the same content, not written by this PC", dir.c_str(), name);
        else
        {
            if (!ours)
            {
                listed += std::string(name) + "\n";
                if (!writeState(listed))
                {
                    Log("share: could not update %s - %s not written, the earlier copies stay", state.c_str(), name);
                    return;
                }
            }
            CreateDirectoryA(dir.c_str(), nullptr);
            const std::string tmp = PluginsDir() + "FusionFix.pipelinecache.tmp";
            if (!CopyFileA(own.c_str(), tmp.c_str(), FALSE) || !MoveFileExA(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                Log("share: could not write %s%s (error %lu) - the earlier copies stay", dir.c_str(), name, GetLastError());
                DeleteFileA(tmp.c_str());
                return;
            }
            ours = true;
            Log("share: wrote %s%s (%llu KB) - copy that folder's files to plugins\\pipelinecache\\ on your other PCs",
                dir.c_str(), name, ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) >> 10);
        }

        std::string after;
        for (auto& n : wrote)
        {
            if (n == name) continue;
            if (DeleteFileA((dir + n).c_str()))
                Log("share: deleted %s, the copy an earlier launch wrote", n.c_str());
            else if (GetLastError() != ERROR_FILE_NOT_FOUND)
                after += n + "\n";   // try again next launch
        }
        if (ours) after += std::string(name) + "\n";
        if (after != listed && !writeState(after)) Log("share: could not update %s", state.c_str());
    }

    // Other PCs' recordings, and this player's own Steam pre-cache buckets.
    //
    // plugins\pipelinecache\ is read flat: the .foz files at its top level, whatever
    // their names. A folder inside it is not read, and a D3D9 cache file (.bin) in it
    // is not loaded; either gets one line in the log saying where files go. Sorted,
    // so each launch goes through them in the same order. A file that cannot be read
    // is skipped with a line in the log (ReplayFile).
    static std::vector<std::string> ForeignInputs()
    {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        std::error_code ec;
        uint32_t folders = 0, unnamed = 0;
        // A file in the wrong folder of the two gets one line, from whichever of this
        // and the D3D9 replay finds it first (pipelinekeys::HintMisplaced).
        struct Wrong { uint32_t n = 0; std::string first; } bins, foz;
        auto lower = [](fs::path p) { std::wstring e = p.extension().wstring(); for (auto& c : e) c = towlower(c); return e; };
        auto note = [](Wrong& w, const fs::path& p) {
            if (w.n++) return;
            try { w.first = p.filename().string(); } catch (const std::exception&) { w.first = "?"; }
        };
        for (fs::directory_iterator it(fs::path(PipelineCacheDir()), ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (it->is_directory(fec)) { folders++; continue; }
            if (!it->is_regular_file(fec)) continue;
            const std::wstring ext = lower(it->path());
            if (ext == L".bin") note(bins, it->path());
            if (ext != L".foz") continue;
            try { out.push_back(it->path().string()); }
            catch (const std::exception&) { unnamed++; }   // a name the ANSI code page cannot hold
        }
        for (fs::directory_iterator it(fs::path(PluginsDir() + "d3d9cache\\"), ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (it->is_regular_file(fec) && lower(it->path()) == L".foz") note(foz, it->path());
        }
        if (folders)
            Log("replay: %u folder(s) in plugins\\pipelinecache\\ not read - put .foz files directly in "
                "plugins\\pipelinecache\\, not in folders inside it", folders);
        pipelinekeys::HintMisplaced("[VkCapture] ", pipelinekeys::kBinInPipelineCache, bins.n, bins.first);
        pipelinekeys::HintMisplaced("[VkCapture] ", pipelinekeys::kFozInD3d9Cache, foz.n, foz.first);
        if (unnamed)
            Log("replay: %u .foz file(s) in plugins\\pipelinecache\\ not read - rename them to plain ASCII names", unnamed);
        std::sort(out.begin(), out.end());

        // <library>\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe ->
        // <library>\steamapps\shadercache\<appid>\fozpipelinesv6\steamapprun_pipeline_cache.<bucket>\*.foz
        // Read only. The top-level steam_pipeline_cache.foz beside those buckets is the
        // every-DXVK-since-1.7 mix and is left alone. Rockstar Launcher installs have
        // no steamapps, and nothing is found.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path steamapps = fs::path(exe).parent_path().parent_path().parent_path().parent_path();
        if (steamapps.filename() != "steamapps") return out;
        for (const char* app : { "12210", "12220" })
            for (auto& b : fs::directory_iterator(steamapps / "shadercache" / app / "fozpipelinesv6", ec))
            {
                if (!b.is_directory(ec) || b.path().filename().string().rfind("steamapprun_pipeline_cache.", 0) != 0)
                    continue;
                for (auto& f : fs::directory_iterator(b.path(), ec))
                {
                    const std::string name = f.path().filename().string();
                    if (f.is_regular_file(ec) && f.path().extension() == ".foz" &&
                        name.rfind("replay_cache", 0) != 0 && name.find("whitelist") == std::string::npos)
                        out.push_back(f.path().string());
                }
            }
        return out;
    }

    // Each input once per content (HashFile): this PC's own copy in
    // plugins\pipelinecache\ is the recording the replay goes through first anyway,
    // and one file dropped in under two names is one file.
    static std::vector<std::string> Distinct(const std::vector<std::string>& inputs)
    {
        std::unordered_map<uint64_t, std::string> seen;
        if (haveOwnHash) seen.emplace(ownHash, "this PC's own recording");
        std::vector<std::string> out;
        for (auto& p : inputs)
        {
            uint64_t h = 0;
            if (!HashFile(p, h)) { out.push_back(p); continue; }   // ReplayFile says it cannot be read
            auto [it, fresh] = seen.emplace(h, p);
            if (fresh) out.push_back(p);
            else Log("replay: %s skipped - the same content as %s", p.c_str(), it->second.c_str());
        }
        return out;
    }

    // If creating a replayed object ever faults, say exactly what was being replayed
    // and where it faulted, instead of the game just exiting. Only the replay thread,
    // only real errors (0xC... codes); the exception still goes on to the game's own
    // handlers unchanged.
    static inline thread_local bool onReplayThread = false;
    static inline thread_local Fossilize::Hash currentEntry = 0;
    static inline thread_local const std::string* currentFile = nullptr;

    static LONG CALLBACK ReplayFaultHandler(EXCEPTION_POINTERS* ep)
    {
        if (!onReplayThread) return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if ((code & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
        static std::atomic<int> reported{ 0 };
        if (reported++ >= 3) return EXCEPTION_CONTINUE_SEARCH;

        void* addr = ep->ExceptionRecord->ExceptionAddress;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) && mod)
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        Log("replay thread FAULT %08lx at %p (%s+0x%lx) while replaying entry %016llx of %s",
            (unsigned long)code, addr, base ? base + 1 : name,
            mod ? (unsigned long)(reinterpret_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)) : 0ul,
            (unsigned long long)currentEntry, currentFile ? currentFile->c_str() : "?");

        // How the replay got there: walk the frame-pointer chain (x86) and name each
        // return address by module + offset. Stops at the first frame outside
        // readable memory.
        auto where = [](uintptr_t a, char* out, size_t n) {
            HMODULE m = nullptr;
            char nm[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(a), &m) && m)
                GetModuleFileNameA(m, nm, MAX_PATH);
            const char* b = strrchr(nm, '\\');
            snprintf(out, n, "%s+0x%lx", b ? b + 1 : nm, m ? (unsigned long)(a - reinterpret_cast<uintptr_t>(m)) : (unsigned long)a);
        };
#if defined(_M_IX86)
        uintptr_t ebp = ep->ContextRecord->Ebp;
        for (int i = 0; i < 12 && ebp; i++)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(ebp), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
            uintptr_t ret = reinterpret_cast<uintptr_t*>(ebp)[1];
            uintptr_t next = reinterpret_cast<uintptr_t*>(ebp)[0];
            char w[MAX_PATH + 32];
            where(ret, w, sizeof(w));
            Log("  frame %d: %s", i, w);
            if (next <= ebp) break;
            ebp = next;
        }
#endif
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static void FossilizeLog(Fossilize::LogLevel, const char* message, void*)
    {
        // Rejected foreign entries make Fossilize complain once each; keep a few.
        static std::atomic<uint32_t> seen{ 0 };
        if (seen++ < 5)
        {
            std::string m(message ? message : "");
            while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
            Log("fossilize: %s", m.c_str());
        }
    }

    static void ReplayThread(VkDevice dev, Device* d)
    {
        try { ReplayAll(dev, d); }
        catch (const std::exception& e) { Log("replay: stopped (%s)", e.what()); }
        pipelinekeys::VulkanReplay().running = false;
    }

    static void ReplayAll(VkDevice dev, Device* d)
    {
        auto& vr = pipelinekeys::VulkanReplay();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        Fossilize::set_thread_log_callback(&FossilizeLog, nullptr);
        onReplayThread = true;
        AddVectoredExceptionHandler(1, &ReplayFaultHandler);
        auto since = [](std::chrono::steady_clock::time_point a) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        };
        std::unordered_set<Fossilize::Hash> done;
        Totals t;

        // Everything this launch will go through, counted for the bar while the game is
        // still loading.
        const std::string own = PluginsDir() + "FusionFix.vkpipelines.foz";
        const bool haveOwn = GetFileAttributesA(own.c_str()) != INVALID_FILE_ATTRIBUTES;
        const auto found = ForeignInputs();
        const bool foreign = !found.empty() && replayForeign && d->haveFilter;
        const auto inputs = foreign ? Distinct(found) : found;
        const uint32_t ownEntries = haveOwn ? CountPipelines(own) : 0;
        uint32_t foreignEntries = 0;
        if (foreign)
            for (auto& p : inputs) foreignEntries += CountPipelines(p);
        vr.total = ownEntries + foreignEntries;
        if (!found.empty() && !replayForeign)
            Log("replay: %zu foreign database(s) found (other PCs / Steam pre-cache) - not replayed, "
                "see ReplayVulkanPipelinesForeign", found.size());
        else if (!found.empty() && !d->haveFilter)
            Log("replay: %zu foreign database(s) skipped - no feature filter for this device", found.size());
        if (!ownEntries && !foreign)
        {
            Log("replay: nothing to replay");
            return;
        }

        // Nothing is created before the loading-screen pass and everything DXVK
        // compiled for it are done (VulkanReplayState): the D3D9 pass comes first, and
        // then the loading screen is held for this. If the pass never comes, this
        // goes on in the background.
        //
        // The cap used to be 10 minutes, which was longer than any pass could take
        // when it only replayed a recording. The engine warm phase can legitimately
        // run for tens of minutes (PrecompileEngineWarm), and a cap that fires while
        // the pass is still working breaks the one ordering rule that matters: this
        // replay creates thousands of pipelines on most of the cores, and doing that
        // WHILE the D3D9 pass is drawing turns the pass's own timings into noise and
        // makes the two compete for the compiler threads. An hour is past any real
        // pass and still bounded.
        const auto w0 = std::chrono::steady_clock::now();
        if (vr.passPlanned)
        {
            Log("replay: %u pipeline entries ready (%u own, %u in %zu other file(s)) - waiting for the loading-screen pass",
                vr.total.load(), ownEntries, foreignEntries, foreign ? inputs.size() : (size_t)0);
            while (!d->stop && !vr.passDone && since(w0) < 3600.0) Sleep(20);
        }
        if (d->stop) return;
        const auto t0 = std::chrono::steady_clock::now();
        Log("replay: starting t=%.3f %s, %llu MB of address space free", vr.Seconds(),
            vr.holding ? "with the loading screen held" :
            !vr.passPlanned ? "in the background (PrecompileShaders = 0)" :
            vr.passDone ? "in the background (the loading screen was not held)" :
                          "in the background (the loading-screen pass did not finish in an hour)",
            (unsigned long long)(AvailableVA() >> 20));

        // While the loading screen is held: most of the cores. Otherwise a few
        // lowest-priority threads for the own recording and one for the rest, out of
        // the game's way.
        const uint32_t hw = (std::max)(1u, std::thread::hardware_concurrency());
        auto parts = [&](bool isOwn) {
            return vr.holding ? (std::max)(1u, (std::min)(6u, hw / 2)) : isOwn ? (std::max)(1u, (std::min)(4u, hw / 4)) : 1u;
        };
        if (ownEntries)
        {
            vr.phase = pipelinekeys::VulkanReplayState::kOwn;
            Log("replay: 1. this PC's own recording, t=%.3f", vr.Seconds());
            ReplayParallel(own, dev, d, true, parts(true), done, t);
        }

        if (foreign)
        {
            vr.phase = pipelinekeys::VulkanReplayState::kForeign;
            Log("replay: 2. other PCs' files and the Steam pre-cache (%zu file(s)), t=%.3f", inputs.size(), vr.Seconds());
            // Foreign entries are judged by what DXVK uses here, so let it show as much
            // of that as it will first: the loading-screen pass creates every pipeline
            // the game has been seen to use. Without that pass (PrecompileShaders = 0),
            // or if it never came, the game's own first pipelines have to do.
            if (!vr.passDone)
                for (int i = 0; i < 240 && !d->stop && learned.pipelines < 64; i++) Sleep(500);
            if (learned.pipelines < 64)
            {
                Log("replay: only %u pipelines seen from DXVK - not enough to validate against, %zu foreign "
                    "database(s) skipped this launch", learned.pipelines.load(), inputs.size());
                vr.done += foreignEntries;
            }
            else
            {
                const auto f0 = std::chrono::steady_clock::now();
                Totals ft;
                uint32_t mostParts = 1;
                for (auto& p : inputs)
                {
                    if (d->stop || t.lowVA || ft.lowVA) break;
                    const uint32_t n = parts(false);
                    mostParts = (std::max)(mostParts, n);
                    ReplayParallel(p, dev, d, false, n, done, ft);
                }
                t.Add(ft);
                Log("replay: foreign, %u files in %.1fs on up to %u threads (%u DXVK pipelines learned first): %s",
                    ft.files, since(f0), mostParts, learned.pipelines.load(), ft.Outcome().c_str());
            }
        }

        Log("replay: done t=%.3f in %.1fs - %u files: %s%s", vr.Seconds(), since(t0), t.files, t.Outcome().c_str(),
            t.Stale().c_str());
    }

    // ---- loader-level wrappers --------------------------------------------

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name)
    {
        PFN_vkVoidFunction real = realGDPA(device, name);
        if (!real || !name) return real;
        Device* d = Get(device);
        if (!d) return real;
        const bool primary = d->primary;
        struct { const char* name; PFN_vkVoidFunction fn; } wrap[] = {
            { "vkCreateGraphicsPipelines",   reinterpret_cast<PFN_vkVoidFunction>(&CreateGraphicsPipelines) },
            { "vkCreateComputePipelines",    reinterpret_cast<PFN_vkVoidFunction>(&CreateComputePipelines) },
            { "vkCreateShaderModule",        reinterpret_cast<PFN_vkVoidFunction>(&CreateShaderModule) },
            { "vkCreatePipelineLayout",      reinterpret_cast<PFN_vkVoidFunction>(&CreatePipelineLayout) },
            { "vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(&CreateDescriptorSetLayout) },
            { "vkCreateSampler",             reinterpret_cast<PFN_vkVoidFunction>(&CreateSampler) },
            { "vkCreateRenderPass",          reinterpret_cast<PFN_vkVoidFunction>(&CreateRenderPass) },
            { "vkCreateRenderPass2",         reinterpret_cast<PFN_vkVoidFunction>(&CreateRenderPass2) },
            { "vkCreateRenderPass2KHR",      reinterpret_cast<PFN_vkVoidFunction>(&CreateRenderPass2KHR) },
            { "vkDestroyDevice",             reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice) },
            // The game's frames only. Proton's d3d11 DXVK presents its own, and
            // counting those into the frame-time report this whole feature is
            // measured by would be measuring somebody else's swapchain with a
            // function pointer taken from a different device.
            { "vkQueuePresentKHR",           (primary && realQueuePresent)
                                                 ? reinterpret_cast<PFN_vkVoidFunction>(&QueuePresentKHR) : real },
        };
        for (auto& w : wrap)
            if (strcmp(w.name, name) == 0) return w.fn;
        return real;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkInstance* out)
    {
        VkResult res = realCreateInstance(info, alloc, out);
        if (res != VK_SUCCESS) return res;
        instance = *out;
        if (info->pApplicationInfo)
        {
            appInfo = *info->pApplicationInfo;
            appInfo.pNext = nullptr;
            appName = appInfo.pApplicationName ? appInfo.pApplicationName : "";
            engineName = appInfo.pEngineName ? appInfo.pEngineName : "";
            appInfo.pApplicationName = appName.c_str();
            appInfo.pEngineName = engineName.c_str();
            haveAppInfo = true;
            // With the session clock and the address space, because a Vulkan
            // instance appearing in the MIDDLE of gameplay is a subsystem
            // re-initialising, and the 2026-09-20 full+engine run that died had
            // two of those and no other run had any.
            Log("instance created by \"%s\" / engine \"%s\" %u.%u.%u, t=%.3f, memory: %s",
                appName.c_str(), engineName.c_str(),
                VK_API_VERSION_MAJOR(appInfo.engineVersion), VK_API_VERSION_MINOR(appInfo.engineVersion),
                VK_API_VERSION_PATCH(appInfo.engineVersion), pipelinekeys::VulkanReplay().Seconds(),
                pipelinekeys::VaLine().c_str());
        }
        return res;
    }

    // Set up Fossilize's feature filter the way fossilize-replay sets it up for its own
    // device (cli/device.cpp), but from DXVK's: the extensions and the feature chain
    // DXVK enabled, the physical device's properties, and the device itself for format
    // and set-layout queries.
    static void InitFilter(Device& d, VkPhysicalDevice gpu, VkDevice dev, const VkDeviceCreateInfo* info)
    {
        auto gipa = [](const char* n) { return shGIPA.stdcall<PFN_vkVoidFunction>(instance, n); };
        auto GetProperties  = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa("vkGetPhysicalDeviceProperties"));
        auto GetProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa("vkGetPhysicalDeviceProperties2"));
        auto EnumerateExts  = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa("vkEnumerateDeviceExtensionProperties"));
        d.query.gpu = gpu;
        d.query.device = dev;
        d.query.FormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa("vkGetPhysicalDeviceFormatProperties"));
        d.query.Features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa("vkGetPhysicalDeviceFeatures2"));
        d.query.SetLayoutSupport = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(realGDPA(dev, "vkGetDescriptorSetLayoutSupport"));
        if (!GetProperties || !GetProperties2 || !EnumerateExts || !d.query.FormatProperties || !d.query.Features2 ||
            !d.query.SetLayoutSupport)
        {
            Log("no Vulkan 1.1 device queries - no feature filter, foreign pipelines cannot be validated");
            return;
        }

        // The device's API version: what DXVK asked for, capped by the driver.
        VkPhysicalDeviceProperties base{};
        GetProperties(gpu, &base);
        const uint32_t api = (std::min)(haveAppInfo && appInfo.apiVersion ? appInfo.apiVersion : VK_API_VERSION_1_0, base.apiVersion);

        // Properties are facts about the GPU, so ask for those of every extension it
        // supports -- as fossilize-replay's enable-everything device does. Built from
        // DXVK's list alone, the filter would never see e.g. the float-controls limits:
        // their struct is chained for VK_KHR_shader_float_controls, which a Vulkan 1.3
        // application like DXVK does not name, and every shader using a rounding or
        // denormal mode would then look unsupported.
        uint32_t count = 0;
        EnumerateExts(gpu, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> supported(count);
        EnumerateExts(gpu, nullptr, &count, supported.data());
        std::vector<const char*> supportedNames;
        for (uint32_t i = 0; i < count; i++) supportedNames.push_back(supported[i].extensionName);
        Fossilize::VulkanProperties props{};
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = Fossilize::build_pnext_chain(props, api, supportedNames.data(), count);
        GetProperties2(gpu, &props2);

        auto exts = const_cast<const char**>(info->ppEnabledExtensionNames);

        // DXVK's feature chain as a VkPhysicalDeviceFeatures2: the filter takes the core
        // features from it and walks the rest (Vulkan 1.1-1.4 and extension structs).
        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        if (auto* f2 = static_cast<const VkPhysicalDeviceFeatures2*>(FindPNext(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)))
            features.features = f2->features;
        else if (info->pEnabledFeatures)
            features.features = *info->pEnabledFeatures;
        features.pNext = const_cast<void*>(info->pNext);

        d.haveFilter = d.filter.init(api, exts, info->enabledExtensionCount, &features, &props2);
        d.filter.set_device_query_interface(&d.query);
        Log("feature filter: Vulkan %u.%u, the %u extensions and the features DXVK enabled on %s",
            VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), info->enabledExtensionCount, base.deviceName);
        if (replayTrace)
            for (uint32_t i = 0; i < info->enabledExtensionCount; i += 8)
            {
                std::string line;
                for (uint32_t k = i; k < info->enabledExtensionCount && k < i + 8; k++) { line += ' '; line += exts[k]; }
                Log("trace: device extensions%s", line.c_str());
            }
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkDevice* out)
    {
        VkResult res = realCreateDevice(gpu, info, alloc, out);
        if (res != VK_SUCCESS) return res;

        if (!realGDPA)
            realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                shGIPA.stdcall<PFN_vkVoidFunction>(instance, "vkGetDeviceProcAddr"));
        if (!realGDPA) { Log("no vkGetDeviceProcAddr - not recording"); return res; }

        // One recorder, one file. GTA IV has one D3D9 device, but DXVK is not the
        // only Vulkan user in the process: under Proton a second DXVK (the one
        // behind Proton's dxgi/d3d11, 2.7.1 here) creates instances of its own,
        // and from the warm-device work the MOD creates a D3D9 device of its own
        // for the engine walk. A second device must not append to the same
        // archive concurrently, must not start a replay on a device that is
        // about to be destroyed, and must not report gameplay twice.
        //
        // It must still be REGISTERED, though. GetDeviceProcAddr hands out the
        // real entry points for a device this does not know, so an unregistered
        // one is completely unwrapped -- and then its pipeline creations are
        // missing from the counters the warm pass measures itself with, and from
        // the signals its "wait until DXVK is quiet" watches. That was harmless
        // while the only unregistered device was Proton's idle d3d11 one.
        bool primary;
        {
            std::shared_lock lock(devLock);
            primary = devices.empty();
        }
        const bool mine = pipelinekeys::VulkanReplay().modOwnedDevice.load();
        // DXVK's device only: the wrappers are always on, for the metrics.
        if (haveAppInfo && engineName != "DXVK")
        {
            Log("device %p (engine \"%s\") left alone: not DXVK's", (void*)*out, engineName.c_str());
            return res;
        }

        auto d = std::make_unique<Device>();
        d->primary = primary;
        VkDevice dev = *out;
        auto load = [&](const char* n) { return realGDPA(dev, n); };

        // WHICH COMPILATION REGIME THIS DEVICE IS IN, because the engine warm
        // pass's chunked walk depends on it. With VK_EXT_graphics_pipeline_library
        // enabled, DXVK's draw takes a fast-linked base pipeline and QUEUES the
        // optimized one to its worker threads (dxvk_graphics.cpp:1111); a GPU
        // fence does not wait for those, and ~DxvkPipelineManager drops whatever
        // is still queued. Without it -- this rig, where the option is off and
        // RADV is refused it anyway -- every optimized pipeline is built inline
        // on the CS thread and a drained fence IS the compile. The walk waits
        // until DXVK is quiet either way, which covers both; this is here so the
        // log SAYS which regime the measurement was taken in.
        if (primary && info->ppEnabledExtensionNames)
        {
            bool gpl = false;
            for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
                if (info->ppEnabledExtensionNames[i] &&
                    strcmp(info->ppEnabledExtensionNames[i], "VK_EXT_graphics_pipeline_library") == 0)
                    gpl = true;
            pipelinekeys::VulkanReplay().gplEnabled = gpl;
            // NECESSARY, NOT SUFFICIENT, and this rig is the proof: DXVK enables
            // the extension on the device whenever the driver offers it, and
            // then decides separately whether to USE it --
            // canUseGraphicsPipelineLibrary() also wants
            // graphicsPipelineLibraryIndependentInterpolationDecoration and
            // `dxvk.enableGraphicsPipelineLibrary != False`, and this machine's
            // dxvk.conf sets that option to False. So this line says what can be
            // seen from outside DXVK and no more.
            Log("device %p: VK_EXT_graphics_pipeline_library is %s on the device. That is what "
                "decides whether DXVK compiles optimized pipelines inline on its CS thread "
                "(a drained fence is then the compile) or queues them to its worker threads "
                "(only a quiet wait can see those finish) - but the extension being enabled "
                "does not mean DXVK uses it: dxvk.enableGraphicsPipelineLibrary and the "
                "driver decide, and neither is readable from here. Every wait in the warm "
                "pass is a quiet wait, which covers both.",
                (void*)dev, gpl ? "enabled" : "NOT enabled");
        }
        d->CreateGraphicsPipelines   = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(load("vkCreateGraphicsPipelines"));
        d->CreateComputePipelines    = reinterpret_cast<PFN_vkCreateComputePipelines>(load("vkCreateComputePipelines"));
        d->CreateShaderModule        = reinterpret_cast<PFN_vkCreateShaderModule>(load("vkCreateShaderModule"));
        d->CreatePipelineLayout      = reinterpret_cast<PFN_vkCreatePipelineLayout>(load("vkCreatePipelineLayout"));
        d->CreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(load("vkCreateDescriptorSetLayout"));
        d->CreateSampler             = reinterpret_cast<PFN_vkCreateSampler>(load("vkCreateSampler"));
        d->CreateRenderPass          = reinterpret_cast<PFN_vkCreateRenderPass>(load("vkCreateRenderPass"));
        d->CreateRenderPass2         = reinterpret_cast<PFN_vkCreateRenderPass2>(load("vkCreateRenderPass2"));
        d->CreateRenderPass2KHR      = reinterpret_cast<PFN_vkCreateRenderPass2>(load("vkCreateRenderPass2KHR"));
        d->DestroyDevice             = reinterpret_cast<PFN_vkDestroyDevice>(load("vkDestroyDevice"));
        d->DestroyPipeline           = reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
        d->DestroyShaderModule       = reinterpret_cast<PFN_vkDestroyShaderModule>(load("vkDestroyShaderModule"));
        d->DestroySampler            = reinterpret_cast<PFN_vkDestroySampler>(load("vkDestroySampler"));
        d->DestroyDescriptorSetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(load("vkDestroyDescriptorSetLayout"));
        d->DestroyPipelineLayout     = reinterpret_cast<PFN_vkDestroyPipelineLayout>(load("vkDestroyPipelineLayout"));
        d->DestroyRenderPass         = reinterpret_cast<PFN_vkDestroyRenderPass>(load("vkDestroyRenderPass"));
        if (primary) realQueuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(load("vkQueuePresentKHR"));

        auto* ident = static_cast<const VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT*>(
            FindPNext(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT));
        if (ident && ident->shaderModuleIdentifier)
        {
            d->GetShaderModuleIdentifierEXT = reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(load("vkGetShaderModuleIdentifierEXT"));
            d->GetShaderModuleCreateInfoIdentifierEXT =
                reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(load("vkGetShaderModuleCreateInfoIdentifierEXT"));
            d->usesIdentifiers = d->GetShaderModuleIdentifierEXT && d->GetShaderModuleCreateInfoIdentifierEXT;
        }

        if (!primary)
        {
            Log("device %p (engine \"%s\")%s: wrapped for the counters only - not recording, "
                "not replaying, not reporting, because another device is already the primary one",
                (void*)dev, engineName.c_str(), mine ? ", the mod's own warm device" : "");
            std::unique_lock lock(devLock);
            devices[dev] = std::move(d);
            return res;
        }

        // The enabled features, as a VkPhysicalDeviceFeatures2 chain -- what a replay
        // has to match. Built the way the Fossilize layer builds it.
        VkPhysicalDeviceFeatures2 pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        const void* devicePNext = info->pNext;
        if (!FindPNext(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))
        {
            if (info->pEnabledFeatures) pdf2.features = *info->pEnabledFeatures;
            pdf2.pNext = const_cast<void*>(info->pNext);
            devicePNext = &pdf2;
        }

        // The hash Fossilize links every blob to: this application info (DXVK build)
        // plus these features. Replay trusts exactly the own entries that carry it.
        d->appHash = Fossilize::Hashing::compute_combined_application_feature_hash(
            Fossilize::Hashing::compute_application_feature_hash(haveAppInfo ? &appInfo : nullptr, devicePNext));

        d->path = PluginsDir() + "FusionFix.vkpipelines.foz";
        // Before this session appends anything (see SHARING BETWEEN PCs at the top).
        static std::atomic<bool> shared{ false };
        if ((enabled || replayEnabled) && !shared.exchange(true))
            ShareOwnRecording(d->path);
        if (enabled)
        {
            d->db.reset(Fossilize::create_stream_archive_database(d->path.c_str(), Fossilize::DatabaseMode::Append));
            if (!d->db)
                Log("cannot open %s - not recording", d->path.c_str());
            else
            {
                d->recorder = std::make_unique<Fossilize::StateRecorder>();
                d->recorder->set_database_enable_compression(true);
                d->recorder->set_database_enable_checksum(true);
                if (haveAppInfo && !d->recorder->record_application_info(appInfo))
                    Log("could not record the application info");
                if (!d->recorder->record_physical_device_features(devicePNext))
                    Log("could not record the device features");
                d->recorder->init_recording_thread(d->db.get());
            }
        }

        Log("device %p (application/feature hash %016llx): %s%s%s", (void*)dev, (unsigned long long)d->appHash,
            d->recorder ? ("recording to " + d->path).c_str() : "not recording",
            replayEnabled ? ", replaying" : "",
            d->usesIdentifiers ? " (with shader module identifiers)" : "");
        if (replayEnabled)
            InitFilter(*d, gpu, dev, info);
        Device* raw = d.get();
        {
            std::unique_lock lock(devLock);
            devices[dev] = std::move(d);
        }
        auto& vr = pipelinekeys::VulkanReplay();
        vr.compilerCpuUs = &CompilerCpuUs;
        vr.watching = true;
        metricsOn = true;
        raw->metrics = std::thread(&ReportMetrics, raw);
        if (replayEnabled)
        {
            // Before the thread starts, so the loading screen can never miss it.
            vr.running = true;
            raw->replay = std::thread(&ReplayThread, dev, raw);
        }
        return res;
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char* name)
    {
        PFN_vkVoidFunction real = shGIPA.stdcall<PFN_vkVoidFunction>(inst, name);
        if (!real || !name) return real;
        if (strcmp(name, "vkCreateInstance") == 0)
        {
            realCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
        }
        if (strcmp(name, "vkCreateDevice") == 0)
        {
            realCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
        }
        if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        {
            realGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
        }
        return real;
    }

    // winevulkan.dll ends the process with ExitProcess(3) when the Unix side of a call
    // faults: Wine hands the fault back as the call's status ("Exception %#lx in Unix
    // call", an ERR Proton does not print) and raises no Windows exception, so no
    // handler ever sees the fault itself -- only whatever DLL then crashes on the way
    // out. Say what happened first.
    static void WINAPI VulkanLibExit(UINT code)
    {
        if (onReplayThread)
            Log("%s is ending the process (exit code %u): the Unix side of a Vulkan call faulted while replaying "
                "entry %016llx of %s", hookedLib.c_str(), code, (unsigned long long)currentEntry,
                currentFile ? currentFile->c_str() : "?");
        else
            Log("%s is ending the process (exit code %u): the Unix side of a Vulkan call faulted on thread %lu",
                hookedLib.c_str(), code, GetCurrentThreadId());
        ExitProcess(code);
    }

    // Runs inside the LoadLibraryA that loads the Vulkan library -- DXVK's, if it
    // is the first to ask -- so the hook is in place before anyone resolves it.
    static void InstallHook(const wchar_t* lib)
    {
        if (shGIPA) return;
        HMODULE mod = GetModuleHandleW(lib);
        auto target = mod ? GetProcAddress(mod, "vkGetInstanceProcAddr") : nullptr;
        if (!target) { Log("%ls has no vkGetInstanceProcAddr - not recording", lib); return; }
        shGIPA = safetyhook::create_inline(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&GetInstanceProcAddr));
        char narrow[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, lib, -1, narrow, sizeof(narrow), nullptr, nullptr);
        hookedLib = narrow;
        Log(shGIPA ? "hooked vkGetInstanceProcAddr in %s" : "could not hook vkGetInstanceProcAddr in %s", narrow);
        if (_wcsicmp(lib, L"winevulkan.dll") == 0)
            IATHook::Replace(mod, "kernel32.dll", std::forward_as_tuple("ExitProcess", &VulkanLibExit));
    }

public:
    VkCapture()
    {
        CIniReader ini("");
        enabled       = ini.ReadInteger("SHADERS", "CaptureVulkanPipelines", 1) != 0;
        replayEnabled = ini.ReadInteger("SHADERS", "ReplayVulkanPipelines", 1) != 0;
        replayTrace   = ini.ReadInteger("SHADERS", "ReplayVulkanPipelinesTrace", 0) != 0;
        replayForeign = ini.ReadInteger("SHADERS", "ReplayVulkanPipelinesForeign", 1) != 0;

        // The session clock every "t=" in the log counts from.
        const SYSTEMTIME& t = pipelinekeys::VulkanReplay().epochLocal;
        Log("session clock: t=0 is %04u-%02u-%02u %02u:%02u:%02u.%03u local time", t.wYear, t.wMonth, t.wDay, t.wHour,
            t.wMinute, t.wSecond, t.wMilliseconds);

        // Hooked whatever the settings: the gameplay metrics need DXVK's device too.
        // DXVK prefers winevulkan.dll and only falls back to vulkan-1.dll; under Wine,
        // vulkan-1.dll forwards to winevulkan.dll. Hook exactly the one DXVK uses.
        bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        static const wchar_t* lib = wine ? L"winevulkan.dll" : L"vulkan-1.dll";
        CallbackHandler::RegisterCallback(lib, [] { InstallHook(lib); });
        FusionFix::onShutdownEvent() += [] { ReportFinal(); };
    }
} VkCapture;
