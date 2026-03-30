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
        bool via_sl = GetModuleHandleW(L"sl.common.dll") != nullptr;
        ul_log::Write("VkHook: game uses native Reflex (vkSetLatencySleepModeNV, streamline=%s)",
                       via_sl ? "yes" : "no");
    }
    static uint64_t s_ssm_count = 0;
    s_ssm_count++;
    if (s_ssm_count <= 5 || (s_ssm_count % 100) == 0) {
        ul_log::Write("VkHook: SetSleepMode #%llu (ll=%d, boost=%d, interval=%u)",
                       s_ssm_count,
                       pSleepModeInfo ? pSleepModeInfo->lowLatencyMode : -1,
                       pSleepModeInfo ? pSleepModeInfo->lowLatencyBoost : -1,
                       pSleepModeInfo ? pSleepModeInfo->minimumIntervalUs : 0);
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

// Intercept vkLatencySleepNV: swallow the game's sleep call when we own
// pacing (no Streamline). When Streamline is present, forward to the real
// driver — Streamline's FG pipeline depends on the sleep actually blocking
// for synchronization. Swallowing it breaks Streamline's internal timing
// and causes it to stop issuing markers after the first frame.
static VkResult Wrapped_vkLatencySleepNV(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkLatencySleepInfoNV* pSleepInfo)
{
    static uint64_t s_sleep_count = 0;
    s_sleep_count++;
    if (s_sleep_count <= 5 || (s_sleep_count % 500) == 0) {
        ul_log::Write("VkHook: Sleep #%llu (swapchain=%llu)", s_sleep_count, swapchain);
    }
    if (GetModuleHandleW(L"sl.common.dll") != nullptr) {
        // Streamline present — forward sleep to driver
        if (s_real_vkLatencySleep)
            return s_real_vkLatencySleep(device, swapchain, pSleepInfo);
        return VK_SUCCESS;
    }
    // No Streamline — swallow (our grid handles pacing)
    (void)device; (void)swapchain; (void)pSleepInfo;
    return VK_SUCCESS;
}

// Intercept vkSetLatencyMarkerNV: record timestamp in g_ring, fire callback,
// forward to driver. Returned by Hooked_vkGetDeviceProcAddr when the game
// resolves this function — no MinHook on the function itself.
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

    // Periodic log to confirm markers are flowing
    static uint64_t s_marker_count = 0;
    s_marker_count++;
    if (s_marker_count <= 10 || (s_marker_count % 100) == 0) {
        ul_log::Write("VkHook: marker #%llu (type=%d, presentID=%llu, streamline=%s)",
                       s_marker_count, mt, fid,
                       GetModuleHandleW(L"sl.common.dll") ? "yes" : "no");
    }

    // Record in ring buffer (same as DX Hook_SetMarker)
    bool is_dup = false;
    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);

        if (g_ring[slot].seen_frame[mt].load(std::memory_order_relaxed) == fid) {
            is_dup = true;
        } else {
            g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);
        }
    }

    // For PRESENT_FINISH: forward to driver first, then notify callback.
    // For all others: notify callback first, then forward.
    bool forwarded = false;
    if (mt == static_cast<int>(PRESENT_FINISH)) {
        if (s_real_vkSetLatencyMarker)
            s_real_vkSetLatencyMarker(device, swapchain, pLatencyMarkerInfo);
        forwarded = true;
    }

    if (!is_dup && s_vk_marker_cb) s_vk_marker_cb(mt, fid);

    if (!forwarded && s_real_vkSetLatencyMarker)
        s_real_vkSetLatencyMarker(device, swapchain, pLatencyMarkerInfo);
}

static void* Hooked_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName && strcmp(pName, "vkSetLatencySleepModeNV") == 0) {
        if (!s_real_vkSetLatencySleepMode && s_orig_vkGetDeviceProcAddr) {
            s_real_vkSetLatencySleepMode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkSetLatencySleepMode) {
            ul_log::Write("VkHook: GDPA intercepted vkSetLatencySleepModeNV (device=%p)", device);
            return reinterpret_cast<void*>(&Wrapped_vkSetLatencySleepModeNV);
        }
    }
    if (pName && strcmp(pName, "vkLatencySleepNV") == 0) {
        if (!s_real_vkLatencySleep && s_orig_vkGetDeviceProcAddr) {
            s_real_vkLatencySleep = reinterpret_cast<PFN_vkLatencySleepNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkLatencySleep) {
            ul_log::Write("VkHook: GDPA intercepted vkLatencySleepNV (device=%p)", device);
            return reinterpret_cast<void*>(&Wrapped_vkLatencySleepNV);
        }
    }
    if (pName && strcmp(pName, "vkSetLatencyMarkerNV") == 0) {
        if (!s_real_vkSetLatencyMarker && s_orig_vkGetDeviceProcAddr) {
            s_real_vkSetLatencyMarker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
                s_orig_vkGetDeviceProcAddr(device, pName));
        }
        if (s_real_vkSetLatencyMarker) {
            ul_log::Write("VkHook: GDPA intercepted vkSetLatencyMarkerNV (device=%p)", device);
            return reinterpret_cast<void*>(&Wrapped_vkSetLatencyMarkerNV);
        }
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

// Hook the LL2 functions via MinHook for marker interception.
// Pure observation mode — all calls forwarded unmodified.
static void HookLL2Functions(VkDevice device) {
    if (!device || !s_orig_vkGetDeviceProcAddr) return;

    // Only resolve trampolines — no MinHook. The PCL hook handles marker
    // interception at the Streamline API level. MinHook on the ICD functions
    // breaks Streamline (markers stop after 1 frame, sleep forwarding causes
    // FPS lock to ~15).
    if (!s_real_vkSetLatencySleepMode)
        s_real_vkSetLatencySleepMode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
            s_orig_vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV"));
    if (!s_real_vkLatencySleep)
        s_real_vkLatencySleep = reinterpret_cast<PFN_vkLatencySleepNV>(
            s_orig_vkGetDeviceProcAddr(device, "vkLatencySleepNV"));
    if (!s_real_vkSetLatencyMarker)
        s_real_vkSetLatencyMarker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
            s_orig_vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV"));

    ul_log::Write("VkHook: HookLL2Functions resolved (setSleep=%p, sleep=%p, marker=%p)",
                   s_real_vkSetLatencySleepMode, s_real_vkLatencySleep, s_real_vkSetLatencyMarker);
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
        ext_names.push_back("VK_NV_low_latency2");
        ul_log::Write("VkHook: injecting VK_NV_low_latency2 into vkCreateDevice");

        // Check for VK_EXT_present_timing support
        {
            auto check_fn = s_orig_vkEnumExts ? s_orig_vkEnumExts
                : reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),
                                   "vkEnumerateDeviceExtensionProperties"));
            if (check_fn) {
                uint32_t ec = 0;
                if (check_fn(physicalDevice, nullptr, &ec, nullptr) == VK_SUCCESS && ec > 0) {
                    std::vector<VkExtensionProperties> ep(ec);
                    if (check_fn(physicalDevice, nullptr, &ec, ep.data()) == VK_SUCCESS) {
                        for (uint32_t i = 0; i < ec; i++) {
                            if (strcmp(ep[i].extensionName, "VK_EXT_present_timing") == 0) {
                                ul_log::Write("VkHook: VK_EXT_present_timing AVAILABLE (specVersion=%u)", ep[i].specVersion);
                                break;
                            }
                        }
                    }
                }
            }
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

    // Inject VK_EXT_present_timing — display-level frame pacing and feedback.
    {
        bool has_pt = false;
        bool has_pid2 = false;
        for (auto& n : ext_names) {
            if (n && strcmp(n, "VK_EXT_present_timing") == 0) has_pt = true;
            if (n && strcmp(n, "VK_KHR_present_id2") == 0) has_pid2 = true;
        }
        if (!has_pt) {
            auto check_fn = s_orig_vkEnumExts ? s_orig_vkEnumExts
                : reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),
                                   "vkEnumerateDeviceExtensionProperties"));
            bool pt_supported = false;
            bool pid2_supported = false;
            if (check_fn) {
                uint32_t ec = 0;
                if (check_fn(physicalDevice, nullptr, &ec, nullptr) == VK_SUCCESS && ec > 0) {
                    std::vector<VkExtensionProperties> ep(ec);
                    if (check_fn(physicalDevice, nullptr, &ec, ep.data()) == VK_SUCCESS) {
                        for (uint32_t i = 0; i < ec; i++) {
                            if (strcmp(ep[i].extensionName, "VK_EXT_present_timing") == 0) pt_supported = true;
                            if (strcmp(ep[i].extensionName, "VK_KHR_present_id2") == 0) pid2_supported = true;
                        }
                    }
                }
            }
            if (pt_supported) {
                ext_names.push_back("VK_EXT_present_timing");
                ul_log::Write("VkHook: injecting VK_EXT_present_timing");
            }
            if (pid2_supported && !has_pid2) {
                ext_names.push_back("VK_KHR_present_id2");
                ul_log::Write("VkHook: injecting VK_KHR_present_id2 (present_timing dependency)");
            }
        }
    }

    VkDeviceCreateInfo modified = *pCreateInfo;
    modified.enabledExtensionCount = static_cast<uint32_t>(ext_names.size());
    modified.ppEnabledExtensionNames = ext_names.data();

    VkResult res = s_orig_vkCreateDevice(physicalDevice, &modified, pAllocator, pDevice);
    if (res == VK_SUCCESS) {
        s_extension_injected.store(true, std::memory_order_relaxed);
        ul_log::Write("VkHook: vkCreateDevice succeeded (extensions: %u)", modified.enabledExtensionCount);

        // Hook LL2 functions via MinHook for marker interception.
        if (pDevice && *pDevice)
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
    VkPresentTimingHookCreateSwapchain();

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
// Streamline PCL marker hook — intercepts slPCLSetMarker at the Streamline
// API level. This works regardless of whether Streamline uses Vulkan LL2 or
// NVAPI internally, because we hook the game→Streamline boundary.
//
// Written from public Streamline SDK headers (MIT license):
//   - sl_pcl.h: PCLMarker enum, PFun_slPCLSetMarker signature
//   - sl_core_api.h: slGetFeatureFunction export
//   - sl_consts.h: kFeaturePCL = 4
// ============================================================================

// Minimal Streamline types — just enough to call slGetFeatureFunction and
// hook slPCLSetMarker without including the full Streamline SDK.
namespace sl_compat {
    using Result = int32_t;  // sl::Result is an enum, but ABI-compatible with int32_t
    constexpr int32_t eOk = 0;
    constexpr uint32_t kFeaturePCL = 4;

    // sl::FrameToken is an opaque struct — we just pass the reference through
    struct FrameToken;

    // PCLMarker values from sl_pcl.h (MIT)
    enum PCLMarker : uint32_t {
        eSimulationStart = 0,
        eSimulationEnd = 1,
        eRenderSubmitStart = 2,
        eRenderSubmitEnd = 3,
        ePresentStart = 4,
        ePresentEnd = 5,
        eTriggerFlash = 7,
        ePCLatencyPing = 8,
    };

    using PFun_slGetFeatureFunction = Result(uint32_t feature, const char* functionName, void*& function);
    using PFun_slPCLSetMarker = Result(uint32_t marker, const void* frame);
}

static sl_compat::PFun_slPCLSetMarker* s_orig_slPCLSetMarker = nullptr;
static std::atomic<bool> s_pcl_hook_installed{false};

// Streamline Reflex sleep hook — swallow the game's sleep call.
// Our limiter handles all pacing. Matches DX Hook_Sleep behavior.
using PFun_slReflexSleep = sl_compat::Result(const void* frame);
static PFun_slReflexSleep* s_orig_slReflexSleep = nullptr;
static const void* s_last_frame_token = nullptr;

static sl_compat::Result Hooked_slReflexSleep(const void* frame) {
    // Stash the frame token. The limiter will call InvokeStreamlineSleep
    // after its own grid pacing completes, so Streamline's internal state
    // advances on our schedule rather than the game's.
    if (frame) s_last_frame_token = frame;
    // Don't call the trampoline here — the limiter calls it via
    // InvokeStreamlineSleep after DoOwnSleep.
    return sl_compat::eOk;
}

// Invoke the real Streamline sleep (trampoline past our hook).
// Called by the limiter on its own schedule — equivalent to DX InvokeSleep.
bool InvokeStreamlineSleep() {
    if (!s_orig_slReflexSleep) return false;
    const void* token = s_last_frame_token;
    if (!token) return false;  // No frame token yet — game hasn't called sleep
    static bool s_logged = false;
    if (!s_logged) {
        ul_log::Write("PCLHook: InvokeStreamlineSleep first call");
        s_logged = true;
    }
    __try {
        s_orig_slReflexSleep(token);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ul_log::Write("PCLHook: InvokeStreamlineSleep crashed — disabling");
        s_orig_slReflexSleep = nullptr;
        return false;
    }
    return true;
}

// Streamline Reflex SetOptions hook — capture game's params, swallow the call.
// Our limiter controls sleep mode via VkReflex::SetSleepMode on its own schedule.
// Matches DX Hook_SetSleepMode behavior.
using PFun_slReflexSetOptions = sl_compat::Result(const void* options);
static PFun_slReflexSetOptions* s_orig_slReflexSetOptions = nullptr;
static const void* s_last_reflex_options = nullptr;

static sl_compat::Result Hooked_slReflexSetOptions(const void* options) {
    // Intercept and forward with frameLimitUs=0 to disable the driver's
    // frame rate cap. Our grid handles all pacing. Low latency mode stays on
    // so the driver's queue management and FG pipeline work correctly.
    g_game_uses_reflex.store(true, std::memory_order_relaxed);
    if (options) s_last_reflex_options = options;
    static bool s_logged = false;
    if (!s_logged) {
        ul_log::Write("PCLHook: slReflexSetOptions intercepted (interval zeroed)");
        s_logged = true;
    }
    // Forward with interval=0, lowLatency=on, boost=off
    return InvokeStreamlineSetOptions(0, true, false) ? sl_compat::eOk : sl_compat::eOk;
}

// Invoke the real slReflexSetOptions with our interval.
// Clones the game's last options struct and patches frameLimitUs.
// ReflexOptions layout (from sl_reflex.h, MIT):
//   BaseStructure (32 bytes): next(8) + structType(16) + structVersion(8)
//   ReflexMode mode (4 bytes) at offset 32
//   uint32_t frameLimitUs at offset 36
bool InvokeStreamlineSetOptions(uint32_t interval_us, bool low_latency, bool boost) {
    if (!s_orig_slReflexSetOptions || !s_last_reflex_options) return false;
    
    // Clone the game's options (up to 128 bytes covers the full struct + padding)
    alignas(16) char buf[128];
    memcpy(buf, s_last_reflex_options, sizeof(buf));
    
    // Patch mode at offset 32
    int mode = low_latency ? (boost ? 2 : 1) : 0;  // eOff=0, eLowLatency=1, eLowLatencyWithBoost=2
    memcpy(buf + 32, &mode, sizeof(int));
    
    // Patch frameLimitUs at offset 36
    memcpy(buf + 36, &interval_us, sizeof(uint32_t));
    
    __try {
        s_orig_slReflexSetOptions(buf);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ul_log::Write("PCLHook: InvokeStreamlineSetOptions crashed — disabling");
        s_orig_slReflexSetOptions = nullptr;
        return false;
    }
    return true;
}

static sl_compat::Result Hooked_slPCLSetMarker(uint32_t marker, const void* frame) {
    // Map PCLMarker to our LatencyMarker enum (values 0-7 match VkLatencyMarkerNV)
    int mt = static_cast<int>(marker);

    // Extract frame ID from the FrameToken via its virtual operator uint32_t().
    // FrameToken is a polymorphic struct with a vtable. The virtual function
    // is the first (and only) virtual method, so it's at vtable slot 0.
    // On MSVC x64, the vtable pointer is at offset 0 of the object.
    // We call through the vtable to get the frame index.
    uint64_t fid = 0;
    if (frame) {
        // vtable is at offset 0, first virtual function (operator uint32_t) is slot 0
        // But destructor is typically slot 0 in MSVC... let's use a safe approach:
        // Cast to a minimal struct with the same virtual layout
        struct FakeFrameToken {
            virtual ~FakeFrameToken() = default;
            virtual uint32_t GetIndex() const = 0;
        };
        // Actually, MSVC puts the destructor in the vtable only if there's a
        // virtual destructor. FrameToken inherits from BaseStructure which has
        // no virtual destructor. The only virtual is operator uint32_t().
        // So vtable[0] = operator uint32_t() const.
        //
        // Call convention: thiscall (rcx = this), returns uint32_t
        auto vtable = *reinterpret_cast<void* const* const*>(frame);
        using GetIndexFn = uint32_t(__thiscall*)(const void*);
        auto getIndex = reinterpret_cast<GetIndexFn>(vtable[0]);
        __try {
            fid = static_cast<uint64_t>(getIndex(frame));
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fid = 0;
        }
    }

    g_game_uses_reflex.store(true, std::memory_order_relaxed);

    static uint64_t s_pcl_count = 0;
    s_pcl_count++;
    if (s_pcl_count <= 10 || (s_pcl_count % 500) == 0) {
        ul_log::Write("PCLHook: marker #%llu (type=%d, frame=%llu)", s_pcl_count, mt, fid);
    }

    // Only process markers in the VkLatencyMarkerNV range (0-7) for pacing.
    // PCL has additional marker types (8+) that don't map to LL2 markers.
    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);

        // Dedup: skip if we already saw this marker for this frame
        if (g_ring[slot].seen_frame[mt].load(std::memory_order_relaxed) == fid) {
            // Still forward to Streamline
            if (s_orig_slPCLSetMarker)
                return s_orig_slPCLSetMarker(marker, frame);
            return 0;
        }
        g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);

        // PRESENT_FINISH: forward to Streamline first, then notify callback.
        // All others: notify callback first, then forward.
        // This matches the DX Hook_SetMarker ordering.
        if (mt == static_cast<int>(PRESENT_FINISH)) {
            sl_compat::Result ret = 0;
            if (s_orig_slPCLSetMarker)
                ret = s_orig_slPCLSetMarker(marker, frame);
            if (s_vk_marker_cb) s_vk_marker_cb(mt, fid);
            return ret;
        }

        if (s_vk_marker_cb) s_vk_marker_cb(mt, fid);
    }

    // Forward to real Streamline function
    if (s_orig_slPCLSetMarker)
        return s_orig_slPCLSetMarker(marker, frame);
    return 0;
}

bool HookStreamlinePCL() {
    if (s_pcl_hook_installed.load(std::memory_order_relaxed)) return true;

    HMODULE sl_dll = GetModuleHandleW(L"sl.interposer.dll");
    if (!sl_dll) {
        ul_log::Write("PCLHook: sl.interposer.dll not loaded");
        return false;
    }

    auto slGetFeatureFunction = reinterpret_cast<sl_compat::PFun_slGetFeatureFunction*>(
        GetProcAddress(sl_dll, "slGetFeatureFunction"));
    if (!slGetFeatureFunction) {
        ul_log::Write("PCLHook: slGetFeatureFunction not found");
        return false;
    }

    void* pcl_func = nullptr;
    sl_compat::Result res = slGetFeatureFunction(sl_compat::kFeaturePCL, "slPCLSetMarker", pcl_func);
    if (res != sl_compat::eOk || !pcl_func) {
        ul_log::Write("PCLHook: slGetFeatureFunction(PCL, slPCLSetMarker) failed (res=%d)", res);
        return false;
    }

    ul_log::Write("PCLHook: slPCLSetMarker resolved at %p", pcl_func);

    MH_STATUS st = MH_CreateHook(pcl_func,
        reinterpret_cast<void*>(&Hooked_slPCLSetMarker),
        reinterpret_cast<void**>(&s_orig_slPCLSetMarker));
    if (st != MH_OK) {
        ul_log::Write("PCLHook: MH_CreateHook failed (%d)", st);
        return false;
    }
    st = MH_EnableHook(pcl_func);
    if (st != MH_OK) {
        ul_log::Write("PCLHook: MH_EnableHook failed (%d)", st);
        MH_RemoveHook(pcl_func);
        return false;
    }

    s_pcl_hook_installed.store(true, std::memory_order_relaxed);
    ul_log::Write("PCLHook: slPCLSetMarker hooked OK");

    // Hook slReflexSleep — swallow the game's sleep call so our limiter
    // owns all pacing. Matches DX Hook_Sleep behavior.
    void* sleep_func = nullptr;
    constexpr uint32_t kFeatureReflex = 3;
    res = slGetFeatureFunction(kFeatureReflex, "slReflexSleep", sleep_func);
    if (res == sl_compat::eOk && sleep_func) {
        ul_log::Write("PCLHook: slReflexSleep resolved at %p", sleep_func);
        st = MH_CreateHook(sleep_func,
            reinterpret_cast<void*>(&Hooked_slReflexSleep),
            reinterpret_cast<void**>(&s_orig_slReflexSleep));
        if (st == MH_OK) {
            st = MH_EnableHook(sleep_func);
            if (st == MH_OK) {
                ul_log::Write("PCLHook: slReflexSleep hooked OK (swallowed)");
            } else {
                MH_RemoveHook(sleep_func);
                ul_log::Write("PCLHook: MH_EnableHook slReflexSleep failed (%d)", st);
            }
        } else {
            ul_log::Write("PCLHook: MH_CreateHook slReflexSleep failed (%d)", st);
        }
    } else {
        ul_log::Write("PCLHook: slReflexSleep not found (res=%d) — sleep not swallowed", res);
    }

    // Hook slReflexSetOptions — capture game's Reflex params, swallow the call.
    // Our limiter controls SetSleepMode on its own schedule.
    // Matches DX Hook_SetSleepMode behavior.
    void* setopts_func = nullptr;
    res = slGetFeatureFunction(kFeatureReflex, "slReflexSetOptions", setopts_func);
    if (res == sl_compat::eOk && setopts_func) {
        ul_log::Write("PCLHook: slReflexSetOptions resolved at %p", setopts_func);
        st = MH_CreateHook(setopts_func,
            reinterpret_cast<void*>(&Hooked_slReflexSetOptions),
            reinterpret_cast<void**>(&s_orig_slReflexSetOptions));
        if (st == MH_OK) {
            st = MH_EnableHook(setopts_func);
            if (st == MH_OK) {
                ul_log::Write("PCLHook: slReflexSetOptions hooked OK (swallowed)");
            } else {
                MH_RemoveHook(setopts_func);
                ul_log::Write("PCLHook: MH_EnableHook slReflexSetOptions failed (%d)", st);
            }
        } else {
            ul_log::Write("PCLHook: MH_CreateHook slReflexSetOptions failed (%d)", st);
        }
    } else {
        ul_log::Write("PCLHook: slReflexSetOptions not found (res=%d)", res);
    }

    return true;
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

    // Re-hook LL2 functions on every swapchain init. After a swapchain
    // destroy/recreate, Streamline may re-resolve its LL2 function pointers
    // to different driver addresses, bypassing our previous MinHook patches.
    // HookLL2Functions removes stale hooks and re-applies to current addresses.
    ul_log::Write("VkDeferredHook: re-hooking LL2 functions");
    HookLL2Functions(device);
}

// ============================================================================
// VK_EXT_present_timing — display-level frame pacing and feedback
// Written from public Vulkan specification (MIT/Apache-2.0):
//   - Khronos VK_EXT_present_timing proposal
// ============================================================================

static PFN_vkGetSwapchainTimingPropertiesEXT s_pfn_GetTimingProps = nullptr;
static PFN_vkSetSwapchainPresentTimingQueueSizeEXT s_pfn_SetTimingQueueSize = nullptr;
static PFN_vkGetPastPresentationTimingEXT s_pfn_GetPastTiming = nullptr;
static VkDevice s_pt_device = nullptr;
static VkSwapchainKHR s_pt_swapchain = 0;
static std::atomic<bool> s_pt_available{false};
static std::atomic<uint64_t> s_pt_refresh_duration_ns{0};
static std::atomic<bool> s_pt_is_vrr{false};

bool VkPresentTimingAvailable() {
    return s_pt_available.load(std::memory_order_relaxed);
}

bool VkPresentTimingInit(VkDevice device, VkSwapchainKHR swapchain) {
    if (!device || !swapchain) return false;

    // Resolve function pointers via the unhooked vkGetDeviceProcAddr
    PFN_vkGetDeviceProcAddr gdpa = s_orig_vkGetDeviceProcAddr;
    if (!gdpa) {
        HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
        if (vk_dll)
            gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
    }
    if (!gdpa) {
        ul_log::Write("PresentTiming: no vkGetDeviceProcAddr");
        return false;
    }

    s_pfn_GetTimingProps = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
        gdpa(device, "vkGetSwapchainTimingPropertiesEXT"));
    s_pfn_SetTimingQueueSize = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
        gdpa(device, "vkSetSwapchainPresentTimingQueueSizeEXT"));
    s_pfn_GetPastTiming = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
        gdpa(device, "vkGetPastPresentationTimingEXT"));

    if (!s_pfn_GetTimingProps) {
        ul_log::Write("PresentTiming: vkGetSwapchainTimingPropertiesEXT not found — extension not active");
        s_pt_available.store(false, std::memory_order_relaxed);
        return false;
    }

    ul_log::Write("PresentTiming: functions resolved (props=%p, queueSize=%p, pastTiming=%p)",
                   s_pfn_GetTimingProps, s_pfn_SetTimingQueueSize, s_pfn_GetPastTiming);

    s_pt_device = device;
    s_pt_swapchain = swapchain;

    // Set up the timing results queue (16 slots should be plenty)
    if (s_pfn_SetTimingQueueSize) {
        VkResult res = s_pfn_SetTimingQueueSize(device, swapchain, 16);
        ul_log::Write("PresentTiming: SetTimingQueueSize(16) = %d", res);
    }

    // Query initial timing properties
    VkSwapchainTimingPropertiesEXT props = {};
    props.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
    uint64_t counter = 0;
    VkResult res = s_pfn_GetTimingProps(device, swapchain, &props, &counter);
    if (res == VK_SUCCESS) {
        double refresh_hz = (props.refreshDuration > 0) ? 1e9 / static_cast<double>(props.refreshDuration) : 0;
        bool vrr = (props.refreshInterval == UINT64_MAX);
        ul_log::Write("PresentTiming: refreshDuration=%lluns (%.1f Hz), refreshInterval=%llu%s, counter=%llu",
                       props.refreshDuration, refresh_hz,
                       props.refreshInterval,
                       vrr ? " (VRR)" : "",
                       counter);
        if (props.refreshDuration > 0)
            s_pt_refresh_duration_ns.store(props.refreshDuration, std::memory_order_relaxed);
        s_pt_is_vrr.store(vrr, std::memory_order_relaxed);
        s_pt_available.store(true, std::memory_order_relaxed);
    } else if (res == VK_NOT_READY) {
        ul_log::Write("PresentTiming: timing properties not ready yet (need at least one present)");
        s_pt_available.store(true, std::memory_order_relaxed);  // will be ready after first present
    } else {
        ul_log::Write("PresentTiming: GetTimingProperties failed (%d)", res);
        s_pt_available.store(false, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool VkPresentTimingPollProperties() {
    if (!s_pfn_GetTimingProps || !s_pt_device || !s_pt_swapchain) return false;

    VkSwapchainTimingPropertiesEXT props = {};
    props.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
    uint64_t counter = 0;
    VkResult res = s_pfn_GetTimingProps(s_pt_device, s_pt_swapchain, &props, &counter);
    if (res != VK_SUCCESS) return false;

    // Cache refresh duration for VRR ceiling computation
    if (props.refreshDuration > 0)
        s_pt_refresh_duration_ns.store(props.refreshDuration, std::memory_order_relaxed);
    s_pt_is_vrr.store(props.refreshInterval == UINT64_MAX, std::memory_order_relaxed);

    static uint64_t s_last_counter = 0;
    if (counter != s_last_counter) {
        double refresh_hz = (props.refreshDuration > 0) ? 1e9 / static_cast<double>(props.refreshDuration) : 0;
        bool vrr = (props.refreshInterval == UINT64_MAX);
        ul_log::Write("PresentTiming: properties changed — refreshDuration=%lluns (%.1f Hz), interval=%llu%s",
                       props.refreshDuration, refresh_hz,
                       props.refreshInterval,
                       vrr ? " (VRR)" : "");
        s_last_counter = counter;
    }

    return true;
}

// ============================================================================
// vkCreateSwapchainKHR hook — injects VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
// ============================================================================

static PFN_vkCreateSwapchainKHR s_orig_vkCreateSwapchain = nullptr;
static std::atomic<bool> s_swapchain_hook_installed{false};

static VkResult Hooked_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                              const void* pAllocator, VkSwapchainKHR* pSwapchain) {
    if (!pCreateInfo || !s_orig_vkCreateSwapchain) {
        return s_orig_vkCreateSwapchain ? s_orig_vkCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain)
                                        : VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    // We can't modify the const struct directly, but VkSwapchainCreateInfoKHR
    // is a large struct (~104 bytes). We'll copy the first 12 bytes (sType + pNext + flags),
    // patch flags, then use a trick: cast away const and patch in-place since
    // the caller owns the memory and we restore it after.
    VkFlags orig_flags = pCreateInfo->flags;
    const_cast<VkSwapchainCreateInfoKHR*>(pCreateInfo)->flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;

    static uint64_t s_sc_count = 0;
    s_sc_count++;
    if (s_sc_count <= 5) {
        ul_log::Write("PresentTiming: injected PRESENT_TIMING_BIT into swapchain flags (0x%X -> 0x%X)",
                       orig_flags, pCreateInfo->flags);
    }

    VkResult res = s_orig_vkCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);

    // Restore original flags
    const_cast<VkSwapchainCreateInfoKHR*>(pCreateInfo)->flags = orig_flags;

    if (res != VK_SUCCESS) {
        ul_log::Write("PresentTiming: swapchain creation with timing bit failed (%d), retrying without", res);
        res = s_orig_vkCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
    }

    return res;
}

bool VkPresentTimingHookCreateSwapchain() {
    if (s_swapchain_hook_installed.load(std::memory_order_relaxed)) return true;

    // Hook via vkGetDeviceProcAddr — the device-level function, not the loader export.
    // This ensures we hook the right dispatch level.
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return false;

    auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkCreateSwapchainKHR"));
    if (!target) {
        ul_log::Write("PresentTiming: vkCreateSwapchainKHR not found");
        return false;
    }

    MH_STATUS st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Hooked_vkCreateSwapchainKHR),
        reinterpret_cast<void**>(&s_orig_vkCreateSwapchain));
    if (st != MH_OK) {
        ul_log::Write("PresentTiming: MH_CreateHook vkCreateSwapchainKHR failed (%d)", st);
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        ul_log::Write("PresentTiming: MH_EnableHook vkCreateSwapchainKHR failed (%d)", st);
        MH_RemoveHook(target);
        return false;
    }

    s_swapchain_hook_installed.store(true, std::memory_order_relaxed);
    ul_log::Write("PresentTiming: vkCreateSwapchainKHR hooked OK");
    return true;
}

// ============================================================================
// vkQueuePresentKHR hook — attaches VkPresentTimingsInfoEXT to each present
// ============================================================================

static PFN_vkQueuePresentKHR s_orig_vkQueuePresent = nullptr;
static std::atomic<bool> s_present_hook_installed{false};
static std::atomic<uint64_t> s_pt_target_time_ns{0};
static std::atomic<uint64_t> s_pt_present_id{1};

// Feedback from vkGetPastPresentationTimingEXT
static std::atomic<uint64_t> s_pt_last_display_ns{0};
static std::atomic<uint64_t> s_pt_last_target_ns{0};
static std::atomic<bool> s_pt_feedback_ready{false};

static VkResult Hooked_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!pPresentInfo || !s_pt_available.load(std::memory_order_relaxed) || !s_orig_vkQueuePresent) {
        return s_orig_vkQueuePresent ? s_orig_vkQueuePresent(queue, pPresentInfo) : VK_SUCCESS;
    }

    static uint64_t s_present_count = 0;
    s_present_count++;
    if (s_present_count <= 5 || (s_present_count % 1000) == 0) {
        ul_log::Write("PresentTiming: present #%llu (swapchains=%u, pNext=%p)",
                       s_present_count, pPresentInfo->swapchainCount, pPresentInfo->pNext);
    }

    uint64_t target = s_pt_target_time_ns.load(std::memory_order_relaxed);
    uint64_t pid = s_pt_present_id.fetch_add(1, std::memory_order_relaxed);

    // Build per-swapchain timing info
    // We set presentStageQueries to request IMAGE_FIRST_PIXEL_OUT feedback
    // and targetTime to our grid's next slot (0 = no scheduling, just query)
    VkPresentTimingInfoEXT timing_info = {};
    timing_info.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    timing_info.pNext = nullptr;
    // When we have a target time, ask the driver to present at the nearest
    // refresh cycle to our target. Without a target, just collect feedback.
    timing_info.flags = (target > 0) ? 0x00000002 : 0;  // PRESENT_AT_NEAREST_REFRESH_CYCLE
    timing_info.targetTime = target;
    timing_info.timeDomainId = 0;     // default time domain
    timing_info.presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                    | VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
    timing_info.targetTimeDomainPresentStage = 0;

    // Build the array (one per swapchain in the present)
    // Most games present one swapchain at a time
    VkPresentTimingInfoEXT timing_infos[4];
    uint32_t sc_count = pPresentInfo->swapchainCount;
    if (sc_count > 4) sc_count = 4;
    for (uint32_t i = 0; i < sc_count; i++) {
        timing_infos[i] = timing_info;
    }

    VkPresentTimingsInfoEXT timings_ext = {};
    timings_ext.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    timings_ext.pNext = pPresentInfo->pNext;  // chain with existing pNext
    timings_ext.swapchainCount = sc_count;
    timings_ext.pTimingInfos = timing_infos;

    // Patch the pNext chain — we need to modify the const struct
    VkPresentInfoKHR modified = *pPresentInfo;
    modified.pNext = &timings_ext;

    VkResult res = s_orig_vkQueuePresent(queue, &modified);

    // Poll for past presentation timing feedback
    if (s_pfn_GetPastTiming && s_pt_device && s_pt_swapchain) {
        // First query: how many results are available?
        VkPastPresentationTimingInfoEXT past_info = {};
        past_info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
        past_info.flags = 0x00000001 | 0x00000002;  // ALLOW_PARTIAL | ALLOW_OUT_OF_ORDER
        past_info.swapchain = s_pt_swapchain;

        VkPastPresentationTimingPropertiesEXT past_props = {};
        past_props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
        past_props.presentationTimingCount = 0;
        past_props.pPresentationTimings = nullptr;

        VkResult poll_res = s_pfn_GetPastTiming(s_pt_device, &past_info, &past_props);

        static uint64_t s_poll_count = 0;
        s_poll_count++;
        if (s_poll_count <= 5 || (s_poll_count % 1000) == 0) {
            ul_log::Write("PresentTiming: poll #%llu res=%d available=%u",
                           s_poll_count, poll_res, past_props.presentationTimingCount);
        }

        if (poll_res == VK_SUCCESS && past_props.presentationTimingCount > 0) {
            // Fetch one result
            VkPresentStageTimeEXT stages[4] = {};
            VkPastPresentationTimingEXT past_timing = {};
            past_timing.sType = static_cast<VkStructureType>(1000208004);
            past_timing.presentStageCount = 4;
            past_timing.pPresentStages = stages;

            past_props.presentationTimingCount = 1;
            past_props.pPresentationTimings = &past_timing;

            poll_res = s_pfn_GetPastTiming(s_pt_device, &past_info, &past_props);
            if (poll_res == VK_SUCCESS && past_props.presentationTimingCount > 0) {
                // Log all stage results
                static uint64_t s_fb_count = 0;
                s_fb_count++;
                if (s_fb_count <= 10 || (s_fb_count % 500) == 0) {
                    ul_log::Write("PresentTiming: feedback #%llu pid=%llu target=%llu complete=%d stages=%u",
                                   s_fb_count, past_timing.presentId, past_timing.targetTime,
                                   past_timing.reportComplete, past_timing.presentStageCount);
                    for (uint32_t i = 0; i < past_timing.presentStageCount; i++) {
                        ul_log::Write("  stage[%u]: flags=0x%X time=%llu", i, stages[i].stage, stages[i].time);
                    }
                }

                // Find best available stage time
                uint64_t display_time = 0;
                for (uint32_t i = 0; i < past_timing.presentStageCount; i++) {
                    if (stages[i].time > 0) {
                        if (stages[i].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
                            display_time = stages[i].time;
                        else if (display_time == 0)
                            display_time = stages[i].time;
                    }
                }
                if (display_time > 0) {
                    s_pt_last_display_ns.store(display_time, std::memory_order_relaxed);
                    s_pt_last_target_ns.store(past_timing.targetTime, std::memory_order_relaxed);
                    s_pt_feedback_ready.store(true, std::memory_order_relaxed);
                }
            }
        }
    }

    return res;
}

bool VkPresentTimingHookPresent() {
    if (s_present_hook_installed.load(std::memory_order_relaxed)) return true;

    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk_dll) return false;

    auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkQueuePresentKHR"));
    if (!target) {
        ul_log::Write("PresentTiming: vkQueuePresentKHR not found");
        return false;
    }

    MH_STATUS st = MH_CreateHook(target,
        reinterpret_cast<void*>(&Hooked_vkQueuePresentKHR),
        reinterpret_cast<void**>(&s_orig_vkQueuePresent));
    if (st != MH_OK) {
        ul_log::Write("PresentTiming: MH_CreateHook vkQueuePresentKHR failed (%d)", st);
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        ul_log::Write("PresentTiming: MH_EnableHook vkQueuePresentKHR failed (%d)", st);
        MH_RemoveHook(target);
        return false;
    }

    s_present_hook_installed.store(true, std::memory_order_relaxed);
    ul_log::Write("PresentTiming: vkQueuePresentKHR hooked OK");
    return true;
}

void VkPresentTimingUnhookPresent() {
    if (!s_present_hook_installed.load(std::memory_order_relaxed)) return;
    HMODULE vk_dll = GetModuleHandleW(L"vulkan-1.dll");
    if (vk_dll) {
        auto target = reinterpret_cast<void*>(GetProcAddress(vk_dll, "vkQueuePresentKHR"));
        if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
    }
    s_present_hook_installed.store(false, std::memory_order_relaxed);
    s_orig_vkQueuePresent = nullptr;
}

void VkPresentTimingSetTargetTime(uint64_t target_ns) {
    s_pt_target_time_ns.store(target_ns, std::memory_order_relaxed);
}

bool VkPresentTimingGetFeedback(uint64_t* out_display_ns, uint64_t* out_target_ns) {
    if (!s_pt_feedback_ready.exchange(false, std::memory_order_relaxed)) return false;
    if (out_display_ns) *out_display_ns = s_pt_last_display_ns.load(std::memory_order_relaxed);
    if (out_target_ns) *out_target_ns = s_pt_last_target_ns.load(std::memory_order_relaxed);
    return true;
}

uint64_t VkPresentTimingGetRefreshDurationNs() {
    return s_pt_refresh_duration_ns.load(std::memory_order_relaxed);
}

bool VkPresentTimingIsVRR() {
    return s_pt_is_vrr.load(std::memory_order_relaxed);
}
