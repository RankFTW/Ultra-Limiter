// Ultra Limiter — NVIDIA Reflex hook implementation
// Clean-room from public docs:
//   - NVAPI SDK (MIT): nvapi_interface.h for function IDs, nvapi.h for structs
//   - MinHook (BSD-2): MH_CreateHook, MH_EnableHook, MH_DisableHook, MH_RemoveHook
//   - Windows: LoadLibraryA, GetProcAddress, DXGI vtable layout
// No code from Display Commander, Special K, or any other project.

#include "ul_reflex.hpp"
#include "ul_config.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"

#include "nvapi_interface.h"  // MIT licensed — function name -> ID table

#include <MinHook.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <cstring>
#include <intrin.h>  // _ReturnAddress for RTSS filter

// ============================================================================
// Globals
// ============================================================================

MarkerSlot g_ring[kRingSize] = {};
std::atomic<bool> g_game_uses_reflex{false};
std::atomic<uint64_t> g_present_count{0};

static HMODULE s_nvapi_dll = nullptr;
static NvQueryInterface_fn s_qi = nullptr;

// Trampoline pointers (original functions, set by MinHook)
static NvSetSleepMode_fn s_orig_sleep_mode = nullptr;
static NvSleep_fn s_orig_sleep = nullptr;
static NvSetMarker_fn s_orig_marker = nullptr;

// Hook target addresses
static void* s_tgt_sleep_mode = nullptr;
static void* s_tgt_sleep = nullptr;
static void* s_tgt_marker = nullptr;

// GetLatency — resolved but not hooked
static NvGetLatency_fn s_get_latency = nullptr;

static std::atomic<bool> s_hooked{false};
static std::atomic<bool> s_nvapi_ready{false};

static GameReflexState s_game_state;

// QueryInterface hook — block flip metering (NvAPI_D3D12_SetFlipConfig)
static NvQueryInterface_fn s_orig_qi = nullptr;
static constexpr NvU32 kSetFlipConfigId = 0xF3148C42;

// Safe mode: REFramework + Streamline detected.
// When set, swapchain vtable hooks are skipped to avoid crashing Streamline's
// internal swapchain management during recreation.
static std::atomic<bool> s_streamline_safe_mode{false};

// Out-of-band INPUT_SAMPLE reordering state.
// INPUT_SAMPLE must arrive between SIM_START and SIM_END to be valid.
// If a game fires it at the wrong time, we queue it and re-inject it
// right after the next SIM_START.
static std::atomic<bool> s_input_queued{false};
static std::atomic<int> s_last_marker_type{PRESENT_FINISH};

// Duplicate Sleep guard — track which present frame last called Sleep
static std::atomic<uint64_t> s_last_sleep_frame{UINT64_MAX};

// Streamline proxy
static SLPresentCb s_sl_cb = nullptr;
static MarkerCb s_marker_cb = nullptr;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static PresentFn s_orig_present = nullptr;
static std::atomic<bool> s_sl_hooked{false};

// VSync override Present hook (game's main swapchain)
static PresentFn s_orig_game_present = nullptr;
static std::atomic<bool> s_vsync_hooked{false};
static std::atomic<bool> s_tearing_supported{false};

// Frame-latency override (IDXGISwapChain2)
using SetMaxFrameLatency_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT);
using GetMaxFrameLatency_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT*);
static SetMaxFrameLatency_fn s_orig_set_max_latency = nullptr;
static GetMaxFrameLatency_fn s_orig_get_max_latency = nullptr;
static std::atomic<bool> s_latency_hooked{false};
static std::atomic<UINT> s_game_latency{0};
static IDXGISwapChain2* s_latency_sc2 = nullptr;

// ============================================================================
// Interface table lookup
// ============================================================================

static NvU32 FindFuncId(const char* name) {
    for (int i = 0; nvapi_interface_table[i].func != nullptr; i++) {
        if (strcmp(nvapi_interface_table[i].func, name) == 0)
            return nvapi_interface_table[i].id;
    }
    return 0;
}

// ============================================================================
// QueryInterface detour — block flip metering pacer
// ============================================================================
// NvAPI_D3D12_SetFlipConfig (0xF3148C42) is NVIDIA's flip metering pacer
// used by Streamline/DLSS FG to control frame presentation cadence.
// Blocking it gives UL full control over FG frame pacing.
// Exception: Smooth Motion requires flip metering, so allow it through.

static void* __cdecl Hook_QueryInterface(NvU32 id) {
    if (id == kSetFlipConfigId) {
        // Allow through if Smooth Motion is active — it needs flip metering
        if (GetModuleHandleW(L"NvPresent64.dll") != nullptr) {
            return s_orig_qi ? s_orig_qi(id) : nullptr;
        }
        static bool s_logged = false;
        if (!s_logged) {
            ul_log::Write("Hook_QueryInterface: blocked SetFlipConfig (0x%08X)", id);
            s_logged = true;
        }
        return nullptr;
    }
    return s_orig_qi ? s_orig_qi(id) : nullptr;
}

// ============================================================================
// Load nvapi64.dll and initialize
// ============================================================================

static bool LoadNvapi() {
    if (s_nvapi_ready.load(std::memory_order_acquire)) return true;

    HMODULE dll = LoadLibraryA("nvapi64.dll");
    if (!dll) {
        ul_log::Write("LoadNvapi: LoadLibraryA failed (err=%lu)", GetLastError());
        return false;
    }

    auto qi = reinterpret_cast<NvQueryInterface_fn>(GetProcAddress(dll, "nvapi_QueryInterface"));
    if (!qi) { ul_log::Write("LoadNvapi: nvapi_QueryInterface not found"); return false; }

    // Hook QueryInterface to block flip metering (SetFlipConfig)
    // Skip entirely when REFramework is present — its Streamline integration
    // calls nvapi_QueryInterface during swapchain recreation and blocking
    // SetFlipConfig at that point causes a hard crash in sl.dlss_g.dll.
    // REFramework can appear as reframework.dll or load via dinput8.dll.
    // When dinput8.dll is present alongside Streamline (sl.common.dll), the
    // QI hook blocking SetFlipConfig during swapchain recreation crashes
    // sl.dlss_g.dll. Check all known indicators.
    bool reframework = GetModuleHandleW(L"reframework.dll") != nullptr
                    || GetModuleHandleW(L"REFramework.dll") != nullptr;
    if (!reframework) {
        // dinput8.dll is REFramework's proxy DLL in RE Engine games.
        // Only treat it as REFramework if Streamline is also present —
        // dinput8.dll alone (e.g. from other mods) is harmless.
        bool dinput8 = GetModuleHandleW(L"dinput8.dll") != nullptr;
        bool streamline = GetModuleHandleW(L"sl.common.dll") != nullptr
                       || GetModuleHandleW(L"sl.dlss_g.dll") != nullptr;
        if (dinput8 && streamline) reframework = true;
    }
    if (reframework) {
        ul_log::Write("LoadNvapi: REFramework detected — skipping QueryInterface hook (crash prevention)");
        s_orig_qi = nullptr;
        s_streamline_safe_mode.store(true, std::memory_order_release);
    } else if (MH_CreateHook(reinterpret_cast<void*>(qi), reinterpret_cast<void*>(Hook_QueryInterface),
                       reinterpret_cast<void**>(&s_orig_qi)) == MH_OK) {
        if (MH_EnableHook(reinterpret_cast<void*>(qi)) == MH_OK) {
            ul_log::Write("LoadNvapi: QueryInterface hooked (flip metering block active)");
        } else {
            MH_RemoveHook(reinterpret_cast<void*>(qi));
            ul_log::Write("LoadNvapi: QueryInterface hook enable failed");
            s_orig_qi = nullptr;
        }
    } else {
        ul_log::Write("LoadNvapi: QueryInterface hook create failed");
        s_orig_qi = nullptr;
    }

    // Use the original (unhooked) QI for our own internal lookups
    NvQueryInterface_fn real_qi = s_orig_qi ? s_orig_qi : qi;

    NvU32 init_id = FindFuncId("NvAPI_Initialize");
    if (!init_id) { ul_log::Write("LoadNvapi: NvAPI_Initialize ID not in table"); return false; }

    auto init_fn = reinterpret_cast<NvInitialize_fn>(real_qi(init_id));
    if (!init_fn) { ul_log::Write("LoadNvapi: QueryInterface(Initialize) null"); return false; }

    NvStatus st = init_fn();
    if (st != NV_OK) { ul_log::Write("LoadNvapi: Initialize returned %d", st); return false; }

    s_nvapi_dll = dll;
    s_qi = real_qi;  // store the original QI for our own use
    s_nvapi_ready.store(true, std::memory_order_release);
    ul_log::Write("LoadNvapi: OK");
    return true;
}

// ============================================================================
// Detour functions
// ============================================================================

// Intercept SetSleepMode: capture game's params, swallow the call.
// We control sleep mode ourselves via InvokeSetSleepMode.
static NvStatus __cdecl Hook_SetSleepMode(IUnknown* dev, NvSleepParams* p) {
    if (p) {
        s_game_state.low_latency.store(p->bLowLatencyMode != 0, std::memory_order_relaxed);
        s_game_state.boost.store(p->bLowLatencyBoost != 0, std::memory_order_relaxed);
        s_game_state.use_markers.store(p->bUseMarkersToOptimize != 0, std::memory_order_relaxed);
        s_game_state.captured.store(true, std::memory_order_release);
    }
    return NV_OK;  // swallow — we set sleep mode on our own schedule
}

// Intercept Sleep: track per-frame call count for diagnostics, swallow all.
// Games like Monster Hunter Wilds call Sleep multiple times per frame.
// We suppress everything here — UlLimiter calls InvokeSleep on its own schedule.
// The dedup tracking lets us detect misbehaving games in logs if needed.
static NvStatus __cdecl Hook_Sleep(IUnknown* dev) {
    uint64_t cur = g_present_count.load(std::memory_order_relaxed);
    s_last_sleep_frame.store(cur, std::memory_order_relaxed);
    return NV_OK;
}

// Forward a single marker to the driver, record it in the ring buffer,
// and optionally notify the pacing callback.
// Helper used by Hook_SetMarker and the input reordering logic.
static NvStatus ForwardMarker(IUnknown* dev, NvMarkerParams* p, bool notify) {
    int mt = static_cast<int>(p->markerType);
    uint64_t fid = p->frameID;

    // Record in ring buffer so pacing logic sees the timestamp
    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);
        g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);
    }

    NvStatus ret = NV_OK;
    if (s_orig_marker) ret = s_orig_marker(dev, p);
    if (notify && s_marker_cb)
        s_marker_cb(mt, fid);
    return ret;
}

// Intercept SetLatencyMarker: record timestamp, reorder out-of-band input
// markers, notify callback, forward to driver.
static NvStatus __cdecl Hook_SetMarker(IUnknown* dev, NvMarkerParams* p) {
    if (!dev || !p) {
        if (s_orig_marker) return s_orig_marker(dev, p);
        return NV_BAD_ARG;
    }

    // Filter out RTSS (RivaTuner Statistics Server) — it fires latency
    // markers but is NOT native Reflex.  Both SK and DC filter this.
    // Re-check each call since RTSS can load after the game starts.
    {
        HMODULE rtss = GetModuleHandleW(L"RTSSHooks64.dll");
        if (rtss) {
            HMODULE caller = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(_ReturnAddress()),
                               &caller);
            if (caller == rtss)
                return NV_OK;
        }
    }

    g_game_uses_reflex.store(true, std::memory_order_relaxed);

    int mt = static_cast<int>(p->markerType);
    uint64_t fid = p->frameID;

    // ----------------------------------------------------------------
    // Out-of-band INPUT_SAMPLE reordering.
    // INPUT_SAMPLE is only valid between SIM_START and SIM_END.
    // If the game fires it at the wrong time we queue it and re-inject
    // it at the next valid opportunity (SIM_START or SIM_END boundary).
    // ----------------------------------------------------------------
    if (mt == INPUT_SAMPLE) {
        int prev = s_last_marker_type.load(std::memory_order_relaxed);
        if (prev != SIM_START) {
            // Not inside a simulation window — queue for later
            s_input_queued.store(true, std::memory_order_relaxed);
            return NV_OK;
        }
    }

    // At SIM_START or SIM_END, flush any queued input marker.
    // Use the CURRENT marker's frameID for the synthetic input marker.
    // The original input's frameID is lost (we only store a boolean flag),
    // but using the triggering marker's ID keeps the driver's per-frame
    // accounting consistent — the input is attributed to the frame that
    // will actually process it.
    if ((mt == SIM_START || mt == SIM_END) &&
        s_input_queued.exchange(false, std::memory_order_relaxed))
    {
        NvMarkerParams inp = *p;
        inp.markerType = static_cast<LatencyMarker>(INPUT_SAMPLE);

        // SIM_START: forward SIM_START first, then inject queued input after it
        // SIM_END:   inject queued input before the end marker
        bool before = (mt == SIM_START);
        if (before) {
            ForwardMarker(dev, p, true);
            ForwardMarker(dev, &inp, true);
        } else {
            ForwardMarker(dev, &inp, true);
            ForwardMarker(dev, p, true);
        }

        // Update tracking state
        if (mt != INPUT_SAMPLE)
            s_last_marker_type.store(mt, std::memory_order_relaxed);
        return NV_OK;
    }

    // Track last non-INPUT marker type for the reordering logic
    if (mt != INPUT_SAMPLE)
        s_last_marker_type.store(mt, std::memory_order_relaxed);

    // Record in ring buffer
    if (mt >= 0 && mt < kMarkerCount) {
        size_t slot = static_cast<size_t>(fid % kRingSize);
        g_ring[slot].frame_id.store(fid, std::memory_order_relaxed);
        g_ring[slot].timestamp_ns[mt].store(ul_timing::NowNs(), std::memory_order_relaxed);

        // Dedup: skip forwarding if we already saw this marker for this frame
        if (g_ring[slot].seen_frame[mt].load(std::memory_order_relaxed) == fid)
            return NV_OK;
        g_ring[slot].seen_frame[mt].store(fid, std::memory_order_relaxed);
    }

    // For PRESENT_FINISH: forward to driver first, then notify callback.
    // For all others: notify callback first, then forward.
    bool forwarded = false;
    if (mt == static_cast<int>(PRESENT_FINISH)) {
        if (s_orig_marker) s_orig_marker(dev, p);
        forwarded = true;
    }

    if (s_marker_cb) s_marker_cb(mt, fid);

    if (!forwarded && s_orig_marker)
        return s_orig_marker(dev, p);
    return NV_OK;
}

// ============================================================================
// Streamline proxy Present detour
// ============================================================================

static HRESULT STDMETHODCALLTYPE Hook_SLPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (s_sl_cb) s_sl_cb();
    // Apply VSync override
    int ovr = g_cfg.vsync_override.load(std::memory_order_relaxed);
    if (ovr == 1) { sync = 1; flags &= ~DXGI_PRESENT_ALLOW_TEARING; }
    else if (ovr == 2) {
        sync = 0;
        if (s_tearing_supported.load(std::memory_order_relaxed))
            flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    return s_orig_present ? s_orig_present(sc, sync, flags) : sc->Present(sync, flags);
}

// ============================================================================
// Public API — hook management
// ============================================================================

bool SetupReflexHooks() {
    if (s_hooked.load(std::memory_order_acquire)) return true;
    if (!LoadNvapi()) return false;

    // Resolve function addresses via QueryInterface
    auto resolve = [](const char* name) -> void* {
        NvU32 id = FindFuncId(name);
        if (!id) { ul_log::Write("SetupReflexHooks: %s not in table", name); return nullptr; }
        void* ptr = s_qi(id);
        ul_log::Write("SetupReflexHooks: %s id=0x%08X addr=%p", name, id, ptr);
        return ptr;
    };

    s_tgt_sleep_mode = resolve("NvAPI_D3D_SetSleepMode");
    s_tgt_sleep = resolve("NvAPI_D3D_Sleep");
    s_tgt_marker = resolve("NvAPI_D3D_SetLatencyMarker");
    if (!s_tgt_sleep_mode || !s_tgt_sleep || !s_tgt_marker) return false;

    // Resolve GetLatency (not hooked)
    NvU32 gl_id = FindFuncId("NvAPI_D3D_GetLatency");
    if (gl_id) s_get_latency = reinterpret_cast<NvGetLatency_fn>(s_qi(gl_id));

    // Install hooks via MinHook
    struct HookDef { void* target; void* detour; void** orig; const char* name; };
    HookDef hooks[] = {
        { s_tgt_sleep_mode, (void*)Hook_SetSleepMode, (void**)&s_orig_sleep_mode, "SetSleepMode" },
        { s_tgt_sleep,      (void*)Hook_Sleep,         (void**)&s_orig_sleep,      "Sleep" },
        { s_tgt_marker,     (void*)Hook_SetMarker,     (void**)&s_orig_marker,     "SetMarker" },
    };

    for (auto& h : hooks) {
        MH_STATUS st = MH_CreateHook(h.target, h.detour, h.orig);
        if (st != MH_OK) {
            ul_log::Write("SetupReflexHooks: MH_CreateHook(%s) failed=%d", h.name, st);
            // Rollback any already-created hooks
            for (auto& r : hooks) { if (r.target) MH_RemoveHook(r.target); }
            return false;
        }
        st = MH_EnableHook(h.target);
        if (st != MH_OK) {
            ul_log::Write("SetupReflexHooks: MH_EnableHook(%s) failed=%d", h.name, st);
            for (auto& r : hooks) { MH_DisableHook(r.target); MH_RemoveHook(r.target); }
            return false;
        }
        ul_log::Write("SetupReflexHooks: %s hooked OK", h.name);
    }

    s_hooked.store(true, std::memory_order_release);
    ul_log::Write("SetupReflexHooks: all hooks installed");
    return true;
}

void TeardownReflexHooks() {
    if (!s_hooked.exchange(false, std::memory_order_acq_rel)) return;

    void* targets[] = { s_tgt_marker, s_tgt_sleep, s_tgt_sleep_mode };
    for (auto t : targets) {
        if (t) { MH_DisableHook(t); MH_RemoveHook(t); }
    }
    s_orig_sleep_mode = nullptr;
    s_orig_sleep = nullptr;
    s_orig_marker = nullptr;

    // Clean up frame latency hooks
    if (s_latency_hooked.exchange(false, std::memory_order_acq_rel)) {
        // MH_Uninitialize will handle the MinHook side, but release our COM ref
        if (s_latency_sc2) { s_latency_sc2->Release(); s_latency_sc2 = nullptr; }
        s_orig_set_max_latency = nullptr;
        s_orig_get_max_latency = nullptr;
    }
}

bool ReflexActive() { return s_hooked.load(std::memory_order_acquire); }

NvStatus InvokeSetSleepMode(IUnknown* dev, NvSleepParams* p) {
    return (s_orig_sleep_mode && dev) ? s_orig_sleep_mode(dev, p) : NV_NO_IMPL;
}

NvStatus InvokeSleep(IUnknown* dev) {
    return (s_orig_sleep && dev) ? s_orig_sleep(dev) : NV_NO_IMPL;
}

NvStatus InvokeSetMarker(IUnknown* dev, NvMarkerParams* p) {
    return (s_orig_marker && dev) ? s_orig_marker(dev, p) : NV_NO_IMPL;
}

// Global flag to disable GetLatency calls (set when conflicting frameworks detected)
static std::atomic<bool> s_getlatency_blocked{false};

NvStatus InvokeGetLatency(IUnknown* dev, NvLatencyResult* p) {
    if (s_getlatency_blocked.load(std::memory_order_relaxed)) return NV_NO_IMPL;
    return (s_get_latency && dev) ? s_get_latency(dev, p) : NV_NO_IMPL;
}

void BlockGetLatency() {
    s_getlatency_blocked.store(true, std::memory_order_relaxed);
}

bool IsStreamlineSafeMode() {
    return s_streamline_safe_mode.load(std::memory_order_acquire);
}

const GameReflexState& GetGameState() { return s_game_state; }

// ============================================================================
// Streamline proxy hook
// ============================================================================

bool HookSLProxy(IDXGISwapChain* sc) {
    if (!sc || s_sl_hooked.load(std::memory_order_acquire)) return false;

    void** vtable = *reinterpret_cast<void***>(sc);
    if (!vtable) return false;

    // IDXGISwapChain::Present is vtable index 8
    if (MH_CreateHook(vtable[8], (void*)Hook_SLPresent, (void**)&s_orig_present) != MH_OK)
        return false;
    if (MH_EnableHook(vtable[8]) != MH_OK) { MH_RemoveHook(vtable[8]); return false; }

    s_sl_hooked.store(true, std::memory_order_release);
    ul_log::Write("HookSLProxy: Present hooked");
    return true;
}

void SetSLPresentCb(SLPresentCb cb) { s_sl_cb = cb; }
void SetMarkerCb(MarkerCb cb) { s_marker_cb = cb; }

// ============================================================================
// VSync override — hook game's main swapchain Present
// ============================================================================

static HRESULT STDMETHODCALLTYPE Hook_GamePresent(IDXGISwapChain* sc, UINT SyncInterval, UINT Flags) {
    int ovr = g_cfg.vsync_override.load(std::memory_order_relaxed);
    if (ovr == 1) {
        // Force VSync ON
        SyncInterval = 1;
        Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
    } else if (ovr == 2) {
        // Force VSync OFF
        SyncInterval = 0;
        if (s_tearing_supported.load(std::memory_order_relaxed))
            Flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    return s_orig_game_present ? s_orig_game_present(sc, SyncInterval, Flags) : sc->Present(SyncInterval, Flags);
}

bool HookVSyncPresent(IDXGISwapChain* sc) {
    if (!sc || s_vsync_hooked.load(std::memory_order_acquire)) return false;

    // Check tearing support via IDXGIFactory5
    {
        IDXGIFactory5* factory5 = nullptr;
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev) {
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory)) && factory) {
                    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&factory5)) && factory5) {
                        BOOL allow = FALSE;
                        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                            s_tearing_supported.store(allow != FALSE, std::memory_order_relaxed);
                        factory5->Release();
                    }
                    factory->Release();
                }
                adapter->Release();
            }
            dxgiDev->Release();
        }
        ul_log::Write("HookVSyncPresent: tearing %s", s_tearing_supported.load() ? "supported" : "not supported");
    }

    void** vtable = *reinterpret_cast<void***>(sc);
    if (!vtable) return false;

    // If SL proxy already hooked this vtable slot, skip
    if (s_sl_hooked.load(std::memory_order_acquire)) {
        ul_log::Write("HookVSyncPresent: skipped — SL proxy already hooked Present");
        return false;
    }

    if (MH_CreateHook(vtable[8], (void*)Hook_GamePresent, (void**)&s_orig_game_present) != MH_OK)
        return false;
    if (MH_EnableHook(vtable[8]) != MH_OK) { MH_RemoveHook(vtable[8]); return false; }

    s_vsync_hooked.store(true, std::memory_order_release);
    ul_log::Write("HookVSyncPresent: Present hooked for VSync control");
    return true;
}

// ============================================================================
// 5XXX Exclusive Pacing Optimization — frame latency override
// ============================================================================

// IDXGISwapChain2::SetMaximumFrameLatency hook.
// When exclusive_pacing is enabled we force the value to 1 (single-frame
// queue = lowest input latency). When disabled we pass through the game's
// original value unchanged.

static HRESULT STDMETHODCALLTYPE Hook_SetMaxFrameLatency(IDXGISwapChain* sc, UINT MaxLatency) {
    // Remember what the game wanted so we can restore it if the user toggles off
    s_game_latency.store(MaxLatency, std::memory_order_relaxed);

    if (g_cfg.exclusive_pacing.load(std::memory_order_relaxed)) {
        // Throttle logging — only log the first override and when the value changes
        static UINT s_last_logged = UINT_MAX;
        if (MaxLatency != s_last_logged) {
            ul_log::Write("Hook_SetMaxFrameLatency: game requested %u, overriding to 1", MaxLatency);
            s_last_logged = MaxLatency;
        }
        MaxLatency = 1;
    }

    return s_orig_set_max_latency ? s_orig_set_max_latency(sc, MaxLatency) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Hook_GetMaxFrameLatency(IDXGISwapChain* sc, UINT* pMaxLatency) {
    // If we're overriding, report the game's original value back so it doesn't
    // get confused by our override.
    if (g_cfg.exclusive_pacing.load(std::memory_order_relaxed) && pMaxLatency) {
        UINT game_val = s_game_latency.load(std::memory_order_relaxed);
        if (game_val > 0) {
            *pMaxLatency = game_val;
            return S_OK;
        }
    }
    return s_orig_get_max_latency ? s_orig_get_max_latency(sc, pMaxLatency) : E_FAIL;
}

bool HookFrameLatency(IDXGISwapChain* sc) {
    if (!sc || s_latency_hooked.load(std::memory_order_acquire)) return false;

    // Only hook if the swapchain was created with the waitable object flag.
    // SetMaximumFrameLatency on IDXGISwapChain2 is only valid for waitable
    // swapchains; calling it otherwise returns DXGI_ERROR_INVALID_CALL and
    // can confuse some driver versions.
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc)) ||
            !(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
            ul_log::Write("HookFrameLatency: skipped — swapchain not waitable (flags=0x%X)",
                          desc.Flags);
            return false;
        }
    }

    // Need IDXGISwapChain2 for SetMaximumFrameLatency (vtable slots 31/32)
    IDXGISwapChain2* sc2 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) || !sc2)
        return false;

    void** vtable = *reinterpret_cast<void***>(sc2);
    if (!vtable) { sc2->Release(); return false; }

    // vtable[31] = SetMaximumFrameLatency, vtable[32] = GetMaximumFrameLatency
    MH_STATUS st = MH_CreateHook(vtable[31], (void*)Hook_SetMaxFrameLatency,
                                 (void**)&s_orig_set_max_latency);
    if (st != MH_OK) {
        ul_log::Write("HookFrameLatency: MH_CreateHook(Set) failed=%d", st);
        sc2->Release();
        return false;
    }
    if (MH_EnableHook(vtable[31]) != MH_OK) {
        MH_RemoveHook(vtable[31]);
        sc2->Release();
        return false;
    }

    st = MH_CreateHook(vtable[32], (void*)Hook_GetMaxFrameLatency,
                       (void**)&s_orig_get_max_latency);
    if (st != MH_OK) {
        ul_log::Write("HookFrameLatency: MH_CreateHook(Get) failed=%d", st);
        // Set hook is already live, leave it — Get is optional
    } else {
        MH_EnableHook(vtable[32]);
    }

    s_latency_sc2 = sc2;  // keep the ref — do NOT Release here
    s_latency_hooked.store(true, std::memory_order_release);

    // If exclusive pacing is already enabled at startup, apply immediately.
    // Call the trampoline directly to avoid going through our hook (which
    // would overwrite s_game_latency with our override value).
    if (g_cfg.exclusive_pacing.load(std::memory_order_relaxed) && s_orig_set_max_latency) {
        s_orig_set_max_latency(reinterpret_cast<IDXGISwapChain*>(sc2), 1);
        ul_log::Write("HookFrameLatency: applied initial override to 1");
    }

    ul_log::Write("HookFrameLatency: hooked SetMaximumFrameLatency/GetMaximumFrameLatency");
    return true;
}

void ApplyFrameLatency() {
    if (!s_latency_hooked.load(std::memory_order_acquire) || !s_latency_sc2) return;
    if (!s_orig_set_max_latency) return;

    // Call the trampoline directly — going through the vtable would hit our
    // hook and corrupt s_game_latency with the override value.
    if (g_cfg.exclusive_pacing.load(std::memory_order_relaxed)) {
        s_orig_set_max_latency(reinterpret_cast<IDXGISwapChain*>(s_latency_sc2), 1);
        ul_log::Write("ApplyFrameLatency: set to 1 (exclusive pacing ON)");
    } else {
        UINT restore = s_game_latency.load(std::memory_order_relaxed);
        if (restore == 0) restore = 3;  // sensible default if game never called it
        s_orig_set_max_latency(reinterpret_cast<IDXGISwapChain*>(s_latency_sc2), restore);
        ul_log::Write("ApplyFrameLatency: restored to %u (exclusive pacing OFF)", restore);
    }
}
