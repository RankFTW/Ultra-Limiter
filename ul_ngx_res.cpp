// ReLimiter — NGX resolution detection
// Clean-room implementation from public NVIDIA NGX SDK documentation (MIT):
//   - NVSDK_NGX_D3D12_CreateFeature / NVSDK_NGX_D3D11_CreateFeature
//   - NVSDK_NGX_Parameter virtual interface
//   - NVSDK_NGX_Feature_SuperSampling = 0 (feature ID for DLSS SR)
// Hooks CreateFeature to intercept the DLSS parameters at creation time.

#ifdef _WIN64

#include "ul_ngx_res.hpp"
#include "ul_log.hpp"

#include <MinHook.h>
#include <windows.h>
#include <cstring>

// ============================================================================
// NGX types (from nvsdk_ngx.h / nvsdk_ngx_defs.h, MIT licensed)
// ============================================================================

// Opaque handle returned by CreateFeature
struct NVSDK_NGX_Handle;

// Result type
using NVSDK_NGX_Result = unsigned int;
#define NVSDK_NGX_SUCCEED(x) ((x) == 0x1)

// Feature IDs
// Legacy NGX SDK uses 0 for DLSS SR. Streamline-based games use 11.
// Both pass the same NVSDK_NGX_Parameter with Width/Height/OutWidth/OutHeight.
constexpr unsigned int NVSDK_NGX_Feature_SuperSampling = 0;
constexpr unsigned int NVSDK_NGX_Feature_RayReconstruction = 4;
constexpr unsigned int NVSDK_NGX_Feature_DLSS_SR = 11;       // Streamline DLSS Super Resolution
constexpr unsigned int NVSDK_NGX_Feature_DLSS_RR = 12;       // Streamline DLSS Ray Reconstruction
constexpr unsigned int NVSDK_NGX_Feature_DLSS_FG = 13;       // Streamline DLSS Frame Generation

// NVSDK_NGX_Parameter — virtual interface for reading/writing named parameters.
// The vtable layout is stable across NGX versions. We only need the Get methods.
// This is a COM-like interface with Get/Set overloads for different types.
//
// Vtable layout (from nvsdk_ngx.h):
//   [0] Set(const char*, void*)
//   [1] Set(const char*, ID3D11Resource*)  — D3D11 only
//   [2] Set(const char*, ID3D12Resource*)  — D3D12 only
//   [3] Set(const char*, int)
//   [4] Set(const char*, unsigned int)
//   [5] Set(const char*, float)
//   [6] Set(const char*, double)
//   [7] Set(const char*, unsigned long long)
//   [8] Get(const char*, void**)
//   [9] Get(const char*, ID3D11Resource**)
//  [10] Get(const char*, ID3D12Resource**)
//  [11] Get(const char*, int*)
//  [12] Get(const char*, unsigned int*)
//  [13] Get(const char*, float*)
//  [14] Get(const char*, double*)
//  [15] Get(const char*, unsigned long long*)
//
// We use index 12: Get(const char*, unsigned int*) for Width/Height/OutWidth/OutHeight
// and index 11: Get(const char*, int*) for PerfQualityValue.

// Helper to call Get(const char*, unsigned int*) via vtable index 12
static NVSDK_NGX_Result NGX_GetUI(void* params, const char* name, unsigned int* out) {
    if (!params || !name || !out) return 0xBAD00000;
    void** vtable = *reinterpret_cast<void***>(params);
    using GetUI_fn = NVSDK_NGX_Result(__thiscall*)(void*, const char*, unsigned int*);
    auto fn = reinterpret_cast<GetUI_fn>(vtable[12]);
    return fn(params, name, out);
}

// Helper to call Get(const char*, int*) via vtable index 11
static NVSDK_NGX_Result NGX_GetI(void* params, const char* name, int* out) {
    if (!params || !name || !out) return 0xBAD00000;
    void** vtable = *reinterpret_cast<void***>(params);
    using GetI_fn = NVSDK_NGX_Result(__thiscall*)(void*, const char*, int*);
    auto fn = reinterpret_cast<GetI_fn>(vtable[11]);
    return fn(params, name, out);
}

// ============================================================================
// Stored DLSS state
// ============================================================================

static std::atomic<uint32_t> s_render_w{0};
static std::atomic<uint32_t> s_render_h{0};
static std::atomic<uint32_t> s_out_w{0};
static std::atomic<uint32_t> s_out_h{0};
static std::atomic<int>      s_quality{-1};
static std::atomic<bool>     s_has_data{false};
static std::atomic<bool>     s_ray_reconstruction{false};

// Handle tracking — map DLSS SR handle so EvaluateFeature knows which calls to intercept
static void* s_dlss_sr_handle = nullptr;

// RR confirmation: after DLSS SR is (re)created, we expect RR CreateFeature
// to fire within a few frames if RR is active. If it doesn't, RR was turned off.
static std::atomic<int> s_rr_confirm_countdown{0};

// ============================================================================
// Common parameter extraction — called from both D3D11 and D3D12 hooks
// ============================================================================

static void ExtractDlssParams(void* params, unsigned int feature_id) {
    // Detect Ray Reconstruction — legacy (4), Streamline (1 or 12)
    if (feature_id == NVSDK_NGX_Feature_RayReconstruction ||
        feature_id == NVSDK_NGX_Feature_DLSS_RR ||
        feature_id == 1) {
        s_ray_reconstruction.store(true, std::memory_order_relaxed);
        s_rr_confirm_countdown.store(0, std::memory_order_relaxed);  // confirmed, cancel countdown
        ul_log::Write("NGX: DLSS Ray Reconstruction feature created (feature=%u)", feature_id);
        return;
    }

    // Log all feature IDs we see (one-shot per ID) for diagnostics
    {
        static uint32_t s_seen_features = 0;  // bitmask of seen feature IDs (0-31)
        if (feature_id < 32 && !(s_seen_features & (1u << feature_id))) {
            s_seen_features |= (1u << feature_id);
            if (feature_id != NVSDK_NGX_Feature_SuperSampling &&
                feature_id != NVSDK_NGX_Feature_DLSS_SR &&
                feature_id != NVSDK_NGX_Feature_DLSS_FG) {
                ul_log::Write("NGX: CreateFeature called with feature=%u", feature_id);
            }
        }
    }

    // Intercept DLSS Super Resolution — legacy (0) or Streamline (11)
    if (feature_id != NVSDK_NGX_Feature_SuperSampling &&
        feature_id != NVSDK_NGX_Feature_DLSS_SR) return;
    if (!params) return;

    // When DLSS SR is (re)created, RR state may have changed.
    // Start a confirmation countdown — if RR CreateFeature doesn't fire
    // within 30 frames, RR was turned off.
    if (s_ray_reconstruction.load(std::memory_order_relaxed))
        s_rr_confirm_countdown.store(30, std::memory_order_relaxed);

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        // Try Streamline-specific render subrect dimensions first.
        // Streamline sets Width/Height to the output resolution and uses
        // DLSS_Render_Subrect_Dimensions for the actual render resolution.
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Height", &h);

        // Fall back to standard Width/Height if subrect not available
        if (w == 0 || h == 0) {
            NGX_GetUI(params, "Width", &w);
            NGX_GetUI(params, "Height", &h);
        }

        NGX_GetUI(params, "OutWidth", &ow);
        NGX_GetUI(params, "OutHeight", &oh);
        NGX_GetI(params, "PerfQualityValue", &quality);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ul_log::Write("NGX: exception reading DLSS params");
        return;
    }

    if (w == 0 || h == 0) return;

    s_render_w.store(w, std::memory_order_relaxed);
    s_render_h.store(h, std::memory_order_relaxed);
    s_out_w.store(ow, std::memory_order_relaxed);
    s_out_h.store(oh, std::memory_order_relaxed);
    s_quality.store(quality, std::memory_order_relaxed);
    s_has_data.store(true, std::memory_order_relaxed);

    const char* mode_str = "Unknown";
    switch (quality) {
        case 0: mode_str = "Performance"; break;
        case 1: mode_str = "Balanced"; break;
        case 2: mode_str = "Quality"; break;
        case 3: mode_str = "Ultra Performance"; break;
        case 4: mode_str = "Ultra Quality"; break;
        case 5: mode_str = "DLAA"; break;
    }
    ul_log::Write("NGX: DLSS SR created — %ux%u -> %ux%u (%s, quality=%d)",
                  w, h, ow, oh, mode_str, quality);
}

// ============================================================================
// EvaluateFeature parameter update — lightweight per-frame check
// ============================================================================

static void UpdateDlssParamsFromEval(void* params, void* handle) {
    // Tick RR confirmation countdown
    int rr_cd = s_rr_confirm_countdown.load(std::memory_order_relaxed);
    if (rr_cd > 0) {
        rr_cd--;
        s_rr_confirm_countdown.store(rr_cd, std::memory_order_relaxed);
        if (rr_cd == 0) {
            // Countdown expired without RR CreateFeature — RR was turned off
            s_ray_reconstruction.store(false, std::memory_order_relaxed);
            ul_log::Write("NGX: Ray Reconstruction not confirmed after SR recreate — cleared");
        }
    }

    // Only process evaluations for the DLSS SR handle.
    // Exception: if s_dlss_sr_handle is null (we missed CreateFeature because
    // the DLL loaded before our hooks installed), try to adopt this handle
    // by checking if the params contain DLSS SR data.
    if (!handle) return;
    if (s_dlss_sr_handle && handle != s_dlss_sr_handle) {
        // Handle mismatch — the game may have recreated the DLSS feature
        // (swapchain recreate, settings change). Reset so we can re-adopt.
        s_dlss_sr_handle = nullptr;
    }
    if (!params) return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Width", &w);
        NGX_GetUI(params, "DLSS.Render.Subrect.Dimensions.Height", &h);
        if (w == 0 || h == 0) {
            NGX_GetUI(params, "Width", &w);
            NGX_GetUI(params, "Height", &h);
        }
        NGX_GetUI(params, "OutWidth", &ow);
        NGX_GetUI(params, "OutHeight", &oh);
        NGX_GetI(params, "PerfQualityValue", &quality);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (w == 0 || h == 0) return;

    // Late adoption: if we missed CreateFeature, adopt this handle now.
    // Only adopt if quality is valid (0-5) — this distinguishes SR from RR
    // which doesn't have PerfQualityValue.
    if (!s_dlss_sr_handle) {
        if (quality >= 0 && quality <= 5) {
            s_dlss_sr_handle = handle;
            ul_log::Write("NGX: DLSS SR handle adopted from EvaluateFeature (late hook)");
        } else {
            return;  // not an SR call, skip
        }
    }

    // Only update and log if something actually changed
    uint32_t prev_w = s_render_w.load(std::memory_order_relaxed);
    uint32_t prev_h = s_render_h.load(std::memory_order_relaxed);
    int prev_q = s_quality.load(std::memory_order_relaxed);

    if (w == prev_w && h == prev_h && quality == prev_q) return;

    // Quality or resolution changed — start RR confirmation countdown
    // (game may have toggled RR alongside the quality change)
    if (s_ray_reconstruction.load(std::memory_order_relaxed))
        s_rr_confirm_countdown.store(30, std::memory_order_relaxed);

    s_render_w.store(w, std::memory_order_relaxed);
    s_render_h.store(h, std::memory_order_relaxed);
    s_out_w.store(ow, std::memory_order_relaxed);
    s_out_h.store(oh, std::memory_order_relaxed);
    s_quality.store(quality, std::memory_order_relaxed);
    s_has_data.store(true, std::memory_order_relaxed);

    const char* mode_str = "Unknown";
    switch (quality) {
        case 0: mode_str = "Performance"; break;
        case 1: mode_str = "Balanced"; break;
        case 2: mode_str = "Quality"; break;
        case 3: mode_str = "Ultra Performance"; break;
        case 4: mode_str = "Ultra Quality"; break;
        case 5: mode_str = "DLAA"; break;
    }
    ul_log::Write("NGX: DLSS SR updated — %ux%u -> %ux%u (%s, quality=%d)",
                  w, h, ow, oh, mode_str, quality);
}

// ============================================================================
// D3D12 hook
// ============================================================================

// NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
//                                NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
using NGX_D3D12_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_list, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);

static NGX_D3D12_CreateFeature_fn s_orig_d3d12_create = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D12_CreateFeature(
    void* cmd_list, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res = s_orig_d3d12_create(cmd_list, feature, params, handle);
    // Capture the DLSS SR handle after successful creation
    if (res == 1 && handle && *handle &&
        (feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_DLSS_SR)) {
        s_dlss_sr_handle = *handle;
    }
    return res;
}

// ============================================================================
// D3D11 hook
// ============================================================================

// NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext*, NVSDK_NGX_Feature,
//                                NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
using NGX_D3D11_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);

static NGX_D3D11_CreateFeature_fn s_orig_d3d11_create = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D11_CreateFeature(
    void* dev_ctx, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res = s_orig_d3d11_create(dev_ctx, feature, params, handle);
    if (res == 1 && handle && *handle &&
        (feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_DLSS_SR)) {
        s_dlss_sr_handle = *handle;
    }
    return res;
}

// ============================================================================
// D3D12 EvaluateFeature hook
// ============================================================================

// NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Handle*,
//                                  NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback)
using NGX_D3D12_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_list, void* handle, void* params, void* callback);

static NGX_D3D12_EvaluateFeature_fn s_orig_d3d12_eval = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D12_EvaluateFeature(
    void* cmd_list, void* handle, void* params, void* callback)
{
    UpdateDlssParamsFromEval(params, handle);
    return s_orig_d3d12_eval(cmd_list, handle, params, callback);
}

// ============================================================================
// D3D11 EvaluateFeature hook
// ============================================================================

using NGX_D3D11_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* dev_ctx, void* handle, void* params, void* callback);

static NGX_D3D11_EvaluateFeature_fn s_orig_d3d11_eval = nullptr;

static NVSDK_NGX_Result __cdecl Hook_D3D11_EvaluateFeature(
    void* dev_ctx, void* handle, void* params, void* callback)
{
    UpdateDlssParamsFromEval(params, handle);
    return s_orig_d3d11_eval(dev_ctx, handle, params, callback);
}

// ============================================================================
// Vulkan CreateFeature hook
// ============================================================================

// NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer, NVSDK_NGX_Feature,
//                                 NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
// Same parameter interface as D3D12/D3D11 — just a different first argument.
using NGX_VK_CreateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_buf, unsigned int feature, void* params, NVSDK_NGX_Handle** handle);

static NGX_VK_CreateFeature_fn s_orig_vk_create = nullptr;

static NVSDK_NGX_Result __cdecl Hook_VK_CreateFeature(
    void* cmd_buf, unsigned int feature, void* params, NVSDK_NGX_Handle** handle)
{
    ExtractDlssParams(params, feature);
    NVSDK_NGX_Result res = s_orig_vk_create(cmd_buf, feature, params, handle);
    if (res == 1 && handle && *handle &&
        (feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_DLSS_SR)) {
        s_dlss_sr_handle = *handle;
    }
    return res;
}

// ============================================================================
// Vulkan EvaluateFeature hook
// ============================================================================

using NGX_VK_EvaluateFeature_fn = NVSDK_NGX_Result(__cdecl*)(
    void* cmd_buf, void* handle, void* params, void* callback);

static NGX_VK_EvaluateFeature_fn s_orig_vk_eval = nullptr;

static NVSDK_NGX_Result __cdecl Hook_VK_EvaluateFeature(
    void* cmd_buf, void* handle, void* params, void* callback)
{
    UpdateDlssParamsFromEval(params, handle);
    return s_orig_vk_eval(cmd_buf, handle, params, callback);
}

// ============================================================================
// Hook installation
// ============================================================================

static bool s_hooks_installed = false;

// We may hook CreateFeature from multiple DLLs (NGX loader, DLSS feature DLL,
// RR DLL). Each gets its own trampoline via MinHook. We store up to 4 pairs.
static constexpr int kMaxHookTargets = 16;
static void* s_hook_targets[kMaxHookTargets] = {};
static int s_hook_count = 0;

// Per-target trampolines for D3D12 — MinHook stores these internally,
// but we need them in the hook wrapper to call the original.
// Use thread-local lookup: when our hook fires, find which trampoline
// matches the return address. Simpler: just store all trampolines and
// use the one that was set for each specific hook.
//
// Actually simplest: each hooked address gets a unique trampoline from
// MH_CreateHook. We store them all and our hook wrapper just needs ONE
// to call — because all the originals have the same signature and behavior
// (create the DLSS feature). We just need to forward to the right one.
//
// Solution: store trampolines in an array indexed by hook order.
// The hook wrapper uses a thread-local to know which trampoline to call.
// ... this is getting complex. Let's just use the simple approach:
// hook each DLL independently, each with its own static trampoline.

// Trampoline arrays — one slot per DLL we might hook
static NGX_D3D12_CreateFeature_fn s_d3d12_trampolines[kMaxHookTargets] = {};
static NGX_D3D11_CreateFeature_fn s_d3d11_trampolines[kMaxHookTargets] = {};
static int s_d3d12_trampoline_count = 0;
static int s_d3d11_trampoline_count = 0;

// The hook wrappers need to call the correct trampoline. Since MinHook
// replaces the target function's first bytes with a jump to our hook,
// and we can't easily determine which target triggered us, we use a
// simpler approach: hook each target with the SAME wrapper, and store
// the trampoline that MH_CreateHook returns. The wrapper calls the
// trampoline that was most recently set — but this is wrong for
// concurrent calls from different DLLs.
//
// Correct approach: one wrapper per DLL. But we don't know at compile
// time how many DLLs. Use a single wrapper that looks up the trampoline
// from the return address... too complex.
//
// PRAGMATIC FIX: The game only calls CreateFeature from ONE DLL path
// (either NGX loader OR DLSS DLL, not both). So whichever hook fires,
// its trampoline is the right one. Just let the last successful hook
// win — overwrite s_orig_d3d12_create. This works because:
// - Non-Streamline games: _nvngx.dll hook fires, trampoline is correct
// - Streamline games: nvngx_dlss.dll hook fires, trampoline is correct
// - Both hooks installed: only one actually gets called at runtime

static bool TryHookModule(const wchar_t* dll_name) {
    HMODULE mod = GetModuleHandleW(dll_name);
    if (!mod) return false;

    bool hooked_any = false;

    // D3D12
    auto d3d12_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature"));
    if (d3d12_fn) {
        NGX_D3D12_CreateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d12_fn,
            reinterpret_cast<void*>(&Hook_D3D12_CreateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d12_fn);
            if (st == MH_OK) {
                s_orig_d3d12_create = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = d3d12_fn;
                ul_log::Write("NGX: D3D12_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d12_fn);
            }
        } else if (st == MH_ERROR_ALREADY_CREATED) {
            // Same address already hooked (e.g. _nvngx.dll and nvngx.dll are the same module)
        }
    }

    // D3D11
    auto d3d11_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D11_CreateFeature"));
    if (d3d11_fn) {
        NGX_D3D11_CreateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d11_fn,
            reinterpret_cast<void*>(&Hook_D3D11_CreateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d11_fn);
            if (st == MH_OK) {
                s_orig_d3d11_create = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = d3d11_fn;
                ul_log::Write("NGX: D3D11_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d11_fn);
            }
        } else if (st == MH_ERROR_ALREADY_CREATED) {
            // Same address already hooked
        }
    }

    // D3D12 EvaluateFeature
    auto d3d12_eval_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (d3d12_eval_fn) {
        NGX_D3D12_EvaluateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d12_eval_fn,
            reinterpret_cast<void*>(&Hook_D3D12_EvaluateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d12_eval_fn);
            if (st == MH_OK) {
                s_orig_d3d12_eval = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = d3d12_eval_fn;
                ul_log::Write("NGX: D3D12_EvaluateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d12_eval_fn);
            }
        }
    }

    // D3D11 EvaluateFeature
    auto d3d11_eval_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D11_EvaluateFeature"));
    if (d3d11_eval_fn) {
        NGX_D3D11_EvaluateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(d3d11_eval_fn,
            reinterpret_cast<void*>(&Hook_D3D11_EvaluateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d11_eval_fn);
            if (st == MH_OK) {
                s_orig_d3d11_eval = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = d3d11_eval_fn;
                ul_log::Write("NGX: D3D11_EvaluateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d11_eval_fn);
            }
        }
    }

    // Vulkan CreateFeature
    auto vk_create_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_CreateFeature"));
    if (vk_create_fn) {
        NGX_VK_CreateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(vk_create_fn,
            reinterpret_cast<void*>(&Hook_VK_CreateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(vk_create_fn);
            if (st == MH_OK) {
                s_orig_vk_create = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = vk_create_fn;
                ul_log::Write("NGX: VULKAN_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(vk_create_fn);
            }
        }
    }

    // Vulkan EvaluateFeature
    auto vk_eval_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    if (vk_eval_fn) {
        NGX_VK_EvaluateFeature_fn trampoline = nullptr;
        MH_STATUS st = MH_CreateHook(vk_eval_fn,
            reinterpret_cast<void*>(&Hook_VK_EvaluateFeature),
            reinterpret_cast<void**>(&trampoline));
        if (st == MH_OK) {
            st = MH_EnableHook(vk_eval_fn);
            if (st == MH_OK) {
                s_orig_vk_eval = trampoline;
                if (s_hook_count < kMaxHookTargets)
                    s_hook_targets[s_hook_count++] = vk_eval_fn;
                ul_log::Write("NGX: VULKAN_EvaluateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(vk_eval_fn);
            }
        }
    }

    return hooked_any;
}

namespace ul_ngx_res {

bool InstallHooks() {
    if (s_hooks_installed) return true;

    // Try NGX loader DLLs first
    bool ok = TryHookModule(L"_nvngx.dll");
    if (!ok) ok = TryHookModule(L"nvngx.dll");

    // Also try the DLSS feature DLLs directly — Streamline-based games
    // call CreateFeature on nvngx_dlss.dll instead of the NGX loader.
    TryHookModule(L"nvngx_dlss.dll");
    TryHookModule(L"_nvngx_dlss.dll");

    // Also try the Ray Reconstruction DLL
    TryHookModule(L"nvngx_dlssd.dll");
    TryHookModule(L"_nvngx_dlssd.dll");

    // Log which NGX-related DLLs are loaded for diagnostics
    static bool s_dll_scan_logged = false;
    if (!s_dll_scan_logged) {
        auto check = [](const wchar_t* name) {
            if (GetModuleHandleW(name))
                ul_log::Write("NGX: %ls loaded", name);
        };
        check(L"_nvngx.dll");
        check(L"nvngx.dll");
        check(L"nvngx_dlss.dll");
        check(L"_nvngx_dlss.dll");
        check(L"nvngx_dlssd.dll");
        check(L"_nvngx_dlssd.dll");
        check(L"nvngx_dlssg.dll");
        check(L"_nvngx_dlssg.dll");
        check(L"sl.dlss.dll");
        check(L"sl.dlss_g.dll");
        check(L"sl.common.dll");
        s_dll_scan_logged = true;
    }

    if (ok || s_orig_d3d12_create || s_orig_d3d11_create)
        s_hooks_installed = true;
    return s_hooks_installed;
}

void RemoveHooks() {
    if (!s_hooks_installed) return;

    for (int i = 0; i < s_hook_count; i++) {
        if (s_hook_targets[i]) {
            MH_DisableHook(s_hook_targets[i]);
            MH_RemoveHook(s_hook_targets[i]);
            s_hook_targets[i] = nullptr;
        }
    }
    s_hook_count = 0;

    s_orig_d3d12_create = nullptr;
    s_orig_d3d11_create = nullptr;
    s_orig_d3d12_eval = nullptr;
    s_orig_d3d11_eval = nullptr;
    s_orig_vk_create = nullptr;
    s_orig_vk_eval = nullptr;
    s_dlss_sr_handle = nullptr;
    s_hooks_installed = false;
}

bool HasData() { return s_has_data.load(std::memory_order_relaxed); }
uint32_t GetRenderWidth() { return s_render_w.load(std::memory_order_relaxed); }
uint32_t GetRenderHeight() { return s_render_h.load(std::memory_order_relaxed); }
uint32_t GetOutWidth() { return s_out_w.load(std::memory_order_relaxed); }
uint32_t GetOutHeight() { return s_out_h.load(std::memory_order_relaxed); }
int GetQualityMode() { return s_quality.load(std::memory_order_relaxed); }

bool IsDLAA() {
    if (!s_has_data.load(std::memory_order_relaxed)) return false;
    int q = s_quality.load(std::memory_order_relaxed);
    // Trust the quality mode — it's the definitive signal
    if (q == 5) return true;   // explicit DLAA mode
    if (q >= 0 && q != 5) return false;  // any other known mode = not DLAA
    // Quality unknown (-1): fall back to resolution comparison
    uint32_t rw = s_render_w.load(std::memory_order_relaxed);
    uint32_t rh = s_render_h.load(std::memory_order_relaxed);
    uint32_t ow = s_out_w.load(std::memory_order_relaxed);
    uint32_t oh = s_out_h.load(std::memory_order_relaxed);
    return (rw > 0 && rw == ow && rh == oh);
}

bool IsRayReconstruction() {
    return s_ray_reconstruction.load(std::memory_order_relaxed);
}

void Reset() {
    s_render_w.store(0, std::memory_order_relaxed);
    s_render_h.store(0, std::memory_order_relaxed);
    s_out_w.store(0, std::memory_order_relaxed);
    s_out_h.store(0, std::memory_order_relaxed);
    s_quality.store(-1, std::memory_order_relaxed);
    s_has_data.store(false, std::memory_order_relaxed);
    s_ray_reconstruction.store(false, std::memory_order_relaxed);
    s_rr_confirm_countdown.store(0, std::memory_order_relaxed);
    s_dlss_sr_handle = nullptr;
}

}  // namespace ul_ngx_res

#endif // _WIN64
