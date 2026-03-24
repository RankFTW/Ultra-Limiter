# Changelog

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
- Embedded version metadata in DLL (visible in file properties)
- Removed: pacing presets, manual FG multiplier, manual boost override, and related INI keys
