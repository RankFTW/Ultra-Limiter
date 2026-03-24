#pragma once
// ReLimiter — Vulkan Reflex backend (VK_NV_low_latency2)
// Clean-room implementation from public Vulkan specification:
//   - Khronos Vulkan Registry: VK_NV_low_latency2 extension (MIT)
//   - vkSetLatencySleepModeNV, vkLatencySleepNV, vkGetLatencyTimingsNV,
//     vkSetLatencyMarkerNV function signatures and struct layouts
// No code from any other project.

#include "ul_reflex.hpp"  // NvSleepParams, NvLatencyResult, LatencyMarker, etc.
#include <atomic>
#include <cstdint>

// Vulkan handle types (opaque pointers — no Vulkan SDK dependency)
// These match the Vulkan spec's non-dispatchable handle definitions.
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;

VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)

// VkDevice is a dispatchable handle (pointer-sized)
typedef struct VkDevice_T* VkDevice;

// Vulkan base types
typedef uint32_t VkBool32;
typedef int32_t  VkResult;
typedef uint32_t VkStructureType;
typedef uint32_t VkFlags;

// VkResult codes we care about
constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_NOT_READY = 1;
constexpr VkResult VK_ERROR_EXTENSION_NOT_PRESENT = -7;

// VkStructureType values for VK_NV_low_latency2
// From the Vulkan registry (MIT-licensed headers)
constexpr VkStructureType VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV = 1000505000;
constexpr VkStructureType VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV = 1000505002;
constexpr VkStructureType VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV = 1000505006;
constexpr VkStructureType VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV = 1000505004;
constexpr VkStructureType VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV = 1000505005;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO = 1000207002;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9;
constexpr VkStructureType VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO = 1000207004;

// VkSemaphoreType
constexpr uint32_t VK_SEMAPHORE_TYPE_TIMELINE = 1;

// VkLatencyMarkerNV — maps 1:1 with our LatencyMarker enum
typedef uint32_t VkLatencyMarkerNV;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_SIMULATION_START_NV = 0;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_SIMULATION_END_NV = 1;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_RENDERSUBMIT_START_NV = 2;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_RENDERSUBMIT_END_NV = 3;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_PRESENT_START_NV = 4;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_PRESENT_END_NV = 5;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_INPUT_SAMPLE_NV = 6;
constexpr VkLatencyMarkerNV VK_LATENCY_MARKER_TRIGGER_FLASH_NV = 7;

// ============================================================================
// VK_NV_low_latency2 structures (from Vulkan spec, MIT)
// ============================================================================

struct VkLatencySleepModeInfoNV {
    VkStructureType sType;
    const void*     pNext;
    VkBool32        lowLatencyMode;
    VkBool32        lowLatencyBoost;
    uint32_t        minimumIntervalUs;
};

struct VkLatencySleepInfoNV {
    VkStructureType sType;
    const void*     pNext;
    VkSemaphore     signalSemaphore;
    uint64_t        value;
};

struct VkSetLatencyMarkerInfoNV {
    VkStructureType    sType;
    const void*        pNext;
    uint64_t           presentID;
    VkLatencyMarkerNV  marker;
};

struct VkLatencyTimingsFrameReportNV {
    VkStructureType sType;
    void*           pNext;
    uint64_t        presentID;
    uint64_t        inputSampleTimeUs;
    uint64_t        simStartTimeUs;
    uint64_t        simEndTimeUs;
    uint64_t        renderSubmitStartTimeUs;
    uint64_t        renderSubmitEndTimeUs;
    uint64_t        presentStartTimeUs;
    uint64_t        presentEndTimeUs;
    uint64_t        driverStartTimeUs;
    uint64_t        driverEndTimeUs;
    uint64_t        osRenderQueueStartTimeUs;
    uint64_t        osRenderQueueEndTimeUs;
    uint64_t        gpuRenderStartTimeUs;
    uint64_t        gpuRenderEndTimeUs;
};

struct VkGetLatencyMarkerInfoNV {
    VkStructureType                  sType;
    const void*                      pNext;
    uint32_t                         timingCount;
    VkLatencyTimingsFrameReportNV*   pTimings;
};

// Semaphore creation structs (Vulkan 1.2 timeline semaphores)
struct VkSemaphoreCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
};

struct VkSemaphoreTypeCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        semaphoreType;  // VK_SEMAPHORE_TYPE_TIMELINE
    uint64_t        initialValue;
};

struct VkSemaphoreWaitInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkFlags            flags;
    uint32_t           semaphoreCount;
    const VkSemaphore* pSemaphores;
    const uint64_t*    pValues;
};

// ============================================================================
// Function pointer types for VK_NV_low_latency2
// ============================================================================

using PFN_vkSetLatencySleepModeNV = VkResult(*)(VkDevice, VkSwapchainKHR, const VkLatencySleepModeInfoNV*);
using PFN_vkLatencySleepNV = VkResult(*)(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV*);
using PFN_vkSetLatencyMarkerNV = void(*)(VkDevice, VkSwapchainKHR, const VkSetLatencyMarkerInfoNV*);
using PFN_vkGetLatencyTimingsNV = void(*)(VkDevice, VkSwapchainKHR, VkGetLatencyMarkerInfoNV*);

// Vulkan core functions we need
using PFN_vkGetDeviceProcAddr = void*(*)(VkDevice, const char*);
using PFN_vkCreateSemaphore = VkResult(*)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*);
using PFN_vkDestroySemaphore = void(*)(VkDevice, VkSemaphore, const void*);
using PFN_vkWaitSemaphores = VkResult(*)(VkDevice, const VkSemaphoreWaitInfo*, uint64_t);

// VkPhysicalDevice is a dispatchable handle
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkInstance_T* VkInstance;

// Minimal VkDeviceCreateInfo (from vulkan_core.h, MIT)
struct VkDeviceQueueCreateInfo;  // opaque — we don't modify it
struct VkDeviceCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkFlags                     flags;
    uint32_t                    queueCreateInfoCount;
    const void*                 pQueueCreateInfos;
    uint32_t                    enabledLayerCount;
    const char* const*          ppEnabledLayerNames;
    uint32_t                    enabledExtensionCount;
    const char* const*          ppEnabledExtensionNames;
    const void*                 pEnabledFeatures;
};

// VkExtensionProperties (from vulkan_core.h, MIT)
struct VkExtensionProperties {
    char     extensionName[256];
    uint32_t specVersion;
};

// Function pointer types for vkCreateDevice hook
using PFN_vkCreateDevice = VkResult(*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
using PFN_vkEnumerateDeviceExtensionProperties = VkResult(*)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkGetInstanceProcAddr = void*(*)(VkInstance, const char*);

// ============================================================================
// VkReflex — Vulkan low-latency backend
// ============================================================================

class VkReflex {
public:
    // Initialize from a VkDevice. Returns true if VK_NV_low_latency2 is available.
    bool Init(VkDevice device);

    // Attach to a swapchain. Must be called when the swapchain is created.
    bool AttachSwapchain(VkSwapchainKHR swapchain);

    // Detach from the current swapchain (call before swapchain destruction).
    void DetachSwapchain();

    void Shutdown();

    bool IsActive() const { return active_; }

    // Pacing interface — mirrors the DX Reflex API
    bool SetSleepMode(bool low_latency, bool boost, uint32_t interval_us);
    bool Sleep();
    void SetMarker(uint64_t present_id, VkLatencyMarkerNV marker);

    // Get latency timings — fills an NvLatencyResult for compatibility with
    // the existing pacing engine (same struct layout as NVAPI GetLatency).
    bool GetLatencyTimings(NvLatencyResult* out);

private:
    VkDevice device_ = nullptr;
    VkSwapchainKHR swapchain_ = 0;
    VkSemaphore sleep_semaphore_ = 0;
    uint64_t semaphore_value_ = 0;
    bool active_ = false;

    // VK_NV_low_latency2 function pointers
    PFN_vkSetLatencySleepModeNV pfn_SetLatencySleepMode_ = nullptr;
    PFN_vkLatencySleepNV pfn_LatencySleep_ = nullptr;
    PFN_vkSetLatencyMarkerNV pfn_SetLatencyMarker_ = nullptr;
    PFN_vkGetLatencyTimingsNV pfn_GetLatencyTimings_ = nullptr;

    // Core Vulkan functions
    PFN_vkCreateSemaphore pfn_CreateSemaphore_ = nullptr;
    PFN_vkDestroySemaphore pfn_DestroySemaphore_ = nullptr;
    PFN_vkWaitSemaphores pfn_WaitSemaphores_ = nullptr;
};

// Global Vulkan Reflex instance
extern VkReflex g_vk_reflex;

// ============================================================================
// vkCreateDevice hook — injects VK_NV_low_latency2 into the extension list
// Must be called early (DLL_PROCESS_ATTACH) before the game creates its device.
// ============================================================================

bool VkReflexHookCreateDevice();
void VkReflexUnhookCreateDevice();

// Returns true if the extension was successfully injected into the last
// vkCreateDevice call (i.e., the device should support low_latency2).
bool VkReflexExtensionInjected();
