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

// ============================================================================
// Globals
// ============================================================================

MarkerSlot g_ring[kRingSize] = {};
std::atomic<bool> g_game_uses_reflex{false};

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

    NvU32 init_id = FindFuncId("NvAPI_Initialize");
    if (!init_id) { ul_log::Write("LoadNvapi: NvAPI_Initialize ID not in table"); return false; }

    auto init_fn = reinterpret_cast<NvInitialize_fn>(qi(init_id));
    if (!init_fn) { ul_log::Write("LoadNvapi: QueryInterface(Initialize) null"); return false; }

    NvStatus st = init_fn();
    if (st != NV_OK) { ul_log::Write("LoadNvapi: Initialize returned %d", st); return false; }

    s_nvapi_dll = dll;
    s_qi = qi;
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

// Intercept Sleep: suppress — we call Sleep when we want to pace.
static NvStatus __cdecl Hook_Sleep(IUnknown*) {
    return NV_OK;
}

// Intercept SetLatencyMarker: record timestamp, notify callback, forward to driver.
static NvStatus __cdecl Hook_SetMarker(IUnknown* dev, NvMarkerParams* p) {
    if (!dev || !p) {
        if (s_orig_marker) return s_orig_marker(dev, p);
        return NV_BAD_ARG;
    }

    g_game_uses_reflex.store(true, std::memory_order_relaxed);

    int mt = static_cast<int>(p->markerType);
    uint64_t fid = p->frameID;

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

NvStatus InvokeGetLatency(IUnknown* dev, NvLatencyResult* p) {
    return (s_get_latency && dev) ? s_get_latency(dev, p) : NV_NO_IMPL;
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
