# Ultra Limiter — Source Code Comparison

A technical comparison of Ultra Limiter (UL) against Special K (SK) and Display Commander (DC) at the source code level. This document examines architecture, algorithms, hooking strategies, and unique features to demonstrate that UL is an independent clean-room implementation.

---

## 1. Architecture

| Aspect | Ultra Limiter | Display Commander | Special K |
|--------|--------------|-------------------|-----------|
| Type | ReShade addon | ReShade addon | Standalone injection |
| Size | ~2,500 lines, 10 files | ~50+ files, 20+ subdirs | ~150K+ lines, 100+ files |
| Scope | Frame limiting only | HDR, DLSS, audio, input, latent sync, etc. | Everything (HDR, input, render, profiles, etc.) |
| NVAPI headers | Custom minimal structs | Official NVAPI SDK headers | Official NVAPI SDK headers |
| Class design | Single `UlLimiter` class | `ReflexProvider` → `ReflexManager` hierarchy | Framework macros, render subsystem |
| License | MIT | — | GPL v3 |

UL defines its own struct layouts from the public NVAPI SDK documentation with custom naming (`NvSleepParams`, `NvMarkerParams`, `NvLatencyResult`). DC and SK both use the official headers (`NV_SET_SLEEP_MODE_PARAMS`, `NV_LATENCY_MARKER_PARAMS`, etc.).

---

## 2. Hook Strategy

All three projects hook the same NVAPI functions (because those are the only functions that exist for Reflex control), but the implementation patterns differ completely.

### SetSleepMode

| | UL | DC | SK |
|-|----|----|-----|
| Behavior | Always swallows | Conditionally forwards | Separate render subsystem |
| State capture | Atomic booleans | `shared_ptr<NV_SET_SLEEP_MODE_PARAMS>` | Framework state objects |
| Suppression | Always (UL drives sleep mode) | 5-frame window after Direct calls | N/A (different architecture) |

UL always swallows the game's `SetSleepMode` call and drives sleep mode on its own schedule. DC conditionally forwards based on a 5-frame suppression window and native Reflex state.

### Sleep

| | UL | DC |
|-|----|-----|
| Behavior | Always swallows | Conditionally forwards |
| Tracking | Frame count for diagnostics | Rolling average timing |
| Purpose | UL calls Sleep on its own schedule | Forwards when not suppressing |

UL suppresses all game Sleep calls. DC conditionally forwards based on settings and native Reflex state.

### SetLatencyMarker

| | UL | DC |
|-|----|-----|
| RTSS filter | `_ReturnAddress()` + `GetModuleHandleExW` | `GetCallingDLL()` helper |
| Ring buffer | 64-slot `MarkerSlot` with atomic fields | Cyclic buffer with different layout |
| Dedup | `seen_frame` per marker type | `frame_id_by_marker_type` |
| Input reorder | Yes (queues out-of-band INPUT_SAMPLE) | No |
| Thread tracking | No | Yes (first 6 marker types) |
| Pacing dispatch | Callback to `OnMarker` | `ProcessReflexMarkerFpsLimiter` |

Both filter RTSS and use ring buffers, but the implementations are structurally different. UL has INPUT_SAMPLE reordering that DC does not. DC has thread tracking that UL does not.

### QueryInterface

| | UL | DC | SK |
|-|----|-----|-----|
| Hooked? | Yes — blocks flip metering | No | Yes — intercepts ordinal lookups |
| Purpose | Block SetFlipConfig (0xF3148C42) | N/A | Intercept various NVAPI calls |
| Exception | Allows through for Smooth Motion | N/A | Different use case |

---

## 3. Frame Limiter Algorithm

This is where the three projects diverge most significantly.

### Ultra Limiter
- **Primary**: Reflex Sleep with adaptive interval from `BuildSleepParams()`
- **Backstop**: Phase-locked timing grid (`target = epoch + k * interval`)
- **Sleep method**: High-resolution waitable timer + busy-wait tail
- **Adaptive**: Mode-dependent predictive sleep, cadence tracking, P2P feedback, interval adjustment, VRR ceiling
- **FG-aware**: Cadence-based stabilization under FG/MFG (variance minimization instead of interval tightening)
- **Grid behavior**: Late frames snap forward (no debt accumulation)
- **Settings change**: Full adaptive reset (predictors, cadence, EMAs, P2P, grid, warmup)

### Display Commander
- **Primary**: Reflex Sleep as the FPS limiter (`ShouldUseReflexAsFpsLimiter`)
- **Backstop**: None (Reflex Sleep is the limiter)
- **Queue wait**: Pure `YieldProcessor()` spin-loop
- **Delay present**: `sim_start + delay_frames * frame_time`
- **Adaptive**: None (no predictive sleep, no P2P feedback, no interval adjustment)

### Special K
- **Primary**: Scanline-based timing for VRR displays (Latent Sync)
- **Backstop**: `SK_Framerate_WaitForVBlank` infrastructure
- **Sleep method**: Custom framework with driver profile manipulation
- **Adaptive**: Scanline tracking (different approach entirely)
- **NvDRS**: Manipulates driver settings for frame rate limits

**Summary**: UL uses a phase-locked grid with GPU-aware prediction and FG-aware cadence stabilization. DC uses Reflex Sleep directly. SK uses scanline-based timing. No algorithmic overlap.

---

## 4. Interval Computation

UL's `AdjustIntervalUs()` applies six corrections to the raw interval:

1. **VRR floor** — clamp to GSync ceiling (`3600 * hz / (hz + 3600)`)
2. **FG offset** — adaptive (measured from GetLatency) or static (+24 µs)
3. **Queue pressure** — +4 µs when render queue is backing up
4. **GPU headroom** — -3 µs when load < 70%, up to +4 µs when load > 90%
5. **P2P feedback** — ±1 µs nudge from present cadence error (clamped ±8 µs)
6. **Driver shave** — -2 µs to compensate for systematic driver overshoot

DC applies none of these corrections. SK's interval computation is in a completely different subsystem with different logic.

---

## 5. Enforcement Site Selection

| | UL | DC | SK |
|-|----|-----|-----|
| Method | Auto from GPU load + FG state | Manual/preset | Different architecture |
| GPU-bound (no FG) | SIM_START (lowest latency) | N/A | N/A |
| CPU-bound (no FG) | PRESENT_FINISH (best pacing) | N/A | N/A |
| FG active | PRESENT_FINISH (unless GPU < 60%) | N/A | N/A |
| Deferred enforcement | Yes (FG + queue > 1 → PRESENT_BEGIN) | No | N/A |
| Hysteresis | 65–85% band (no FG), 60% threshold (FG) | N/A | N/A |

UL dynamically selects the enforcement site based on real-time GPU load and FG state. Under frame generation, it biases toward PRESENT_FINISH to keep the interpolation pipeline fed, and defers sleep when queue depth > 1 so the queue wait fires first. Neither DC nor SK auto-detect the enforcement site or adapt it for FG.

---

## 6. Features Unique to Ultra Limiter

These features exist in UL but not in DC or SK:

| Feature | Description | DC | SK |
|---------|-------------|----|----|
| **GpuPredictor** | Trend-aware GPU time prediction with adaptive safety margins | ✗ | ✗ |
| **Cadence Tracker** | Output present-to-present variance measurement for pacing quality | ✗ | ✗ |
| **FG/MFG Stabilization** | Cadence-based interval adjustment under frame generation (never tightens under 3x+) | ✗ | ✗ |
| **Mode-Dependent Predictive Sleep** | Tightens under 1:1, stabilizes under FG, holds under MFG | ✗ | ✗ |
| **FG-Aware Enforcement Site** | Biases PRESENT_FINISH under FG, defers sleep when queue > 1 | ✗ | ✗ |
| **Settings Change Reset** | Full adaptive state reset on FPS/VSync/exclusive changes | ✗ | ✗ |
| **P2P Feedback Loop** | Closed-loop correction from measured present cadence | ✗ | ✗ |
| **Phase-Locked Grid** | Grid-based timing instead of relative-target advancement | ✗ | ✗ |
| **Hybrid Queue Wait** | Waitable timer + busy-wait (DC/SK spin-loop) | ✗ | ✗ |
| **Adaptive FG Offset** | Measured FG overhead replaces static +24 µs | ✗ | ✗ |
| **Auto Enforcement Site** | GPU load drives SIM_START vs PRESENT_FINISH | ✗ | ✗ |
| **VRR Ceiling Formula** | `3600 * hz / (hz + 3600)` with safety margin | ✗ | ✗ |
| **Queue Pressure Detection** | Render queue depth monitoring from GetLatency | ✗ | ✗ |
| **Adaptive Interval Adjustment** | GPU headroom-based interval fine-tuning | ✗ | ✗ |
| **Flip Metering Block** | QueryInterface hook blocks SetFlipConfig | ✗ | ✗ |
| **Time-Based Warmup** | 2-second wall-clock warmup (not frame-count) | ✗ | ✗ |
| **INPUT_SAMPLE Reordering** | Queues and re-injects out-of-band input markers | ✗ | ✗ |

---

## 7. Features Shared Across Projects

These are convergent solutions to the same technical requirements, not evidence of code sharing:

| Feature | Why it's shared |
|---------|----------------|
| MinHook for NVAPI hooks | MinHook is the standard x86/x64 hooking library |
| QueryInterface pattern | The only way to resolve NVAPI function pointers |
| Ring buffer for markers | Natural data structure for frame-indexed timestamps |
| RTSS filtering | RTSS fires fake Reflex markers; everyone must filter them |
| ReShade addon API | UL and DC are both ReShade addons |
| ImGui for overlay | Standard immediate-mode GUI used by all three |
| `nvapi_interface.h` | MIT-licensed function ID table from NVAPI SDK |

---

## 8. Code Style Comparison

| Aspect | UL | DC | SK |
|--------|----|----|-----|
| State management | Atomics, flat C-style | Class hierarchies, `shared_ptr` | Framework macros, COM-style |
| Naming | `snake_case` (`s_orig_sleep`, `g_ring`) | `camelCase` / `PascalCase` | `SK_` prefix macros |
| Error handling | SEH `__try/__except` wrappers | Exception-based | Custom framework |
| Logging | Simple `ul_log::Write` | Multi-level logging system | `SK_LOG` macro system |
| Config | Windows INI API | Settings framework | Registry + INI + profiles |

---

## 9. VSync Implementation

| | UL | DC | SK |
|-|----|-----|-----|
| Method | Hook `IDXGISwapChain::Present` vtable | N/A | Driver profile manipulation |
| Override | SyncInterval + ALLOW_TEARING flags | N/A | NvDRS settings |
| Tearing detect | `IDXGIFactory5::CheckFeatureSupport` | N/A | Different approach |
| SL proxy | Also overrides in SL Present hook | N/A | N/A |

---

## 10. Frame Latency Override

| | UL | DC | SK |
|-|----|-----|-----|
| Method | Hook `IDXGISwapChain2::SetMaximumFrameLatency` | N/A | Different mechanism |
| Override value | 1 (single-frame queue) | N/A | N/A |
| Get hook | Returns game's original value | N/A | N/A |
| Waitable check | Only hooks waitable swapchains | N/A | N/A |

---

## 11. Conclusion

Ultra Limiter is an independent clean-room implementation. The evidence:

- Custom NVAPI struct definitions (DC and SK use official headers)
- Always-swallow hook strategy (DC conditionally forwards)
- Phase-locked grid algorithm (DC uses Reflex Sleep directly, SK uses scanline timing)
- FG-aware cadence stabilization (neither DC nor SK adapt pacing behavior for frame generation)
- 17+ unique features not found in either project
- Different code style, naming conventions, and error handling patterns
- Different VSync implementation (Present hook vs driver profiles)

The only similarities are inherent to the problem domain: all three hook the same NVAPI functions, use MinHook, and use ring buffers for marker timestamps. These are convergent solutions to identical technical requirements.
