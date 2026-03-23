# Ultra Limiter — Code Independence Analysis

Comparison of Ultra Limiter v1.0 against Display Commander (MapleHinata/display-commander)
and Special K (SpecialKO/SpecialK). This document examines architecture, NVAPI usage,
hooking approach, frame limiter algorithms, and struct definitions to verify that
Ultra Limiter shares no code with either project.

---

## 1. Project Architecture

### Ultra Limiter
- Single-purpose ReShade addon (~2,500 lines across 10 source files)
- Flat file structure: `main.cpp`, `ul_limiter.cpp/hpp`, `ul_reflex.cpp/hpp`,
  `ul_config.cpp/hpp`, `ul_timing.cpp/hpp`, `ul_log.cpp/hpp`
- One class (`UlLimiter`) owns all limiter state
- No external NVAPI headers — defines minimal structs inline in `ul_reflex.hpp`
- Uses `nvapi_interface.h` (MIT-licensed function ID table) for QueryInterface lookups

### Display Commander
- Large multi-feature ReShade addon (~50+ source files, 20+ subdirectories)
- Features: HDR, DLSS, audio, input remapping, latent sync, PresentMon, autoclick, etc.
- Class hierarchy: `ReflexProvider` → `ReflexManager` → NVAPI direct calls
- Uses official NVAPI SDK headers (`nvapi.h`, `NV_LATENCY_MARKER_PARAMS`, etc.)
- Includes Streamline PCLStats integration (`pclstats.h`)
- Has a `HookSuppressionManager` system for selective hook installation

### Special K
- Massive Swiss Army Knife tool (~100+ source files, 150K+ lines)
- `nvapi.cpp` (157KB) handles HDR color control, display settings, driver profiles,
  SLI, Ansel, VRR, Vulkan bridge — not Reflex hooks or frame limiting
- Reflex/frame limiter code lives in separate files (`framerate.cpp`, render subsystem)
- Uses official NVAPI headers with extensive NvDRS (driver settings) manipulation
- GPL v3 licensed

---

## 2. NVAPI Struct Definitions

### Ultra Limiter (`ul_reflex.hpp`)
Defines minimal custom structs from scratch:
```cpp
#pragma pack(push, 8)
struct NvSleepParams {
    NvU32 version;
    NvU32 bLowLatencyMode : 1;
    NvU32 bLowLatencyBoost : 1;
    NvU32 bUseMarkersToOptimize : 1;
    NvU32 bReservedBits : 29;
    NvU32 minimumIntervalUs;
    NvU32 reserved[4];
};
```
- Custom naming: `NvSleepParams`, `NvMarkerParams`, `NvLatencyResult`, `NvLatencyFrameReport`
- Custom type aliases: `NvStatus`, `NvU32`
- Custom enum: `LatencyMarker` with values `SIM_START=0` through `OOB_PRESENT_END=12`
- Version macros: `NV_SLEEP_PARAMS_VER`, `NV_MARKER_PARAMS_VER`, `NV_LATENCY_RESULT_VER`

### Display Commander
Uses official NVAPI SDK headers directly:
```cpp
NV_SET_SLEEP_MODE_PARAMS params = {};
params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
params.bLowLatencyMode = NV_TRUE;
```
- Official types: `NV_SET_SLEEP_MODE_PARAMS`, `NV_LATENCY_MARKER_PARAMS`, `NV_LATENCY_RESULT_PARAMS`
- Official enums: `NV_LATENCY_MARKER_TYPE`, `SIMULATION_START`, `PRESENT_END`
- Official constants: `NVAPI_OK`, `NVAPI_NO_IMPLEMENTATION`, `NV_TRUE`, `NV_FALSE`

### Special K
Uses official NVAPI SDK headers:
- References `NV_HDR_CAPABILITIES`, `NV_HDR_COLOR_DATA`, `NV_COLOR_DATA`
- Uses `NvAPI_Status`, `NVAPI_OK`, `NVAPI_NO_IMPLEMENTATION`
- Hooks via `SK_CreateFuncHook` (custom wrapper around MinHook)

**Verdict**: Ultra Limiter defines its own minimal struct layouts from the public NVAPI SDK
documentation. The naming, field layout, and version macros are all custom. Neither DC's
official-header approach nor SK's official-header approach is used.

---

## 3. Hook Installation

### Ultra Limiter (`ul_reflex.cpp`)
```cpp
bool SetupReflexHooks() {
    if (!LoadNvapi()) return false;
    // Resolve via QueryInterface using nvapi_interface.h IDs
    s_tgt_sleep_mode = resolve("NvAPI_D3D_SetSleepMode");
    s_tgt_sleep = resolve("NvAPI_D3D_Sleep");
    s_tgt_marker = resolve("NvAPI_D3D_SetLatencyMarker");
    // GetLatency resolved but NOT hooked
    // MinHook: MH_CreateHook + MH_EnableHook in a loop
}
```
- Loads `nvapi64.dll` via `LoadLibraryA`, gets `nvapi_QueryInterface` via `GetProcAddress`
- Resolves function IDs from `nvapi_interface.h` string table
- Hooks 3 functions: SetSleepMode, Sleep, SetLatencyMarker
- GetLatency is resolved but called directly (not hooked)
- Rollback on failure: removes all hooks if any single hook fails

### Display Commander (`nvapi_hooks.cpp`)
```cpp
bool InstallNVAPIHooks(HMODULE nvapi_dll) {
    NvAPI_QueryInterface_pfn queryInterface =
        reinterpret_cast<NvAPI_QueryInterface_pfn>(GetProcAddress(nvapi_dll, "nvapi_QueryInterface"));
    // Hooks 5 functions + HDR capabilities
    const char* reflex_functions[] = {
        "NvAPI_D3D_SetLatencyMarker", "NvAPI_D3D_SetSleepMode",
        "NvAPI_D3D_Sleep", "NvAPI_D3D_GetLatency", "NvAPI_D3D_GetSleepStatus"
    };
}
```
- Receives `nvapi_dll` handle from external initialization
- Hooks 5 Reflex functions + `NvAPI_Disp_GetHdrCapabilities` (6 total)
- Also hooks GetLatency and GetSleepStatus (UL does not hook these)
- Uses `CreateAndEnableHook` helper (wraps MinHook)
- Has `HookSuppressionManager` to conditionally skip hooks

### Special K (`nvapi.cpp`)
```cpp
SK_CreateFuncHook(L"NvAPI_Disp_HdrColorControl",
    NvAPI_QueryInterface(891134500),
    NvAPI_Disp_HdrColorControl_Override,
    static_cast_p2p<void>(&NvAPI_Disp_HdrColorControl_Original));
```
- Uses numeric ordinals (not string names) for QueryInterface
- Custom `SK_CreateFuncHook` / `SK_CreateDLLHook2` wrappers
- Hooks `nvapi_QueryInterface` itself to intercept ordinal lookups
- `nvapi.cpp` hooks HDR/display functions, not Reflex sleep/marker functions
- Reflex hooks are in separate render subsystem files

**Verdict**: All three use MinHook + QueryInterface (the only practical way to hook NVAPI),
but the implementation patterns are completely different. UL hooks 3 functions with string
lookup and manual rollback. DC hooks 6 functions with a loop and suppression manager.
SK hooks via numeric ordinals and intercepts QueryInterface itself.

---

## 4. SetSleepMode Hook Behavior

### Ultra Limiter
```cpp
static NvStatus __cdecl Hook_SetSleepMode(IUnknown* dev, NvSleepParams* p) {
    // Capture game's params into GameReflexState atomics
    s_game_state.low_latency.store(p->bLowLatencyMode != 0);
    s_game_state.boost.store(p->bLowLatencyBoost != 0);
    s_game_state.use_markers.store(p->bUseMarkersToOptimize != 0);
    s_game_state.captured.store(true);
    return NV_OK;  // SWALLOW — UL controls sleep mode on its own schedule
}
```
- Always swallows the call (returns OK without forwarding)
- Captures game state into atomic booleans
- UL calls `InvokeSetSleepMode` on its own schedule via `MaybeUpdateSleepMode`

### Display Commander
```cpp
NvAPI_Status __cdecl NvAPI_D3D_SetSleepMode_Detour(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* p) {
    // Store params for UI display
    auto params = std::make_shared<NV_SET_SLEEP_MODE_PARAMS>(*p);
    g_last_nvapi_sleep_mode_params.store(params);
    SetGameReflexSleepModeParams(p->bLowLatencyMode != 0, ...);
    // Suppress if _Direct was called within last 5 frames
    if (frames_since_direct <= 5) return NVAPI_OK;
    if (!IsNativeReflexActive()) return NVAPI_OK;
    // Forward to original
    return NvAPI_D3D_SetSleepMode_Original(pDev, p);
}
```
- Conditionally forwards to driver (not always swallowed)
- Uses `shared_ptr` for parameter storage
- Has frame-based suppression logic (5-frame window after Direct calls)
- Checks `IsNativeReflexActive()` before forwarding

**Verdict**: Completely different strategies. UL always swallows and drives sleep mode itself.
DC conditionally forwards with a 5-frame suppression window.

---

## 5. Sleep Hook Behavior

### Ultra Limiter
```cpp
static NvStatus __cdecl Hook_Sleep(IUnknown* dev) {
    uint64_t cur = g_present_count.load();
    s_last_sleep_frame.store(cur);
    return NV_OK;  // Swallow — UL calls InvokeSleep on its own schedule
}
```
- Always swallows. Tracks frame count for diagnostics only.

### Display Commander
```cpp
NvAPI_Status __cdecl NvAPI_D3D_Sleep_Detour(IUnknown* pDev) {
    // Record timing for rolling average
    g_sleep_reflex_native_ns.store(delta);
    // Check suppression setting
    if (settings::suppress_reflex_sleep.GetValue() && fps_limiter_mode == kReflex)
        return NVAPI_OK;
    if (!IsNativeReflexActive()) return NVAPI_OK;
    return NvAPI_D3D_Sleep_Original(pDev);
}
```
- Conditionally forwards based on settings and native Reflex state
- Records timing statistics with rolling averages

**Verdict**: UL always swallows Sleep. DC conditionally forwards it. Different logic entirely.

---

## 6. SetLatencyMarker Hook

### Ultra Limiter
- Filters RTSS via `_ReturnAddress()` + `GetModuleHandleExW`
- Implements INPUT_SAMPLE reordering (queues out-of-band input markers)
- Records timestamps in a 64-slot ring buffer (`MarkerSlot`)
- Deduplicates markers per frame via `seen_frame` tracking
- Notifies pacing callback (`s_marker_cb`) for frame pacing
- Different forwarding order for PRESENT_FINISH vs other markers

### Display Commander
- Filters RTSS via `GetCallingDLL()` helper (similar to SK's `SK_GetCallingDLL`)
- Records timestamps in a cyclic buffer (`g_latency_marker_buffer`)
- Deduplicates via `frame_id_by_marker_type` tracking
- Calls `ProcessReflexMarkerFpsLimiter` for FPS limiting logic
- Handles delay-present-start and queued frame waiting inline
- Thread tracking for first 6 marker types

**Verdict**: Both filter RTSS and use ring buffers, but the implementations are structurally
different. UL has INPUT_SAMPLE reordering (DC does not). DC has thread tracking and
`ProcessReflexMarkerFpsLimiter` dispatch (UL does not). Ring buffer layouts differ.

---

## 7. Frame Limiter Algorithm

### Ultra Limiter
- Phase-locked timing grid: `target = epoch + k * interval`
- Hybrid sleep: waitable timer for bulk, busy-wait for final stretch
- Kernel timer resolution via `ZwSetTimerResolution`
- Overflow-safe QPC-to-nanoseconds: `sec * 1e9 + rem * 1e9 / freq`
- Predictive sleep: GPU-aware wake scheduling with trend detection
- Present-to-present feedback loop (closed-loop correction)
- Adaptive interval adjustment based on GPU load ratio
- VRR ceiling computation: `ceil = 3600 * hz / (hz + 3600)`

### Display Commander
- Uses Reflex Sleep as the primary FPS limiter (`ShouldUseReflexAsFpsLimiter`)
- `ProcessReflexMarkerFpsLimiter` dispatches based on marker type
- Queued frame waiting with `YieldProcessor()` spin-loop (no hybrid sleep)
- Delay-present-start: waits until `sim_start + delay_frames * frame_time`
- `wait_until_ns` helper for timing (separate from UL's `SleepUntilNs`)
- No predictive sleep, no P2P feedback loop, no adaptive interval

### Special K
- `framerate.cpp` handles frame limiting (separate from `nvapi.cpp`)
- Uses scanline-based timing for VRR displays (Latent Sync)
- Has its own `SK_Framerate_WaitForVBlank` and related infrastructure
- Driver profile manipulation for frame rate limits via NvDRS

**Verdict**: Completely different algorithms. UL uses a phase-locked grid with predictive
sleep and P2P feedback. DC uses Reflex Sleep as the limiter with spin-loop waiting.
SK uses scanline-based timing. No algorithmic overlap.

---

## 8. Unique Features (Not Found in Either DC or SK)

Ultra Limiter has several features that exist in neither Display Commander nor Special K:

1. **GpuPredictor** — Trend-aware GPU time prediction with adaptive safety margins
2. **Present-to-present feedback loop** — Closed-loop correction from measured present cadence
3. **Phase-locked timing grid** — Grid-based frame pacing instead of relative-target advancement
4. **Hybrid queue wait** — Waitable timer + busy-wait for HandleQueuedFrames (DC/SK spin-loop)
5. **Adaptive FG offset** — Replaces static +24µs with measured FG overhead from GetLatency
6. **Auto enforcement site** — GPU load ratio drives SIM_START vs PRESENT_FINISH selection
7. **VRR ceiling formula** — `3600 * hz / (hz + 3600)` with 0.5% safety margin

---

## 9. Shared Dependencies (Not Shared Code)

All three projects necessarily use the same external components:
- **MinHook** — The standard x86/x64 hooking library (BSD-2 license)
- **nvapi_interface.h** — MIT-licensed function ID table from NVAPI SDK
- **NVAPI QueryInterface pattern** — The only way to hook NVAPI functions
- **ReShade addon API** — UL and DC are both ReShade addons
- **Dear ImGui** — Used for overlay UI in all three

These are shared dependencies, not shared code. Using the same libraries is expected
and does not indicate code copying.

---

## 10. Conclusion

Ultra Limiter shares zero code with Display Commander or Special K. The evidence:

- **Struct definitions**: UL defines custom minimal structs; DC and SK use official NVAPI headers
- **Hook strategy**: UL swallows SetSleepMode/Sleep entirely; DC conditionally forwards them
- **Limiter algorithm**: UL uses a phase-locked grid with predictive sleep; DC uses Reflex Sleep; SK uses scanline timing
- **Unique features**: UL has 7+ features (GpuPredictor, P2P feedback, etc.) not found in either project
- **Code style**: UL uses flat C-style with atomics; DC uses class hierarchies with shared_ptr; SK uses its own framework macros
- **Naming conventions**: All three use completely different naming for equivalent concepts

The only similarities are inherent to the problem domain: all three hook the same NVAPI
functions (because those are the only functions that exist), use MinHook (because it's the
standard hooking library), and use ring buffers for marker timestamps (because that's the
natural data structure for frame-indexed data). These are convergent solutions to the same
technical requirements, not evidence of code sharing.
