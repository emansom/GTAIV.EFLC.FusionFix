module;

#include <common.hxx>
#include "vulkan/vulkan.h"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "pipelinekeys.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <unordered_map>

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
// ===========================================================================

class VkCapture
{
    static inline bool enabled = false;
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

        std::unique_ptr<Fossilize::DatabaseInterface> db;
        std::unique_ptr<Fossilize::StateRecorder>     recorder;
        std::string path;

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

    // ---- device-level wrappers (Fossilize layer/dispatch.cpp, normal mode) ----

    static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateGraphicsPipelines(device, cache, count, infos, alloc, out);
        // Recorded only on success: a VK_PIPELINE_COMPILE_REQUIRED probe creates
        // nothing, and DXVK records the pipeline when it then compiles it for real.
        if (res != VK_SUCCESS || !d->recorder) return res;
        for (uint32_t i = 0; i < count; i++)
        {
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
        if (res != VK_SUCCESS || !d->recorder) return res;
        for (uint32_t i = 0; i < count; i++)
        {
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
        if (res != VK_SUCCESS || !d->recorder) return res;

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
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_pipeline_layout(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateDescriptorSetLayout(device, info, alloc, out);
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
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_render_pass(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* alloc, VkRenderPass* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateRenderPass2(device, info, alloc, out);
        if (res == VK_SUCCESS && d->recorder)
            (d->recorder->record_render_pass2(*out, *info) ? d->other : d->failed)++;
        return res;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
        const VkAllocationCallbacks* alloc, VkRenderPass* out)
    {
        Device* d = Get(device);
        VkResult res = d->CreateRenderPass2KHR(device, info, alloc, out);
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

        d->path = PluginsDir() + "FusionFix.vkpipelines.foz";
        d->db.reset(Fossilize::create_stream_archive_database(d->path.c_str(), Fossilize::DatabaseMode::Append));
        if (!d->db) { Log("cannot open %s - not recording", d->path.c_str()); return res; }
        d->recorder = std::make_unique<Fossilize::StateRecorder>();
        d->recorder->set_database_enable_compression(true);
        d->recorder->set_database_enable_checksum(true);
        if (haveAppInfo && !d->recorder->record_application_info(appInfo))
            Log("could not record the application info");
        if (!d->recorder->record_physical_device_features(devicePNext))
            Log("could not record the device features");
        d->recorder->init_recording_thread(d->db.get());

        Log("device %p: recording to %s%s", (void*)dev, d->path.c_str(),
            d->usesIdentifiers ? " (with shader module identifiers)" : "");
        std::unique_lock lock(devLock);
        devices[dev] = std::move(d);
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
    }

public:
    VkCapture()
    {
        CIniReader ini("");
        enabled = ini.ReadInteger("SHADERS", "CaptureVulkanPipelines", 0) != 0;
        if (!enabled) return;

        // DXVK prefers winevulkan.dll and only falls back to vulkan-1.dll; under Wine,
        // vulkan-1.dll forwards to winevulkan.dll. Hook exactly the one DXVK uses.
        bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        static const wchar_t* lib = wine ? L"winevulkan.dll" : L"vulkan-1.dll";
        CallbackHandler::RegisterCallback(lib, [] { InstallHook(lib); });
    }
} VkCapture;
