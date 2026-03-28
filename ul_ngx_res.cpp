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
constexpr unsigned int NVSDK_NGX_Feature_SuperSampling = 0;
constexpr unsigned int NVSDK_NGX_Feature_RayReconstruction = 4;

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

// ============================================================================
// Common parameter extraction — called from both D3D11 and D3D12 hooks
// ============================================================================

static void ExtractDlssParams(void* params, unsigned int feature_id) {
    // Detect Ray Reconstruction (feature 4)
    if (feature_id == NVSDK_NGX_Feature_RayReconstruction) {
        s_ray_reconstruction.store(true, std::memory_order_relaxed);
        ul_log::Write("NGX: DLSS Ray Reconstruction feature created");
        return;
    }

    // Only intercept DLSS Super Resolution (feature 0)
    if (feature_id != NVSDK_NGX_Feature_SuperSampling) return;
    if (!params) return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    int quality = -1;

    __try {
        NGX_GetUI(params, "Width", &w);
        NGX_GetUI(params, "Height", &h);
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
    return s_orig_d3d12_create(cmd_list, feature, params, handle);
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
    return s_orig_d3d11_create(dev_ctx, feature, params, handle);
}

// ============================================================================
// Hook installation
// ============================================================================

static bool s_hooks_installed = false;

static bool TryHookModule(const wchar_t* dll_name) {
    HMODULE mod = GetModuleHandleW(dll_name);
    if (!mod) return false;

    bool hooked_any = false;

    // D3D12
    auto d3d12_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D12_CreateFeature"));
    if (d3d12_fn && !s_orig_d3d12_create) {
        MH_STATUS st = MH_CreateHook(d3d12_fn,
            reinterpret_cast<void*>(&Hook_D3D12_CreateFeature),
            reinterpret_cast<void**>(&s_orig_d3d12_create));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d12_fn);
            if (st == MH_OK) {
                ul_log::Write("NGX: D3D12_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d12_fn);
                s_orig_d3d12_create = nullptr;
            }
        }
    }

    // D3D11
    auto d3d11_fn = reinterpret_cast<void*>(
        GetProcAddress(mod, "NVSDK_NGX_D3D11_CreateFeature"));
    if (d3d11_fn && !s_orig_d3d11_create) {
        MH_STATUS st = MH_CreateHook(d3d11_fn,
            reinterpret_cast<void*>(&Hook_D3D11_CreateFeature),
            reinterpret_cast<void**>(&s_orig_d3d11_create));
        if (st == MH_OK) {
            st = MH_EnableHook(d3d11_fn);
            if (st == MH_OK) {
                ul_log::Write("NGX: D3D11_CreateFeature hooked from %ls", dll_name);
                hooked_any = true;
            } else {
                MH_RemoveHook(d3d11_fn);
                s_orig_d3d11_create = nullptr;
            }
        }
    }

    return hooked_any;
}

namespace ul_ngx_res {

bool InstallHooks() {
    if (s_hooks_installed) return true;

    // Try both possible NGX loader DLL names
    bool ok = TryHookModule(L"_nvngx.dll");
    if (!ok) ok = TryHookModule(L"nvngx.dll");

    if (ok) s_hooks_installed = true;
    return ok;
}

void RemoveHooks() {
    if (!s_hooks_installed) return;

    auto unhook = [](const wchar_t* dll, const char* fn_name) {
        HMODULE mod = GetModuleHandleW(dll);
        if (!mod) return;
        auto fn = reinterpret_cast<void*>(GetProcAddress(mod, fn_name));
        if (fn) { MH_DisableHook(fn); MH_RemoveHook(fn); }
    };

    unhook(L"_nvngx.dll", "NVSDK_NGX_D3D12_CreateFeature");
    unhook(L"_nvngx.dll", "NVSDK_NGX_D3D11_CreateFeature");
    unhook(L"nvngx.dll", "NVSDK_NGX_D3D12_CreateFeature");
    unhook(L"nvngx.dll", "NVSDK_NGX_D3D11_CreateFeature");

    s_orig_d3d12_create = nullptr;
    s_orig_d3d11_create = nullptr;
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
    if (q == 5) return true;  // explicit DLAA mode
    // Also detect DLAA by resolution match (render == output)
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
}

}  // namespace ul_ngx_res

#endif // _WIN64
