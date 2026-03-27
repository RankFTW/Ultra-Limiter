# Changelog

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
