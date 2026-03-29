// ReLimiter — Vulkan Reflex backend (VK_NV_low_latency2)
// Clean-room implementation from public Vulkan specification:
//   - Khronos Vulkan Registry: VK_NV_low_latency2 extension (MIT)
//   - Vulkan 1.2 timeline semaphores
// No code from any other project.

#include "ul_vk_reflex.hpp"
#include "ul_reflex.hpp"   // g_ring, kRingSize, kMarkerCount, MarkerCb, LatencyMarker
#include "ul_timing.hpp"   // ul_timing::NowNs
#include "ul_log.hpp"

#include <MinHook.h>
#include <cstring>
#include <vector>

VkReflex g_vk_reflex;

// Forward declaration — defined early so VkReflex::Init can use the trampoline
// to resolve real (unhooked) driver functions.
static PFN_vkGetDeviceProcAddr s_orig_vkGetDeviceProcAddr = nullptr;

bool VkReflex::Init(VkDevice device) {
    if (!device) return false;
    device_ = device;

    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) {
        ul_log::Write("VkReflex::Init: vulkan-1.dll not loaded");
        return false;
    }

    // Use the original (unhooked) vkGetDeviceProcAddr if available, so we
    // get the real driver functions rather than our own wrappers.
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = s_orig_vkGetDeviceProcAddr;
    if (!vkGetDeviceProcAddr) {
        vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
    }
    if (!vkGetDeviceProcAddr) {
        ul_log::Write("VkReflex::Init: vkGetDeviceProcAddr not found");
        return false;
    }

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

    pfn_CreateSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(
        vkGetDeviceProcAddr(device, "vkCreateSemaphore"));
    pfn_DestroySemaphore_ = reinterpret_cast<PFN_vkDestroySemaphore>(
        vkGetDeviceProcAddr(device, "vkDestroySemaphore"));
    pfn_WaitSemaphores_ = reinterpret_cast<PFN_vkWaitSemaphores>(
        vkGetDeviceProcAddr(device, "vkWaitSemaphores"));

    // Optional — used for semaphore recovery if a wait fails or times out
    pfn_GetSemaphoreCounterValue_ = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
        vkGetDeviceProcAddr(device, "vkGetSemaphoreCounterValue"));

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

    // Don't call SetSleepMode here — defer to DoReflexSleep on first present.
    // Calling it eagerly can conflict with games that manage their own Reflex.

    active_ = true;
    ul_log::Write("VkReflex::AttachSwapchain: active (swapchain=%llu)", swapchain);
    return true;
}

void VkReflex::DetachSwapchain() {
    if (!active_) return;

    if (device_ && sleep_semaphore_ && pfn_DestroySemaphore_) {
        __try {
            pfn_DestroySemaphore_(device_, sleep_semaphore_, nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
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

    semaphore_value_++;

    VkLatencySleepInfoNV sleep_info = {};
    sleep_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
    sleep_info.signalSemaphore = sleep_semaphore_;
    sleep_info.value = semaphore_value_;

    VkResult res;
    __try {
        res = pfn_LatencySleep_(device_, swapchain_, &sleep_info);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        active_ = false;
        return false;
    }
    if (res != VK_SUCCESS) return false;

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

    // Recovery: if the wait timed out or failed, re-sync our tracked value
    // with the driver's actual semaphore counter. This handles missed signals
    // from crashes, device transitions, or driver hiccups.
    if (res != VK_SUCCESS && pfn_GetSemaphoreCounterValue_) {
        uint64_t actual_val = 0;
        __try {
            if (pfn_GetSemaphoreCounterValue_(device_, sleep_semaphore_, &actual_val) == VK_SUCCESS) {
                semaphore_value_ = actual_val;
                ul_log::Write("VkReflex::Sleep: semaphore re-synced to %llu after wait failure", actual_val);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
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

    uint32_t count = marker_info.timingCount;
    if (count > 64) count = 64;

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

        if (vk.gpuRenderEndTimeUs > vk.gpuRenderStartTimeUs) {
            uint64_t delta = vk.gpuRenderEndTimeUs - vk.gpuRenderStartTimeUs;
            nv.gpuActiveRenderTimeUs = (delta < 200'000) ? static_cast<NvU32>(delta) : 0;
        }
    }

    return true;
}

// ============================================================================
// Native Reflex detection — hooks vkGetDeviceProcAddr to intercept the game's
// resolution of VK_NV_low_latency2 functions. When the game asks for
// vkSetLatencySleepModeNV, we return a wrapper that sets g_game_uses_reflex
// and forwards to the real function.
// ============================================================================

static PFN_vkSetLatencySleepModeNV s_real_vkSetLatencySleepMode = nullptr;
static PFN_vkSetLatencyMarkerNV s_real_vkSetLatencyMarker = nullptr;
static PFN_vkLatencySleepNV s_real_vkLatencySleep = nullptr;
static MarkerCb s_vk_marker_cb = nullptr;
static std::atomic<bool> s_gdpa_hook_installed{false};

static VkResult Wrapped_vkSetLatencySleepModeNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkLatencySleepModeInfoNV* pSleepModeInfo)
{
    if (!g_game_uses_reflex.load(std::memory_order_relaxed)) {
        g_game_uses_reflex.store(true, std::memory_order_relaxed);
        ul_log::Write("VkHook: game uses native Reflex (vkSetLatencySleepModeNV)");
    }
    // Forward to driver — the game's SetSleepMode enables the low-latency
    // pipeline which DLSS FG depends on. We override the interval later via
    // MaybeUpdateSleepMode, but the initial enable must reach the driver.
    // This matches DX where Hook_SetSleepMode captures params but the limiter
    // still calls InvokeSetSleepMode (the real driver function) on its own.
    if (s_real_vkSetLatencySleepMode)
        return s_real_vkSetLatencySleepMode(device, swapchain, pSleepModeInfo);
    return VK_SUCCESS;
}

// Intercept vkLatencySleepNV: swallow the game's sleep call.
// Our grid (DoOwnSleep) handles all pacing. Matches DX Hook_Sleep behavior.
// The driver's LL2 pipeline stays coherent because SetSleepMode is forwarded
// and markers flow — the sleep call itself is just the timing wait.
static VkResult Wrapped_vkLatencySleepNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkLatencySleepInfoNV* pSleepInfo)
{
    (void)device; (void)swapchain; (void)pSleepInfo;
    return VK_SUCCESS;
}

// Intercept vkSetLatencyMarkerNV: record timestamp in g_ring, fire callback,
// forward to driver. Mirrors the DX Hook_SetMarker logic for Vulkan native
// Reflex games so marker-based enforcement pacing works identically.
static void Wrapped_vkSetLatencyMarkerNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo)
{
    if (!pLatencyMarkerInfo) {
        if (s_real_vkSetLatencyMarker)
            s_real_vkSetLatencyMarker(device, swapchain, pLatencyMarkerInfo);
        return;
    }

    g_game_uses_reflex.store(true, std::memory_order_relaxed);

    // VkLatencyMarkerNV values map 1:1 with LatencyMarker enum (0-7)
    int mt = static_cast<int>(pLatencyMarkerInfo->marker);
    uint64_t fid = pLatencyMarkerInfo->presentID;

    // One-shot log to confirm markers are flowing
    static bool s_marker_logged = false;
    if (!s_marker_logged) {
        ul_log::Write("VkHook: first marker received (type=%d, presentID=%llu)", mt, fid);
        s_marker_logged = true;
    }

    // Record in ring buffer (same as DX Hook_SetMarker)
    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);

        // Dedup: skip if we already saw this marker for this frame
        if (g_ring[slot].seen_frame[mt].load(std::memory_order_relaxed) == fid) {
            return;
        }
        g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);
    }

    // For PRESENT_FINISH: forward to driver first, then notify callback.
    // For all others: notify callback first, then forward.
    bool forwarded = false;
    if (mt == static_cast<int>(PRESENT_FINISH)) {
        if (s_real_vkSetLatencyMarker)
            s_real_vkSetLatencyMarker(device, swapchain, pLatencyMarkerInfo);
        forwarded = true;
    }

    if (s_vk_marker_cb) s_vk_marker_cb(mt, fid);

    if (!forwarded && s_real_vkSetLatencyMarker)
        s_real_vkSetLatencyMarker(device, swapchain, pLatencyMarkerInfo);
}

static void* Hooked_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName && strcmp(pName, "vkSetLatencySleepModeNV") == 0) {
        if (!s_real_vkSetLatencySleepMode && s_orig_vkGetDeviceProcAddr) {
            s_real_vkSetLatencySleepMode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkSetLatencySleepMode)
            return reinterpret_cast<void*>(&Wrapped_vkSetLatencySleepModeNV);
    }
    if (pName && strcmp(pName, "vkLatencySleepNV") == 0) {
        if (!s_real_vkLatencySleep && s_orig_vkGetDeviceProcAddr) {
            s_real_vkLatencySleep = reinterpret_cast<PFN_vkLatencySleepNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkLatencySleep)
            return reinterpret_cast<void*>(&Wrapped_vkLatencySleepNV);
    }
    if (pName && strcmp(pName, "vkSetLatencyMarkerNV") == 0) {
        if (!s_real_vkSetLatencyMarker && s_orig_vkGetDeviceProcAddr) {
            s_real_vkSetLatencyMarker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkSetLatencyMarker)
            return reinterpret_cast<void*>(&Wrapped_vkSetLatencyMarkerNV);
    }
    return s_orig_vkGetDeviceProcAddr ? s_orig_vkGetDeviceProcAddr(device, pName) : nullptr;
}

static void HookVkGetDeviceProcAddr() {
    if (s_gdpa_hook_installed.load(std::memory_order_relaxed)) return;

    // Hook vkGetDeviceProcAddr in vulkan-1.dll (the loader).
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return;

    auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
    if (!target) return;

    MH_STATUS st = MH_CreateHook(target,
                                  reinterpret_cast<void*>(&Hooked_vkGetDeviceProcAddr),
                                  reinterpret_cast<void**>(&s_orig_vkGetDeviceProcAddr));
    if (st != MH_OK) {
        ul_log::Write("VkHook: MH_CreateHook vkGetDeviceProcAddr failed (%d)", st);
        return;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        ul_log::Write("VkHook: MH_EnableHook vkGetDeviceProcAddr failed (%d)", st);
        MH_RemoveHook(target);
        return;
    }
    s_gdpa_hook_installed.store(true, std::memory_order_relaxed);
    ul_log::Write("VkHook: vkGetDeviceProcAddr hook installed (vulkan-1.dll)");

    // NOTE: sl.interposer.dll hooks disabled — patching Streamline's exports
    // breaks DLSS FG. The vulkan-1.dll hook + deferred LL2 hooks are sufficient.
}

static void UnhookVkGetDeviceProcAddr() {
    if (!s_gdpa_hook_installed.load(std::memory_order_relaxed)) return;

    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (vk_dll) {
        auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }
    s_gdpa_hook_installed.store(false, std::memory_order_relaxed);
    s_orig_vkGetDeviceProcAddr = nullptr;
    s_real_vkSetLatencySleepMode = nullptr;
    s_real_vkSetLatencyMarker = nullptr;
    s_real_vkLatencySleep = nullptr;
}

// ============================================================================
// vkEnumerateDeviceExtensionProperties hook — strip VK_NV_present_metering
// on non-Blackwell GPUs. Present metering is the Vulkan equivalent of
// D3D12 flip metering (SetFlipConfig). On pre-Blackwell, it adds latency.
// ============================================================================

static PFN_vkEnumerateDeviceExtensionProperties s_orig_vkEnumExts = nullptr;
static std::atomic<bool> s_enum_exts_hooked{false};

static VkResult Hooked_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    VkResult res = s_orig_vkEnumExts(physicalDevice, pLayerName, pPropertyCount, pProperties);

    if (res == VK_SUCCESS && pProperties && pPropertyCount && !IsBlackwell()) {
        uint32_t count = *pPropertyCount;
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(pProperties[i].extensionName, "VK_NV_present_metering") == 0) {
                // Overwrite with an adjacent extension to hide it
                if (i > 0)
                    memcpy(&pProperties[i], &pProperties[i - 1], sizeof(VkExtensionProperties));
                else if (count > 1)
                    memcpy(&pProperties[i], &pProperties[i + 1], sizeof(VkExtensionProperties));
                static bool s_logged = false;
                if (!s_logged) {
                    ul_log::Write("VkHook: stripped VK_NV_present_metering (non-Blackwell)");
                    s_logged = true;
                }
                break;
            }
        }
    }

    return res;
}

static void HookVkEnumExtensions() {
    if (s_enum_exts_hooked.load(std::memory_order_relaxed)) return;

    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return;

    auto target = reinterpret_cast<void*>(
        GetProcAddress(vk_dll, "vkEnumerateDeviceExtensionProperties"));
    if (!target) return;

    MH_STATUS st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Hooked_vkEnumerateDeviceExtensionProperties),
        reinterpret_cast<void**>(&s_orig_vkEnumExts));
    if (st != MH_OK) return;
    st = MH_EnableHook(target);
    if (st != MH_OK) { MH_RemoveHook(target); return; }
    s_enum_exts_hooked.store(true, std::memory_order_relaxed);
    ul_log::Write("VkHook: vkEnumerateDeviceExtensionProperties hooked (present metering filter)");
}

static void UnhookVkEnumExtensions() {
    if (!s_enum_exts_hooked.load(std::memory_order_relaxed)) return;
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (vk_dll) {
        auto target = reinterpret_cast<void*>(
            GetProcAddress(vk_dll, "vkEnumerateDeviceExtensionProperties"));
        if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
    }
    s_enum_exts_hooked.store(false, std::memory_order_relaxed);
    s_orig_vkEnumExts = nullptr;
}

// ============================================================================
// vkCreateDevice hook — injects VK_NV_low_latency2 into the extension list
// ============================================================================

static PFN_vkCreateDevice s_orig_vkCreateDevice = nullptr;
static std::atomic<bool> s_extension_injected{false};
static std::atomic<bool> s_hook_installed{false};

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

// Hook the actual VK_NV_low_latency2 functions via MinHook after device creation.
// This catches all calls regardless of whether the game uses the global or
// device-level vkGetDeviceProcAddr dispatch.
static void HookLL2Functions(VkDevice device) {
    if (!device || !s_orig_vkGetDeviceProcAddr) return;

    // Resolve real function addresses via the trampoline (unhooked path)
    auto real_set_sleep = reinterpret_cast<void*>(
        s_orig_vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV"));
    auto real_sleep = reinterpret_cast<void*>(
        s_orig_vkGetDeviceProcAddr(device, "vkLatencySleepNV"));
    auto real_marker = reinterpret_cast<void*>(
        s_orig_vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV"));

    if (real_set_sleep && !s_real_vkSetLatencySleepMode) {
        MH_STATUS st = MH_CreateHook(real_set_sleep,
            reinterpret_cast<void*>(&Wrapped_vkSetLatencySleepModeNV),
            reinterpret_cast<void**>(&s_real_vkSetLatencySleepMode));
        if (st == MH_OK) {
            MH_EnableHook(real_set_sleep);
            ul_log::Write("VkHook: vkSetLatencySleepModeNV hooked via MinHook");
        } else {
            ul_log::Write("VkHook: MH_CreateHook vkSetLatencySleepModeNV failed (%d)", st);
        }
    }

    if (real_sleep && !s_real_vkLatencySleep) {
        MH_STATUS st = MH_CreateHook(real_sleep,
            reinterpret_cast<void*>(&Wrapped_vkLatencySleepNV),
            reinterpret_cast<void**>(&s_real_vkLatencySleep));
        if (st == MH_OK) {
            MH_EnableHook(real_sleep);
            ul_log::Write("VkHook: vkLatencySleepNV hooked via MinHook");
        } else {
            ul_log::Write("VkHook: MH_CreateHook vkLatencySleepNV failed (%d)", st);
        }
    }

    if (real_marker && !s_real_vkSetLatencyMarker) {
        MH_STATUS st = MH_CreateHook(real_marker,
            reinterpret_cast<void*>(&Wrapped_vkSetLatencyMarkerNV),
            reinterpret_cast<void**>(&s_real_vkSetLatencyMarker));
        if (st == MH_OK) {
            MH_EnableHook(real_marker);
            ul_log::Write("VkHook: vkSetLatencyMarkerNV hooked via MinHook");
        } else {
            ul_log::Write("VkHook: MH_CreateHook vkSetLatencyMarkerNV failed (%d)", st);
        }
    }
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

    // Build the extension list:
    // - Inject VK_NV_low_latency2 if not already present
    // - Inject VK_KHR_present_id for accurate frame-to-present mapping
    // - Inject VK_KHR_timeline_semaphore for our sleep semaphore
    bool has_ll2 = false;
    bool has_present_id = false;
    bool has_timeline_sem = false;

    std::vector<const char*> ext_names;
    ext_names.reserve(pCreateInfo->enabledExtensionCount + 3);
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char* name = pCreateInfo->ppEnabledExtensionNames[i];
        if (!name) continue;
        if (strcmp(name, "VK_NV_low_latency2") == 0) has_ll2 = true;
        if (strcmp(name, "VK_KHR_present_id") == 0) has_present_id = true;
        if (strcmp(name, "VK_KHR_timeline_semaphore") == 0) has_timeline_sem = true;
        ext_names.push_back(name);
    }

    if (has_ll2) {
        g_game_uses_reflex.store(true, std::memory_order_relaxed);
        ul_log::Write("VkHook: game already enables VK_NV_low_latency2 — native Reflex detected");
    } else if (PhysicalDeviceSupportsLL2(physicalDevice)) {
        // Check if Streamline is managing LL2 through its own layer.
        // Streamline (sl.common.dll) handles VK_NV_low_latency2 internally —
        // it adds LL2 support after device creation through its interposer,
        // not through the game's extension list. If we inject LL2 into the
        // extension list ourselves, both we and Streamline think we own the
        // LL2 state, which conflicts and prevents FG from initializing.
        //
        // When Streamline is present, skip injection and let Streamline
        // handle LL2 entirely. Our vkGetDeviceProcAddr hook still provides
        // native Reflex detection and marker interception.
        bool streamline_present = GetModuleHandleW(L"sl.common.dll") != nullptr;
        if (streamline_present) {
            ul_log::Write("VkHook: Streamline detected (sl.common.dll) — "
                          "skipping LL2 injection, Streamline manages LL2");
        } else {
            ext_names.push_back("VK_NV_low_latency2");
            ul_log::Write("VkHook: injecting VK_NV_low_latency2 into vkCreateDevice");
        }
    } else {
        ul_log::Write("VkHook: VK_NV_low_latency2 not supported by physical device");
        return s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    // Inject VK_KHR_present_id — accurate frame-to-present mapping for
    // GetLatencyTimings, especially under FG where presents interleave.
    if (!has_present_id) {
        auto check_fn = s_orig_vkEnumExts ? s_orig_vkEnumExts
            : reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),
                               "vkEnumerateDeviceExtensionProperties"));
        bool supported = false;
        if (check_fn) {
            uint32_t ec = 0;
            if (check_fn(physicalDevice, nullptr, &ec, nullptr) == VK_SUCCESS && ec > 0) {
                std::vector<VkExtensionProperties> ep(ec);
                if (check_fn(physicalDevice, nullptr, &ec, ep.data()) == VK_SUCCESS) {
                    for (uint32_t i = 0; i < ec; i++) {
                        if (strcmp(ep[i].extensionName, "VK_KHR_present_id") == 0) {
                            supported = true; break;
                        }
                    }
                }
            }
        }
        if (supported) {
            ext_names.push_back("VK_KHR_present_id");
            ul_log::Write("VkHook: injecting VK_KHR_present_id");
        }
    }

    // Inject VK_KHR_timeline_semaphore — required for our sleep semaphore.
    if (!has_timeline_sem) {
        ext_names.push_back("VK_KHR_timeline_semaphore");
        ul_log::Write("VkHook: injecting VK_KHR_timeline_semaphore");
    }

    VkDeviceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = static_cast<uint32_t>(ext_names.size());
    modified.ppEnabledExtensionNames = ext_names.data();

    VkResult res = s_orig_vkCreateDevice(physicalDevice, &modified, pAllocator, pDevice);
    if (res == VK_SUCCESS) {
        s_extension_injected.store(true, std::memory_order_relaxed);
        ul_log::Write("VkHook: vkCreateDevice succeeded (extensions: %u)", modified.enabledExtensionCount);

        // Only hook LL2 functions when WE injected the extension.
        // When Streamline manages LL2, don't patch its internal dispatch —
        // our MinHook patches on the LL2 functions would intercept
        // Streamline's internal calls and disrupt FG initialization.
        bool streamline_present = GetModuleHandleW(L"sl.common.dll") != nullptr;
        if (!streamline_present && pDevice && *pDevice)
            HookLL2Functions(*pDevice);
    } else {
        ul_log::Write("VkHook: vkCreateDevice with modified exts failed (%d), retrying original", res);
        res = s_orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    return res;
}

bool VkReflexHookCreateDevice() {
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return false;

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
    ul_log::Write("VkReflexHookCreateDevice: hook installed (vulkan-1.dll)");

    // NOTE: sl.interposer.dll hooks disabled — patching Streamline's exports
    // breaks DLSS FG.

    HookVkGetDeviceProcAddr();
    HookVkEnumExtensions();

    return true;
}

void VkReflexUnhookCreateDevice() {
    if (!s_hook_installed.load(std::memory_order_relaxed)) return;

    UnhookVkGetDeviceProcAddr();
    UnhookVkEnumExtensions();

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

void SetVkMarkerCb(MarkerCb cb) {
    s_vk_marker_cb = cb;
}

// ============================================================================
// Deferred hook attempt — called from OnInitSwapchain when the Vulkan device
// is known to exist. By this point sl.interposer.dll is loaded (if present)
// and the device has LL2 functions available. This catches the case where
// vkCreateDevice was called through Streamline before our DllMain hooks
// installed, so Hooked_vkCreateDevice never fired.
// ============================================================================

void VkReflexDeferredHook(VkDevice device) {
    if (!device) return;

    HMODULE sl_dll = GetModuleHandleW(L"sl.interposer.dll");
    if (sl_dll)
        ul_log::Write("VkDeferredHook: sl.interposer.dll detected");

    ul_log::Write("VkDeferredHook: LL2 MinHook disabled (FG compat)");
}
