# Ultra Limiter v1.0 — Feature Guide

Ultra Limiter is a ReShade addon that provides frame rate limiting with deep NVIDIA Reflex integration, frame generation awareness, and adaptive GPU-driven pacing. It loads as a `.addon64` file through ReShade and works with any D3D11 or D3D12 game.

---

## Installation

1. Place `ultra_limiter.addon64` in your ReShade addon directory (typically next to the game executable alongside ReShade).
2. Launch the game. Ultra Limiter registers itself automatically.
3. Open the ReShade overlay (default: Home key) and find the "Ultra Limiter" tab.
4. A configuration file (`ultra_limiter.ini`) is created next to the addon on first launch. All settings persist there.
5. A log file (`ultra_limiter.log`) is written alongside the addon for diagnostics.

---

## FPS Limit

The primary control. Set your target output frame rate with the slider (0–500). Setting 0 means unlimited (no cap).

Quick-select buttons are provided for common targets:
- **30, 60, 120, 240** — fixed presets.
- **Reflex (N)** — calculated from your monitor's refresh rate using the Reflex cap formula: `fps = hz - (hz² / 4096)`, rounded down. This is the sweet spot for Reflex-based pacing — just below the display's native rate so the driver always has a frame ready without exceeding the VSync window.

When frame generation is active, the limiter automatically divides the target by the FG multiplier to compute the real render rate. For example, at 120 FPS with DLSS FG 2x, the GPU renders at 60 FPS and the driver interpolates the rest.

---

## Pacing Presets

The preset selector controls the frame pacing strategy. Each preset configures a combination of sub-settings (marker pacing, queue depth, delay present, SL proxy).

### Low Latency (Native)
- Paces on `SIM_START` marker with no queue limit.
- Lowest possible input latency.
- Best for competitive/fast-paced games where latency matters more than smoothness.

### Low Latency (Markers)
- Paces via Reflex latency markers with max 1 queued frame.
- Slightly smoother than Native while still prioritizing latency.

### Balanced
- Marker-based pacing with max 2 queued frames.
- Good middle ground between latency and frame consistency.

### Stability
- Marker-based pacing with max 3 queued frames.
- Prioritizes smooth frame delivery over raw latency.
- Best for cinematic/story games or when frame pacing consistency matters most.

### Pace Generated (SL Proxy)
- Hooks the Streamline proxy swapchain to pace generated (interpolated) frames.
- Designed for games using NVIDIA's Streamline-based frame generation pipeline.
- Calls Reflex Sleep on the proxy present path rather than the main present.

### Custom
- Unlocks manual control of all sub-settings:
  - **Use Reflex Markers** — pace using the game's Reflex latency markers.
  - **Max Queued Frames** (0–4) — how many frames can be in-flight before the limiter waits. 0 means no queue limit.
  - **Delay Present Start** — adds a deliberate delay between `SIM_START` and `PRESENT_BEGIN` to spread CPU work more evenly across the frame.
  - **Delay Amount** — how much to delay, expressed in frame-times (e.g., 1.0 = one full frame interval).
  - **Use SL Proxy** — enable Streamline proxy pacing.

---

## Enforcement Site (Automatic)

The limiter automatically selects where in the frame pipeline to call Reflex Sleep, based on real-time GPU load:

- **GPU-bound (load > 85%)** → `SIM_START` — sleep fires at the start of simulation, giving the GPU maximum time to finish. Lowest latency.
- **CPU-bound (load < 65%)** → `PRESENT_FINISH` — sleep fires at present, giving the CPU maximum time. Best frame pacing.
- **In between (65–85%)** — hysteresis band, keeps the current site to prevent flip-flopping.

The current enforcement site is shown in the Status section of the overlay (e.g., "Marker (Sim Start)" or "Marker (Present)").

---

## VSync Override

Override the game's VSync setting at the DXGI Present call level:
- **Game** — no change, use whatever the game sets.
- **On** — force VSync on (SyncInterval = 1).
- **Off** — force VSync off (SyncInterval = 0).

This is implemented by hooking `IDXGISwapChain::Present` and overriding the sync interval parameter.

---

## 5XXX Exclusive Pacing Optimization

Forces single-frame queue depth by hooking `IDXGISwapChain2::SetMaximumFrameLatency` and overriding it to 1. This prevents the GPU from queuing extra frames, reducing input latency.

Designed for NVIDIA 50-series (Blackwell) GPUs with flip metering support, where the hardware can enforce single-frame pacing without the throughput penalty seen on older architectures.

---

## Boost Override

Controls NVIDIA Reflex Low Latency Boost:
- **Game** — pass through whatever the game sets.
- **On** — force boost enabled (GPU clocks stay high to minimize render time).
- **Off** — force boost disabled.

Boost increases power consumption but reduces GPU render time, which directly reduces latency.

---

## FG Mode (Frame Generation)

Controls how the limiter accounts for frame generation when computing the render rate:
- **Auto** — detects FG technology from loaded DLLs:
  - DLSS FG: `nvngx_dlssg.dll`, `_nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll`
  - FSR FG: `amd_fidelityfx_framegeneration.dll`, `ffx_framegeneration.dll`
  - Smooth Motion: `NvPresent64.dll`
- **Off** — no FG compensation. Target FPS = user FPS.
- **2x, 3x, 4x** — manual multiplier override.

When FG is active, the limiter divides the target FPS by the multiplier to get the real GPU render rate. For example, 120 FPS target with 2x FG means the GPU renders at 60 FPS.

---

## Adaptive Features (Under the Hood)

These features run automatically based on data from `NvAPI_D3D_GetLatency`. They require an NVIDIA GPU with Reflex hooks active.

### Adaptive Interval Adjustment
Monitors GPU load ratio (GPU active time / target interval):
- **Load < 70%** — GPU has headroom. Shaves 3µs from the sleep interval for tighter pacing.
- **Load > 90%** — GPU is near budget. Adds up to 4µs buffer to prevent frame drops.
- **70–90%** — no adjustment.

### Present-to-Present Feedback Loop
A closed-loop correction that measures actual present cadence vs. the target:
- Accumulates signed timing error over 30-frame windows.
- Nudges the sleep interval ±1µs per window to compensate for systematic driver over/undershoot.
- Clamped to ±8µs total correction.
- Only active without frame generation (FG has non-linear timing).
- Resets when the FPS limit changes.

### Render Queue Monitoring
Tracks the OS render queue time from `GetLatency` reports:
- If average queue time exceeds 1.5x the target interval, flags "queue pressure."
- When flagged, adds 4µs to the sleep interval to proactively throttle and prevent latency spikes.

### Adaptive FG Pacing Offset
When frame generation is active, the interpolation pipeline adds scheduling overhead that causes the driver to overshoot the requested interval. Instead of using a static +24µs offset, the limiter measures the actual FG overhead (gap between GPU render end and present end) and uses that as the offset. Clamped to 4–60µs.

### Predictive Sleep
When GPU-bound, the limiter predicts the next frame's GPU time and computes a tighter sleep interval:
- Tracks recent GPU active render times in an 8-sample circular buffer.
- Uses weighted averaging (recent frames weighted 4x, 3x, 2x, 1x).
- Detects upward trends (3+ consecutive frames rising) and adds the per-frame increase to the prediction. Does NOT bias downward — being conservative on the way down is free.
- Adaptive safety margin starts at 500µs, widens by 50µs per miss, tightens by 10µs every 30 stable frames. Clamped to 200–2000µs.
- Only active when GPU-bound (`auto_site == SIM_START`) and enough data is available.
- The predictive interval can only tighten (reduce) the base interval — it's a limiter, not an accelerator.

### VRR / GSync Ceiling
On VRR displays, the driver enforces an upper frame rate: `ceiling = 3600 * hz / (hz + 3600)`. The limiter clamps the sleep interval so it never requests a rate above this ceiling (with a 0.5% margin). Prevents the driver from dropping out of the VRR window, which would cause judder.

### Smooth Motion Detection
Detects NVIDIA Smooth Motion (driver-level frame interpolation via `NvPresent64.dll`). When active, Reflex Sleep conflicts with the driver's presentation timing, so the limiter falls back to timing-only pacing.

---

## Timing Fallback

A QPC-based hard backstop that always runs, even when Reflex is active. Uses a phase-locked timing grid:

- Frames are aligned to a fixed grid: `target[k] = epoch + k * interval`.
- If a frame arrives early, the limiter sleeps until the next grid slot.
- If a frame arrives late, the grid snaps forward — no accumulated debt or burst catch-up.
- Uses high-resolution waitable timers (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) for the bulk of the sleep, with a busy-wait tail for sub-millisecond precision.
- Requests maximum kernel timer resolution via `ZwSetTimerResolution` at startup.

This is fundamentally different from the naive `next_target += interval` approach (which drifts) — the grid eliminates phase drift entirely.

---

## On-Screen Display (OSD)

The OSD draws directly to the game's foreground via ImGui. Toggle it with a hotkey (default: END). Each element can be individually enabled/disabled in the overlay settings. Position is adjustable via X/Y sliders.

### OSD Elements

| Element | Color | Description |
|---------|-------|-------------|
| **FPS** | White | Total output frame rate (including generated frames). When Smooth Motion is active, the display swaps so FPS shows the higher output rate. |
| **Native FPS** | White | Real render rate excluding generated frames. Derived from `SIM_START` marker timing. Only shows when the game uses Reflex markers. |
| **Frame** | White | Rolling average frame time in milliseconds (64-frame window). |
| **GPU** | Cyan | GPU active render time from NVAPI `GetLatency` (`gpuActiveRenderTimeUs`). Falls back to the limiter's internal EMA stats if the direct poll isn't available. |
| **Render Lat** | Cyan | Sim-start to GPU-render-end latency. Requires the game to send Reflex `SIM_START` markers. |
| **Present Lat** | Cyan | Present start-to-end latency from `GetLatency` reports. |
| **FG** | Gold | Detected frame generation technology and multiplier (e.g., "DLSS FG 2x", "FSR FG", "Smooth Motion", "None"). |
| **Res** | Gold | Render and output resolution with scale percentage (e.g., "1440x810 -> 2560x1440 (56%)"). Detected by tracking the most frequently bound sub-native viewport per frame. |
| **Graph** | Green | Rolling frametime history graph (128 samples). Auto-scales to the maximum value with a 33.3ms floor. |

---

## Monitor Switching

Move the game window to a different display without alt-tabbing:
- **Refresh Monitors** — re-enumerates connected displays.
- **Monitor dropdown** — select a target monitor or "No Override."
- The selected monitor is saved and automatically applied when the game's swapchain initializes.
- Monitor moves happen on a background worker thread to avoid D3D re-entrancy issues.

---

## Window Mode Override

Force the game's window mode:
- **Default** — no change.
- **Fullscreen** — forces `WS_POPUP` style, removes `WS_OVERLAPPEDWINDOW`, positions to fill the monitor.
- **Borderless Fullscreen** — removes caption, thick frame, and system menu styles, adds `WS_POPUP`, positions to fill the monitor.

Applied on swapchain init and when changed in the overlay.

---

## Keybinds

- **Toggle OSD** — rebindable hotkey (default: END). Click the button in the overlay, then press any key to rebind. Supports F1–F12, navigation keys, numpad, letters, and digits.

---

## Reflex Hook Architecture

Ultra Limiter hooks three NVAPI functions via MinHook:

1. **NvAPI_D3D_SetSleepMode** — swallowed. The game's calls are captured (low latency mode, boost, marker optimization flags) but not forwarded. The limiter controls sleep mode on its own schedule.
2. **NvAPI_D3D_Sleep** — swallowed. The game's sleep calls are suppressed entirely. The limiter calls Sleep on its own schedule to prevent double-pacing.
3. **NvAPI_D3D_SetLatencyMarker** — intercepted and forwarded. Timestamps are recorded in a 64-slot ring buffer. Out-of-band `INPUT_SAMPLE` markers (fired outside the `SIM_START`–`SIM_END` window) are queued and re-injected at the next valid boundary.

**NvAPI_D3D_GetLatency** is resolved but not hooked — called directly to read the driver's frame report ring buffer.

---

## Status Display

The bottom of the overlay shows:
- **Reflex** — "Hooked" or "Not hooked" (whether NVAPI hooks are active).
- **Native Reflex** — "Detected" or "No" (whether the game itself calls `SetLatencyMarker`).
- **Pacing** — current pacing mode: "Timing (no Reflex)", "Marker (Sim Start)", "Marker (Present)", "Present-based (Reflex Sleep)", or "SL Proxy".
- **Target** — current FPS target.

---

## INI Reference

All settings are stored in `ultra_limiter.ini` under the `[UltraLimiter]` section:

```ini
[UltraLimiter]
fps_limit=60
fg_mult=auto          # auto / off / 2x / 3x / 4x
boost=game            # game / on / off
preset=native_pacing  # native_pacing / marker_lowlat / marker_balanced / marker_stability / sl_proxy / custom
use_marker_pacing=true
max_queued_frames=0
delay_present=false
delay_present_amount=1.0
use_sl_proxy=false
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
target_monitor=
window_mode=none      # none / fullscreen / borderless
vsync_override=0      # 0 = game, 1 = force on, 2 = force off
exclusive_pacing=false
osd_toggle_key=35     # virtual key code (35 = END)
```

---

## Logging

A timestamped log file is written to `ultra_limiter.log` next to the addon. It records:
- Startup and shutdown events
- NVAPI initialization and hook status
- Reflex connection and device detection
- Enforcement site changes
- Smooth Motion detection
- Swapchain init/destroy with API type and resolution
- FG detection results
- Errors and exceptions

Timestamps are seconds since addon load, using QPC for precision.
