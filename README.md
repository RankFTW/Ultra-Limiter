# ReLimiter

A ReShade addon for GPU-aware frame rate limiting with NVIDIA Reflex integration, frame generation awareness, and adaptive pacing. Works with 64-bit D3D11/D3D12 and Vulkan games on NVIDIA GPUs. A 32-bit build is included for older DX9/DX11 games (timing-only, no Reflex).

Everything is automatic. The pacing engine detects frame generation, selects enforcement sites, adjusts queue depth, manages Reflex Boost, and stabilizes output cadence — all in real time from pipeline telemetry. No presets, no manual FG multiplier controls.

> **Warning**: Do not use in online multiplayer games. Anti-cheat systems (EAC, BattlEye, Vanguard) will flag DLL injection. Single-player and offline use only.

---

## Installation

1. Place `relimiter.addon64` (and optionally `relimiter.addon32` for 32-bit games) in your ReShade addon directory, typically next to the game executable.
2. Launch the game. ReLimiter registers automatically.
3. Open the ReShade overlay (Home key) → "ReLimiter" tab.
4. `relimiter.ini` and `relimiter.log` are created next to the game executable on first launch.

---

## Settings

### FPS Limit

Slider from 0–500. Set 0 for unlimited. Quick-select buttons for 30, 60, 120, 240, and a Reflex cap calculated from your monitor refresh rate:

```
Reflex Cap = Refresh Rate − (Refresh Rate² / 3600)
```

165 Hz → 157 FPS. This keeps the GPU just below the display's native rate for optimal Reflex pacing.

When frame generation is active, the target is automatically divided by the detected FG multiplier to compute the real render rate.

### Background FPS Limit

Caps frame rate when alt-tabbed (0–120). Set 0 to disable (falls back to fps_limit/3, minimum 10). Uses simple sleep-based limiting with all adaptive evaluation frozen. Full adaptive reset on refocus.

### VSync Override

- **Game** — no change
- **On** — force VSync on (SyncInterval=1, ALLOW_TEARING stripped)
- **Off** — force VSync off (SyncInterval=0, ALLOW_TEARING added if supported)

### 5XXX Exclusive Pacing Optimization

Forces single-frame queue depth by overriding `SetMaximumFrameLatency` to 1. Reduces input latency on NVIDIA 50-series GPUs with flip metering support. Automatically passes through the game's requested queue depth for DMFG sessions.

### Fake Fullscreen

Intercepts exclusive fullscreen and converts to borderless window. Hooks DXGI Factory CreateSwapChain and SetFullscreenState. RE Engine games auto-skipped (incompatible).

---

## Frame Generation

Always auto-detected from loaded DLLs. No manual multiplier override.

| Technology | Detection |
|---|---|
| DLSS FG | `nvngx_dlssg.dll`, `sl.dlss_g.dll`, `dlss-g.dll` |
| FSR FG | `amd_fidelityfx_framegeneration.dll`, `ffx_framegeneration.dll` |
| NVIDIA DMFG | No FG DLL + game latency hint ≥ 3 (RTX 50 series driver-side 3x–6x) |
| Smooth Motion | `NvPresent64.dll` |

The FG multiplier (2x–6x) is inferred from the cadence ratio (output FPS / native FPS) for DLL-based FG, or directly from the game's `MaxFrameLatency` value for DMFG. Tier detection uses hysteresis (30-tick confirmation for demotions) to prevent false downgrades from transient load spikes.

### DMFG (RTX 50 Series)

Driver-side frame generation with no user-space DLL. ReLimiter detects it via the game's requested frame latency and adapts:

- Frame latency override passes through the game's queue depth instead of forcing 1
- Flip metering allowed through (DMFG requires it)
- QPC variance brake disabled — driver-injected frames cause inherently high QPC variance; cadence stddev is the sole stability signal
- FG tier derived from `MaxFrameLatency` (e.g. game_lat=6 → tier 6)
- Cadence-based tier demotion clamped to the latency hint floor

### Smooth Motion

When `NvPresent64.dll` is detected, Reflex Sleep is bypassed (conflicts with driver presentation timing), flip metering block is disabled, and all pacing falls back to the timing grid. OSD FPS/Native values are swapped so FPS shows the higher output rate.

---

## Adaptive Consistency Buffer

A two-mode state machine (STABILIZE / TIGHTEN) that manages a per-frame consistency buffer to smooth output cadence under frame generation and native rendering.

- **STABILIZE**: Increases buffer when cadence variance exceeds the instability threshold. Transitions to TIGHTEN after sustained stability (8–15 consecutive stable ticks depending on tier).
- **TIGHTEN**: Decreases buffer toward zero when cadence is stable. Transitions back to STABILIZE on instability.
- **Perceptual thresholds**: Uses absolute stddev (µs) instead of CV. Derived from the 4ms perceptual floor — variance below ~400µs at high multipliers is imperceptible and doesn't warrant buffer.
- **QPC brake**: Secondary variance signal from local present-to-present timing. Forces immediate STABILIZE on QPC spikes. Disabled for DMFG sessions (inherently noisy QPC).
- **GPU load gate**: Freezes the state machine when GPU load < 50% (menus, loading screens). Resumes without reset.
- **VRR proximity scaling**: Halves the TIGHTEN step when near the VRR ceiling (>90% proximity).

Four tuning tiers: 1:1 (4–20µs), 2x FG (12–50µs), 3x MFG (20–80µs), 4x+ MFG (30–120µs).

---

## GSync / VRR

- **GSync active detection**: Polls `NvAPI_D3D_IsGSyncActive` every 2 seconds. All VRR behavior gated on the result. Defaults to inactive if NVAPI unavailable. 64-bit only.
- **VRR floor clamping**: Prevents the limiter from requesting a rate above the VRR ceiling. Only applied when GSync is confirmed active.
- **Frame splitting disable**: Calls `NvAPI_DISP_SetAdaptiveSyncData` to disable driver frame splitting when GSync is active and FG is running. Restored on shutdown or GSync deactivation.

---

## OSD

Draws directly to the game foreground via ImGui. Toggle with hotkey (default: END). Each element individually toggleable in the settings panel.

### Elements

| Element | Color | Description |
|---|---|---|
| FPS | White | Total output frame rate (including generated frames) |
| 1% Low | White | 99th percentile frame time over 300-frame rolling window |
| Native FPS | White | Real render rate from SIM_START markers |
| Frame | White | Rolling average frame time (64-frame window) |
| GPU | Cyan | GPU active render time from Reflex GetLatency |
| Render Lat | Cyan | Sim-start to GPU-render-end latency |
| Present Lat | Cyan | Present start-to-end latency |
| FG | Gold | Detected FG technology and multiplier |
| Res | Gold | Render → output resolution with scale % |
| Smoothness | Green/Yellow/Red | Cadence smoothness score (CV-based, 0–100%) |
| Graph | Green | Small rolling frametime graph (128 samples) |
| Large Graph | Green | Large frametime graph with labeled dynamic axis limits |

### OSD Options

| Option | Range | Description |
|---|---|---|
| OSD Scale | 100–300% | Scales all text, graphs, spacing, and line thickness |
| Background | 0–100% | Dark backdrop opacity behind OSD text |
| Drop Shadow | On/Off | Shadow behind text for readability |
| Text Brightness | 0–100% | Dims all OSD text |

---

## Diagnostic CSV

Enable `csv_diagnostics=true` in the INI to write per-frame telemetry to `relimiter_diagnostics.csv`. 18 columns:

```
timestamp, smoothness, cadence_stddev_us, cadence_mean_delta_us, cadence_cv,
qpc_cv, gsync_active, cb_mode, cb_buffer_us, pred_gpu_us, pred_sim_us,
pred_fg_us, enforcement_site, queue_depth, fg_tier, vrr_proximity,
gpu_load_ratio, final_interval_us
```

Falls back to the game exe directory if the addon DLL directory is read-only (WindowsApps games).

---

## Monitor & Window

- **Monitor switching**: Move the game to a different display from the settings panel. Saved and auto-applied on swapchain init. Moves happen on a background thread to avoid D3D re-entrancy.
- **Window mode override**: Default / Fullscreen / Borderless Fullscreen.

---

## Adaptive Pacing Engine

These features run automatically. No configuration needed.

### Predictive Sleep
Per-frame interval computed from GPU/sim/FG stage predictions (8-sample weighted average with trend detection). Adaptive safety margin (200–2000µs) widens on misses, tightens on stability.

### Phase-Locked Timing Grid
Drift-free timing backstop. Frames aligned to a fixed grid with high-resolution waitable timers + busy-wait tail for sub-ms precision.

### Present-to-Present Feedback
Closed-loop correction measuring actual present cadence vs target. Nudges interval ±1µs per 30-frame window. Clamped to ±8µs. Disabled under FG.

### Automatic Enforcement Site
Dynamically selects where to call Reflex Sleep based on GPU load and bottleneck detection. GPU-bound → SIM_START (lowest latency). CPU-bound or FG → PRESENT_FINISH (best pacing).

### Dynamic Queue Depth
Voting-window system (1–3 frames) with supermajority thresholds and 4-second hysteresis. FG sessions get minimum depth 2.

### Dynamic Boost Controller
Manages Reflex Boost based on GPU idle gaps, utilization, and thermal detection. Disables boost above 85% load or when thermal throttling is suspected.

### Bottleneck Detection
Identifies the primary bottleneck (GPU, CPU sim, CPU submit, driver, queue) from per-stage EMA times. 40% threshold.

---

## Reflex Hook Architecture

Hooks three NVAPI functions via MinHook:

1. **SetSleepMode** — swallowed. Game's params captured but not forwarded. ReLimiter controls sleep mode on its own schedule.
2. **Sleep** — swallowed. Game's sleep calls suppressed. ReLimiter calls Sleep on its own schedule.
3. **SetLatencyMarker** — intercepted and forwarded. Filters RTSS markers, deduplicates per frame, reorders out-of-band INPUT_SAMPLE markers.

Additionally:
- **GetLatency** — called directly (not hooked) for pipeline telemetry.
- **QueryInterface** — hooked to block flip metering (`SetFlipConfig`). Passthrough for DMFG and Smooth Motion sessions.

---

## INI Reference

All settings in `relimiter.ini` under `[UltraLimiter]`:

```ini
fps_limit=0               # 0=unlimited, 1-500
bg_fps_limit=0            # 0=auto (fps_limit/3), 1-120
osd_on=true
osd_x=10.0
osd_y=10.0
show_fps=true
show_1pct_low=true
show_frametime=true
show_native_fps=true
show_graph=true
show_big_graph=false
show_gpu_time=true
show_render_lat=true
show_present_lat=true
show_fg_mode=true
show_resolution=true
show_smoothness=true
osd_toggle_key=35         # VK_END
osd_bg_opacity=0          # 0-100
osd_drop_shadow=false
osd_text_brightness=100   # 0-100
osd_scale=100             # 100-300
target_monitor=           # e.g. \\.\DISPLAY1
window_mode=none          # none/fullscreen/borderless
vsync_override=0          # 0=game, 1=on, 2=off
exclusive_pacing=false
fake_fullscreen=false
csv_diagnostics=false
```

---

## Logging

`relimiter.log` records startup, NVAPI init, hook status, enforcement site changes, FG detection, swapchain events, background mode, adaptive resets, and errors. Timestamps are seconds since addon load (QPC precision).
