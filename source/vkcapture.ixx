module;

#include <common.hxx>
#include "vulkan/vulkan.h"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "layer/utils.hpp"          // Fossilize's LOG* macros, which fossilize_errors.hpp requires
#include "fossilize_errors.hpp"     // set_thread_log_callback
#include "cli/fossilize_feature_filter.hpp"
#include "pipelinekeys.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
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
//    Right after DXVK creates its device, a low-priority thread replays Fossilize
//    databases on that same device -- through the real, unwrapped entry points, so
//    nothing replayed is recorded again -- to warm the DRIVER's pipeline cache
//    before gameplay asks for the same pipelines. Each pipeline is destroyed as
//    soon as it exists: the goal is the driver cache, not the objects, and GTA IV
//    is a 32-bit process.
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
//    other PCs' files dropped into plugins\pipelinecache\, the player's own Steam
//    pre-cache buckets (read only, never written) -- needs all three.
//    Steam's top-level mixed database (every DXVK since 1.7, ~2 GB) is not read:
//    it matched 0.3% of what DXVK 3.1.1 builds, against 85.8% for its DXVK-3 bucket.
// ===========================================================================

class VkCapture
{
    static inline bool enabled = false;         // CaptureVulkanPipelines
    static inline bool replayEnabled = false;   // ReplayVulkanPipelines
    // ReplayVulkanPipelinesTrace: log every foreign entry just before it is created.
    // The log is flushed per line, so if creating one takes the process down -- in
    // the driver's native code, where no Windows exception is raised -- the last
    // line names it.
    static inline bool replayTrace = false;
    // ReplayVulkanPipelinesForeign: also replay databases this PC did not record with
    // this exact DXVK build (other PCs, Steam's buckets). OFF by default. It used to
    // take the game down within seconds: a shader module rejected as not relevant
    // left VK_NULL_HANDLE in the replayer's handle map, a later pipeline of the same
    // batch was created with it, and RADV dereferenced the missing vertex shader
    // (radv_pipeline_init_vertex_input_state) -- see the three checks above.
    static inline bool replayForeign = false;
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
        std::thread replay;
        std::atomic<bool> stop{ false };

        std::atomic<uint32_t> graphics{ 0 }, compute{ 0 }, modules{ 0 }, other{ 0 }, failed{ 0 };
    };
    static inline std::shared_mutex devLock;
    static inline std::unordered_map<VkDevice, std::unique_ptr<Device>> devices;

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

    // ---- device-level wrappers (Fossilize layer/dispatch.cpp, normal mode) ----

    static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateGraphicsPipelines(device, cache, count, infos, alloc, out);
        // Recorded only on success: a VK_PIPELINE_COMPILE_REQUIRED probe creates
        // nothing, and DXVK records the pipeline when it then compiles it for real.
        if (res != VK_SUCCESS) return res;
        for (uint32_t i = 0; i < count; i++)
        {
            if (replayEnabled) Learn(CollectGraphics(infos[i]), true);
            if (!d->recorder) continue;
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
        VkResult res = d->CreateComputePipelines(device, cache, count, infos, alloc, out);
        if (res != VK_SUCCESS) return res;
        for (uint32_t i = 0; i < count; i++)
        {
            if (replayEnabled) Learn(CollectCompute(infos[i]), true);
            if (!d->recorder) continue;
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
        VkResult res = d->CreateShaderModule(device, info, alloc, out);
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
        // The replay creates objects on this device: it must be finished first.
        d->stop = true;
        if (d->replay.joinable()) d->replay.join();
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
                 failed = 0, already = 0, stale = 0;
        bool lowVA = false;

        void Add(const Totals& o)
        {
            files += o.files; pipelines += o.pipelines; created += o.created; trusted += o.trusted;
            irrelevant += o.irrelevant; unsupported += o.unsupported; incomplete += o.incomplete; failed += o.failed;
            already += o.already; stale += o.stale; lowVA |= o.lowVA;
        }
        // What was created and why the rest was not, for the log.
        std::string Outcome() const
        {
            char buf[384];
            snprintf(buf, sizeof(buf), "%u entries created (%u trusted, %u pipelines in all); skipped: %u not relevant "
                     "(another DXVK build or configuration), %u not supported by this device, %u incomplete (a dependency "
                     "missing or skipped), %u failed; %u already done", created, trusted, pipelines, irrelevant, unsupported,
                     incomplete, failed, already);
            return buf;
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

    // `part` of `parts`: parallel workers each take every parts-th pipeline entry of
    // the same file, through their own database handle and replayer.
    static void ReplayFile(const std::string& path, VkDevice dev, Device* d, bool own,
                           std::unordered_set<Fossilize::Hash>& done, Totals& t,
                           uint32_t part = 0, uint32_t parts = 1)
    {
        std::unique_ptr<Fossilize::DatabaseInterface> db(
            Fossilize::create_stream_archive_database(path.c_str(), Fossilize::DatabaseMode::ReadOnly));
        if (!db || !db->prepare()) { Log("replay: cannot read %s", path.c_str()); return; }
        currentFile = &path;

        auto readAll = [&](Fossilize::ResourceTag tag, auto&& fn) {
            size_t count = 0;
            if (!db->get_hash_list_for_resource_tag(tag, &count, nullptr) || !count) return;
            std::vector<Fossilize::Hash> hashes(count);
            if (!db->get_hash_list_for_resource_tag(tag, &count, hashes.data())) return;
            std::vector<uint8_t> buf;
            for (auto h : hashes)
            {
                if (d->stop) return;
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
                    [&](Fossilize::Hash, const uint8_t* p, size_t n) { (void)lr.parse(links, db.get(), p, n); });
        }

        ReplayCreator creator(dev, d);
        Fossilize::StateReplayer replayer;
        uint32_t already = 0, parsed = 0, stale = 0, index = 0;
        for (auto tag : { Fossilize::RESOURCE_GRAPHICS_PIPELINE, Fossilize::RESOURCE_COMPUTE_PIPELINE })
        {
            readAll(tag, [&](Fossilize::Hash h, const uint8_t* p, size_t n) {
                if (t.lowVA) return;
                if ((index++ % parts) != part) return;
                if (!done.insert(h).second) { already++; return; }
                if (++parsed % 64 == 0)
                {
                    creator.Release();
                    replayer.forget_handle_references();
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
                creator.Settle(replayer.parse(creator, db.get(), p, n));
                // The parsed create-infos are dead once created; keep memory flat
                // in a 32-bit process, as fossilize-replay does between batches.
                replayer.get_allocator().reset();
            });
        }

        Totals f;
        f.files = 1; f.pipelines = creator.pipelines; f.created = creator.created; f.trusted = creator.createdTrusted;
        f.irrelevant = creator.irrelevant; f.unsupported = creator.unsupported; f.incomplete = creator.incomplete;
        f.failed = creator.failed; f.already = already + creator.earlier; f.stale = stale;
        t.Add(f);
        if (parts == 1)
            Log("replay: %s: %s%s", path.c_str(), f.Outcome().c_str(), f.Stale().c_str());
    }

    // This PC's own recording, split across a few workers. Everything in it that
    // is replayed is trusted, so the workers share nothing but the device, and
    // Vulkan object creation is thread-safe without a pipeline cache.
    static void ReplayOwnParallel(const std::string& path, VkDevice dev, Device* d,
                                  std::unordered_set<Fossilize::Hash>& done, Totals& t)
    {
        const uint32_t parts = (std::max)(1u, (std::min)(4u, std::thread::hardware_concurrency() / 4));
        std::vector<Totals> part(parts);
        std::vector<std::unordered_set<Fossilize::Hash>> seen(parts);
        std::vector<std::thread> workers;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < parts; k++)
            workers.emplace_back([&, k] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
                Fossilize::set_thread_log_callback(&FossilizeLog, nullptr);
                onReplayThread = true;
                ReplayFile(path, dev, d, true, seen[k], part[k], k, parts);
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

    // Other PCs' recordings, and this player's own Steam pre-cache buckets.
    static std::vector<std::string> ForeignInputs()
    {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        std::error_code ec;
        const std::string imports = pipelinekeys::ImportDir();
        if (!imports.empty())
            for (auto& e : fs::directory_iterator(imports, ec))
                if (e.is_regular_file(ec) && e.path().extension() == ".foz") out.push_back(e.path().string());

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
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        Fossilize::set_thread_log_callback(&FossilizeLog, nullptr);
        onReplayThread = true;
        AddVectoredExceptionHandler(1, &ReplayFaultHandler);
        const auto t0 = std::chrono::steady_clock::now();
        std::unordered_set<Fossilize::Hash> done;
        Totals t;
        Log("replay: starting, %llu MB of address space free", (unsigned long long)(AvailableVA() >> 20));

        const std::string own = PluginsDir() + "FusionFix.vkpipelines.foz";
        if (GetFileAttributesA(own.c_str()) != INVALID_FILE_ATTRIBUTES)
            ReplayOwnParallel(own, dev, d, done, t);

        // Foreign entries are judged by what DXVK uses here, so let it show enough of
        // that first: the trusted replay above usually already has, otherwise the
        // game's own first pipelines (the loading-screen pass builds hundreds) do.
        // Without the feature filter nothing foreign is validated, so nothing runs.
        auto inputs = ForeignInputs();
        if (!inputs.empty() && !replayForeign)
            Log("replay: %zu foreign database(s) found (other PCs / Steam pre-cache) - not replayed in-process, "
                "see ReplayVulkanPipelinesForeign", inputs.size());
        else if (!inputs.empty() && !d->haveFilter)
            Log("replay: %zu foreign database(s) skipped - no feature filter for this device", inputs.size());
        else if (!inputs.empty())
        {
            for (int i = 0; i < 240 && !d->stop && learned.pipelines < 64; i++) Sleep(500);
            if (learned.pipelines < 64)
                Log("replay: only %u pipelines seen from DXVK - not enough to validate against, %zu foreign "
                    "database(s) skipped this launch", learned.pipelines.load(), inputs.size());
            else
            {
                const auto f0 = std::chrono::steady_clock::now();
                Totals ft;
                for (auto& p : inputs) { if (d->stop || t.lowVA || ft.lowVA) break; ReplayFile(p, dev, d, false, done, ft); }
                t.Add(ft);
                Log("replay: foreign, %u files in %.1fs: %s", ft.files,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count(), ft.Outcome().c_str());
            }
        }

        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Log("replay: done in %.1fs - %u files: %s%s", s, t.files, t.Outcome().c_str(), t.Stale().c_str());
    }

    // ---- loader-level wrappers --------------------------------------------

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name)
    {
        PFN_vkVoidFunction real = realGDPA(device, name);
        if (!real || !name || !Get(device)) return real;
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
            Log("instance created by \"%s\" / engine \"%s\" %u.%u.%u", appName.c_str(), engineName.c_str(),
                VK_API_VERSION_MAJOR(appInfo.engineVersion), VK_API_VERSION_MINOR(appInfo.engineVersion),
                VK_API_VERSION_PATCH(appInfo.engineVersion));
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
        // behind Proton's dxgi/d3d11, 2.7.1 here) creates instances of its own. A
        // second device must not append to the same archive concurrently.
        {
            std::shared_lock lock(devLock);
            if (!devices.empty())
            {
                Log("device %p (engine \"%s\") not recorded: already recording one device",
                    (void*)*out, engineName.c_str());
                return res;
            }
        }

        auto d = std::make_unique<Device>();
        VkDevice dev = *out;
        auto load = [&](const char* n) { return realGDPA(dev, n); };
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

        auto* ident = static_cast<const VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT*>(
            FindPNext(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT));
        if (ident && ident->shaderModuleIdentifier)
        {
            d->GetShaderModuleIdentifierEXT = reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(load("vkGetShaderModuleIdentifierEXT"));
            d->GetShaderModuleCreateInfoIdentifierEXT =
                reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(load("vkGetShaderModuleCreateInfoIdentifierEXT"));
            d->usesIdentifiers = d->GetShaderModuleIdentifierEXT && d->GetShaderModuleCreateInfoIdentifierEXT;
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
        if (replayEnabled)
            raw->replay = std::thread(&ReplayThread, dev, raw);
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
        enabled       = ini.ReadInteger("SHADERS", "CaptureVulkanPipelines", 0) != 0;
        replayEnabled = ini.ReadInteger("SHADERS", "ReplayVulkanPipelines", 0) != 0;
        replayTrace   = ini.ReadInteger("SHADERS", "ReplayVulkanPipelinesTrace", 0) != 0;
        replayForeign = ini.ReadInteger("SHADERS", "ReplayVulkanPipelinesForeign", 0) != 0;
        if (!enabled && !replayEnabled) return;

        // DXVK prefers winevulkan.dll and only falls back to vulkan-1.dll; under Wine,
        // vulkan-1.dll forwards to winevulkan.dll. Hook exactly the one DXVK uses.
        bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        static const wchar_t* lib = wine ? L"winevulkan.dll" : L"vulkan-1.dll";
        CallbackHandler::RegisterCallback(lib, [] { InstallHook(lib); });
    }
} VkCapture;
