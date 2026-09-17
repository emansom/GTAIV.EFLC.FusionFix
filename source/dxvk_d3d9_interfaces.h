/* dxvk_d3d9_interfaces.h — minimal, faithful vendor of DXVK's D3D9<->Vulkan
 * interop declaration used purely for BACKEND DETECTION (DXVK vs native D3D9).
 *
 * Source of truth: DXVK's src/d3d9/d3d9_interfaces.h
 *   (~/Projects/dxvk-hdr-linux/dxvk-hdr-mingw/src/dxvk-hdrmod/src/d3d9/d3d9_interfaces.h).
 * We only need ID3D9VkExtInterface's IID: QueryInterface() for it succeeds on a
 * DXVK D3D9 device/interface and fails on the native Microsoft/WineD3D runtime.
 * IID { 65b55086-e3e3-4c3e-b3a0-86815cce2c4c } — verified in the research doc and
 * the DLSS-IV reimpl (dxvk_interop.h), Frida-confirmed live on this rig.
 *
 * Vulkan handle types are declared as opaque void* so this header needs no
 * <vulkan.h>. We never CALL the interface methods (detection only Queries it),
 * so the exact prototype only has to keep the vtable shape; the IID is all that
 * matters. Guarded for both MSVC (__uuidof / DECLSPEC_UUID) and MinGW
 * (__CRT_UUID_DECL), matching how DXVK associates its own IIDs.
 */
#ifndef FUSIONFIX_DXVK_D3D9_INTERFACES_H
#define FUSIONFIX_DXVK_D3D9_INTERFACES_H

#include <unknwn.h>  /* IUnknown, GUID */

/* Raw IID for callers that pass a GUID directly to QueryInterface (no __uuidof
 * required). Definition kept in one translation unit via the inline-const idiom. */
static const GUID FusionFix_IID_ID3D9VkExtInterface =
    { 0x65b55086, 0xe3e3, 0x4c3e, { 0xb3, 0xa0, 0x86, 0x81, 0x5c, 0xce, 0x2c, 0x4c } };

/* Opaque Vulkan handles — we never dereference or call, so void* is sufficient
 * and avoids a Vulkan SDK dependency in the ASI build. */
typedef void* FusionFix_VkInstance;
typedef void* FusionFix_VkPhysicalDevice;

#if defined(_MSC_VER)
struct __declspec(uuid("65b55086-e3e3-4c3e-b3a0-86815cce2c4c")) ID3D9VkExtInterface;
#endif

struct ID3D9VkExtInterface : public IUnknown {
    virtual void STDMETHODCALLTYPE GetInstanceHandle(FusionFix_VkInstance* pInstance) = 0;
    virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(FusionFix_VkPhysicalDevice* pPhysDev) = 0;
};

#if !defined(_MSC_VER) && defined(__CRT_UUID_DECL)
__CRT_UUID_DECL(ID3D9VkExtInterface, 0x65b55086, 0xe3e3, 0x4c3e, 0xb3, 0xa0, 0x86, 0x81, 0x5c, 0xce, 0x2c, 0x4c);
#endif

#endif /* FUSIONFIX_DXVK_D3D9_INTERFACES_H */
