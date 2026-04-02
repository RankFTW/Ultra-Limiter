// ReLimiter — Streamline PCL hooks for FG detection and marker interception.
// Extracted from ul_vk_reflex.cpp — these hooks work on both DX and Vulkan
// by hooking sl.interposer.dll exports (not Vulkan functions).
//
// Written from public Streamline SDK headers (MIT license):
//   - sl_pcl.h: PCLMarker enum, PFun_slPCLSetMarker signature
//   - sl_core_api.h: slGetFeatureFunction export
//   - sl_consts.h: kFeaturePCL = 4
//   - sl_dlss_g.h: DLSSGOptions, DLSSGState layout
//   - sl_reflex.h: ReflexOptions layout

#include "ul_streamline.hpp"
#include "ul_fg.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"

#include <MinHook.h>
#include <windows.h>
#include <atomic>
#include <cstring>

// ============================================================================
// Marker callback
// ============================================================================

static MarkerCb s_sl_marker_cb = nullptr;

void SetStreamlineMarkerCb(MarkerCb cb) {
    s_sl_marker_cb = cb;
}

// ============================================================================
// Minimal Streamline types
// ============================================================================

namespace sl_compat {
    using Result = int32_t;
    constexpr int32_t eOk = 0;
    constexpr uint32_t kFeaturePCL = 4;

    enum PCLMarker : uint32_t {
        eSimulationStart = 0,
        eSimulationEnd = 1,
        eRenderSubmitStart = 2,
        eRenderSubmitEnd = 3,
        ePresentStart = 4,
        ePresentEnd = 5,
    };

    using PFun_slGetFeatureFunction = Result(uint32_t feature, const char* functionName, void*& function);
    using PFun_slPCLSetMarker = Result(uint32_t marker, const void* frame);
}

// ============================================================================
// Forward declarations and state
// ============================================================================

static sl_compat::PFun_slPCLSetMarker* s_orig_slPCLSetMarker = nullptr;
static std::atomic<bool> s_pcl_hook_installed{false};

using PFun_slDLSSGSetOptions = sl_compat::Result(const void* viewport, const void* options);
static PFun_slDLSSGSetOptions* s_orig_slDLSSGSetOptions = nullptr;
static sl_compat::Result Hooked_slDLSSGSetOptions(const void* viewport, const void* options);

using PFun_slDLSSGGetState = sl_compat::Result(const void* viewport, void* state, const void* options);
static PFun_slDLSSGGetState* s_orig_slDLSSGGetState = nullptr;
static sl_compat::Result Hooked_slDLSSGGetState(const void* viewport, void* state, const void* options);

static sl_compat::PFun_slGetFeatureFunction* s_orig_slGetFeatureFunction = nullptr;

// ============================================================================
// slGetFeatureFunction hook — intercepts game's resolution of slDLSSGSetOptions
// ============================================================================

static sl_compat::Result Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function) {
    if (!s_orig_slGetFeatureFunction) return -1;
    sl_compat::Result res = s_orig_slGetFeatureFunction(feature, functionName, function);
    if (res != sl_compat::eOk || !function || !functionName) return res;

    if (strcmp(functionName, "slDLSSGSetOptions") == 0 && !s_orig_slDLSSGSetOptions) {
        MH_STATUS st = MH_CreateHook(function,
            reinterpret_cast<void*>(&Hooked_slDLSSGSetOptions),
            reinterpret_cast<void**>(&s_orig_slDLSSGSetOptions));
        if (st == MH_OK) {
            st = MH_EnableHook(function);
            if (st == MH_OK) {
                ul_log::Write("PCLHook: slDLSSGSetOptions hooked via slGetFeatureFunction");
            } else {
                MH_RemoveHook(function);
                s_orig_slDLSSGSetOptions = nullptr;
            }
        }
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0 && !s_orig_slDLSSGGetState) {
        MH_STATUS st = MH_CreateHook(function,
            reinterpret_cast<void*>(&Hooked_slDLSSGGetState),
            reinterpret_cast<void**>(&s_orig_slDLSSGGetState));
        if (st == MH_OK) {
            st = MH_EnableHook(function);
            if (st == MH_OK) {
                ul_log::Write("PCLHook: slDLSSGGetState hooked via slGetFeatureFunction");
            } else {
                MH_RemoveHook(function);
                s_orig_slDLSSGGetState = nullptr;
            }
        }
    }

    return res;
}

// ============================================================================
// Streamline Reflex sleep hook
// ============================================================================

using PFun_slReflexSleep = sl_compat::Result(const void* frame);
static PFun_slReflexSleep* s_orig_slReflexSleep = nullptr;
static const void* s_last_frame_token = nullptr;

static sl_compat::Result Hooked_slReflexSleep(const void* frame) {
    if (frame) s_last_frame_token = frame;
    return sl_compat::eOk;
}

bool InvokeStreamlineSleep() {
    if (!s_orig_slReflexSleep) return false;
    const void* token = s_last_frame_token;
    if (!token) return false;
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

// ============================================================================
// Streamline Reflex SetOptions hook
// ============================================================================

using PFun_slReflexSetOptions = sl_compat::Result(const void* options);
static PFun_slReflexSetOptions* s_orig_slReflexSetOptions = nullptr;
static alignas(16) char s_saved_reflex_options[128] = {};
static bool s_has_saved_options = false;

static sl_compat::Result Hooked_slReflexSetOptions(const void* options) {
    if (options) {
        memcpy(s_saved_reflex_options, options, sizeof(s_saved_reflex_options));
        s_has_saved_options = true;
    }
    static bool s_logged = false;
    if (!s_logged) {
        ul_log::Write("PCLHook: slReflexSetOptions passthrough (game controls interval)");
        s_logged = true;
    }
    if (s_orig_slReflexSetOptions && options)
        return s_orig_slReflexSetOptions(options);
    return sl_compat::eOk;
}

bool InvokeStreamlineSetOptions(uint32_t interval_us, bool low_latency, bool boost) {
    if (!s_orig_slReflexSetOptions || !s_has_saved_options) return false;
    alignas(16) char buf[128];
    memcpy(buf, s_saved_reflex_options, sizeof(buf));
    int mode = low_latency ? (boost ? 2 : 1) : 0;
    memcpy(buf + 32, &mode, sizeof(int));
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

// ============================================================================
// slDLSSGSetOptions hook — detect FG multiplier
// ============================================================================

static sl_compat::Result Hooked_slDLSSGSetOptions(const void* viewport, const void* options) {
    if (!s_orig_slDLSSGSetOptions) return -1;
    sl_compat::Result res = s_orig_slDLSSGSetOptions(viewport, options);
    if (res == sl_compat::eOk && options) {
        uint32_t mode = 0, num_frames = 0;
        memcpy(&mode, static_cast<const char*>(options) + 32, sizeof(uint32_t));
        memcpy(&num_frames, static_cast<const char*>(options) + 36, sizeof(uint32_t));
        int multiplier = static_cast<int>(num_frames) + 1;
        if (multiplier < 1) multiplier = 1;
        if (multiplier > 6) multiplier = 6;
        g_fg_multiplier.store(multiplier, std::memory_order_relaxed);
        static uint32_t s_last_mode = UINT32_MAX;
        static uint32_t s_last_frames = UINT32_MAX;
        if (mode != s_last_mode || num_frames != s_last_frames) {
            s_last_mode = mode;
            s_last_frames = num_frames;
            ul_log::Write("DLSSG: SetOptions mode=%u numFramesToGenerate=%u (multiplier=%dx)",
                           mode, num_frames, multiplier);
        }
    }
    return res;
}

// ============================================================================
// slDLSSGGetState hook — determines FG active status
// ============================================================================

static sl_compat::Result Hooked_slDLSSGGetState(const void* viewport, void* state, const void* options) {
    if (!s_orig_slDLSSGGetState) return -1;
    sl_compat::Result res = s_orig_slDLSSGGetState(viewport, state, options);
    if (res == sl_compat::eOk && state) {
        uint32_t status = 0;
        memcpy(&status, static_cast<const char*>(state) + 40, sizeof(uint32_t));
        bool active = (status == 0);
        bool prev = g_fg_active.load(std::memory_order_relaxed);
        g_fg_active.store(active, std::memory_order_relaxed);
        if (active != prev)
            ul_log::Write("DLSSG: GetState status=0x%X -> FG %s", status, active ? "active" : "inactive");
    }
    return res;
}

// ============================================================================
// slPCLSetMarker hook — marker interception at game→Streamline boundary
// ============================================================================

static sl_compat::Result Hooked_slPCLSetMarker(uint32_t marker, const void* frame) {
    int mt = static_cast<int>(marker);

    uint64_t fid = 0;
    if (frame) {
        auto vtable = *reinterpret_cast<void* const* const*>(frame);
        using GetIndexFn = uint32_t(__thiscall*)(const void*);
        auto getIndex = reinterpret_cast<GetIndexFn>(vtable[0]);
        __try {
            fid = static_cast<uint64_t>(getIndex(frame));
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fid = 0;
        }
    }

    static uint64_t s_pcl_count = 0;
    s_pcl_count++;
    if (s_pcl_count <= 10 || (s_pcl_count % 500) == 0)
        ul_log::Write("PCLHook: marker #%llu (type=%d, frame=%llu)", s_pcl_count, mt, fid);

    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);

        if (g_ring[slot].seen_frame[mt].load(std::memory_order_relaxed) == fid) {
            if (s_orig_slPCLSetMarker)
                return s_orig_slPCLSetMarker(marker, frame);
            return 0;
        }
        g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);

        if (mt == static_cast<int>(PRESENT_FINISH)) {
            sl_compat::Result ret = 0;
            if (s_orig_slPCLSetMarker)
                ret = s_orig_slPCLSetMarker(marker, frame);
            if (s_sl_marker_cb) s_sl_marker_cb(mt, fid);
            return ret;
        }

        if (s_sl_marker_cb) s_sl_marker_cb(mt, fid);
    }

    if (s_orig_slPCLSetMarker)
        return s_orig_slPCLSetMarker(marker, frame);
    return 0;
}

// ============================================================================
// HookStreamlinePCL — main entry point
// ============================================================================

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

    // Hook slGetFeatureFunction for DLSSG interception
    {
        MH_STATUS hst = MH_CreateHook(
            reinterpret_cast<void*>(slGetFeatureFunction),
            reinterpret_cast<void*>(&Hooked_slGetFeatureFunction),
            reinterpret_cast<void**>(&s_orig_slGetFeatureFunction));
        if (hst == MH_OK) {
            hst = MH_EnableHook(reinterpret_cast<void*>(slGetFeatureFunction));
            if (hst == MH_OK)
                ul_log::Write("PCLHook: slGetFeatureFunction hooked (DLSSG interception)");
            else
                MH_RemoveHook(reinterpret_cast<void*>(slGetFeatureFunction));
        }
    }

    s_pcl_hook_installed.store(true, std::memory_order_relaxed);

    // NOTE: slPCLSetMarker is NOT hooked on DX — NVAPI Hook_SetMarker handles markers.
    // slReflexSleep and slReflexSetOptions are NOT hooked on DX — NVAPI path handles Reflex.
    // Only slGetFeatureFunction is hooked for FG detection (slDLSSGSetOptions/GetState).

    return true;
}

// ============================================================================
// LoadLibrary hooks — catch sl.interposer.dll loading early
// ============================================================================

static HMODULE(WINAPI* s_orig_LoadLibraryA)(LPCSTR) = nullptr;
static HMODULE(WINAPI* s_orig_LoadLibraryW)(LPCWSTR) = nullptr;
static HMODULE(WINAPI* s_orig_LoadLibraryExA)(LPCSTR, HANDLE, DWORD) = nullptr;
static HMODULE(WINAPI* s_orig_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;
static std::atomic<bool> s_ll_hooks_installed{false};

static void OnModuleLoaded(HMODULE hModule, const wchar_t* name) {
    if (!hModule || !name) return;
    const wchar_t* filename = wcsrchr(name, L'\\');
    if (!filename) filename = wcsrchr(name, L'/');
    if (filename) filename++; else filename = name;
    if (_wcsicmp(filename, L"sl.interposer.dll") == 0) {
        ul_log::Write("LoadLibHook: sl.interposer.dll loaded — installing PCL hooks");
        HookStreamlinePCL();
    }
}

static HMODULE WINAPI Hooked_LoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE result = s_orig_LoadLibraryA(lpLibFileName);
    if (result && lpLibFileName) {
        wchar_t wide[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, wide, MAX_PATH))
            OnModuleLoaded(result, wide);
    }
    return result;
}

static HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE result = s_orig_LoadLibraryW(lpLibFileName);
    if (result && lpLibFileName) OnModuleLoaded(result, lpLibFileName);
    return result;
}

static HMODULE WINAPI Hooked_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = s_orig_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (result && lpLibFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) {
        wchar_t wide[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, lpLibFileName, -1, wide, MAX_PATH))
            OnModuleLoaded(result, wide);
    }
    return result;
}

static HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE result = s_orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (result && lpLibFileName && !(dwFlags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)))
        OnModuleLoaded(result, lpLibFileName);
    return result;
}

void InstallLoadLibraryHooks() {
    if (s_ll_hooks_installed.load(std::memory_order_relaxed)) return;
    MH_STATUS st;
    st = MH_CreateHook(reinterpret_cast<void*>(&LoadLibraryA),
        reinterpret_cast<void*>(&Hooked_LoadLibraryA),
        reinterpret_cast<void**>(&s_orig_LoadLibraryA));
    if (st == MH_OK) MH_EnableHook(reinterpret_cast<void*>(&LoadLibraryA));
    st = MH_CreateHook(reinterpret_cast<void*>(&LoadLibraryW),
        reinterpret_cast<void*>(&Hooked_LoadLibraryW),
        reinterpret_cast<void**>(&s_orig_LoadLibraryW));
    if (st == MH_OK) MH_EnableHook(reinterpret_cast<void*>(&LoadLibraryW));
    st = MH_CreateHook(reinterpret_cast<void*>(&LoadLibraryExA),
        reinterpret_cast<void*>(&Hooked_LoadLibraryExA),
        reinterpret_cast<void**>(&s_orig_LoadLibraryExA));
    if (st == MH_OK) MH_EnableHook(reinterpret_cast<void*>(&LoadLibraryExA));
    st = MH_CreateHook(reinterpret_cast<void*>(&LoadLibraryExW),
        reinterpret_cast<void*>(&Hooked_LoadLibraryExW),
        reinterpret_cast<void**>(&s_orig_LoadLibraryExW));
    if (st == MH_OK) MH_EnableHook(reinterpret_cast<void*>(&LoadLibraryExW));
    s_ll_hooks_installed.store(true, std::memory_order_relaxed);
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (sl) {
        ul_log::Write("LoadLibHook: sl.interposer.dll already loaded — installing PCL hooks");
        HookStreamlinePCL();
    }
    ul_log::Write("LoadLibHook: LoadLibrary hooks installed");
}

void RemoveLoadLibraryHooks() {
    if (!s_ll_hooks_installed.load(std::memory_order_relaxed)) return;
    void* targets[] = {
        reinterpret_cast<void*>(&LoadLibraryA), reinterpret_cast<void*>(&LoadLibraryW),
        reinterpret_cast<void*>(&LoadLibraryExA), reinterpret_cast<void*>(&LoadLibraryExW),
    };
    for (auto t : targets) { MH_DisableHook(t); MH_RemoveHook(t); }
    s_orig_LoadLibraryA = nullptr; s_orig_LoadLibraryW = nullptr;
    s_orig_LoadLibraryExA = nullptr; s_orig_LoadLibraryExW = nullptr;
    s_ll_hooks_installed.store(false, std::memory_order_relaxed);
}
