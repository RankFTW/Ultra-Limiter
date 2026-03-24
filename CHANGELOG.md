# Changelog

## v2.0.4

- Added Vulkan Reflex backend (VK_NV_low_latency2)
- Hooks vkCreateDevice to inject VK_NV_low_latency2 extension automatically
- Hybrid pacing for Vulkan games without native Reflex markers: driver low-latency hints + QPC timing fallback
- Full Reflex sleep pacing for Vulkan games with native markers (vkLatencySleepNV + timeline semaphore)
- Fixed Vulkan viewport Y-flip (negative height) breaking render resolution detection
- OSD now shows output resolution when no upscaling is detected
- OSD FG detection works on Vulkan via DLL-based identification
- No changes to DX pacing paths

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
