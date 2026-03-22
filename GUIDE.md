# Ultra Limiter v2.0 — Feature Guide

## Overview

Ultra Limiter is a ReShade addon that provides frame-generation-aware frame rate limiting with deep NVIDIA Reflex integration. It hooks into the game's rendering pipeline to deliver precise frame pacing, an informational on-screen display, and display management features.

## Installation

1. Install [ReShade](https://reshade.me/) into your game (with addon support enabled).
2. Copy `ultra_limiter.addon64` into the same folder as the game executable (next to ReShade's DLL).
3. Launch the game. Ultra Limiter will auto-load and create a default `.ini` config file beside the addon.

## Configuration

All settings are accessible through the ReShade overlay (default: Home key). Open the overlay and expand the "Ultra Limiter" panel. Changes are saved automatically to the `.ini` file next to the addon DLL.

Settings can also be edited directly in the INI file. The file is created automatically on first launch with sensible defaults.

---

## Frame Rate Limiting

### FPS Limit
- Slider from 0 to 500.
- Set to 0 for unlimited (pacing disabled).
- When frame generation is active, the limiter automatically divides this target to compute the correct native render rate. For example, with a 120 FPS limit and 2x FG detected, the engine is paced at 60 FPS.

### Frame Generation Awareness

Ultra Limiter detects frame generation technology and adjusts pacing accordingly:

- **DLSS Frame Generation** — detected via `nvngx_dlssg.dll`, `sl.dlss_g.dll`, or `dlss-g.dll`
- **FSR Frame Generation** — detected via `amd_fidelityfx_framegeneration.dll` or `ffx_framegeneration.dll`
- **NVIDIA Smooth Motion** — detected via `NvPresent64.dll` (driver-level frame interpolation)

The FG Mode setting controls how the limiter accounts for generated frames:

| Setting | Behavior |
|---------|----------|
| Auto    | Detect from loaded DLLs (default) |
| Off     | No FG — target FPS equals the user-set limit |
| 2x      | Assume 2x frame generation |
| 3x      | Assume 3x multi-frame generation |
| 4x      | Assume 4x multi-frame generation |

### NVIDIA Smooth Motion Compatibility

Smooth Motion is driver-level frame interpolation that operates independently of the game. Ultra Limiter detects it automatically and switches to timing-only pacing (no Reflex Sleep calls), because Reflex Sleep disrupts the driver's frame interpolation cadence. This is handled transparently — no user configuration needed.

---

## Pacing Presets

The Preset dropdown selects the frame pacing strategy. Each preset configures how the limiter synchronizes with the game's render pipeline.

### Low Latency (Native)
- Paces on `SIMULATION_START` markers only.
- No frame queue limit.
- Lowest latency option for games with native Reflex support.

### Low Latency (Markers)
- Paces via Reflex markers with a max queued frame depth of 1.
- Sleeps on `PRESENT_FINISH`, waits for previous frames on `PRESENT_BEGIN`.
- Good balance of low latency and stability.

### Balanced
- Same as Low Latency (Markers) but allows up to 2 queued frames.
- Smoother frame delivery at the cost of slightly higher latency.

### Stability
- Allows up to 3 queued frames.
- Best for games with inconsistent frame times where smoothness matters more than latency.

### Pace Generated (SL Proxy)
- Hooks the Streamline proxy's swapchain Present call directly.
- Paces generated frames via the proxy rather than Reflex markers.
- Use this for games using Streamline-based frame generation.

### Custom
- Unlocks all sub-settings for manual control:
  - **Use Reflex Markers** — enable/disable marker-based pacing
  - **Max Queued Frames** — 0 to 4, how many frames can be in-flight before the limiter waits
  - **Sim Start Only** — only pace on SIMULATION_START (ignores PRESENT markers)
  - **Delay Present Start** — add a configurable delay between SIM_START and PRESENT_BEGIN
  - **Delay (frames)** — how long to delay, measured in frame-time units (0 to 3)
  - **Streamline Proxy** — enable SL proxy Present hooking

---

## Reflex Integration

Ultra Limiter hooks three NVIDIA Reflex functions via MinHook:

- **NvAPI_D3D_SetSleepMode** — intercepted to capture the game's Reflex settings (low latency, boost, marker optimization). The game's call is swallowed; Ultra Limiter controls sleep mode directly.
- **NvAPI_D3D_Sleep** — intercepted and suppressed. Ultra Limiter calls Sleep on its own schedule based on the active pacing preset.
- **NvAPI_D3D_SetLatencyMarker** — intercepted to record timestamps in a ring buffer and trigger pacing callbacks. The marker is still forwarded to the driver.
- **NvAPI_D3D_GetLatency** — resolved but not hooked. Called directly to poll GPU latency data for the OSD.

### Boost Override
Controls the Reflex low-latency boost flag:

| Setting | Behavior |
|---------|----------|
| Game    | Pass through whatever the game sets (default) |
| On      | Force boost enabled |
| Off     | Force boost disabled |

### Status Display
The overlay shows:
- **Reflex**: Hooked / Not hooked
- **Native Reflex**: Detected / No (whether the game itself uses Reflex markers)
- **Pacing**: Current active pacing mode (Timing, Marker, SL Proxy, etc.)
- **Target**: Current FPS target

---

## On-Screen Display (OSD)

The OSD renders directly to the game's foreground draw list via Dear ImGui. Each element can be individually toggled.

### OSD Elements

| Element | Description | Color |
|---------|-------------|-------|
| FPS | Total output frame rate (including generated frames) | White |
| Native | Real render frame rate and frametime (excluding generated frames) | White |
| Frame | Frametime in milliseconds | White |
| GPU | GPU active render time (from NvAPI_D3D_GetLatency) | Cyan |
| Render Lat | Sim-to-GPU-render-end latency | Cyan |
| Present Lat | Present start-to-end latency | Cyan |
| FG | Detected frame generation mode (DLSS FG, FSR FG, Smooth Motion, None) with multiplier | Gold |
| Res | Render resolution → output resolution with scale percentage | Gold |
| Graph | Frametime history graph (128 samples) | Green |

### Resolution Detection
The OSD detects the internal render resolution (DLSS/FSR upscale source) by monitoring viewport binds. When the game binds a viewport smaller than the output resolution with a matching aspect ratio, it's identified as the upscale source. The most frequently bound sub-native viewport per frame is used, filtering out transient post-processing viewports.

Display format: `Res: 1920x1080 -> 3840x2160 (50%)`

When no upscaling is detected: `Res: 3840x2160 (native)`

### OSD Position
- **OSD X** — horizontal position (0 to 7680)
- **OSD Y** — vertical position (0 to 4320)

### Smooth Motion Display Correction
When NVIDIA Smooth Motion is active, the FPS and Native counters are automatically swapped so that FPS shows the total output rate (including interpolated frames) and Native shows the real render rate.

---

## Display Management

### VSync Override

Ultra Limiter can override the game's VSync setting at the DXGI level by hooking `IDXGISwapChain::Present` on the game's swapchain. This intercepts every Present call and modifies the `SyncInterval` parameter before it reaches the driver.

| Setting | Behavior |
|---------|----------|
| Game    | No override — the game's own SyncInterval is passed through unchanged (default) |
| On      | Forces `SyncInterval = 1` on every Present call, synchronizing each frame to the next vertical blank. Removes `DXGI_PRESENT_ALLOW_TEARING` if set. |
| Off     | Forces `SyncInterval = 0`, cancelling any VSync wait. Adds `DXGI_PRESENT_ALLOW_TEARING` if the system supports it (checked via `IDXGIFactory5::CheckFeatureSupport`). |

How it works internally:
1. On swapchain creation, Ultra Limiter hooks vtable index 8 (`Present`) of the game's `IDXGISwapChain` using MinHook.
2. Every time the game calls `Present(SyncInterval, Flags)`, the hook intercepts the call and rewrites `SyncInterval` and `Flags` based on the current setting before forwarding to the real driver function.
3. If the Streamline proxy Present hook is already active (for FG pacing), the VSync override is applied inside that hook instead, avoiding a double-hook on the same vtable slot.
4. Tearing support (`DXGI_PRESENT_ALLOW_TEARING`) is auto-detected at hook time by querying `IDXGIFactory5`. On systems that don't support it (older Windows versions or non-flip-model swapchains), the flag is not added, preventing Present failures.

The setting is persisted to the INI file as `vsync_override` (0 = Game, 1 = On, 2 = Off).

### Monitor Selection
- Lists all connected monitors with resolution, position, and primary status.
- Select a monitor to move the game window to it.
- The selection is saved and automatically applied on game launch.
- **Refresh Monitors** button re-enumerates displays.
- Monitor moves are performed on a background worker thread to avoid D3D re-entrancy issues.

### Window Mode Override

| Setting | Behavior |
|---------|----------|
| Default | No override — use the game's window mode |
| Fullscreen | Force exclusive-style fullscreen (WS_POPUP, no title bar) |
| Borderless Fullscreen | Force borderless fullscreen (strip all window chrome, resize to monitor) |

Window mode changes are also performed on the background worker thread and applied automatically on launch if saved.

---

## Timing System

Ultra Limiter uses a high-precision timing system for frame pacing:

- **QueryPerformanceCounter** for sub-microsecond time measurement.
- **High-resolution waitable timers** (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Win10 1803+) for the bulk of the sleep duration.
- **ZwSetTimerResolution** (ntdll) to request the finest kernel timer resolution available.
- **Busy-wait tail** — the final ~2 timer resolutions are spent in a `YieldProcessor()` spin loop for sub-millisecond precision.

This hybrid approach minimizes CPU usage while maintaining accurate frame delivery.

---

## Logging

Ultra Limiter writes a timestamped log file next to the addon DLL (same name, `.log` extension). The log includes:
- Startup and shutdown events
- Reflex hook status
- Frame generation detection results
- Smooth Motion detection
- Monitor moves and window mode changes
- GPU latency polling status
- Crash information (via unhandled exception filter)

---

## INI Configuration Reference

All settings are stored in `[UltraLimiter]` section. The INI file is created next to the addon DLL.

```ini
[UltraLimiter]
fps_limit=60
fg_mult=auto          # auto, off, 2x, 3x, 4x
boost=game            # game, on, off
preset=native_pacing  # native_pacing, marker_lowlat, marker_balanced,
                      # marker_stability, sl_proxy, custom
use_marker_pacing=true
max_queued_frames=0
sim_start_only=false
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
target_monitor=       # e.g. \\.\DISPLAY1
window_mode=none      # none, fullscreen, borderless
vsync_override=0      # 0 = game (no override), 1 = force on, 2 = force off
osd_toggle_key=35     # virtual key code (35 = VK_END)
```

---

## Build Requirements

- CMake 3.15+
- Visual Studio 2022 or later (x64 target)
- Dependencies (cloned via `setup.bat`):
  - MinHook (BSD-2)
  - ReShade SDK v6.7.3 (BSD-3)
  - Dear ImGui v1.92.5-docking (MIT)
  - NVIDIA NVAPI SDK (MIT)

```
setup.bat   # clone dependencies
build.bat   # configure and build
```

Output: `build\Release\ultra_limiter.addon64`
