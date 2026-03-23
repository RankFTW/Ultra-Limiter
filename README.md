# Ultra Limiter — Comprehensive Feature Guide

Ultra Limiter is a ReShade addon that provides GPU-aware frame rate limiting with deep NVIDIA Reflex integration, frame generation awareness, and adaptive pacing. It loads as a `.addon64` file through ReShade and works with any D3D11 or D3D12 game on NVIDIA GPUs.

The pacing engine is FG-aware — it adapts its behavior based on whether frame generation is active, using cadence-based stabilization under FG/MFG instead of the latency-first approach used in native rendering. This prevents the limiter from fighting the interpolation pipeline, resulting in smooth output even at 3x and 4x frame generation.

This guide covers every feature in detail, including the adaptive systems that run under the hood.

---

## Table of Contents

1. [Installation](#installation)
2. [FPS Limit](#fps-limit)
3. [Background FPS Limit](#background-fps-limit)
4. [VSync Override](#vsync-override)
5. [5XXX Exclusive Pacing Optimization](#5xxx-exclusive-pacing-optimization)
6. [FG Mode (Frame Generation)](#fg-mode-frame-generation)
7. [Boost Override](#boost-override)
8. [Pacing Presets](#pacing-presets)
9. [Display & OSD](#display--osd)
10. [Monitor Switching](#monitor-switching)
11. [Window Mode Override](#window-mode-override)
12. [Keybinds](#keybinds)
13. [Adaptive Features](#adaptive-features)
    - [Predictive Sleep](#1-predictive-sleep)
    - [Phase-Locked Timing Grid](#2-phase-locked-timing-grid)
    - [Present-to-Present Feedback Loop](#3-present-to-present-feedback-loop)
    - [Automatic Enforcement Site](#4-automatic-enforcement-site)
    - [Queue Pressure Detection](#5-queue-pressure-detection)
    - [Adaptive DLSS FG Pacing](#6-adaptive-dlss-fg-pacing)
    - [Cadence Tracking & FG Stabilization](#7-cadence-tracking--fg-stabilization)
    - [Hybrid Queue Wait](#8-hybrid-queue-wait)
    - [Time-Based Warmup](#9-time-based-warmup)
    - [Proper Reflex Restore](#10-proper-reflex-restore)
    - [VRR / GSync Awareness](#11-vrr--gsync-awareness)
    - [Adaptive Interval Adjustment](#12-adaptive-interval-adjustment)
    - [Flip Metering Block](#13-flip-metering-block)
    - [Settings Change Reset](#14-settings-change-reset)
14. [Reflex Hook Architecture](#reflex-hook-architecture)
15. [Smooth Motion Handling](#smooth-motion-handling)
16. [Status Display](#status-display)
17. [INI Reference](#ini-reference)
18. [Logging](#logging)

---

## Installation

1. Place `ultra_limiter.addon64` in your ReShade addon directory (typically next to the game executable alongside ReShade).
2. Launch the game. Ultra Limiter registers itself automatically.
3. Open the ReShade overlay (default: Home key) and find the "Ultra Limiter" tab.
4. A configuration file (`ultra_limiter.ini`) is created next to the addon on first launch.
5. A log file (`ultra_limiter.log`) is written alongside the addon for diagnostics.

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

When frame generation is active, the limiter automatically divides the target by the FG multiplier to compute the real render rate. For example, at 120 FPS with DLSS FG 2x, the GPU renders at 60 FPS and the driver interpolates the rest.

---

## Background FPS Limit

Caps the frame rate when the game window is not focused (alt-tabbed). Set with a slider (0–120). Setting 0 disables the background cap, so the normal FPS limit applies even when unfocused.

When active, the background cap bypasses the normal Reflex-based pacing and uses a simple sleep-based limiter. This keeps GPU and CPU usage low while the game is in the background — useful for saving power and reducing heat when you're browsing or on Discord.

The cap engages automatically when `GetForegroundWindow()` doesn't match the game's HWND, and disengages instantly when you click back into the game. The OSD continues to update while backgrounded.

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

## FG Mode (Frame Generation)

Controls how the limiter accounts for frame generation when computing the render rate:

- **Auto** — detects FG technology from loaded DLLs:
  - DLSS FG: `nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll`
  - FSR FG: `amd_fidelityfx_framegeneration.dll`, `ffx_framegeneration.dll`
  - Smooth Motion: `NvPresent64.dll`
- **Off** — no FG compensation. Target FPS = user FPS.
- **2x, 3x, 4x** — manual multiplier override.

When FG is active, the limiter divides the target FPS by the multiplier to get the real GPU render rate. The OSD also detects the actual multiplier from the ratio of output FPS to native FPS (from SIM_START markers) and displays it alongside the FG technology name.

---

## Boost Override

Controls NVIDIA Reflex Low Latency Boost:

- **Game** — pass through whatever the game sets.
- **On** — force boost enabled (GPU clocks stay high to minimize render time).
- **Off** — force boost disabled.

Boost increases power consumption but reduces GPU render time, which directly reduces latency. The game's original boost setting is captured when it calls `SetSleepMode` and can be restored on shutdown.

---

## Pacing Presets

> **Warning**: ReShade with addon support should not be used in online multiplayer games. Most anti-cheat systems (EAC, BattlEye, Vanguard, etc.) will flag or block DLL injection, and you risk account bans. Ultra Limiter is intended for single-player and offline use only.

The preset selector controls the frame pacing strategy. Each preset configures a combination of sub-settings.

### Low Latency (Native)
- Paces on `SIM_START` marker with no queue limit.
- Lowest possible input latency.
- Best for competitive/fast-paced games.

### Low Latency (Markers)
- Paces via Reflex latency markers with max 1 queued frame.
- Slightly smoother than Native while still prioritizing latency.

### Balanced
- Marker-based pacing with max 2 queued frames.
- Good middle ground between latency and frame consistency.

### Stability
- Marker-based pacing with max 3 queued frames.
- Prioritizes smooth frame delivery over raw latency.
- Best for cinematic/story games.

### Pace Generated (SL Proxy)
- Hooks the Streamline proxy swapchain to pace generated (interpolated) frames.
- Designed for games using NVIDIA's Streamline-based frame generation pipeline.

### Custom
Unlocks manual control of all sub-settings:
- **Use Reflex Markers** — pace using the game's Reflex latency markers.
- **Max Queued Frames** (0–4) — how many frames can be in-flight before the limiter waits.
- **Delay Present Start** — adds a delay between `SIM_START` and `PRESENT_BEGIN`.
- **Delay Amount** — how much to delay, in frame-times (e.g., 1.0 = one full frame interval).
- **Streamline Proxy** — enable SL proxy pacing.

---

## Display & OSD

The OSD draws directly to the game's foreground via ImGui. Toggle with a hotkey (default: END). Each element can be individually enabled/disabled. Position is adjustable via X/Y sliders.

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
- Tracks recent GPU active render times in an 8-sample circular buffer from GetLatency reports.
- Uses weighted averaging: most recent frame weighted 4x, second 3x, third 2x, rest 1x.
- Detects upward trends (3+ consecutive frames rising) and adds the per-frame increase to the prediction. This anticipates continued rise from camera pans, entering heavier geometry, particle effects ramping up.
- Does NOT bias downward for falling trends — being conservative on the way down is free (just slightly more GPU idle time), while being wrong on the way up causes a missed frame.
- An adaptive safety margin starts at 500 µs, widens by 50 µs per prediction miss, tightens by 10 µs every 30 stable frames. Clamped to 200–2000 µs.

**Mode-dependent behavior**:
- **1:1 (no FG)**: Tightens the interval based on predicted GPU time + safety margin. Only active when GPU-bound (enforcement site = SIM_START). The predictive interval can only reduce the base interval — it's a limiter, not an accelerator.
- **FG (2x)**: Does NOT tighten the interval. Instead, applies cadence-based stabilization — adjusts the interval to minimize measured output variance (see Cadence Tracking below).
- **MFG (3x+)**: Same as FG but more conservative — only widens the interval when output is jittery, never tightens. Holds at the best-known interval.

**Why it matters**: A static interval wastes GPU time when the workload varies. Under native rendering, predictive sleep adapts to the actual GPU workload. Under frame generation, tightening the interval fights the interpolation scheduler and causes choppy output — the mode-dependent approach works with the FG pipeline instead of against it.

**UL-exclusive**: Neither Special K nor Display Commander implement GPU time prediction for sleep scheduling, nor mode-dependent FG/MFG interval stabilization.

### 2. Phase-Locked Timing Grid

**What it does**: Provides a drift-free timing backstop that always runs, even when Reflex Sleep is active.

**How it works**:
- Frames are aligned to a fixed grid: `target[k] = epoch + k * interval`.
- If a frame arrives early (before the grid slot), the limiter sleeps until the slot.
- If a frame arrives late, the grid snaps forward to the next slot ahead of `now` — no accumulated debt or burst catch-up.
- When Reflex Sleep already paced the frame, `now` is near the grid slot and the sleep returns immediately.
- Uses high-resolution waitable timers (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) for the bulk of the sleep, with a busy-wait tail for sub-millisecond precision.
- Requests maximum kernel timer resolution via `ZwSetTimerResolution` at startup.

**Why it matters**: The naive `next_target += interval` approach drifts because each target is relative to the previous one. If frame N lands 30 µs early, frame N+1's target shifts — and this compounds. The grid eliminates phase drift entirely.

**UL-exclusive**: Neither SK nor DC use a phase-locked grid. SK uses scanline-based timing for VRR. DC uses Reflex Sleep as the primary limiter with no grid backstop.

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

**Why it matters**: Reflex Sleep's `minimumIntervalUs` is a hint, not a guarantee. The driver may systematically overshoot or undershoot by a few microseconds. This feedback loop closes the gap that other limiters leave open.

**UL-exclusive**: Neither SK nor DC implement present-to-present feedback correction.

### 4. Automatic Enforcement Site

**What it does**: Dynamically selects where in the frame pipeline to call Reflex Sleep, based on real-time GPU load and whether frame generation is active.

**How it works**:
- Monitors GPU load ratio: `avg_gpu_active_us / target_interval_us`.
- **When FG is active**: Biases toward `PRESENT_FINISH` to keep the interpolation pipeline fed. Only switches to `SIM_START` if GPU load is genuinely low (< 60%), meaning the GPU has plenty of headroom to serve both rendering and interpolation. This prevents the limiter from starving the FG chain.
- **When FG is not active**:
  - **GPU-bound (load > 85%)** → `SIM_START` — sleep fires at the start of simulation, giving the GPU maximum time to finish. Lowest latency.
  - **CPU-bound (load < 65%)** → `PRESENT_FINISH` — sleep fires at present, giving the CPU maximum time. Best frame pacing.
  - **65–85%** — hysteresis band, keeps the current site to prevent flip-flopping.
- **Deferred enforcement under FG**: When FG is active and queue depth > 1, sleep is deferred from `SIM_START` to `PRESENT_BEGIN`. This ensures the queue wait fires first and the interpolation pipeline stays fed.

**Why it matters**: The optimal enforcement site depends on whether the CPU or GPU is the bottleneck, and critically, whether frame generation is active. Under FG, enforcing at SIM_START starves the interpolation chain — the GPU can't finish the current frame fast enough to feed the interpolator, causing uneven output cadence. The FG-aware logic keeps the pipeline flowing smoothly.

**UL-exclusive**: Neither SK nor DC auto-detect the enforcement site from GPU load, nor do they adjust enforcement behavior based on FG state.

### 5. Queue Pressure Detection

**What it does**: Detects when the OS render queue is backing up and proactively throttles to prevent latency spikes.

**How it works**:
- Tracks the OS render queue time from GetLatency reports (`osRenderQueueEnd - osRenderQueueStart`).
- If the average queue time exceeds 1.5x the target interval, flags "queue pressure."
- When flagged, adds 4 µs to the sleep interval to proactively slow down frame submission.

**Why it matters**: When the render queue grows, frames pile up waiting for the GPU. This adds latency that isn't visible in frame time measurements. Detecting it early and throttling slightly prevents the queue from spiraling.

**UL-exclusive**: Neither SK nor DC monitor render queue depth from GetLatency.

### 6. Adaptive DLSS FG Pacing

**What it does**: When frame generation is active, replaces the static pacing offset with a measured value derived from actual FG overhead.

**How it works**:
- When FG is active, the interpolation pipeline adds scheduling overhead that causes the driver to overshoot the requested interval.
- The static fallback is +24 µs (derived from SK's enforcement-site offset).
- The adaptive system measures the actual FG overhead: the gap between GPU render end and present end from GetLatency reports.
- Uses an exponential moving average (α = 0.1) to smooth the measurement.
- Clamped to 4–60 µs to reject bad data.
- When enough data is available, the measured offset replaces the static one.

**Why it matters**: The FG overhead varies by game, resolution, and GPU. A static offset is a compromise — too low causes overshoot, too high wastes headroom. Measuring the actual overhead adapts to the specific setup.

**UL-exclusive**: SK uses a static +24 µs offset. DC does not have an FG pacing offset system.

### 7. Cadence Tracking & FG Stabilization

**What it does**: Measures the actual output frame cadence (present-to-present timing) and uses it to stabilize pacing under frame generation. This is the core signal that drives the FG/MFG-aware predictive sleep behavior.

**How it works**:
- Feeds `presentEndTime` values from GetLatency reports into a 32-sample circular buffer.
- Computes rolling mean and standard deviation of present-to-present deltas.
- Classifies output quality:
  - **Smooth**: stddev < 3% of the target output interval.
  - **Jittery**: stddev > 8% of the target output interval.
  - **Neutral**: in between.
- Tracks streak counters for smooth and jittery states.
- Tracks the best-known interval (the one that produced the lowest variance).
- Automatically infers the effective FG multiplier from the ratio of render interval to measured output cadence — handles 2x, 3x, and 4x without configuration.

**FG stabilization response**:
- **2x FG**: Moderate response. Widens the interval by +2 µs per poll when jittery (up to +10 µs). Tightens by -1 µs per poll when confirmed smooth for 15+ consecutive polls (down to -4 µs). Holds when neutral.
- **3x+ MFG**: Very conservative. Only widens when jittery (up to +12 µs). Never tightens — only cautiously reduces the back-off after very long stable streaks (30+ polls), and never goes below zero. This prevents the limiter from fighting the multi-frame interpolation scheduler.

**Why it matters**: Under frame generation, the relationship between the Reflex sleep interval and actual output timing is non-linear. The interpolation scheduler has its own cadence, and tightening the interval disrupts it. Cadence tracking gives the limiter a direct measurement of output quality, so it can back off when the output is jittery and hold steady when it's smooth — instead of blindly tightening and making things worse.

**UL-exclusive**: Neither SK nor DC measure output cadence variance or implement FG-aware interval stabilization.

### 8. Hybrid Queue Wait

**What it does**: When waiting for N-back frames to finish rendering (queue depth enforcement), uses a two-phase wait that saves CPU power.

**How it works**:
- **Phase 1**: Yields via short waitable timer sleeps (~0.5 ms each) to avoid burning a full CPU core.
- **Phase 2**: When less than 1 ms remains, switches to busy-wait (`YieldProcessor()`) for sub-millisecond precision.
- Timeout of 50 ms prevents infinite hangs.

**Why it matters**: Both SK and DC use pure spin-loops for queue waiting, which burns 100% of a CPU core while waiting. The hybrid approach saves power and frees CPU cycles for the render thread, while still achieving the same timing precision.

**UL-exclusive**: Neither SK nor DC use hybrid waits for queue depth enforcement.

### 9. Time-Based Warmup

**What it does**: Delays the start of frame limiting for 2 seconds after the first present, regardless of frame rate.

**How it works**:
- Records the QPC timestamp of the first present call.
- Skips all limiting until 2 seconds have elapsed.
- Uses wall-clock time, not frame count.

**Why it matters**: A frame-count warmup (e.g., "skip first 300 frames") is inconsistent — at 30 FPS that's 10 seconds, at 240 FPS it's 1.25 seconds. Time-based warmup is consistent regardless of the game's initial frame rate, which can vary wildly during loading screens and shader compilation.

### 10. Proper Reflex Restore

**What it does**: On shutdown, restores the game's original Reflex sleep mode parameters.

**How it works**:
- When the game calls `SetSleepMode`, the hook captures the game's parameters (low latency mode, boost, marker optimization) into atomic state.
- On addon unload, if the game had set Reflex params, those are restored via `InvokeSetSleepMode`.
- If the game never called `SetSleepMode`, Reflex is disabled cleanly.

**Why it matters**: Without proper restore, unloading the addon mid-game could leave Reflex in an inconsistent state — either stuck with UL's parameters or disabled when the game expected it to be on.

### 11. VRR / GSync Awareness

**What it does**: On VRR displays, clamps the sleep interval so the limiter never requests a rate above the VRR ceiling.

**How it works**:
- Computes the VRR ceiling: `ceiling_fps = 3600 * hz / (hz + 3600)`.
- Applies a 0.5% safety margin below the ceiling.
- Converts to a minimum interval in microseconds.
- If the requested interval would exceed the ceiling, clamps it.
- Cached for 2 seconds to avoid hitting the display driver every frame.
- Disabled when VSync is forced off (no VRR concern).

**Why it matters**: If the limiter requests a rate above the VRR window, the driver drops out of variable refresh and introduces judder. Clamping to the ceiling keeps the display in VRR mode.

### 12. Adaptive Interval Adjustment

**What it does**: Fine-tunes the sleep interval based on GPU headroom.

**How it works**:
- Monitors GPU load ratio from GetLatency.
- **Load < 70%** — GPU has headroom. Shaves 3 µs for tighter pacing.
- **Load > 90%** — GPU is near budget. Adds up to 4 µs buffer (scaled linearly from 0 to 4 based on how far above 90%).
- **70–90%** — no adjustment.
- A separate -2 µs "driver shave" is always applied to compensate for the driver's tendency to overshoot by a couple microseconds.

### 13. Flip Metering Block

**What it does**: Blocks NVIDIA's flip metering pacer (`NvAPI_D3D12_SetFlipConfig`) to give UL full control over FG frame pacing.

**How it works**:
- Hooks `nvapi_QueryInterface` at load time.
- When ordinal `0xF3148C42` (SetFlipConfig) is requested, returns `nullptr` instead of the real function pointer.
- This prevents the game or Streamline from resolving the flip metering function, so it can never be called.
- Exception: if Smooth Motion is active (`NvPresent64.dll` loaded), the call passes through because Smooth Motion requires flip metering.
- UL's own internal NVAPI lookups use the original (unhooked) QueryInterface pointer, so they're unaffected.
- Always active, no user toggle.

**Why it matters**: NVIDIA's flip metering pacer competes with UL's frame pacing. Blocking it ensures UL has sole control over when frames are presented.

### 14. Settings Change Reset

**What it does**: When the FPS limit, VSync override, or exclusive pacing setting changes, triggers a full reset of all adaptive state so no stale data influences the new configuration.

**How it works**:
- Detects changes to `fps_limit`, `vsync_override`, or `exclusive_pacing` every frame.
- On change, resets: GPU predictor (history, trends, safety margin), cadence tracker (deltas, variance, streaks), GPU stats EMAs, present-to-present feedback loop, timing grid epoch, and the changed-params cache.
- Re-enters the 2-second warmup period so the adaptive systems can re-converge from clean state.
- Logged as `ResetAdaptiveState: full reset`.

**Why it matters**: Adaptive systems accumulate state tuned to the current configuration. Changing the FPS limit without resetting leaves stale predictions, cadence data, and feedback corrections that can cause bad pacing for several seconds. A full reset ensures clean convergence every time.

**UL-exclusive**: Neither SK nor DC perform a coordinated reset of all adaptive state on settings changes.

---

## Reflex Hook Architecture

Ultra Limiter hooks three NVAPI functions via MinHook:

1. **NvAPI_D3D_SetSleepMode** — swallowed. The game's calls are captured (low latency mode, boost, marker optimization flags) but not forwarded. UL controls sleep mode on its own schedule via `MaybeUpdateSleepMode`, which only calls the driver when parameters actually change.

2. **NvAPI_D3D_Sleep** — swallowed. The game's sleep calls are suppressed entirely. UL calls Sleep on its own schedule to prevent double-pacing. Games like Monster Hunter Wilds call Sleep multiple times per frame — all are suppressed.

3. **NvAPI_D3D_SetLatencyMarker** — intercepted and forwarded. Key behaviors:
   - Filters RTSS (RivaTuner Statistics Server) markers via `_ReturnAddress()` — RTSS fires latency markers but is not native Reflex.
   - Records timestamps in a 64-slot ring buffer for pacing logic.
   - Deduplicates markers per frame via `seen_frame` tracking.
   - Reorders out-of-band `INPUT_SAMPLE` markers: if the game fires INPUT_SAMPLE outside the SIM_START–SIM_END window, it's queued and re-injected at the next valid boundary.
   - Notifies the pacing callback for frame pacing decisions.

4. **NvAPI_D3D_GetLatency** — resolved but NOT hooked. Called directly to read the driver's frame report ring buffer for adaptive features.

5. **nvapi_QueryInterface** — hooked to block flip metering (see above).

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
- **Pacing** — current mode: "Timing (no Reflex)", "Marker (Sim Start)", "Marker (Present)", "Present-based (Reflex Sleep)", or "SL Proxy".
- **Target** — current FPS target.

---

## INI Reference

All settings are stored in `ultra_limiter.ini` under the `[UltraLimiter]` section:

```ini
[UltraLimiter]
fps_limit=60              # 0 = unlimited, 1-500
bg_fps_limit=0            # 0 = disabled, 1-120 (background FPS cap when unfocused)
fg_mult=auto              # auto / off / 2x / 3x / 4x
boost=game                # game / on / off
preset=native_pacing      # native_pacing / marker_lowlat / marker_balanced / marker_stability / sl_proxy / custom
use_marker_pacing=true    # (custom preset only)
max_queued_frames=0       # 0-4 (custom preset only)
delay_present=false       # (custom preset only)
delay_present_amount=1.0  # in frame-times (custom preset only)
use_sl_proxy=false        # (custom preset only)
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
```

---

## Logging

A timestamped log file is written to `ultra_limiter.log` next to the addon. It records:
- Startup/shutdown events
- NVAPI initialization and hook status
- QueryInterface hook status (flip metering block)
- Reflex connection and device detection
- Enforcement site changes with GPU load percentage
- Smooth Motion detection/loss
- Swapchain init/destroy with API type and resolution
- VSync hook status and tearing support
- Frame latency hook status
- FG detection results
- Errors and exceptions with codes and addresses

Timestamps are seconds since addon load, using QPC for precision.
