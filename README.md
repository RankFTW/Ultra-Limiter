# Ultra Limiter — Comprehensive Feature Guide

Ultra Limiter is a ReShade addon that provides GPU-aware frame rate limiting with deep NVIDIA Reflex integration, frame generation awareness, and adaptive pacing. It loads as a `.addon64` file through ReShade and works with 64-bit D3D11 or D3D12 games on NVIDIA GPUs. Not all games are compatible — titles with aggressive anti-cheat, custom rendering pipelines, or non-standard DXGI usage may not work correctly.

The pacing engine is fully dynamic — it automatically detects frame generation, selects the optimal enforcement site, adjusts queue depth, manages Reflex Boost, and stabilizes output cadence. There are no presets or manual FG multiplier controls. Everything adapts in real time based on pipeline telemetry from `NvAPI_D3D_GetLatency`.

This guide covers every feature in detail, including the adaptive systems that run under the hood.

> **Warning**: ReShade with addon support should not be used in online multiplayer games. Most anti-cheat systems (EAC, BattlEye, Vanguard, etc.) will flag or block DLL injection, and you risk account bans. Ultra Limiter is intended for single-player and offline use only.

---

## Table of Contents

1. [Installation](#installation)
2. [FPS Limit](#fps-limit)
3. [Background FPS Limit](#background-fps-limit)
4. [VSync Override](#vsync-override)
5. [5XXX Exclusive Pacing Optimization](#5xxx-exclusive-pacing-optimization)
6. [FG Detection](#fg-detection)
7. [Display & OSD](#display--osd)
8. [Monitor Switching](#monitor-switching)
9. [Window Mode Override](#window-mode-override)
10. [Keybinds](#keybinds)
11. [Adaptive Features](#adaptive-features)
    - [Predictive Sleep](#1-predictive-sleep)
    - [Phase-Locked Timing Grid](#2-phase-locked-timing-grid)
    - [Present-to-Present Feedback Loop](#3-present-to-present-feedback-loop)
    - [Automatic Enforcement Site](#4-automatic-enforcement-site)
    - [Queue Pressure Detection](#5-queue-pressure-detection)
    - [Adaptive FG Pacing](#6-adaptive-fg-pacing)
    - [Cadence Tracking & FG Stabilization](#7-cadence-tracking--fg-stabilization)
    - [Unified FG Adjustment](#8-unified-fg-adjustment)
    - [Dynamic Queue Depth](#9-dynamic-queue-depth)
    - [Dynamic Boost Controller](#10-dynamic-boost-controller)
    - [Bottleneck Detection](#11-bottleneck-detection)
    - [Hybrid Queue Wait](#12-hybrid-queue-wait)
    - [Time-Based Warmup](#13-time-based-warmup)
    - [Proper Reflex Restore](#14-proper-reflex-restore)
    - [VRR / GSync Awareness](#15-vrr--gsync-awareness)
    - [Adaptive Interval Adjustment](#16-adaptive-interval-adjustment)
    - [Flip Metering Block](#17-flip-metering-block)
    - [Settings Change Reset](#18-settings-change-reset)
12. [Reflex Hook Architecture](#reflex-hook-architecture)
13. [Smooth Motion Handling](#smooth-motion-handling)
14. [Status Display](#status-display)
15. [INI Reference](#ini-reference)
16. [Logging](#logging)

---

## Installation

1. Place `ultra_limiter.addon64` in your ReShade addon directory (typically next to the game executable alongside ReShade).
2. Launch the game. Ultra Limiter registers itself automatically.
3. Open the ReShade overlay (default: Home key) and find the "Ultra Limiter" tab.
4. A configuration file (`ultra_limiter.ini`) is created next to the game executable on first launch (falls back to the addon directory if the game directory is not writable).
5. A log file (`ultra_limiter.log`) is written alongside the INI for diagnostics.

---

## FPS Limit

The primary control. Set your target output frame rate with the slider (0–500). Setting 0 means unlimited.

Quick-select buttons are provided for common targets:
- **30, 60, 120, 240** — fixed presets.
- **Reflex (N)** — calculated from your monitor's refresh rate using the Reflex cap formula:

```
FPS Cap = Refresh Rate - (Refresh Rate² / 3600)
```

For a 165 Hz monitor: 165² = 27,225 → 27,225 / 3600 ≈ 7.56 → 165 - 7.56 = 157.44 → floored to **157 FPS**.

This is the sweet spot for Reflex-based pacing — just below the display's native rate so the driver always has a frame ready without exceeding the VSync window.

When frame generation is active, the limiter automatically divides the target by the detected FG multiplier to compute the real render rate. For example, at 120 FPS with DLSS FG 2x, the GPU renders at 60 FPS and the driver interpolates the rest. The FG multiplier is always auto-detected — there is no manual override.

---

## Background FPS Limit

Caps the frame rate when the game window is not focused (alt-tabbed). Set with a slider (0–120). Setting 0 disables the dedicated background cap and falls back to `fps_limit / 3` (minimum 10 FPS).

When active, the background cap bypasses the normal Reflex-based pacing and uses a simple sleep-based limiter. All adaptive evaluation is frozen while backgrounded. This keeps GPU and CPU usage low while the game is in the background.

The cap engages automatically when `GetForegroundWindow()` doesn't match the game's HWND. When you click back into the game, a full adaptive state reset is triggered so the pacing engine re-converges from clean state.

---

## VSync Override

Override the game's VSync setting at the DXGI Present call level:

- **Game** — no change, passthrough.
- **On** — force VSync on (`SyncInterval = 1`, `ALLOW_TEARING` flag stripped).
- **Off** — force VSync off (`SyncInterval = 0`, `ALLOW_TEARING` flag added if supported).

Implemented by hooking `IDXGISwapChain::Present` via MinHook. The hook intercepts the game's Present call and overrides the sync interval and flags before forwarding to the driver. If a Streamline proxy is also hooked, the VSync override is applied there too.

Tearing support is auto-detected at hook time via `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`. If the driver/display doesn't support tearing, the "Off" mode still sets `SyncInterval = 0` but omits the tearing flag.

---

## 5XXX Exclusive Pacing Optimization

Forces single-frame queue depth by hooking `IDXGISwapChain2::SetMaximumFrameLatency` and overriding it to 1. This prevents the GPU from queuing extra frames, reducing input latency.

When the game calls `SetMaximumFrameLatency`, the hook captures the game's requested value (so it can be restored later) and substitutes 1. When the game calls `GetMaximumFrameLatency`, the hook returns the game's original value so the game doesn't get confused by the override.

Only hooks swapchains created with the `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` flag — calling `SetMaximumFrameLatency` on non-waitable swapchains returns `DXGI_ERROR_INVALID_CALL` and can confuse some drivers.

Designed for NVIDIA 50-series (Blackwell) GPUs with flip metering support, where the hardware can enforce single-frame pacing without the throughput penalty seen on older architectures.

Toggling this at runtime immediately applies or restores the frame latency value via the trampoline (bypassing the hook to avoid corrupting the stored game value).

---

## FG Detection

Frame generation is always auto-detected from loaded DLLs. There is no manual multiplier override — the engine adapts its behavior entirely based on what it detects at runtime.

Detected technologies:
- **DLSS FG**: `nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll`
- **FSR FG**: `amd_fidelityfx_framegeneration.dll`, `ffx_framegeneration.dll`
- **Smooth Motion**: `NvPresent64.dll`

The actual FG multiplier (2x, 3x, 4x) is inferred from the ratio of output FPS to native FPS (from SIM_START markers) and displayed on the OSD alongside the technology name.

When FG is active, the pacing engine switches to cadence-based stabilization instead of latency-first pacing. Queue depth, enforcement site, interval adjustment, and boost decisions all adapt to the FG state automatically.

---

## Display & OSD

The OSD draws directly to the game's foreground via ImGui. Toggle with a hotkey (default: END). Each element can be individually enabled/disabled. Position is adjustable via X/Y sliders. An optional background opacity setting (0–100%) adds a dark backdrop for readability.

### OSD Elements

| Element | Color | Description |
|---------|-------|-------------|
| FPS | White | Total output frame rate (including generated frames) |
| Native FPS | White | Real render rate from SIM_START markers (excludes generated frames) |
| Frame | White | Rolling average frame time in ms (64-frame window) |
| GPU | Cyan | GPU active render time from NVAPI GetLatency |
| Render Lat | Cyan | Sim-start to GPU-render-end latency |
| Present Lat | Cyan | Present start-to-end latency |
| FG | Gold | Detected FG technology and multiplier (e.g., "DLSS FG 2x") |
| Res | Gold | Render → output resolution with scale % (e.g., "1440x810 → 2560x1440 (56%)") |
| Graph | Green | Rolling frametime history (128 samples, auto-scaled) |

When Smooth Motion is active, the FPS and Native FPS values are swapped for display so that FPS shows the higher output rate and Native shows the real render rate.

Resolution detection works by tracking viewport dimensions bound during rendering. The most frequently bound sub-native viewport per frame is identified as the DLSS/FSR render resolution.

---

## Monitor Switching

Move the game window to a different display without alt-tabbing:
- **Refresh Monitors** — re-enumerates connected displays.
- **Monitor dropdown** — select a target or "No Override."
- The selected monitor is saved and automatically applied on swapchain init.
- Moves happen on a background worker thread to avoid D3D re-entrancy.

---

## Window Mode Override

Force the game's window mode:
- **Default** — no change.
- **Fullscreen** — forces `WS_POPUP`, removes `WS_OVERLAPPEDWINDOW`, fills monitor.
- **Borderless Fullscreen** — removes caption/frame/sysmenu, adds `WS_POPUP`, fills monitor.

---

## Keybinds

- **Toggle OSD** — rebindable hotkey (default: END). Click the button in the overlay, then press any key to rebind. Supports F1–F12, navigation keys, numpad, letters, and digits.

---

## Adaptive Features

These features run automatically under the hood. They are driven by data from `NvAPI_D3D_GetLatency` and require an NVIDIA GPU with Reflex hooks active. No user configuration is needed — they activate when conditions are met.

### 1. Predictive Sleep

**What it does**: Instead of using a static sleep interval derived from the FPS limit, predictive sleep computes a per-frame interval based on how long the GPU is expected to take. The behavior is mode-dependent — it adapts differently for native rendering vs. frame generation.

**How it works**:
- Tracks recent GPU, simulation, and FG overhead times via per-stage `StagePredictor` instances, each with an 8-sample circular buffer.
- Uses weighted averaging: most recent frame weighted 4x, second 3x, third 2x, rest 1x.
- Detects upward trends (3+ consecutive frames rising) and adds the per-frame increase to the prediction. This anticipates continued rise from camera pans, entering heavier geometry, particle effects ramping up.
- Does NOT bias downward for falling trends — being conservative on the way down is free (just slightly more GPU idle time), while being wrong on the way up causes a missed frame.
- An adaptive safety margin starts at 500 µs, widens by 50 µs per prediction miss, tightens by 10 µs every 30 stable frames. Clamped to 200–2000 µs.
- The `PipelinePredictor` combines GPU + sim + FG stage predictions into a total pipeline prediction.

**Mode-dependent behavior**:
- **1:1 (no FG)**: Tightens the interval based on predicted pipeline time + safety margin. Only active when GPU-bound (enforcement site = SIM_START). The predictive interval can only reduce the base interval.
- **FG (2x+)**: Does NOT tighten the interval. Instead, applies cadence-based stabilization — adjusts the interval to minimize measured output variance (see Cadence Tracking below).

### 2. Phase-Locked Timing Grid

**What it does**: Provides a drift-free timing backstop that always runs, even when Reflex Sleep is active.

**How it works**:
- Frames are aligned to a fixed grid: `target[k] = epoch + k * interval`.
- If a frame arrives early (before the grid slot), the limiter sleeps until the slot.
- If a frame arrives late, the grid snaps forward to the next slot ahead of `now` — no accumulated debt or burst catch-up.
- When Reflex Sleep already paced the frame, `now` is near the grid slot and the sleep returns immediately.
- Uses high-resolution waitable timers (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) for the bulk of the sleep, with a busy-wait tail for sub-millisecond precision.

### 3. Present-to-Present Feedback Loop

**What it does**: A closed-loop correction that measures actual present cadence vs. the target and nudges the Reflex sleep interval to compensate for systematic driver over/undershoot.

**How it works**:
- Measures the time between consecutive presents (present-to-present delta).
- Accumulates signed error (actual_us - target_us) over 30-frame windows.
- Every 30 frames, computes average error:
  - If avg error > +1 µs (frames landing late), shaves 1 µs from the interval.
  - If avg error < -1 µs (frames landing early), adds 1 µs.
  - Dead zone of ±1 µs to avoid chasing noise.
- Total correction clamped to ±8 µs.
- Only active without frame generation (FG has non-linear timing where P2P error doesn't map cleanly to an interval correction).
- Resets when the FPS limit changes.

### 4. Automatic Enforcement Site

**What it does**: Dynamically selects where in the frame pipeline to call Reflex Sleep, based on real-time GPU load, bottleneck detection, and whether frame generation is active.

**How it works**:
- Monitors GPU load ratio: `avg_gpu_active_us / target_interval_us`.
- **When FG is active**: Biases toward `PRESENT_FINISH` to keep the interpolation pipeline fed. Only switches to `SIM_START` if GPU load is genuinely low (< 60%).
- **When FG is not active**: Uses bottleneck detection to decide:
  - **GPU bottleneck** → `SIM_START` — lowest latency.
  - **CPU bottleneck (sim or submit)** → `PRESENT_FINISH` — best frame pacing.
  - **Neither dominant** — uses GPU load ratio with hysteresis (>85% → SIM_START, <65% → PRESENT_FINISH).
- **Deferred enforcement under FG**: When FG is active and dynamic queue depth > 1, sleep is deferred from `SIM_START` to `PRESENT_BEGIN` so the queue wait fires first.

### 5. Queue Pressure Detection

**What it does**: Detects when the OS render queue is backing up and proactively throttles to prevent latency spikes.

**How it works**:
- Tracks the OS render queue time from GetLatency reports (`osRenderQueueEnd - osRenderQueueStart`).
- If the average queue time exceeds 1.5x the target interval, flags "queue pressure."
- When flagged, adds 4 µs to the sleep interval and biases dynamic queue depth voting upward.

### 6. Adaptive FG Pacing

**What it does**: When frame generation is active, replaces the static pacing offset with a measured value derived from actual FG overhead.

**How it works**:
- Measures the actual FG overhead: the gap between GPU render end and present end from GetLatency reports.
- Uses an exponential moving average (α = 0.1) to smooth the measurement.
- Clamped to 4–60 µs to reject bad data.
- When enough data is available, the measured offset feeds into the unified FG adjustment system.

### 7. Cadence Tracking & FG Stabilization

**What it does**: Measures the actual output frame cadence (present-to-present timing) and uses it to stabilize pacing under frame generation.

**How it works**:
- Feeds `presentEndTime` values from GetLatency reports into a 32-sample `CadenceTracker`.
- Computes rolling mean and standard deviation of present-to-present deltas.
- Classifies output quality:
  - **Smooth**: stddev < 3% of the target output interval.
  - **Jittery**: stddev > 8% of the target output interval.
- Tracks streak counters for smooth and jittery states.
- Tracks the best-known interval (the one that produced the lowest variance).
- Automatically infers the effective FG multiplier from the ratio of render interval to measured output cadence — handles 2x, 3x, and 4x without configuration.

**FG stabilization response**:
- **2x FG**: Moderate response. Widens the interval when jittery (up to +10 µs). Tightens when confirmed smooth for 15+ consecutive polls (down to -4 µs).
- **3x+ MFG**: Very conservative. Only widens when jittery (up to +12 µs). Never tightens below zero — prevents the limiter from fighting the multi-frame interpolation scheduler.

### 8. Unified FG Adjustment

**What it does**: Combines all FG-related signals into a single interval adjustment value, replacing the separate static offset and cadence correction.

**How it works**:
- Takes the measured FG overhead as a base offset (clamped 4–60 µs).
- Adds the cadence-based correction from the `CadenceTracker`.
- Considers GPU headroom, queue stress, and whether the output is MFG (3x+).
- When jittery and queue-stressed, increases the adjustment more aggressively.
- When smooth with high GPU headroom and no queue stress, cautiously reduces.
- Queue stress prevents negative adjustments (never tighten when the queue is backed up).
- The final `fg_unified_adjust_us` value is applied as a single offset in the interval computation.

### 9. Dynamic Queue Depth

**What it does**: Automatically selects the optimal render queue depth (1–3 frames) based on pipeline conditions, using a voting-window system to prevent oscillation.

**How it works**:
- Each frame, a "suggested" queue depth is computed from:
  - Safety margin and miss rate from the pipeline predictor.
  - Low margin + low miss rate → depth 1 (lowest latency).
  - High margin or high miss rate → depth 2 or 3 (more buffering).
  - FG active → minimum depth 2 (interpolation pipeline needs buffering).
  - Queue pressure → bias +1.
- Suggestions are recorded in a voting window (sized to ~2 seconds of frames).
- Depth only changes when a supermajority of votes agree (60% to go deeper, 80% to go shallower).
- A 4-second hysteresis timer prevents rapid changes.
- The voting window auto-resizes based on the FPS limit.

### 10. Dynamic Boost Controller

**What it does**: Automatically manages NVIDIA Reflex Low Latency Boost based on GPU utilization patterns and thermal behavior, replacing the manual on/off/game toggle.

**How it works**:
- Tracks GPU idle gaps between consecutive render operations from GetLatency.
- Maintains separate EMAs for GPU time after idle gaps vs. steady-state GPU time.
- **Boost On**: When the GPU shows consistent idle gaps (3+ frames) and post-idle GPU time is higher than steady-state (indicating the GPU is ramping up from low clocks), boost is enabled to keep clocks high.
- **Boost Off**: When GPU load exceeds 85% (boost unnecessary — GPU is already fully utilized) or when thermal throttling is suspected.
- **Thermal detection**: If the GPU stage predictor shows a rising trend for 3+ seconds while the sim predictor is flat or falling, thermal throttling is suspected and boost is disabled to reduce heat.
- 6-second hysteresis between changes to prevent oscillation.
- Falls back to the game's original boost setting when the controller defers (insufficient data).

### 11. Bottleneck Detection

**What it does**: Identifies which pipeline stage is the primary bottleneck, informing enforcement site selection and other adaptive decisions.

**How it works**:
- Tracks per-stage EMA times: simulation, render submit, driver, queue, GPU active.
- Computes total pipeline time and each stage's share.
- If any single stage exceeds 40% of the total pipeline time, it's flagged as the bottleneck.
- Bottleneck categories: GPU, CPU (sim), CPU (submit), Driver, Queue, or None.
- The enforcement site selector uses this to choose between SIM_START (GPU-bound) and PRESENT_FINISH (CPU-bound).

### 12. Hybrid Queue Wait

**What it does**: When waiting for N-back frames to finish rendering (queue depth enforcement), uses a two-phase wait that saves CPU power.

**How it works**:
- **Phase 1**: Yields via short waitable timer sleeps (~0.5 ms each) to avoid burning a full CPU core.
- **Phase 2**: When less than 1 ms remains, switches to busy-wait (`YieldProcessor()`) for sub-millisecond precision.
- Timeout of 50 ms prevents infinite hangs.

### 13. Time-Based Warmup

**What it does**: Delays the start of frame limiting for 2 seconds after the first present, regardless of frame rate.

**How it works**:
- Records the QPC timestamp of the first present call.
- Skips all limiting until 2 seconds have elapsed.
- Uses wall-clock time, not frame count — consistent regardless of the game's initial frame rate.

### 14. Proper Reflex Restore

**What it does**: On shutdown, restores the game's original Reflex sleep mode parameters.

**How it works**:
- When the game calls `SetSleepMode`, the hook captures the game's parameters (low latency mode, boost, marker optimization) into atomic state.
- On addon unload, if the game had set Reflex params, those are restored via `InvokeSetSleepMode`.
- If the game never called `SetSleepMode`, Reflex is disabled cleanly.

### 15. VRR / GSync Awareness

**What it does**: On VRR displays, clamps the sleep interval so the limiter never requests a rate above the VRR ceiling.

**How it works**:
- Computes the VRR ceiling: `ceiling_fps = 3600 * hz / (hz + 3600)`.
- Applies a 0.5% safety margin below the ceiling.
- Converts to a minimum interval in microseconds.
- If the requested interval would exceed the ceiling, clamps it.
- Cached for 2 seconds to avoid hitting the display driver every frame.
- Disabled when VSync is forced off (no VRR concern).

### 16. Adaptive Interval Adjustment

**What it does**: Fine-tunes the sleep interval based on GPU headroom.

**How it works**:
- Monitors GPU load ratio from GetLatency.
- **Load < 70%** — GPU has headroom. Shaves 3 µs for tighter pacing.
- **Load > 90%** — GPU is near budget. Adds up to 4 µs buffer (scaled linearly).
- **70–90%** — no adjustment.
- A separate -2 µs "driver shave" is always applied to compensate for the driver's tendency to overshoot by a couple microseconds.

### 17. Flip Metering Block

**What it does**: Blocks NVIDIA's flip metering pacer (`NvAPI_D3D12_SetFlipConfig`) to give UL full control over FG frame pacing.

**How it works**:
- Hooks `nvapi_QueryInterface` at load time.
- When ordinal `0xF3148C42` (SetFlipConfig) is requested, returns `nullptr` instead of the real function pointer.
- Exception: if Smooth Motion is active (`NvPresent64.dll` loaded), the call passes through because Smooth Motion requires flip metering.
- Exception: if REFramework is detected alongside Streamline, the QueryInterface hook is skipped entirely to prevent crashes in `sl.dlss_g.dll` during swapchain recreation.
- UL's own internal NVAPI lookups use the original (unhooked) QueryInterface pointer, so they're unaffected.

### 18. Settings Change Reset

**What it does**: When the FPS limit, VSync override, or exclusive pacing setting changes, triggers a full reset of all adaptive state so no stale data influences the new configuration.

**How it works**:
- Detects changes to `fps_limit`, `vsync_override`, or `exclusive_pacing` every frame.
- On change, resets: pipeline predictor (all stage predictors, cadence tracker, safety margin), pipeline stats EMAs, boost controller, present-to-present feedback loop, timing grid epoch, dynamic queue depth voting window, and sleep mode cache.
- Re-enters the 2-second warmup period so the adaptive systems can re-converge from clean state.

---

## Reflex Hook Architecture

Ultra Limiter hooks three NVAPI functions via MinHook:

1. **NvAPI_D3D_SetSleepMode** — swallowed. The game's calls are captured (low latency mode, boost, marker optimization flags) but not forwarded. UL controls sleep mode on its own schedule via `MaybeUpdateSleepMode`, which only calls the driver when parameters actually change. The dynamic Boost Controller overrides the boost flag when it has a decision; otherwise the game's original value is used.

2. **NvAPI_D3D_Sleep** — swallowed. The game's sleep calls are suppressed entirely. UL calls Sleep on its own schedule to prevent double-pacing. Games like Monster Hunter Wilds call Sleep multiple times per frame — all are suppressed.

3. **NvAPI_D3D_SetLatencyMarker** — intercepted and forwarded. Key behaviors:
   - Filters RTSS (RivaTuner Statistics Server) markers via `_ReturnAddress()` — RTSS fires latency markers but is not native Reflex.
   - Records timestamps in a 64-slot ring buffer for pacing logic.
   - Deduplicates markers per frame via `seen_frame` tracking.
   - Reorders out-of-band `INPUT_SAMPLE` markers: if the game fires INPUT_SAMPLE outside the SIM_START–SIM_END window, it's queued and re-injected at the next valid boundary.
   - Notifies the pacing callback for frame pacing decisions.

4. **NvAPI_D3D_GetLatency** — resolved but NOT hooked. Called directly to read the driver's frame report ring buffer for all adaptive features. Disabled when REFramework + Streamline is detected (crash prevention).

5. **nvapi_QueryInterface** — hooked to block flip metering (see above). Skipped when REFramework is detected.

---

## Smooth Motion Handling

NVIDIA Smooth Motion is driver-level frame interpolation via `NvPresent64.dll`. It is NOT the same as DLSS Frame Generation — it operates at the driver level, not the application level.

When Smooth Motion is detected:
- Reflex Sleep is bypassed entirely (it conflicts with the driver's presentation timing).
- All pacing falls back to the timing-only path (phase-locked grid).
- The flip metering block is disabled (Smooth Motion requires flip metering).
- OSD FPS/Native FPS values are swapped so FPS shows the higher output rate.

Detection is via `GetModuleHandleW(L"NvPresent64.dll")`, checked every 2 seconds.

---

## Status Display

The bottom of the overlay shows:
- **Reflex** — "Hooked" or "Not hooked."
- **Native Reflex** — "Detected" or "No" (whether the game calls SetLatencyMarker).
- **Pacing** — current mode, dynamically resolved:
  - "Timing (no Reflex)" — no Reflex hooks, timing-only fallback.
  - "Marker (Sim Start)" — game uses Reflex markers, enforcement at SIM_START (GPU-bound).
  - "Marker (Present)" — game uses Reflex markers, enforcement at PRESENT_FINISH (CPU-bound or FG).
  - "Present-based (Reflex Sleep)" — game doesn't use markers, UL drives Sleep directly.
- **Target** — current FPS target.

---

## INI Reference

All settings are stored in `ultra_limiter.ini` under the `[UltraLimiter]` section:

```ini
[UltraLimiter]
fps_limit=0               # 0 = unlimited, 1-500
bg_fps_limit=0            # 0 = auto (fps_limit/3, min 10), 1-120
osd_on=true
osd_x=10.0
osd_y=10.0
show_fps=true
show_frametime=true
show_native_fps=true
show_graph=true
show_gpu_time=true
show_render_lat=true
show_present_lat=true
show_fg_mode=true
show_resolution=true
target_monitor=           # device name like \\.\DISPLAY1
window_mode=none          # none / fullscreen / borderless
vsync_override=0          # 0 = game, 1 = force on, 2 = force off
exclusive_pacing=false    # 5XXX exclusive pacing optimization
osd_toggle_key=35         # virtual key code (35 = VK_END)
osd_bg_opacity=0          # 0-100 (OSD background darkness percentage)
```

---

## Logging

A timestamped log file is written to `ultra_limiter.log` next to the INI file. It records:
- Startup/shutdown events
- NVAPI initialization and hook status
- QueryInterface hook status (flip metering block)
- REFramework detection and safe mode activation
- Reflex connection and device detection
- Enforcement site changes with GPU load percentage
- Smooth Motion detection/loss
- Swapchain init/destroy with API type and resolution
- VSync hook status and tearing support
- Frame latency hook status
- FG detection results
- Background mode enter/exit
- Adaptive state resets
- Errors and exceptions with codes and addresses

Timestamps are seconds since addon load, using QPC for precision.
