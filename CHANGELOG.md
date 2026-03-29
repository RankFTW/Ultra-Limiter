# Changelog

## v2.5.3

Black screen fix, OSD retune, and Vulkan FG grid pacing correction.

### Black Screen Fix
- Fixed black screen in DX games like Crimson Desert where generated frames were blocked by the render-rate grid. OnPresent now passes the FG divisor to DoOwnSleep so the grid paces at the output rate instead of the render rate.

### OSD Retune
- Tuned OSD layout spacing and element sizing for improved readability.
- Removed certain OSD elements that were unreliable.

### Vulkan Reflex Sleep Clarification
- Corrected InvokeReflexSleep comment to reflect that the game's vkLatencySleepNV is swallowed by our wrapper, not forwarded to the driver.

## v2.5.2

Vulkan Streamline compatibility, semaphore recovery, FG grid timing, NGX late-hook resolution detection, OSD improvements, and Vulkan polling overhaul.

### Streamline DLSS-G Compatibility (Vulkan)
- Skip VK_NV_low_latency2 injection and MinHook LL2 function patches when Streamline (sl.common.dll) is detected. Streamline manages LL2 through its own interposer layer — our injection conflicted with it and prevented FG from generating frames in games like X4 Foundations.
- UI status correctly shows "Streamline (managed)" for Reflex and "Streamline + Timing" for pacing mode.
- Native FPS and render pacing graph are hidden for Streamline-managed games (no marker data available — no data is better than wrong data).

### Vulkan Semaphore Recovery
- Added `vkGetSemaphoreCounterValue` function pointer to VkReflex (loaded optionally during Init).
- If a timeline semaphore wait times out or fails, the tracked counter is re-synced with the driver's actual value. Handles missed signals from crashes, device transitions, or driver hiccups.

### Vulkan FG Grid Timing Fix
- Two intervals in the Vulkan DoOwnSleep path: SetSleepMode receives the render-rate interval, while the grid receives the output-rate interval (render interval / fg_divisor) to pace present callbacks that fire for every frame including generated ones.
- Without the grid division, generated frames were blocked by the render-rate grid, capping output at the render rate instead of the full FG output rate.

### Vulkan Sleep Passthrough
- Added a `vk_reflex_->Sleep()` passthrough call for non-native Vulkan games after grid sleep. Keeps the driver's semaphore-based frame tracking alive without double-signaling for native Reflex games.

### Vulkan OSD Polling Overhaul
- PollVkGpuLatency now extracts all valid consecutive simStartTime deltas from the 64-frame report buffer per poll (with dedup tracking), instead of just the single most-recent pair.
- Vulkan poll frequency increased from every 30 presents to every 10 presents. Pacing graph and native FPS populate within 1-2 seconds instead of 13+.

### OSD Improvements
- Smoothness score always shows when enabled, even at 0% (previously hidden at 0).
- Increased line gap (4→6) and padding (8→12) to prevent text clipping into graph edges.
- Background box vertical margin matches horizontal padding for consistent spacing.
- Extra gap before render pacing graph to clear text descenders.

### NGX Late-Hook Resolution Detection
- EvaluateFeature hook now adopts the DLSS SR handle when CreateFeature was missed (NGX DLLs loaded after hook installation). Fixes incorrect DLAA display in games like RDR2 where _nvngx.dll loads lazily.
- Handle mismatch detection: when the game recreates the DLSS feature (swapchain recreate, settings change), the stale handle is reset and re-adopted from the next EvaluateFeature call.
- EvaluateFeature path now sets HasData flag so the OSD uses NGX-reported quality mode instead of falling through to the viewport-based heuristic.

## v2.5.0

Vulkan native Reflex overhaul, NGX resolution detection, FG detection rework, and pacing safety improvements.

### Vulkan Native Reflex — Full Marker-Based Pacing
- Hooked all three VK_NV_low_latency2 functions via MinHook (vkSetLatencySleepModeNV, vkLatencySleepNV, vkSetLatencyMarkerNV) in addition to the existing vkGetDeviceProcAddr interception. Catches all calls regardless of whether the game uses global or device-level dispatch.
- Wrapped_vkSetLatencyMarkerNV: intercepts game marker calls, records timestamps in g_ring, performs per-frame dedup, and fires the marker callback — enabling marker-based enforcement pacing on Vulkan identical to the DX path.
- Wrapped_vkLatencySleepNV: forwards game sleep calls to driver to keep the latency measurement pipeline coherent.
- HookLL2Functions: resolves and hooks real LL2 function addresses via MinHook after vkCreateDevice succeeds, using the original (unhooked) vkGetDeviceProcAddr trampoline.
- VkReflex::Init now uses the trampoline to resolve real driver functions instead of the hooked dispatch, preventing self-interception.
- SetVkMarkerCb added — registers the same marker callback for Vulkan that DX uses, called at addon init.
- Hooked_vkCreateDevice refactored with cleaner extension list building.

### Vulkan Pacing Fix
- Fixed FPS cap not working in Vulkan native Reflex games (e.g. RDR2). OnPresent now always calls DoOwnSleep for Vulkan regardless of native/non-native status — grid sleep enforces the FPS cap while SetSleepMode is correctly skipped for native games.
- DoOwnSleep Vulkan path no longer early-returns for native Reflex games. Builds sleep params and runs grid sleep for all Vulkan games; only skips SetSleepMode when the game handles it natively.
- Fixed Vulkan FG games capped at half refresh rate. Grid interval now divided by FG multiplier so generated frames pass through without being blocked by the render-rate grid.

### Vulkan OSD
- Removed render latency and present latency extraction from PollVkGpuLatency — Vulkan timestamps are unreliable when markers are intercepted. Only gpuActiveRenderTimeUs is populated. OSD Render Lat and Present Lat auto-hide when zero.
- Render pacing graph now works on Vulkan — derives native frame cadence from GetLatencyTimings simStartTime deltas instead of relying on marker callbacks (which Streamline games may bypass).
- OSD Smoothness score hidden when 0%.

### NGX Resolution Detection (new module: ul_ngx_res)
- Hooks NVSDK_NGX CreateFeature and EvaluateFeature from _nvngx.dll, nvngx_dlss.dll, _nvngx_dlss.dll, and nvngx_dlssd.dll for D3D12, D3D11, and Vulkan via MinHook.
- Reads DLSS parameters directly from the NVSDK_NGX_Parameter interface — exact render width/height, output width/height, and quality mode (Performance/Balanced/Quality/Ultra Performance/Ultra Quality/DLAA).
- Supports both legacy NGX feature IDs (0 for SR, 4 for RR) and Streamline IDs (11 for SR, 1 for RR, 13 for FG).
- Reads DLSS.Render.Subrect.Dimensions for Streamline games where standard Width/Height report output resolution.
- EvaluateFeature hook provides real-time resolution updates when changing DLSS quality mid-game without feature recreation.
- DLAA detected by quality mode (5) with fallback to render==output comparison.
- DLSS Ray Reconstruction detected via feature ID 1/4/12 (detection code present, OSD display deferred for refinement).
- Viewport-based resolution detection retained as fallback for non-DLSS upscalers (FSR, XeSS).
- Deferred hook installation: retries periodically in OnPresent for lazily-loaded DLSS DLLs (Streamline games).

### FG Detection Rework (from ReLaz2)
- DetectFGDivisor reworked: FPS-based ul_fg_monitor::GetTier() is now ground truth for runtime FG state. DLL presence is fallback only before FPS data is available.
- ul_fg_monitor::HasData() added — returns true once 30+ updates processed.

### Pacing Safety (from ReLaz2)
- Dedicated background timer handle (htimer_bg_) — separate HANDLE for background sleep.
- Cadence tracker reset on FG tier change.
- Predictive sleep gated on marker_pacing and eff_queue <= 1.
- GPU overload detection uses user's original target interval. Overload mode is metrics-only.

### Enforcement Site Stability
- Fixed enforcement site flip-flopping every frame under GPU overload. Enforcement site selection and load gate now use target-based GPU load ratio instead of cadence-recomputed ratio.
- GPU overload log spam eliminated — load gate no longer falsely closes during overload.

### OSD Changes
- Removed small frametime graph and large frametime graph. Only the render pacing graph remains.
- Renamed "Native Cadence" graph to "Render Pacing".
- Viewport-based DLAA detection: threshold broadened to <= 55% with 10-frame hysteresis. Superseded by NGX hook for DLSS games.

### REFramework Detection Fix
- Fixed false positive REFramework detection in games with dinput8.dll + Streamline (e.g. X4 Foundations). Now verifies reframework.dll exports reframework_get_renderer_type before blocking GetLatency. Also checks dinput8.dll path — only triggers for game-directory proxies, not System32.


## v2.2.1

FG tier detection improvements, timing precision, and stability fixes.

- FPS-ratio FG tier monitor: extracted tier computation from OSD into standalone `ul_fg_monitor` module. Tier is computed every frame from `output_fps / native_fps` independent of OSD draw path. Thresholds at > 1.5, > 2.5, > 3.5, > 4.5, > 5.5 for tiers 2-6.
- Tier lock-in hysteresis: first detection is immediate, subsequent tier changes require 30 consecutive frames at the new tier before applying. Prevents oscillation at ratio boundaries (e.g. 3.5 bouncing between tier 3 and 4).
- Tier state resets on background/refocus via `ul_fg_monitor::Reset()`, preventing stale tier from persisting across alt-tab cycles
- Removed `g_fps_fg_tier` global atomic — tier is now accessed via `ul_fg_monitor::GetTier()` from the limiter and `ul_fg_monitor::GetMultiplierString()` from the OSD
- Added `ul_fg_monitor::HasData()` — returns true once 30+ updates processed, used by `DetectFGDivisor` to distinguish "no FG confirmed" from "not enough data yet"
- `DetectFGDivisor` reworked: FPS-based monitor is now ground truth for runtime FG state. DLLs stay loaded even when the game disables FG (e.g. in menus), so DLL presence alone is no longer trusted. When `HasData()` reports tier 0, returns 1 (confirmed no FG). DLL presence is fallback only before FPS data is available.
- FG divisor change detection: `ResetAdaptiveState()` now triggers when FG divisor changes at runtime (e.g. toggling FG in-game), tracked via `last_fg_div_`
- GPU overload state flushed on load gate close (gameplay to menu/loading transition) and on FG tier change — prevents stale overload detection from a previous scene or FG state carrying over
- SEH guard on `DoOwnSleep` Reflex passthrough — wraps `MaybeUpdateSleepMode` + `InvokeSleep` in `__try/__except` to protect against driver crashes during swapchain recreation or alt-tab
- Background early-return in `DoTimingFallback` — prevents the timing fallback path from running while the game is backgrounded
- Timer promotion hooks: `CreateWaitableTimerW` and `CreateWaitableTimerExW` hooked via MinHook to force `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on all waitable timers in the process. Improves timing precision for driver-internal and middleware timers that would otherwise use the default ~15.6ms resolution.
- Thread priority boost to `THREAD_PRIORITY_TIME_CRITICAL` during the busy-wait tail in `SleepUntilNs`, restored to previous priority after. Reduces OS preemption risk during the critical sub-millisecond timing window.
- PLL grid correction from display feedback: reads `presentEndTime` from GetLatency reports, computes phase error between actual present time and nearest grid slot, EMA-smooths (alpha=0.05), and nudges `grid_epoch_ns_` by 10% of the smoothed error. Slowly aligns the timing grid to the display's actual present cadence, correcting for drift that the present-to-present feedback loop alone can't catch.

## v2.2.0

VRR and Frame Generation consistency improvements -- perceptual threshold tuning, GSync detection, frame splitting control, smoothness OSD, diagnostic telemetry, and OSD enhancements.

- Perceptual threshold tuning: ConsistencyBuffer now uses absolute stddev (µs) thresholds instead of CV-based thresholds, derived from the 4ms perceptual floor (Klein et al., 2024). High frame rate sessions no longer penalized with unnecessary buffer when variance is imperceptible. DMFG sessions with high CV but low absolute stddev stay in TIGHTEN instead of being held at max buffer.
- GSync active detection: polls `NvAPI_D3D_IsGSyncActive` every 2 seconds to verify VRR is actually engaged. All VRR-specific behavior (proximity scaling, floor clamping, frame splitting, consistency buffer VRR tuning) gated on the result. Defaults to inactive if NVAPI unavailable. 64-bit only.
- Frame splitting disable: calls `NvAPI_DISP_SetAdaptiveSyncData` to disable driver frame splitting when GSync is active and FG is running. Prevents tear-like artifacts from frames split across VRR refresh cycles. Original state restored on shutdown or GSync deactivation. 64-bit only.
- GetLatency polled every frame instead of every other frame -- cuts detection delay by one frame for pipeline stats and enforcement site changes
- Smoothness OSD: single percentage score computed from cadence CV (`100 - CV * 1000`, clamped). Color-coded green/yellow/red. Gated on 8-sample minimum. Toggled via `show_smoothness` in settings panel and INI (default true).
- OSD scale slider: scales the entire OSD from 100% to 300%. Affects all text, graphs, spacing, and line thickness. Configurable via `osd_scale` INI option (default 100).
- Large frametime graph: new `show_big_graph` toggle adds a 400x120 (scaled) frametime graph with dynamic axis limits, labeled min/mid/max values, background box, and dashed midline. Axis labels auto-size and stay inside the graph area.
- Expanded CSV diagnostics: new `csv_diagnostics` INI option writes 18-column per-frame telemetry to `relimiter_diagnostics.csv`. Columns include smoothness, cadence stats, GSync state, predictor intervals, enforcement site, queue depth, and final interval. Replaces the old `csv_consistency_log` option.
- DMFG QPC brake bypass: QPC variance brake disabled for driver-side DMFG sessions. Driver-injected frames cause inherently high QPC variance from the app's perspective; cadence stddev is the sole stability signal. Standard FG and MFG unaffected.
- Fixed diagnostic CSV not writing in WindowsApps games -- falls back to game exe directory when addon DLL directory is read-only
- Fixed GSyncDetector and FrameSplitController init running before NVAPI was loaded -- moved init into ConnectReflex after SetupReflexHooks succeeds

## v2.1.2

NVIDIA DMFG (Dynamic Multi-Frame Generation) compatibility, FG tier detection improvements, and OSD additions.

- DMFG support (RTX 50 series, driver-side 3x-6x frame generation with no user-space FG DLL):
  - Frame latency override passes through game-requested queue depth instead of forcing 1
  - Flip metering (SetFlipConfig) allowed through for DMFG sessions
  - FG tier derived directly from game's MaxFrameLatency value (e.g. game_lat=6 -> tier 6)
  - All DMFG detection gated behind absence of FG DLLs -- standard DLL-based FG unaffected
  - Added IsDmfgSession() helper for consistent no-DLL + high-latency detection across hooks
- FG tier detection fixes:
- Added GetGameRequestedLatency() export for cross-module DMFG detection
- Fixed OSD resolution showing incorrect 50% upscale when using DLAA -- half-res post-process viewports now detected and suppressed; OSD shows "DLAA" with native resolution instead- correctly detects 2x/3x/4x for DLL-based FG with a FPS cap
  - Fixed cadence-based tier demotion feedback loop -- demotion clamped to DMFG latency hint floor
  - Consistency buffer no longer resets when tier changes between same tuning params (e.g. 4x/5x/6x all use kTier4xPlusMFG)
- Added cadence tier thresholds for 5x (>4.5f) and 6x (>5.5f) multipliers
- Added kTier4xPlusMFG tuning tier for ConsistencyBuffer (30-120 us range, step 8/4, CV thresholds 0.05/0.12)
- Added OSD 5x and 6x display branches in UpdateFGString
- Added 1% low FPS to OSD -- 99th percentile frame time over a 300-frame rolling window, matching FrameView/CapFrameX methodology
- Added GetGameRequestedLatency() export for cross-module DMFG detection

## v2.1.0

Adaptive consistency buffer -- replaces the old cadence response system with a two-mode state machine.

- New ConsistencyBuffer state machine (STABILIZE / TIGHTEN) replaces FGPacingContext, UpdateCadenceResponse, and ComputeFGAdjustment
- Continuous coefficient of variation (CV) signal replaces binary smooth/jittery streak classification
- Three tuning tiers: 1:1 (4-20 us), 2x FG (12-50 us), 3x+ MFG (20-80 us) with tier-appropriate step sizes and thresholds
- QPCCadenceMonitor provides secondary CV signal as early-warning brake -- forces immediate STABILIZE before primary cadence signal confirms instability
- GPU load gate (50% threshold) freezes ConsistencyBuffer tick during menus and loading screens, resumes without reset
- VRR proximity scaling halves TIGHTEN step when near VRR ceiling (>90% proximity)
- Consistency buffer applied on both FG and 1:1 paths (1:1 previously had no cadence-driven buffer)
- FG multiplier detection from cadence ratio -- correctly identifies 2x/3x/4x DLSS FG instead of hardcoding 2x
- Hysteresis on FG tier demotion (30-tick confirmation) prevents false downgrades from transient load spikes
- Diagnostic CSV logging (`csv_consistency_log` INI option) writes per-tick state machine data to `relimiter_consistency.csv`
- New config fields auto-written to INI on first load
- Fixed GPU load gate early return skipping DynamicPacing and BoostController (caused crash in Smooth Motion games)
- Fixed SaveSettings during DLL_PROCESS_ATTACH causing loader lock issues -- deferred to first OnPresent after warmup
- Fixed fake fullscreen hooks installing unconditionally even when disabled, triggering anti-tamper crashes in some games
- Fixed Smooth Motion FPS cap -- real frames now paced at half rate so total output matches the configured limit
- Fixed OSD showing half the actual FPS in Smooth Motion games
- Filtered STATUS_BREAKPOINT from crash log (anti-tamper noise, not a real crash)
- Removed: FGPacingContext, UpdateCadenceResponse, ComputeFGAdjustment, CadenceTracker streak counters and variance ratio constants

## v2.0.10

- Added 32-bit build (relimiter.addon32) for 32-bit DX games -- timing fallback, OSD, VSync, frame latency, fake fullscreen (no Reflex/Vulkan on 32-bit)
- Added DX9 Fake Fullscreen support -- hooks IDirect3DDevice9::Reset to force windowed mode

## v2.0.9
- Added Fake Fullscreen option -- intercepts exclusive fullscreen and converts to borderless window
- Hooks DXGI Factory CreateSwapChain/CreateSwapChainForHwnd to force windowed mode at creation time
- Hooks SetFullscreenState, GetFullscreenState, and ResizeTarget as safety net
- RE Engine games auto-detected and skipped (incompatible with fake fullscreen)
- Skipped in Streamline safe mode
- Added NGX loader DLLs (nvngx.dll, _nvngx.dll) to upscaler detection gate
- Added upscaler detection diagnostic logging
- Fixed OSD resolution line flickering in FG games (60-frame decay instead of instant clear)
- Fixed OSD resolution bouncing between different sub-native viewports (now picks largest instead of most frequent)

## v2.0.8

- Fixed background FPS limiter not sleeping consistently (grid drift from using wall-clock instead of grid-aligned advancement)
- Fixed OSD resolution line showing in non-upscaled games (viewport tracking now gated behind upscaler DLL detection)
- Added DLSS Ray Reconstruction (nvngx_dlssd.dll) to upscaler detection
- Added DLSS FG and Streamline DLSS DLLs to upscaler detection gate (fixes resolution not showing in games like Crimson Desert)

## v2.0.7
- Fixed stale render resolution persisting on OSD in non-upscaled games

## v2.0.6

- Native Reflex detection via vkGetDeviceProcAddr hook (intercepts game's vkSetLatencySleepModeNV calls)
- Deferred SetSleepMode -- no longer called eagerly during swapchain attach, avoids conflicts with native Reflex games
- Native Reflex VK games: ReLimiter is hands-off (no SetSleepMode/Sleep calls), timing fallback as backstop only
- Non-native Reflex VK games: driver low-latency hints + QPC timing fallback (no semaphore wait)
- Added GPU render time, render latency, and present latency OSD metrics for Vulkan games
- Fixed Vulkan OSD render latency showing inflated values due to stale cross-frame timestamps
- Fixed FPS limiter not working in non-Reflex DX and Vulkan games (timing fallback was incorrectly skipped)

## v2.0.4

- Added Vulkan Reflex backend (VK_NV_low_latency2)
- Hooks vkCreateDevice to inject VK_NV_low_latency2 extension automatically
- Hybrid pacing for Vulkan games without native Reflex markers: driver low-latency hints + QPC timing fallback
- Full Reflex sleep pacing for Vulkan games with native markers (vkLatencySleepNV + timeline semaphore)
- Fixed Vulkan viewport Y-flip (negative height) breaking render resolution detection
- OSD FG detection works on Vulkan via DLL-based identification

## v2.0.3

- Added OSD drop shadow option for improved text readability
- Added OSD text brightness slider (0-100%)

## v2.0.2

- Rebranded from Ultra Limiter to ReLimiter
- Renamed addon output to `relimiter.addon64`
- Renamed INI file to `relimiter.ini`
- Renamed log file to `relimiter.log`
- Embedded version metadata in DLL file properties
- Added auto-incrementing version system with CHANGELOG-based release notes

## v2.0.0

Fully dynamic pacing engine rewrite.

- Replaced manual presets, boost override, and FG multiplier with fully automatic adaptive systems
- Pipeline-aware predictive sleep with per-stage StagePredictor instances (GPU, sim, FG)
- Dynamic queue depth (1-3) via voting-window system with supermajority thresholds
- Dynamic Boost controller based on GPU idle gaps, utilization, and thermal detection
- Bottleneck detection across 5 pipeline stages (40% threshold)
- Unified FG adjustment combining measured overhead + cadence + headroom + queue stress
- Cadence tracker with FG/MFG-aware stabilization (32-sample variance measurement)
- Split interval computation: FG path uses unified adjustment, 1:1 path uses independent corrections
- Auto-derive Reflex cap from monitor refresh rate when FPS limit is unlimited and FG is active
- OSD auto-hides FG line when no frame generation detected
- OSD auto-hides resolution line when no upscaling detected
