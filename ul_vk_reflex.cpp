// ReLimiter — Vulkan Reflex backend (VK_NV_low_latency2)
// Clean-room implementation from public Vulkan specification:
//   - Khronos Vulkan Registry: VK_NV_low_latency2 extension (MIT)
//   - Vulkan 1.2 timeline semaphores
// No code from any other project.

#include "ul_vk_reflex.hpp"
#include "ul_log.hpp"

#include <cstring>

VkReflex g_vk_reflex;

bool VkReflex::Init(VkDevice device) {
    if (!device) return false;
    device_ = device;

    // Load vkGetDeviceProcAddr from vulkan-1.dll
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) {
        ul_log::Write("VkReflex::Init: vulkan-1.dll not loaded");
        return false;
    }

    auto vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
    if (!vkGetDeviceProcAddr) {
        ul_log::Write("VkReflex::Init: vkGetDeviceProcAddr not found");
        return false;
    }

    // Load VK_NV_low_latency2 functions
    pfn_SetLatencySleepMode_ = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
        vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV"));
    pfn_LatencySleep_ = reinterpret_cast<PFN_vkLatencySleepNV>(
        vkGetDeviceProcAddr(device, "vkLatencySleepNV"));
    pfn_SetLatencyMarker_ = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
        vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV"));
    pfn_GetLatencyTimings_ = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(
        vkGetDeviceProcAddr(device, "vkGetLatencyTimingsNV"));

    if (!pfn_SetLatencySleepMode_ || !pfn_LatencySleep_ ||
        !pfn_SetLatencyMarker_ || !pfn_GetLatencyTimings_) {
        ul_log::Write("VkReflex::Init: VK_NV_low_latency2 not available");
        return false;
    }

    // Load core Vulkan functions for timeline semaphore
    pfn_CreateSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(
        vkGetDeviceProcAddr(device, "vkCreateSemaphore"));
    pfn_DestroySemaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(
        vkGetDeviceProcAddr(device, "vkDestroySemaphore"));
    pfn_WaitSemaphores_ = reinterpret_cast<PFN_vkWaitSemaphores>(
        vkGetDeviceProcAddr(device, "vkWaitSemaphores"));

    if (!pfn_CreateSemaphore_ || !pfn_DestroySemaphore_ || !pfn_WaitSemaphores_) {
        ul_log::Write("VkReflex::Init: timeline semaphore functions not available");
        return false;
    }

    ul_log::Write("VkReflex::Init: VK_NV_low_latency2 loaded successfully");
    return true;
}

bool VkReflex::AttachSwapchain(VkSwapchainKHR swapchain) {
    if (!device_ || !pfn_SetLatencySleepMode_ || !swapchain) return false;

    swapchain_ = swapchain;
    semaphore_value_ = 0;

    // Create a timeline semaphore for vkLatencySleepNV signaling
    VkSemaphoreTypeCreateInfo type_info = {};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;

    VkSemaphoreCreateInfo sem_info = {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &type_info;
    sem_info.flags = 0;

    VkResult res = pfn_CreateSemaphore_(device_, &sem_info, nullptr, &sleep_semaphore_);
    if (res != VK_SUCCESS || !sleep_semaphore_) {
        ul_log::Write("VkReflex::AttachSwapchain: CreateSemaphore failed (%d)", res);
        return false;
    }

    // Enable low latency mode on this swapchain
    VkLatencySleepModeInfoNV mode = {};
    mode.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    mode.lowLatencyMode = 1;
    mode.lowLatencyBoost = 0;
    mode.minimumIntervalUs = 0;

    res = pfn_SetLatencySleepMode_(device_, swapchain_, &mode);
    if (res != VK_SUCCESS) {
        ul_log::Write("VkReflex::AttachSwapchain: SetLatencySleepMode failed (%d)", res);
        pfn_DestroySemaphore_(device_, sleep_semaphore_, nullptr);
        sleep_semaphore_ = 0;
        return false;
    }

    active_ = true;
    ul_log::Write("VkReflex::AttachSwapchain: active (swapchain=%llu)", swapchain);
    return true;
}

void VkReflex::DetachSwapchain() {
    if (!active_) return;

    // Do NOT call vkSetLatencySleepModeNV here — the swapchain is being
    // destroyed by the game/driver. Calling into it during destruction
    // causes crashes. The driver cleans up low-latency state automatically
    // when the swapchain is destroyed.

    // Destroy the timeline semaphore (safe — it's our own object)
    if (device_ && sleep_semaphore_ && pfn_DestroySemaphore_) {
        __try {
            pfn_DestroySemaphore_(device_, sleep_semaphore_, nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Semaphore may already be invalid if device is being torn down
        }
    }

    sleep_semaphore_ = 0;
    swapchain_ = 0;
    semaphore_value_ = 0;
    active_ = false;
    ul_log::Write("VkReflex::DetachSwapchain");
}

void VkReflex::Shutdown() {
    DetachSwapchain();
    device_ = nullptr;
    pfn_SetLatencySleepMode_ = nullptr;
    pfn_LatencySleep_ = nullptr;
    pfn_SetLatencyMarker_ = nullptr;
    pfn_GetLatencyTimings_ = nullptr;
    pfn_CreateSemaphore_ = nullptr;
    pfn_DestroySemaphore_ = nullptr;
    pfn_WaitSemaphores_ = nullptr;
}

bool VkReflex::SetSleepMode(bool low_latency, bool boost, uint32_t interval_us) {
    if (!active_ || !pfn_SetLatencySleepMode_) return false;

    VkLatencySleepModeInfoNV mode = {};
    mode.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    mode.lowLatencyMode = low_latency ? 1 : 0;
    mode.lowLatencyBoost = boost ? 1 : 0;
    mode.minimumIntervalUs = interval_us;

    VkResult res;
    __try {
        res = pfn_SetLatencySleepMode_(device_, swapchain_, &mode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }
    return (res == VK_SUCCESS);
}

bool VkReflex::Sleep() {
    if (!active_ || !pfn_LatencySleep_ || !pfn_WaitSemaphores_ || !sleep_semaphore_)
        return false;

    // Increment the timeline semaphore value for this sleep cycle
    semaphore_value_++;

    VkLatencySleepInfoNV sleep_info = {};
    sleep_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
    sleep_info.signalSemaphore = sleep_semaphore_;
    sleep_info.value = semaphore_value_;

    VkResult res;
    __try {
        // vkLatencySleepNV returns immediately — the driver will signal the
        // semaphore when it's time for the application to resume CPU work.
        res = pfn_LatencySleep_(device_, swapchain_, &sleep_info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }
    if (res != VK_SUCCESS) return false;

    // Wait on the timeline semaphore for the driver's wake signal.
    // Timeout: 100ms — prevents infinite hangs if the driver misbehaves.
    VkSemaphoreWaitInfo wait = {};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &sleep_semaphore_;
    wait.pValues = &semaphore_value_;

    constexpr uint64_t kTimeoutNs = 100'000'000ULL;  // 100ms
    __try {
        res = pfn_WaitSemaphores_(device_, &wait, kTimeoutNs);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }
    return (res == VK_SUCCESS);
}

void VkReflex::SetMarker(uint64_t present_id, VkLatencyMarkerNV marker) {
    if (!active_ || !pfn_SetLatencyMarker_) return;

    VkSetLatencyMarkerInfoNV info = {};
    info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
    info.presentID = present_id;
    info.marker = marker;

    __try {
        pfn_SetLatencyMarker_(device_, swapchain_, &info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
    }
}

bool VkReflex::GetLatencyTimings(NvLatencyResult* out) {
    if (!active_ || !pfn_GetLatencyTimings_ || !out) return false;

    // First query: get the count of available timing reports
    VkGetLatencyMarkerInfoNV marker_info = {};
    marker_info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
    marker_info.timingCount = 0;
    marker_info.pTimings = nullptr;

    __try {
        pfn_GetLatencyTimings_(device_, swapchain_, &marker_info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }

    if (marker_info.timingCount == 0) return false;

    // Cap to 64 reports (matches NvLatencyResult's frameReport array size)
    uint32_t count = marker_info.timingCount;
    if (count > 64) count = 64;

    // Allocate VK frame reports on the stack
    VkLatencyTimingsFrameReportNV vk_reports[64] = {};
    for (uint32_t i = 0; i < count; i++) {
        vk_reports[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
        vk_reports[i].pNext = nullptr;
    }

    marker_info.timingCount = count;
    marker_info.pTimings = vk_reports;

    __try {
        pfn_GetLatencyTimings_(device_, swapchain_, &marker_info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }

    // Convert VK frame reports → NvLatencyResult for compatibility with
    // the existing pacing engine. The VK struct uses microsecond timestamps
    // directly, while NVAPI uses QPC-based timestamps. The pacing engine
    // only uses deltas between timestamps, so the absolute base doesn't matter
    // as long as it's consistent within a report.
    memset(out, 0, sizeof(*out));
    out->version = NV_LATENCY_RESULT_VER;

    uint32_t actual = marker_info.timingCount;
    if (actual > 64) actual = 64;

    for (uint32_t i = 0; i < actual; i++) {
        auto& vk = vk_reports[i];
        auto& nv = out->frameReport[i];

        nv.frameID = vk.presentID;
        nv.inputSampleTime = vk.inputSampleTimeUs;
        nv.simStartTime = vk.simStartTimeUs;
        nv.simEndTime = vk.simEndTimeUs;
        nv.renderSubmitStartTime = vk.renderSubmitStartTimeUs;
        nv.renderSubmitEndTime = vk.renderSubmitEndTimeUs;
        nv.presentStartTime = vk.presentStartTimeUs;
        nv.presentEndTime = vk.presentEndTimeUs;
        nv.driverStartTime = vk.driverStartTimeUs;
        nv.driverEndTime = vk.driverEndTimeUs;
        nv.osRenderQueueStartTime = vk.osRenderQueueStartTimeUs;
        nv.osRenderQueueEndTime = vk.osRenderQueueEndTimeUs;
        nv.gpuRenderStartTime = vk.gpuRenderStartTimeUs;
        nv.gpuRenderEndTime = vk.gpuRenderEndTimeUs;

        // Compute gpuActiveRenderTimeUs from GPU render start/end
        if (vk.gpuRenderEndTimeUs > vk.gpuRenderStartTimeUs) {
            uint64_t delta = vk.gpuRenderEndTimeUs - vk.gpuRenderStartTimeUs;
            nv.gpuActiveRenderTimeUs = (delta < 200'000) ? static_cast<NvU32>(delta) : 0;
        }
    }

    return true;
}

// ============================================================================
// vkCreateDevice hook — injects VK_NV_low_latency2 into the extension list
// ============================================================================

#include <MinHook.h>
#include <vector>

static PFN_vkCreateDevice s_orig_vkCreateDevice = nullptr;
static std::atomic<bool> s_extension_injected{false};
static std::atomic<bool> s_hook_installed{false};

// Check if the physical device supports VK_NV_low_latency2
static bool PhysicalDeviceSupportsLL2(VkPhysicalDevice physDev) {
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return false;

    auto vkEnumExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        GetProcAddress(vk_dll, "vkEnumerateDeviceExtensionProperties"));
    if (!vkEnumExts) return false;

    uint32_t count = 0;
    if (vkEnumExts(physDev, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;

    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumExts(physDev, nullptr, &count, exts.data()) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, "VK_NV_low_latency2") == 0)
            return true;
    }
    return false;
}

static VkResult Hooked_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const void* pAllocator,
    VkDevice* pDevice)
{
    if (!pCreateInfo || !s_orig_vkCreateDevice) {
        return s_orig_vkCreateDevice
            ? s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice)
            : VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    // Check if the game already enabled VK_NV_low_latency2
    bool already_enabled = false;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        if (pCreateInfo->ppEnabledExtensionNames[i] &&
            strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_NV_low_latency2") == 0) {
            already_enabled = true;
            break;
        }
    }

    if (already_enabled) {
        ul_log::Write("VkHook: game already enables VK_NV_low_latency2");
        s_extension_injected.store(true, std::memory_order_relaxed);
        return s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    // Check if the physical device supports the extension
    if (!PhysicalDeviceSupportsLL2(physicalDevice)) {
        ul_log::Write("VkHook: VK_NV_low_latency2 not supported by physical device");
        return s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    // Inject VK_NV_low_latency2 into the extension list
    ul_log::Write("VkHook: injecting VK_NV_low_latency2 into vkCreateDevice");

    uint32_t new_count = pCreateInfo->enabledExtensionCount + 1;
    std::vector<const char*> ext_names(new_count);
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
        ext_names[i] = pCreateInfo->ppEnabledExtensionNames[i];
    ext_names[pCreateInfo->enabledExtensionCount] = "VK_NV_low_latency2";

    // Make a shallow copy of VkDeviceCreateInfo with the new extension list
    VkDeviceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = new_count;
    modified.ppEnabledExtensionNames = ext_names.data();

    VkResult res = s_orig_vkCreateDevice(physicalDevice, &modified, pAllocator, pDevice);
    if (res == VK_SUCCESS) {
        s_extension_injected.store(true, std::memory_order_relaxed);
        ul_log::Write("VkHook: VK_NV_low_latency2 injected successfully");
    } else {
        ul_log::Write("VkHook: vkCreateDevice with injected ext failed (%d), retrying without", res);
        // Fall back to original call without injection
        res = s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    return res;
}

bool VkReflexHookCreateDevice() {
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) {
        // Vulkan not loaded yet — that's fine, game might be DX
        return false;
    }

    auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateDevice"));
    if (!target) {
        ul_log::Write("VkReflexHookCreateDevice: vkCreateDevice not found");
        return false;
    }

    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&Hooked_vkCreateDevice),
                                  reinterpret_cast<void**>(&s_orig_vkCreateDevice));
    if (st != MH_OK) {
        ul_log::Write("VkReflexHookCreateDevice: MH_CreateHook failed (%d)", st);
        return false;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK) {
        ul_log::Write("VkReflexHookCreateDevice: MH_EnableHook failed (%d)", st);
        MH_RemoveHook(target);
        return false;
    }

    s_hook_installed.store(true, std::memory_order_relaxed);
    ul_log::Write("VkReflexHookCreateDevice: hook installed");
    return true;
}

void VkReflexUnhookCreateDevice() {
    if (!s_hook_installed.load(std::memory_order_relaxed)) return;

    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (vk_dll) {
        auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateDevice"));
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }
    s_hook_installed.store(false, std::memory_order_relaxed);
    s_orig_vkCreateDevice = nullptr;
    ul_log::Write("VkReflexUnhookCreateDevice: hook removed");
}

bool VkReflexExtensionInjected() {
    return s_extension_injected.load(std::memory_order_relaxed);
}
