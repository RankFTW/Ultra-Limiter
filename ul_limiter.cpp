// Ultra Limiter — Frame limiter implementation
// Clean-room from public API docs:
//   - NVAPI SDK (MIT): NvAPI_D3D_SetSleepMode params, NvAPI_D3D_Sleep
//   - Windows: GetModuleHandleW for DLL detection, QPC for timing,
//              EnumDisplaySettings / MonitorFromWindow for refresh rate
// No code from Display Commander or any other project.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ul_limiter.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

// ============================================================================
// GpuPredictor — trend-aware GPU time prediction for predictive sleep
// ============================================================================

void GpuPredictor::Update(float gpu_active_us) {
    if (gpu_active_us <= 0.0f || gpu_active_us > 200'000.0f) return;

    // Detect trend direction before inserting the new sample
    if (count >= 2) {
        int prev_idx = (head - 1 + kHistorySize) % kHistorySize;
        float prev = history[prev_idx];
        float delta = gpu_active_us - prev;
        // Threshold: ignore changes smaller than 2% of the previous value
        float threshold = prev * 0.02f;

        int dir = 0;
        if (delta > threshold) dir = 1;       // rising
        else if (delta < -threshold) dir = -1; // falling

        if (dir == trend_dir && dir != 0)
            trend_len++;
        else {
            trend_dir = dir;
            trend_len = (dir != 0) ? 1 : 0;
        }
    }

    // Insert sample
    history[head] = gpu_active_us;
    head = (head + 1) % kHistorySize;
    if (count < kHistorySize) count++;

    // Compute prediction: weighted average biased toward recent frames
    // Weights: most recent = 4, second = 3, third = 2, rest = 1
    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + kHistorySize) % kHistorySize;
        float w = (i == 0) ? 4.0f : (i == 1) ? 3.0f : (i == 2) ? 2.0f : 1.0f;
        weighted_sum += history[idx] * w;
        weight_total += w;
    }
    predicted_us = weighted_sum / weight_total;

    // Trend bias: if 3+ frames are trending up, add the average per-frame
    // increase to the prediction. This anticipates continued rise (camera
    // panning into heavier geometry, particle effects ramping up).
    // We DON'T bias downward for falling trends — being conservative on
    // the way down is free (just slightly more GPU idle time), while being
    // wrong on the way up causes a missed frame.
    if (trend_dir > 0 && trend_len >= kTrendThreshold && count >= 3) {
        // Average per-frame increase over the trend window
        int oldest_idx = (head - 1 - std::min(trend_len, count - 1) + kHistorySize) % kHistorySize;
        int newest_idx = (head - 1 + kHistorySize) % kHistorySize;
        float rise = history[newest_idx] - history[oldest_idx];
        float per_frame = rise / static_cast<float>(std::min(trend_len, count - 1));
        // Add one frame's worth of predicted rise
        if (per_frame > 0.0f)
            predicted_us += per_frame;
    }
}

void GpuPredictor::RecordMiss(bool missed) {
    if (missed) {
        miss_count++;
        frames_since_miss = 0;
        // Widen safety margin by 50µs per miss
        safety_margin_us = std::min(safety_margin_us + 50.0f, kMaxMargin);
    } else {
        frames_since_miss++;
        // Tighten by 10µs every 30 stable frames
        if (frames_since_miss >= 30) {
            safety_margin_us = std::max(safety_margin_us - 10.0f, kMinMargin);
            frames_since_miss = 0;
        }
    }
}

void GpuPredictor::Reset() {
    head = 0;
    count = 0;
    trend_dir = 0;
    trend_len = 0;
    predicted_us = 0.0f;
    active = false;
    safety_margin_us = 500.0f;
    frames_since_miss = 0;
    miss_count = 0;
}

// ============================================================================
// Smooth Motion detection
// ============================================================================

// NVIDIA Smooth Motion is driver-level frame interpolation via NvPresent64.dll.
// When active, Reflex Sleep conflicts with the driver's presentation timing,
// causing low FPS. We detect it and fall back to timing-only pacing.
static std::atomic<bool> s_smooth_motion{false};
static int64_t s_sm_check_qpc = 0;

static bool DetectSmoothMotion() {
    return GetModuleHandleW(L"NvPresent64.dll") != nullptr;
}

// ============================================================================
// VRR / GSync ceiling
// ============================================================================

// On VRR displays the driver enforces an upper-bound frame rate derived from
// the panel's native refresh rate:  ceil = hz - hz^2 / 3600.
// If VSync is forced on (vsync_override == 1) and the monitor looks like a
// high-refresh VRR panel, we clamp minimumIntervalUs so the limiter never
// requests a rate above the VRR ceiling. Without this the driver may drop
// out of the VRR window and introduce judder.
static NvU32 ComputeVrrFloorIntervalUs(HWND hwnd) {
    // Cache the result — EnumDisplaySettingsA hits the display driver and
    // shouldn't be called every frame. Refresh every 2 seconds.
    static NvU32 s_cached = 0;
    static int64_t s_last_check_qpc = 0;
    static int s_last_vsync_override = -1;

    int ovr = g_cfg.vsync_override.load(std::memory_order_relaxed);
    int64_t now_qpc = ul_timing::NowQpc();

    bool stale = (now_qpc - s_last_check_qpc > ul_timing::g_qpc_freq * 2)
              || (ovr != s_last_vsync_override);

    if (!stale) return s_cached;
    s_last_check_qpc = now_qpc;
    s_last_vsync_override = ovr;

    // VSync forced off — no VRR concern
    if (ovr == 2) { s_cached = 0; return 0; }

    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) { s_cached = 0; return 0; }

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hm) { s_cached = 0; return 0; }

    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, &mi)) { s_cached = 0; return 0; }

    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) { s_cached = 0; return 0; }

    double hz = static_cast<double>(dm.dmDisplayFrequency);
    if (hz < 30.0) { s_cached = 0; return 0; }

    double ceiling_fps = 3600.0 * hz / (hz + 3600.0);
    if (ceiling_fps < 10.0) { s_cached = 0; return 0; }

    ceiling_fps *= 0.995;

    s_cached = static_cast<NvU32>(std::round(1'000'000.0 / ceiling_fps));
    return s_cached;
}

// ============================================================================
// FG pacing offset
// ============================================================================

// When frame generation is active the interpolation pipeline adds scheduling
// overhead that causes the driver to slightly overshoot the requested interval.
// We compensate with a µs bump so the driver lands closer to the target.
//
// SK uses +6 in its SetSleepMode passthrough (game-driven sleep) and +24 in
// its enforcement site (SK-driven sleep).  Since UL always drives sleep calls
// itself (we swallow the game's Sleep), we use the enforcement-site value.
// After the -2 driver shave this nets +22 µs effective offset.
//
// This is the static fallback — when adaptive FG offset (#5) has enough data,
// it replaces this with a measured value derived from GetLatency reports.
static constexpr NvU32 kFGPacingOffsetUs = 24;

// ============================================================================
// FG detection
// ============================================================================

int UlLimiter::DetectFGDivisor() const {
    FGMultiplier m = g_cfg.fg_mult.load(std::memory_order_relaxed);
    switch (m) {
        case FGMultiplier::Off: return 1;
        case FGMultiplier::X2:  return 2;
        case FGMultiplier::X3:  return 3;
        case FGMultiplier::X4:  return 4;
        case FGMultiplier::Auto:
            // Check all known DLSS Frame Generation DLL variants
            if (GetModuleHandleW(L"nvngx_dlssg.dll"))  return 2;
            if (GetModuleHandleW(L"_nvngx_dlssg.dll")) return 2;
            if (GetModuleHandleW(L"sl.dlss_g.dll"))    return 2;
            if (GetModuleHandleW(L"dlss-g.dll"))       return 2;
            // FSR Frame Generation
            if (GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll")) return 2;
            if (GetModuleHandleW(L"ffx_framegeneration.dll"))            return 2;
            return 1;
    }
    return 1;
}

float UlLimiter::ComputeRenderFps() const {
    float target = g_cfg.fps_limit.load(std::memory_order_relaxed);
    if (target <= 0.0f) return 0.0f;
    int div = DetectFGDivisor();
    return (div > 1) ? target / static_cast<float>(div) : target;
}

// ============================================================================
// Lifecycle
// ============================================================================

void UlLimiter::Init() {
    // Allocate latency buffer on heap — NvLatencyResult is ~10KB,
    // too large for the stack in a hot path.
    if (!latency_buf_) {
        latency_buf_ = new (std::nothrow) NvLatencyResult{};
    }
}

void UlLimiter::Shutdown() {
    // Restore game's original sleep mode before unhooking.
    // If the game set Reflex params before we hooked (captured in Hook_SetSleepMode),
    // restore those. Otherwise disable Reflex cleanly.
    if (dev_ && ReflexActive()) {
        const auto& gs = GetGameState();
        NvSleepParams p = {};
        p.version = NV_SLEEP_PARAMS_VER;
        if (gs.captured.load(std::memory_order_acquire)) {
            p.bLowLatencyMode = gs.low_latency.load(std::memory_order_relaxed) ? 1 : 0;
            p.bLowLatencyBoost = gs.boost.load(std::memory_order_relaxed) ? 1 : 0;
            p.bUseMarkersToOptimize = gs.use_markers.load(std::memory_order_relaxed) ? 1 : 0;
            // minimumIntervalUs = 0 — let the game re-set its own limit
        }
        InvokeSetSleepMode(dev_, &p);
    }
    TeardownReflexHooks();
    dev_ = nullptr;
    last_sleep_valid_ = false;

    if (htimer_delay_) { CloseHandle(htimer_delay_); htimer_delay_ = nullptr; }
    if (htimer_fallback_) { CloseHandle(htimer_fallback_); htimer_fallback_ = nullptr; }
    if (htimer_queue_) { CloseHandle(htimer_queue_); htimer_queue_ = nullptr; }

    delete latency_buf_;
    latency_buf_ = nullptr;
}

bool UlLimiter::ConnectReflex(IUnknown* device) {
    if (!device) { ul_log::Write("ConnectReflex: null device"); return false; }
    dev_ = device;
    if (!SetupReflexHooks()) {
        ul_log::Write("ConnectReflex: hook setup failed — timing fallback only");
        return false;
    }
    ul_log::Write("ConnectReflex: hooks active");
    return true;
}

// ============================================================================
// Interval adjustment — VRR floor, FG offset, driver shave
// ============================================================================

// Takes a raw minimumIntervalUs and applies corrections:
//  1. VRR floor: clamp so we don't exceed the GSync ceiling
//  2. FG offset: add a µs bump when frame gen is active (adaptive or static)
//  3. Queue pressure: add a small bump when the render queue is backing up
//  4. Adaptive GPU headroom: shave µs when GPU has headroom, add when tight
//  5. P2P feedback: closed-loop correction from measured present cadence
//  6. Driver shave: subtract 2µs so the driver doesn't overshoot
static NvU32 AdjustIntervalUs(NvU32 raw, int fg_divisor, HWND hwnd,
                              const GpuStats& gs, int32_t ptp_correction_us) {
    if (raw == 0) return 0;

    // 1. VRR floor — ensure we don't request faster than the VRR ceiling
    NvU32 vrr_floor = ComputeVrrFloorIntervalUs(hwnd);
    if (vrr_floor > 0 && raw < vrr_floor)
        raw = vrr_floor;

    // 2. FG pacing offset — adaptive if we have data, otherwise static
    if (fg_divisor > 1) {
        if (gs.valid && gs.adaptive_fg_offset_us >= 0)
            raw += static_cast<NvU32>(gs.adaptive_fg_offset_us);
        else
            raw += kFGPacingOffsetUs;
    }

    // 3. Queue pressure — when the OS render queue is growing, add a small
    //    bump to proactively throttle and prevent latency spikes.
    if (gs.valid && gs.queue_pressure && raw > 100)
        raw += 4;

    // 4. Adaptive GPU headroom — when GPU is well under budget, shave a
    //    couple µs for tighter pacing. When GPU is near budget, add a
    //    small buffer to avoid frame drops.
    if (gs.valid && gs.interval_adjust_us != 0) {
        int32_t adjusted = static_cast<int32_t>(raw) + gs.interval_adjust_us;
        if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
    }

    // 5. Present-to-present feedback correction — closed-loop nudge based on
    //    measured present cadence error. Positive = frames landing late, shave.
    //    Negative = frames landing early, add. Clamped to [-8, +8] µs.
    if (ptp_correction_us != 0) {
        int32_t adjusted = static_cast<int32_t>(raw) + ptp_correction_us;
        if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
    }

    // 6. Driver shave — the driver tends to overshoot by a couple µs;
    //    trimming the request helps land closer to the target.
    if (raw > 50)
        raw -= 2;

    return raw;
}

// ============================================================================
// Sleep param helpers — build + changed-params optimization
// ============================================================================

// Build the NvSleepParams struct from current config and game state.
// Centralizes the logic so DoReflexSleep and OnPresent don't duplicate it.
NvSleepParams UlLimiter::BuildSleepParams() const {
    float render_fps = ComputeRenderFps();
    const auto& gs = GetGameState();

    NvSleepParams p = {};
    p.version = NV_SLEEP_PARAMS_VER;
    p.bLowLatencyMode = 1;

    // Boost override
    BoostMode bm = g_cfg.boost.load(std::memory_order_relaxed);
    if (bm == BoostMode::ForceOn)
        p.bLowLatencyBoost = 1;
    else if (bm == BoostMode::ForceOff)
        p.bLowLatencyBoost = 0;
    else if (gs.captured.load(std::memory_order_acquire))
        p.bLowLatencyBoost = gs.boost.load(std::memory_order_relaxed) ? 1 : 0;

    // Preserve game's marker optimization flag
    if (gs.captured.load(std::memory_order_acquire))
        p.bUseMarkersToOptimize = gs.use_markers.load(std::memory_order_relaxed) ? 1 : 0;

    // Frame rate interval with VRR / FG / driver / adaptive corrections
    NvU32 raw_interval = (render_fps > 0.0f)
        ? static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(render_fps)))
        : 0;

    // --- PREDICTIVE SLEEP ---
    // When the GPU predictor is active (GPU-bound with enough data), compute
    // a tighter interval based on predicted GPU time + safety margin instead
    // of the static fps_limit-derived interval. This maximizes GPU utilization
    // by waking the CPU closer to when the GPU will actually finish.
    //
    // The predictive interval can only TIGHTEN (reduce) the base interval —
    // we're a limiter, not an accelerator. If the GPU is faster than the
    // fps_limit allows, we still cap at the fps_limit interval.
    if (raw_interval > 0 && gpu_predictor_.active) {
        NvU32 predictive_interval = static_cast<NvU32>(
            std::round(gpu_predictor_.predicted_us + gpu_predictor_.safety_margin_us));
        // Only use predictive interval if it's tighter than the base
        if (predictive_interval < raw_interval)
            raw_interval = predictive_interval;
    }

    p.minimumIntervalUs = AdjustIntervalUs(raw_interval, DetectFGDivisor(), hwnd_,
                                            gpu_stats_, ptp_correction_us_);

    return p;
}

// Only call SetSleepMode when params actually changed.
// SK does this to avoid unnecessary driver overhead — despite what NVAPI docs
// say about it being cheap, many Streamline games call it every frame and
// the driver does non-trivial work each time.
bool UlLimiter::MaybeUpdateSleepMode(const NvSleepParams& p) {
    if (last_sleep_valid_ &&
        last_sleep_params_.bLowLatencyMode == p.bLowLatencyMode &&
        last_sleep_params_.bLowLatencyBoost == p.bLowLatencyBoost &&
        last_sleep_params_.bUseMarkersToOptimize == p.bUseMarkersToOptimize &&
        last_sleep_params_.minimumIntervalUs == p.minimumIntervalUs) {
        return false;  // no change
    }

    NvSleepParams copy = p;
    NvStatus st = InvokeSetSleepMode(dev_, &copy);
    if (st == NV_OK) {
        last_sleep_params_ = p;
        last_sleep_valid_ = true;
    }
    return (st == NV_OK);
}

// ============================================================================
// GPU stats from GetLatency — adaptive interval, enforcement site, queue, FG
// ============================================================================

void UlLimiter::UpdateGpuStats() {
    if (!ReflexActive() || !dev_ || !latency_buf_) return;

    memset(latency_buf_, 0, sizeof(*latency_buf_));
    latency_buf_->version = NV_LATENCY_RESULT_VER;

    // SEH guard — GetLatency can fault on some driver versions
    NvStatus st;
    __try { st = InvokeGetLatency(dev_, latency_buf_); }
    __except(EXCEPTION_EXECUTE_HANDLER) { st = NV_NO_IMPL; }
    if (st != NV_OK) return;

    // Scan the most recent valid reports (up to 16)
    float sum_gpu = 0, sum_queue = 0, sum_fg = 0;
    int n_gpu = 0, n_queue = 0, n_fg = 0;
    float target_interval_us = 0.0f;
    {
        float rfps = ComputeRenderFps();
        if (rfps > 0.0f) target_interval_us = 1'000'000.0f / rfps;
    }

    bool fg_active = (DetectFGDivisor() > 1);

    for (int i = 63; i >= 0 && n_gpu < 16; i--) {
        auto& f = latency_buf_->frameReport[i];
        if (!f.frameID) continue;

        // GPU active render time
        if (f.gpuActiveRenderTimeUs > 0 && f.gpuActiveRenderTimeUs < 200'000) {
            sum_gpu += static_cast<float>(f.gpuActiveRenderTimeUs);
            n_gpu++;
        }

        // OS render queue time (how long frames wait before GPU picks them up)
        if (f.osRenderQueueStartTime > 0 && f.osRenderQueueEndTime > f.osRenderQueueStartTime) {
            float q_us = static_cast<float>(f.osRenderQueueEndTime - f.osRenderQueueStartTime);
            if (q_us < 500'000.0f) { sum_queue += q_us; n_queue++; }
        }

        // FG overhead: gap between GPU render end and present end
        // When FG is active, this captures the interpolation scheduling overhead.
        if (fg_active && f.gpuRenderEndTime > 0 && f.presentEndTime > f.gpuRenderEndTime) {
            float fg_us = static_cast<float>(f.presentEndTime - f.gpuRenderEndTime);
            if (fg_us > 0.0f && fg_us < 200'000.0f) { sum_fg += fg_us; n_fg++; }
        }
    }

    // Update EMAs
    auto& gs = gpu_stats_;
    constexpr float a = GpuStats::kEmaAlpha;

    if (n_gpu > 0) {
        float cur_gpu = sum_gpu / static_cast<float>(n_gpu);
        gs.avg_gpu_active_us = gs.valid ? gs.avg_gpu_active_us * (1.0f - a) + cur_gpu * a : cur_gpu;
    }
    if (n_queue > 0) {
        float cur_q = sum_queue / static_cast<float>(n_queue);
        gs.avg_queue_time_us = gs.valid ? gs.avg_queue_time_us * (1.0f - a) + cur_q * a : cur_q;
    }
    if (n_fg > 0) {
        float cur_fg = sum_fg / static_cast<float>(n_fg);
        gs.avg_fg_overhead_us = gs.valid ? gs.avg_fg_overhead_us * (1.0f - a) + cur_fg * a : cur_fg;
    }

    gs.samples += n_gpu;
    if (gs.samples < GpuStats::kMinSamples) return;
    gs.valid = true;

    // --- Feature #1: Adaptive interval adjustment ---
    // When GPU has headroom (load < 0.7), shave up to 3µs for tighter pacing.
    // When GPU is near budget (load > 0.9), add up to 4µs buffer.
    // In between: no adjustment.
    if (target_interval_us > 0.0f) {
        gs.gpu_load_ratio = gs.avg_gpu_active_us / target_interval_us;

        if (gs.gpu_load_ratio < 0.70f)
            gs.interval_adjust_us = -3;  // GPU has headroom, tighten
        else if (gs.gpu_load_ratio > 0.90f)
            gs.interval_adjust_us = static_cast<int32_t>(
                std::min(4.0f, (gs.gpu_load_ratio - 0.90f) * 40.0f));  // scale 0..4
        else
            gs.interval_adjust_us = 0;
    }

    // --- Feature #3: Auto enforcement site ---
    // GPU-bound (ratio > 0.85) → SIM_START for lowest latency
    // CPU-bound (ratio < 0.65) → PRESENT_FINISH for best frame pacing
    // Hysteresis band prevents flip-flopping
    if (gs.gpu_load_ratio > 0.85f)
        gs.auto_site = SIM_START;
    else if (gs.gpu_load_ratio < 0.65f)
        gs.auto_site = PRESENT_FINISH;
    // else: keep current value (hysteresis)

    // --- Feature #4: Render queue depth monitoring ---
    // If the average queue time exceeds 1.5x the target interval, the pipeline
    // is backing up. Flag queue_pressure so AdjustIntervalUs adds a small bump.
    if (target_interval_us > 0.0f)
        gs.queue_pressure = (gs.avg_queue_time_us > target_interval_us * 1.5f);
    else
        gs.queue_pressure = false;

    // --- Feature #5: Adaptive DLSSG pacing offset ---
    // Replace the static +24µs with the measured FG overhead.
    // Clamp to [4, 60] µs to avoid insane values from bad data.
    if (fg_active && n_fg > 0) {
        int32_t measured = static_cast<int32_t>(std::round(gs.avg_fg_overhead_us));
        measured = std::clamp(measured, 4, 60);
        gs.adaptive_fg_offset_us = measured;
    } else if (!fg_active) {
        gs.adaptive_fg_offset_us = -1;  // not applicable
    }

    // --- Feature #6: Predictive sleep — GPU-aware wake scheduling ---
    // Feed per-frame GPU times to the predictor and track prediction accuracy.
    // The predictor is only active when GPU-bound (auto_site == SIM_START),
    // because when CPU-bound the GPU isn't the bottleneck and predictive
    // sleep wouldn't help.
    {
        auto& pred = gpu_predictor_;

        // Feed individual frame GPU times to the predictor, but only NEW
        // reports we haven't seen before. Since we poll every 2 frames,
        // most reports in the 64-slot ring buffer are repeats. We track
        // the highest frameID we've processed to skip duplicates.
        float recent_gpu[16] = {};
        uint64_t recent_ids[16] = {};
        int n_recent = 0;
        for (int i = 63; i >= 0 && n_recent < 16; i--) {
            auto& f = latency_buf_->frameReport[i];
            if (!f.frameID) continue;
            // Skip reports we already fed to the predictor
            if (f.frameID <= last_pred_frame_id_) continue;
            if (f.gpuActiveRenderTimeUs > 0 && f.gpuActiveRenderTimeUs < 200'000) {
                recent_gpu[n_recent] = static_cast<float>(f.gpuActiveRenderTimeUs);
                recent_ids[n_recent] = f.frameID;
                n_recent++;
            }
        }

        // Feed in chronological order (oldest first) so trend detection works.
        // The ring buffer isn't necessarily sorted by frameID (it's circular),
        // so we sort our collected samples by frameID ascending.
        for (int i = 0; i < n_recent - 1; i++) {
            for (int j = i + 1; j < n_recent; j++) {
                if (recent_ids[j] < recent_ids[i]) {
                    std::swap(recent_gpu[i], recent_gpu[j]);
                    std::swap(recent_ids[i], recent_ids[j]);
                }
            }
        }
        for (int i = 0; i < n_recent; i++)
            pred.Update(recent_gpu[i]);

        // Update the high-water mark
        if (n_recent > 0)
            last_pred_frame_id_ = recent_ids[n_recent - 1];  // last = highest after sort

        // Check for prediction misses: was the most recent actual GPU time
        // significantly above what we predicted + safety margin?
        if (n_recent > 0 && pred.predicted_us > 0.0f) {
            bool missed = recent_gpu[n_recent - 1] > (pred.predicted_us + pred.safety_margin_us);
            pred.RecordMiss(missed);
        }

        // Activate predictive sleep only when GPU-bound and we have enough data
        pred.active = (gs.auto_site == SIM_START) && (pred.count >= 4)
                   && (pred.predicted_us > 0.0f) && (target_interval_us > 0.0f);
    }
}

// Determine where to call Reflex Sleep based on GPU load.
// Purely dynamic — no preset override. The auto-detection in UpdateGpuStats
// picks SIM_START when GPU-bound and PRESENT_FINISH when CPU-bound.
int UlLimiter::ResolveEnforcementSite() const {
    if (gpu_stats_.valid)
        return gpu_stats_.auto_site;

    // Default before we have data: PRESENT_FINISH (safest for frame pacing)
    return PRESENT_FINISH;
}

// ============================================================================
// Reflex Sleep pacing — configure SetSleepMode then call Sleep
// ============================================================================

void UlLimiter::DoReflexSleep() {
    if (!ReflexActive() || !dev_) return;

    NvSleepParams p = BuildSleepParams();
    MaybeUpdateSleepMode(p);
    InvokeSleep(dev_);
}

// ============================================================================
// Delay PRESENT_BEGIN after SIM_START
// ============================================================================

void UlLimiter::HandleDelayPresent(uint64_t frame_id) {
    ExpandedSettings es = ExpandPreset();
    if (!es.delay_present) return;

    float delay_amt = g_cfg.delay_present_amount.load(std::memory_order_relaxed);
    if (delay_amt <= 0.0f) return;

    size_t slot = static_cast<size_t>(frame_id % kRingSize);
    int64_t sim_ns = g_ring[slot].timestamp_ns[SIM_START].load(std::memory_order_relaxed);
    if (sim_ns <= 0) return;

    float fps = ComputeRenderFps();
    if (fps <= 0.0f) return;

    int64_t frame_ns = static_cast<int64_t>(1e9 / static_cast<double>(fps));
    if (frame_ns < 1) frame_ns = 1;

    int64_t target = sim_ns + static_cast<int64_t>(delay_amt * static_cast<float>(frame_ns));
    if (target > ul_timing::NowNs())
        ul_timing::SleepUntilNs(target, htimer_delay_);
}

// ============================================================================
// Wait for N-back frame to finish rendering
// ============================================================================

void UlLimiter::HandleQueuedFrames(uint64_t frame_id, int max_q) {
    if (max_q <= 0 || frame_id < static_cast<uint64_t>(max_q)) return;

    size_t prev = static_cast<size_t>((frame_id + kRingSize - max_q) % kRingSize);
    size_t prev_m1 = (prev - 1 + kRingSize) % kRingSize;

    // Check if the frame before was actually rendered
    bool rendered = g_ring[prev_m1].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
                  < g_ring[prev_m1].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed);
    if (!rendered) return;

    if (g_ring[prev].frame_id.load(std::memory_order_relaxed) != frame_id - max_q) return;

    // Hybrid wait: use a short waitable timer sleep to avoid burning a full
    // CPU core, then busy-wait for the final stretch. Neither DC nor SK do
    // this — they both pure-spinlock which wastes power and steals cycles
    // from the render thread.
    int64_t start = ul_timing::NowNs();
    constexpr int64_t kTimeout = 50'000'000LL;  // 50ms

    auto still_waiting = [&]() {
        return g_ring[prev].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
             > g_ring[prev].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed);
    };

    // Phase 1: yield via short timer sleeps (~0.5ms each) to save CPU
    while (still_waiting()) {
        if (ul_timing::NowNs() - start > kTimeout) return;
        int64_t remaining = kTimeout - (ul_timing::NowNs() - start);
        if (remaining < 1'000'000LL) break;  // < 1ms left, switch to busy-wait
        // Sleep for ~0.5ms using the waitable timer
        int64_t wake = ul_timing::NowNs() + 500'000LL;  // 0.5ms
        ul_timing::SleepUntilNs(wake, htimer_queue_);
    }

    // Phase 2: busy-wait for the final sub-millisecond
    while (still_waiting()) {
        if (ul_timing::NowNs() - start > kTimeout) break;
        YieldProcessor();
    }
}

// ============================================================================
// Marker-based pacing — called from SetLatencyMarker detour
// ============================================================================

void UlLimiter::OnMarker(int marker_type, uint64_t frame_id) {
    if (!ReflexActive() || !dev_) return;
    if (!warmup_done_) return;

    // When Smooth Motion is active, all pacing is handled by DoTimingFallback
    // in OnPresent. Reflex Sleep in marker callbacks would conflict with the
    // driver's frame interpolation.
    if (s_smooth_motion.load(std::memory_order_relaxed)) return;

    ExpandedSettings es = ExpandPreset();
    if (!es.use_marker_pacing) return;

    // Resolve enforcement site: auto-detected from GPU load
    int site = ResolveEnforcementSite();

    // Only pace on real (non-generated) frames.
    // Generated frames (from DLSS FG / smooth motion) fire PRESENT markers
    // but never SIM_START. Check the ring buffer to see if this frame has a
    // SIM_START timestamp — if not, it's a generated frame and we skip pacing.
    size_t slot = static_cast<size_t>(frame_id % kRingSize);
    bool is_real_frame = (g_ring[slot].frame_id.load(std::memory_order_relaxed) == frame_id)
                      && (g_ring[slot].timestamp_ns[SIM_START].load(std::memory_order_relaxed) > 0);
    if (!is_real_frame) return;

    // Queue wait + delay present always fire on PRESENT_BEGIN regardless of
    // enforcement site. This ensures the queue depth is enforced before any
    // sleep, whether sleep fires at SIM_START or PRESENT_FINISH.
    if (marker_type == PRESENT_BEGIN) {
        HandleQueuedFrames(frame_id, es.max_queued_frames);
        HandleDelayPresent(frame_id);
    }

    // When site is SIM_START and we have queued frames, we need sleep to
    // happen AFTER the queue wait. Since queue wait fires on PRESENT_BEGIN
    // (which comes after SIM_START in the marker sequence), defer sleep
    // to PRESENT_BEGIN as well so the ordering is: queue wait → sleep.
    int effective_site = site;
    if (site == SIM_START && es.max_queued_frames > 0)
        effective_site = PRESENT_BEGIN;

    if (marker_type == effective_site) {
        DoReflexSleep();
    }
}

// ============================================================================
// Timing fallback — QPC-based hard backstop
// ============================================================================

void UlLimiter::DoTimingFallback() {
    // Use raw fps_limit (no FG division) — this is the output FPS cap
    float target = g_cfg.fps_limit.load(std::memory_order_relaxed);
    if (target <= 0.0f) return;

    int64_t frame_ns = static_cast<int64_t>(1e9 / static_cast<double>(target));

    // Reset grid when the user changes the FPS limit
    if (target != last_fps_limit_) {
        grid_epoch_ns_ = 0;
        grid_next_ns_ = 0;
        grid_interval_ns_ = 0;
        // Also reset feedback loop — old correction is for the old target
        ptp_error_accum_us_ = 0.0f;
        ptp_correction_us_ = 0;
        ptp_sample_count_ = 0;
        last_ptp_present_ns_ = 0;
        // Reset predictive sleep — old GPU time history is for the old target
        gpu_predictor_.Reset();
        last_pred_frame_id_ = 0;
        last_fps_limit_ = target;
    }

    int64_t now = ul_timing::NowNs();

    // ----------------------------------------------------------------
    // Phase-locked timing grid.
    //
    // Instead of the naive `next_target += interval` (which drifts because
    // each target is relative to the previous), we lock to a fixed grid:
    //   target[k] = epoch + k * interval
    //
    // Benefits:
    //   - If frame N lands 30µs early, frame N+1's target doesn't shift —
    //     it stays on the grid. No accumulated phase drift.
    //   - If a frame is late, we snap to the next grid slot ahead of `now`
    //     without accumulating debt or causing burst catch-up.
    //   - When Reflex Sleep already paced the frame, `now` is near the grid
    //     slot and SleepUntilNs returns immediately — same as before.
    //
    // The grid uses the fixed interval from fps_limit, not an EMA. The old
    // EMA approach was needed to dampen jitter in the drifting `next += dt`
    // scheme. The grid IS the smoothing — fixed spacing eliminates drift.
    // ----------------------------------------------------------------

    if (grid_epoch_ns_ == 0) {
        // First frame — seed the grid.
        // Align epoch to `now` so the first sleep is one full interval away.
        grid_epoch_ns_ = now;
        grid_interval_ns_ = frame_ns;
        grid_next_ns_ = now + frame_ns;
        return;
    }

    // Update interval if it changed (shouldn't happen without fps_limit change,
    // but guard against floating point edge cases)
    grid_interval_ns_ = frame_ns;

    // Compute the next grid slot at or after `now`.
    // grid_next_ns_ is the slot we're aiming for. If we're past it (late frame
    // or Reflex already consumed the budget), advance to the next slot ahead.
    if (grid_next_ns_ <= now) {
        // Frame arrived on time or late — no fallback sleep needed.
        // Snap the grid forward so the next frame has a valid target.
        int64_t elapsed = now - grid_epoch_ns_;
        int64_t slots_elapsed = elapsed / grid_interval_ns_;
        grid_next_ns_ = grid_epoch_ns_ + (slots_elapsed + 1) * grid_interval_ns_;
        return;  // already at or past the target — don't add extra sleep
    }

    // Frame arrived early (before the grid slot) — sleep the remainder.
    // This is the actual limiting: the game/Reflex finished early, and we
    // hold until the grid slot to maintain consistent cadence.
    ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);

    // Advance to the next slot for the next frame
    grid_next_ns_ += grid_interval_ns_;
}

// ============================================================================
// OnPresent — main entry from ReShade present callback
// ============================================================================

void UlLimiter::OnPresent() {
    frame_num_++;
    g_present_count.fetch_add(1, std::memory_order_relaxed);

    // Time-based warmup: 2 seconds from first present.
    // Consistent across frame rates — 300 frames was 10s at 30fps but 1.25s at 240fps.
    int64_t now_qpc = ul_timing::NowQpc();
    if (!warmup_done_) {
        if (warmup_start_qpc_ == 0) {
            warmup_start_qpc_ = now_qpc;
            ul_log::Write("OnPresent: warmup (%lld seconds)", kWarmupDurationSec);
            return;
        }
        if (now_qpc - warmup_start_qpc_ < ul_timing::g_qpc_freq * kWarmupDurationSec)
            return;
        warmup_done_ = true;
        ul_log::Write("OnPresent: warmup done, limiting active (frame %llu)", frame_num_);
    }

    // Periodically check for Smooth Motion (driver-level FG)
    if (now_qpc - s_sm_check_qpc > ul_timing::g_qpc_freq * 2) {
        bool prev = s_smooth_motion.load(std::memory_order_relaxed);
        bool cur = DetectSmoothMotion();
        if (cur != prev) {
            s_smooth_motion.store(cur, std::memory_order_relaxed);
            ul_log::Write("Smooth Motion: %s", cur ? "detected" : "not detected");
        }
        s_sm_check_qpc = now_qpc;
    }

    // Poll GetLatency for adaptive features every 2 frames.
    // Predictive sleep needs fresh GPU times to track trends — polling every
    // ~1 second was too slow to react to scene changes. GetLatency just reads
    // a driver ring buffer, so the overhead is negligible.
    if (frame_num_ % 2 == 0) {
        int prev_site = gpu_stats_.auto_site;
        UpdateGpuStats();
        // Log enforcement site changes
        if (gpu_stats_.valid && gpu_stats_.auto_site != prev_site) {
            ul_log::Write("Auto enforcement site: %s (GPU load %.0f%%)",
                          gpu_stats_.auto_site == SIM_START ? "SIM_START" : "PRESENT_FINISH",
                          gpu_stats_.gpu_load_ratio * 100.0f);
        }
    }

    // When Smooth Motion is active, Reflex Sleep conflicts with the driver's
    // frame interpolation timing. Use timing-only pacing instead.
    if (s_smooth_motion.load(std::memory_order_relaxed)) {
        DoTimingFallback();
        return;
    }

    // ----------------------------------------------------------------
    // Present-to-present feedback loop.
    //
    // Measures the actual present cadence and compares it to the target.
    // If frames consistently land late (driver overshooting the interval),
    // we shave minimumIntervalUs. If early, we add. This closes the loop
    // that DC and SK leave open — they set the interval and hope.
    //
    // Design:
    //   - Accumulate signed error (actual_us - target_us) over 30 frames
    //   - Every 30 frames, compute average error and nudge correction ±1µs
    //   - Clamp total correction to [-8, +8] µs
    //   - Only active when Reflex is driving sleep (not timing-only mode)
    //   - Reset on FPS limit change (handled in DoTimingFallback)
    // ----------------------------------------------------------------
    // Only run feedback when FG is not active — with FG, the relationship
    // between minimumIntervalUs and actual present cadence is non-linear
    // (FG interpolates between renders), so P2P error doesn't map cleanly
    // to an interval correction. The adaptive FG offset handles FG pacing.
    if (ReflexActive() && dev_ && DetectFGDivisor() == 1) {
        float out_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
        if (out_fps > 0.0f) {
            float target_us = 1'000'000.0f / out_fps;
            int64_t now_ns = ul_timing::NowNs();

            if (last_ptp_present_ns_ > 0) {
                float actual_us = static_cast<float>(now_ns - last_ptp_present_ns_) / 1000.0f;
                // Only accumulate when the frame time is sane (within 3x target)
                if (actual_us > 0.0f && actual_us < target_us * 3.0f) {
                    ptp_error_accum_us_ += (actual_us - target_us);
                    ptp_sample_count_++;
                }
            }
            last_ptp_present_ns_ = now_ns;

            // Every 30 frames, update the correction
            constexpr int kPtpUpdateInterval = 30;
            if (ptp_sample_count_ >= kPtpUpdateInterval) {
                float avg_error = ptp_error_accum_us_ / static_cast<float>(ptp_sample_count_);
                // Nudge: if avg error > +1µs (landing late), shave 1µs from interval.
                //         if avg error < -1µs (landing early), add 1µs.
                //         Dead zone ±1µs to avoid chasing noise.
                if (avg_error > 1.0f)
                    ptp_correction_us_ = std::max(ptp_correction_us_ - 1, -8);
                else if (avg_error < -1.0f)
                    ptp_correction_us_ = std::min(ptp_correction_us_ + 1, 8);
                // else: within dead zone, no change

                ptp_error_accum_us_ = 0.0f;
                ptp_sample_count_ = 0;
            }
        }
    }

    // Reflex path
    if (ReflexActive() && dev_) {
        ExpandedSettings es = ExpandPreset();
        if (es.use_marker_pacing && g_game_uses_reflex.load(std::memory_order_relaxed)) {
            // Marker pacing handles sleep in OnMarker — only update sleep mode params here.
            // Do NOT call InvokeSleep, that would double-pace.
            NvSleepParams p = BuildSleepParams();
            MaybeUpdateSleepMode(p);
            // Fall through to timing fallback as a hard backstop.
            // Reflex Sleep is a hint — some DX11 drivers don't enforce the interval.
        } else {
            DoReflexSleep();
        }
    }

    // Timing fallback as hard backstop (always runs)
    DoTimingFallback();
}

// ============================================================================
// Streamline proxy present
// ============================================================================

void UlLimiter::OnSLPresent() {
    ExpandedSettings es = ExpandPreset();
    if (!es.use_sl_proxy || !ReflexActive() || !dev_) return;
    DoReflexSleep();
}
