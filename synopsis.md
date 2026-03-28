# ReLimiter v2.2.1 — Session Synopsis

## Project
ReLimiter — ReShade addon (.addon64/.addon32) for GPU-aware frame rate limiting with NVIDIA Reflex integration. Uses NVAPI hooks (SetSleepMode/Sleep/SetLatencyMarker via MinHook), phase-locked timing grid, adaptive pacing engine driven by GetLatency pipeline telemetry, frame generation detection (DLSS FG, FSR FG, DMFG, Smooth Motion), and ImGui OSD overlay. Workspace: `C:\Users\Mark\OneDrive\Desktop\New Ultra Limiter`.

## Current State
Source is at v2.2.1, matching `C:\Users\Mark\OneDrive\Desktop\New Ultra Limiter\ReMat3` exactly (all 15 source files confirmed identical via fc.exe). Build system: CMake + Visual Studio, `build.bat` in workspace root. Outputs `relimiter.addon64` (64-bit, AVX2) and `relimiter.addon32` (32-bit). C++17, MSVC.

## Source Files (15)
- `main.cpp` — ReShade addon entry, OSD rendering, FG detection, GPU latency polling
- `ul_limiter.cpp/hpp` — Adaptive pacing orchestrator, pipeline stats, enforcement site, PLL, consistency buffer
- `ul_reflex.cpp/hpp` — NVAPI hooking, marker handling, swapchain hooks (VSync, frame latency, fake fullscreen)
- `ul_timing.cpp/hpp` — QPC timing, waitable timers, phase-locked grid, busy-wait, timer promotion hooks
- `ul_config.cpp/hpp` — INI loading/saving, atomic config
- `ul_fg_monitor.cpp/hpp` — FPS-ratio FG tier detection with hysteresis
- `ul_log.cpp/hpp` — File logging with QPC timestamps
- `ul_vk_reflex.cpp/hpp` — Vulkan Reflex backend (VK_NV_low_latency2, 64-bit only)

---

## Changes Made This Session

### 1. ReLaz Merge — FG Detection Improvements
Applied changes from the ReLaz reference to improve frame generation tier detection reliability:

- **`ul_fg_monitor.cpp/hpp`**: Added `HasData()` function and `s_update_count` tracking. Returns true once 30+ `Update()` calls have been processed, allowing `DetectFGDivisor` to distinguish "confirmed no FG" from "not enough data yet". Count resets on `Reset()`.

- **`ul_limiter.hpp`**: Added `int last_fg_div_ = 0` member to track previous FG divisor for change detection.

- **`ul_limiter.cpp` — `DetectFGDivisor()` reworked**: FPS-based `ul_fg_monitor::GetTier()` is now ground truth for runtime FG state. FG DLLs stay loaded even when the game disables FG (e.g. in menus), so DLL presence alone caused false 2x detection. Now: when `HasData()` reports tier ≥2, use it directly. When `HasData()` reports tier 0, return 1 (confirmed no FG). DLL presence is fallback only before FPS data is available. DMFG latency hint still used for no-DLL case before FPS data arrives.

- **FG divisor change detection**: `ResetAdaptiveState()` now triggers when FG divisor changes at runtime (e.g. toggling FG in-game), tracked via `last_fg_div_` in the settings change detection block.

- **Background sleep safety cap**: Background sleep in `OnPresent` capped to one background interval maximum (`max_wake = now_ns + bg_ns`). Prevents `grid_next_ns_` from drifting far ahead while backgrounded, which would cause `SleepUntilNs` to block indefinitely on alt-tab back.

### 2. ReMat2 Merge — Timer Promotion, Safety Fixes, Overload Handling

- **Timer promotion hooks** (`ul_timing.cpp/hpp`): New system hooks `CreateWaitableTimerW` and `CreateWaitableTimerExW` from kernel32.dll via MinHook to inject `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on every waitable timer created in the process. ~100 lines of new code. Improves timing precision for driver-internal and middleware timers that would otherwise use the default ~15.6ms resolution. Install/remove called from `main.cpp` at DLL_PROCESS_ATTACH and shutdown.

- **Thread priority boost** (`ul_timing.cpp`): Busy-wait tail in `SleepUntilNs` now boosts to `THREAD_PRIORITY_TIME_CRITICAL` before the loop and restores previous priority after. Reduces OS preemption risk during the critical sub-millisecond timing window.

- **SEH guard on DoOwnSleep** (`ul_limiter.cpp`): `MaybeUpdateSleepMode(p)` + `InvokeSleep(dev_)` wrapped in `__try/__except(EXCEPTION_EXECUTE_HANDLER)`. Protects against driver crashes when device/swapchain is in a transitional state during alt-tab or swapchain recreation.

- **Background early-return in DoTimingFallback** (`ul_limiter.cpp`): Added `if (is_background_) return;` after warmup check. Prevents the timing fallback path from running while the game is backgrounded.

- **GPU overload flushes re-added** (`ul_limiter.cpp`): Overload state (`gpu_overload_mode_`, `gpu_overload_count_`, `gpu_recover_count_`) flushed on load gate close (gameplay→menu transition via `was_gate_open` tracking) and on FG tier change. Prevents stale overload detection from a previous scene or FG state carrying over. GPU overload evaluation re-gated on `gpu_load_gate_open_` (only during gameplay).

### 3. ReMat3 Merge — PLL Grid Correction

- **PLL from display feedback** (`ul_limiter.cpp/hpp`): New ~45-line block in `UpdatePipelineStats`. Reads the most recent `presentEndTime` from GetLatency frame reports, converts QPC ticks to nanoseconds, computes phase error between actual present time and nearest grid slot, wraps to ±half-interval for slot ambiguity, EMA-smooths (alpha=0.05), and nudges `grid_epoch_ns_` by `smoothed_error * 0.1`. Slowly aligns the timing grid to the display's actual present cadence, correcting for drift that the present-to-present feedback loop alone can't catch. New members: `pll_smoothed_error_ns_`, `kPllAlpha` (0.05), `kPllCorrectionGain` (0.1), `last_pll_frame_id_`. All reset in `ResetAdaptiveState()`.

### 4. Changelog Updated
Rewrote v2.2.1 section in `CHANGELOG.md` to document all features listed above.

---

## GreedFall 2 Smooth Motion CTD — Investigated, Not Resolved

**Symptom:** CTD during menu transition at ~15s after launch. Smooth Motion (`NvPresent64.dll`) active. Rapid swapchain destroy/recreate cycles (4-5 in ~1.5s).

**Fixes attempted (all reverted — none worked):**
1. Skip VSync Present vtable hook for Smooth Motion — log confirmed skip, still crashed
2. SEH guards on both InvokeSleep call sites — still crashed
3. Skip GSync surface acquisition NVAPI call for Smooth Motion — still crashed
4. Skip entire ConnectReflex (no Reflex hooks, no device pointer, no GSync init) — still crashed

**Conclusion:** Crash is NOT caused by our swapchain vtable hooks, NVAPI calls during init, or unguarded Reflex calls. With ConnectReflex fully skipped, the only remaining hook is `nvapi_QueryInterface` (flip metering block, which already allows SM through). Crash may be in the QI hook trampoline during rapid swapchain cycling, or unrelated to our hooks entirely (ReShade + NvPresent64.dll interaction).

**Next steps to try:**
- Skip `nvapi_QueryInterface` hook entirely for Smooth Motion
- Test if game crashes without ReLimiter but with ReShade alone
- Test without ReShade entirely
- Attach debugger / check crash dump to confirm fault location

---

## Reference Folders
- `C:\Users\Mark\OneDrive\Desktop\New Ultra Limiter\ReLaz` — Pre-ReMat2 state (ReLaz FG improvements only)
- `C:\Users\Mark\OneDrive\Desktop\New Ultra Limiter\ReMat2` — ReLaz + timer promotion + safety fixes
- `C:\Users\Mark\OneDrive\Desktop\New Ultra Limiter\ReMat3` — Current state (ReMat2 + PLL). **Matches workspace exactly.**
