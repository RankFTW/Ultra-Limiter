# Changelog

## v2.1.0

Adaptive consistency buffer — replaces the old cadence response system with a two-mode state machine.

- New ConsistencyBuffer state machine (STABILIZE / TIGHTEN) replaces FGPacingContext, UpdateCadenceResponse, and ComputeFGAdjustment
- Continuous coefficient of variation (CV) signal replaces binary smooth/jittery streak classification
- Three tuning tiers: 1:1 (4–20 µs), 2x FG (12–50 µs), 3x+ MFG (20–80 µs) with tier-appropriate step sizes and thresholds
- QPCCadenceMonitor provides secondary CV signal as early-warning brake — forces immediate STABILIZE before primary cadence signal confirms instability
- GPU load gate (50% threshold) freezes ConsistencyBuffer tick during menus and loading screens, resumes without reset
- VRR proximity scaling halves TIGHTEN step when near VRR ceiling (>90% proximity)
- Consistency buffer applied on both FG and 1:1 paths (1:1 previously had no cadence-driven buffer)
- FG multiplier detection from cadence ratio — correctly identifies 2x/3x/4x DLSS FG instead of hardcoding 2x
- Hysteresis on FG tier demotion (30-tick confirmation) prevents false downgrades from transient load spikes
- Diagnostic CSV logging (`csv_consistency_log` INI option) writes per-tick state machine data to `relimiter_consistency.csv`
- New config fields auto-written to INI on first load
- Fixed GPU load gate early return skipping DynamicPacing and BoostController (caused crash in Smooth Motion games)
- Fixed SaveSettings during DLL_PROCESS_ATTACH causing loader lock issues — deferred to first OnPresent after warmup
- Removed: FGPacingContext, UpdateCadenceResponse, ComputeFGAdjustment, CadenceTracker streak counters and variance ratio constants

## v2.0.10

- Added 32-bit build (relimiter.addon32) for 32-bit DX games — timing fallback, OSD, VSync, frame latency, fake fullscreen (no Reflex/Vulkan on 32-bit)
- Added DX9 Fake Fullscreen support — hooks IDirect3DDevice9::Reset to force windowed mode

## v2.0.9
- Added Fake Fullscreen option — intercepts exclusive fullscreen and converts to borderless window
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
- Deferred SetSleepMode — no longer called eagerly during swapchain attach, avoids conflicts with native Reflex games
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
- Added OSD text brightness slider (0–100%)

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
- Dynamic queue depth (1–3) via voting-window system with supermajority thresholds
- Dynamic Boost controller based on GPU idle gaps, utilization, and thermal detection
- Bottleneck detection across 5 pipeline stages (40% threshold)
- Unified FG adjustment combining measured overhead + cadence + headroom + queue stress
- Cadence tracker with FG/MFG-aware stabilization (32-sample variance measurement)
- Split interval computation: FG path uses unified adjustment, 1:1 path uses independent corrections
- Auto-derive Reflex cap from monitor refresh rate when FPS limit is unlimited and FG is active
- OSD auto-hides FG line when no frame generation detected
- OSD auto-hides resolution line when no upscaling detected
