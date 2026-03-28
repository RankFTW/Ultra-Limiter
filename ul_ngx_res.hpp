#pragma once
// ReLimiter — NGX resolution detection
// Clean-room implementation from public NVIDIA NGX SDK documentation (MIT):
//   - NVSDK_NGX_D3D12_CreateFeature / NVSDK_NGX_D3D11_CreateFeature signatures
//   - NVSDK_NGX_Parameter interface (Get/Set with string keys)
//   - Parameter names: "Width", "Height", "OutWidth", "OutHeight", "PerfQualityValue"
// Hooks CreateFeature to read the DLSS render resolution and quality mode
// directly from the game's NGX parameters. No guessing from viewports.

#ifdef _WIN64  // NGX is 64-bit only

#include <cstdint>
#include <atomic>

namespace ul_ngx_res {

// DLSS quality modes (from nvsdk_ngx_defs.h, MIT)
enum class DlssQuality : int {
    MaxPerf = 0,          // Performance
    Balanced = 1,
    MaxQuality = 2,       // Quality
    UltraPerformance = 3,
    UltraQuality = 4,
    DLAA = 5,
};

// Install hooks on NVSDK_NGX_D3D12_CreateFeature and D3D11 variant.
// Call after the game has loaded nvngx.dll / _nvngx.dll.
// Safe to call multiple times (no-ops after first success).
bool InstallHooks();

// Remove hooks. Call during shutdown.
void RemoveHooks();

// Returns true if a DLSS Super Resolution feature has been created
// and we have valid render resolution data.
bool HasData();

// Render resolution reported by DLSS (the input to the upscaler).
// Returns 0x0 if no DLSS feature has been created.
uint32_t GetRenderWidth();
uint32_t GetRenderHeight();

// Output resolution reported by DLSS.
uint32_t GetOutWidth();
uint32_t GetOutHeight();

// DLSS quality mode. Returns -1 if unknown.
int GetQualityMode();

// True if the quality mode is DLAA (render == output).
bool IsDLAA();

// True if DLSS Ray Reconstruction feature has been created.
bool IsRayReconstruction();

// Reset stored state (e.g. on swapchain recreation).
void Reset();

}  // namespace ul_ngx_res

#endif // _WIN64
