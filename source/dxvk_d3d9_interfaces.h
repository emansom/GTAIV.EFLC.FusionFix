/* dxvk_d3d9_interfaces.h — the one piece of DXVK's D3D9<->Vulkan interop we use:
 * asking a D3D9 device whether it is DXVK, and if so which Vulkan driver is under it.
 *
 * Source of truth: upstream DXVK src/d3d9/d3d9_interfaces.h (checked against
 * doitsujin/dxvk 260237c, 2026-09-18).
 *
 * WHY THE DEVICE INTERFACE AND NOT ID3D9VkExtInterface
 *   This used to query the DEVICE for ID3D9VkExtInterface. DXVK only answers that on
 *   the IDirect3D9 object (d3d9_interface.cpp, D3D9InterfaceEx::QueryInterface); the
 *   device answers ID3D9VkInteropDevice (d3d9_device.cpp, D3D9DeviceEx::QueryInterface).
 *   So the query failed on every DXVK device and detection fell through to matching
 *   the adapter name, which says "RADV" on Linux and nothing DXVK-specific on
 *   Windows: the 2026-09-19 Windows run logged "backend = native" while running on
 *   DXVK. Loading DLL names are no better a tell -- FusionFix ships DXVK as
 *   vulkan.dll behind its own d3d9.dll forwarder, Proton ships it as d3d9.dll.
 *
 * WHY THE VULKAN DRIVER
 *   Under DXVK, D3DADAPTER_IDENTIFIER9::DriverVersion is a constant DXVK makes up
 *   (it read 32767.65535.65535.65535 on both machines), so it cannot tell two
 *   contributions apart. VkPhysicalDeviceDriverProperties can: "radv" + "Mesa ...",
 *   "AMD proprietary driver" + "25.x", "NVIDIA" + "58x.xx".
 *
 * No Vulkan SDK dependency: the handful of types needed are declared here. Only
 * the first vtable slot of ID3D9VkInteropDevice is declared because it is the only
 * one called; the IID and that slot's position are what have to match upstream.
 */
#ifndef FUSIONFIX_DXVK_D3D9_INTERFACES_H
#define FUSIONFIX_DXVK_D3D9_INTERFACES_H

#include <windows.h>
#include <unknwn.h>  /* IUnknown, GUID */
#include <cstdint>
#include <cstring>

static const GUID FusionFix_IID_ID3D9VkInteropDevice =
    { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };

/* Dispatchable Vulkan handles are pointers. */
typedef void* FusionFix_VkInstance;
typedef void* FusionFix_VkPhysicalDevice;
typedef void* FusionFix_VkDevice;

struct ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(
        FusionFix_VkInstance* pInstance,
        FusionFix_VkPhysicalDevice* pPhysDev,
        FusionFix_VkDevice* pDevice) = 0;
    /* GetSubmissionQueue, TransitionTextureLayout, ... follow upstream; never called. */
};

/* ---- the minimum of vulkan_core.h needed for vkGetPhysicalDeviceProperties2 ---- */
/* VKAPI_PTR is __stdcall on Win32 (vk_platform.h). It matters on this x86 target. */
typedef void (__stdcall* FusionFix_PFN_vkVoidFunction)(void);
typedef FusionFix_PFN_vkVoidFunction (__stdcall* FusionFix_PFN_vkGetInstanceProcAddr)(
    FusionFix_VkInstance, const char*);

struct FusionFix_VkPhysicalDeviceDriverProperties {
    uint32_t sType;              /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES */
    void*    pNext;
    uint32_t driverID;           /* VkDriverId */
    char     driverName[256];    /* VK_MAX_DRIVER_NAME_SIZE */
    char     driverInfo[256];    /* VK_MAX_DRIVER_INFO_SIZE */
    uint8_t  conformanceVersion[4];
};

/* VkPhysicalDeviceProperties begins { apiVersion, driverVersion, vendorID, deviceID,
 * deviceType, deviceName[256], ... } and is 8-aligned because VkPhysicalDeviceLimits
 * holds VkDeviceSize. Only that prefix is read; the buffer is oversized for the rest. */
struct FusionFix_VkPhysicalDeviceProperties2 {
    uint32_t sType;              /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 */
    void*    pNext;
    alignas(8) uint8_t properties[1024];
};
typedef void (__stdcall* FusionFix_PFN_vkGetPhysicalDeviceProperties2)(
    FusionFix_VkPhysicalDevice, FusionFix_VkPhysicalDeviceProperties2*);

struct FusionFixDxvkInfo {
    bool     isDxvk = false;
    bool     haveDriver = false;  /* false if the Vulkan side could not be queried */
    uint32_t apiVersion = 0;      /* VK_MAKE_API_VERSION-encoded */
    uint32_t driverVersion = 0;   /* vendor-encoded; driverInfo is the readable form */
    char     driverName[256] = {};
    char     driverInfo[256] = {};
};

/* True when `device` is a DXVK D3D9 device. Fills the Vulkan driver identity when the
 * Vulkan library DXVK already loaded can answer; never loads anything itself. */
static inline bool FusionFixQueryDxvk(IUnknown* device, FusionFixDxvkInfo* out)
{
    *out = FusionFixDxvkInfo{};
    if (!device) return false;

    ID3D9VkInteropDevice* interop = nullptr;
    if (FAILED(device->QueryInterface(FusionFix_IID_ID3D9VkInteropDevice, (void**)&interop)) || !interop)
        return false;
    out->isDxvk = true;

    FusionFix_VkInstance inst = nullptr;
    FusionFix_VkPhysicalDevice phys = nullptr;
    FusionFix_VkDevice vkdev = nullptr;
    interop->GetVulkanHandles(&inst, &phys, &vkdev);
    interop->Release();
    if (!inst || !phys) return true;

    /* Resolve through the same library DXVK created the instance with. DXVK tries
     * winevulkan.dll before vulkan-1.dll (src/vulkan/vulkan_loader.cpp), so under
     * Proton vulkan-1.dll is never loaded at all -- checking it alone found nothing. */
    HMODULE loader = GetModuleHandleW(L"winevulkan.dll");
    if (!loader) loader = GetModuleHandleW(L"vulkan-1.dll");
    if (!loader) return true;
    auto gipa = (FusionFix_PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
    if (!gipa) return true;
    auto getProps2 = (FusionFix_PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!getProps2) return true;   /* core in 1.1; DXVK requires 1.3, so not expected */

    FusionFix_VkPhysicalDeviceDriverProperties drv{};
    drv.sType = 1000196000;        /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES */
    FusionFix_VkPhysicalDeviceProperties2 props{};
    props.sType = 1000059001;      /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 */
    props.pNext = &drv;
    getProps2(phys, &props);

    memcpy(&out->apiVersion, props.properties + 0, 4);
    memcpy(&out->driverVersion, props.properties + 4, 4);
    drv.driverName[sizeof(drv.driverName) - 1] = '\0';
    drv.driverInfo[sizeof(drv.driverInfo) - 1] = '\0';
    strcpy_s(out->driverName, drv.driverName);
    strcpy_s(out->driverInfo, drv.driverInfo);
    out->haveDriver = out->driverName[0] != '\0';
    return true;
}

#endif /* FUSIONFIX_DXVK_D3D9_INTERFACES_H */
