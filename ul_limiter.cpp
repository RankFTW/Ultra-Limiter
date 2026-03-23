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
        float threshold = prev * 0.02f;

        int dir = 0;
        if (delta > threshold) dir = 1;
        else if (delta < -threshold) dir = -1;

        if (dir == trend_dir && dir != 0)
            trend_len++;
        else {
            trend_dir = dir;
            trend_len = (dir != 0) ? 1 : 0;
        }
    }

    history[head] = gpu_active_us;
    head = (head + 1) % kHistorySize;
    if (count < kHistorySize) count++;

    // Weighted average biased toward recent frames (4/3/2/1)
    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + kHistorySize) % kHistorySize;
        float w = (i == 0) ? 4.0f : (i == 1) ? 3.0f : (i == 2) ? 2.0f : 1.0f;
        weighted_sum += history[idx] * w;
        weight_total += w;
    }
    predicted_us = weighted_sum / weight_total;

    // Upward trend bias: anticipate continued rise when 3+ frames trending up
    if (trend_dir > 0 && trend_len >= kTrendThreshold && count >= 3) {
        int oldest_idx = (head - 1 - std::min(trend_len, count - 1) + kHistorySize) % kHistorySize;
        int newest_idx = (head - 1 + kHistorySize) % kHistorySize;
        float rise = history[newest_idx] - history[oldest_idx];
        float per_frame = rise / static_cast<float>(std::min(trend_len, count - 1));
        if (per_frame > 0.0f)
            predicted_us += per_frame;
    }
}

void GpuPredictor::RecordMiss(bool missed) {
    if (missed) {
        miss_count++;
        frames_since_miss = 0;
        safety_margin_us = std::min(safety_margin_us + 50.0f, kMaxMargin);
    } else {
        frames_since_miss++;
        if (frames_since_miss >= 30) {
            safety_margin_us = std::max(safety_margin_us - 10.0f, kMinMargin);
            frames_since_miss = 0;
        }
    }
}

// ============================================================================
// CadenceTracker — output cadence variance measurement (ported from v2)
// ============================================================================

void CadenceTracker::Feed(uint64_t present_end_time) {
    if (present_end_time == 0) return;

    if (last_present_time > 0 && present_end_time > last_present_time) {
        float delta_us = static_cast<float>(present_end_time - last_present_time);
        // Sanity: skip deltas that are clearly bogus (> 200ms or < 100µs)
        if (delta_us > 100.0f && delta_us < 200'000.0f) {
            deltas_us[head] = delta_us;
            head = (head + 1) % kWindowSize;
            if (count < kWindowSize) count++;
        }
    }
    last_present_time = present_end_time;
}

void CadenceTracker::ComputeStats() {
    if (count < 4) {
        variance_us2 = 0.0f;
        stddev_us = 0.0f;
        mean_delta_us = 0.0f;
        return;
    }

    // Compute mean
    float sum = 0.0f;
    for (int i = 0; i < count; i++)
        sum += deltas_us[(head - count + i + kWindowSize) % kWindowSize];
    mean_delta_us = sum / static_cast<float>(count);

    // Compute variance
    float var_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float d = deltas_us[(head - count + i + kWindowSize) % kWindowSize] - mean_delta_us;
        var_sum += d * d;
    }
    variance_us2 = var_sum / static_cast<float>(count);
    stddev_us = std::sqrt(variance_us2);
}

void CadenceTracker::Reset() {
    head = 0;
    count = 0;
    last_present_time = 0;
    last_fed_frame_id = 0;
    variance_us2 = 0.0f;
    stddev_us = 0.0f;
    mean_delta_us = 0.0f;
    best_interval_us = 0.0f;
    best_variance_us2 = 1e12f;
    stable_streak = 0;
    jitter_streak = 0;
}

// ============================================================================
// GpuPredictor — FG/MFG cadence response (ported from v2)
// ============================================================================
//
// Under FG: instead of tightening the interval (which fights the interpolation
// scheduler), we track output cadence variance and adjust the interval to
// minimize jitter.
//
// Under MFG (3x+): even more conservative — only back off when jittery,
// never tighten, freeze at the best-known interval.

void GpuPredictor::UpdateCadenceResponse(bool fg_active, int fg_divisor,
                                          float target_interval_us) {
    if (!fg_active || target_interval_us <= 0.0f) {
        fg_cadence_adjust_us = 0;
        return;
    }

    auto& ct = cadence;
    ct.ComputeStats();

    if (ct.count < 8) {
        fg_cadence_adjust_us = 0;
        return;
    }

    // Infer effective FG multiplier from measured output cadence vs render interval.
    // DetectFGDivisor() only returns 1 or 2 (DLL presence check). For the
    // actual runtime multiplier (2x vs 3x vs 4x), infer from the measured
    // output cadence vs the render interval. This handles MFG automatically.
    int effective_divisor = fg_divisor;
    if (ct.mean_delta_us > 0.0f && target_interval_us > 0.0f) {
        float ratio = target_interval_us / ct.mean_delta_us;
        if (ratio > 3.5f)      effective_divisor = 4;
        else if (ratio > 2.5f) effective_divisor = 3;
        else if (ratio > 1.3f) effective_divisor = 2;
    }

    float output_interval_us = target_interval_us / static_cast<float>(effective_divisor);
    float low_thresh = output_interval_us * CadenceTracker::kLowVarianceRatio;
    float high_thresh = output_interval_us * CadenceTracker::kHighVarianceRatio;

    bool is_smooth = (ct.stddev_us < low_thresh);
    bool is_jittery = (ct.stddev_us > high_thresh);

    // Update streak counters
    if (is_smooth) {
        ct.stable_streak++;
        ct.jitter_streak = 0;
    } else if (is_jittery) {
        ct.jitter_streak++;
        ct.stable_streak = 0;
    } else {
        if (ct.stable_streak > 0) ct.stable_streak--;
        if (ct.jitter_streak > 0) ct.jitter_streak--;
    }

    // Track the best interval (lowest variance seen)
    if (ct.variance_us2 < ct.best_variance_us2) {
        ct.best_variance_us2 = ct.variance_us2;
        ct.best_interval_us = ct.mean_delta_us;
    }

    bool is_mfg = (effective_divisor >= 3);

    if (is_mfg) {
        // ── MFG mode: very conservative ──
        // Only react to confirmed jitter. Never tighten.
        if (ct.jitter_streak >= CadenceTracker::kJitterThreshold) {
            fg_cadence_adjust_us = std::min(fg_cadence_adjust_us + 2, static_cast<int32_t>(12));
        } else if (ct.stable_streak >= CadenceTracker::kStableThreshold * 2) {
            // Very long stable streak — cautiously reduce the back-off
            // but never go negative (never tighten under MFG)
            fg_cadence_adjust_us = std::max(fg_cadence_adjust_us - 1, static_cast<int32_t>(0));
        }
    } else {
        // ── FG (2x) mode: moderate ──
        // Can both tighten and widen based on cadence quality.
        if (ct.jitter_streak >= CadenceTracker::kJitterThreshold) {
            fg_cadence_adjust_us = std::min(fg_cadence_adjust_us + 2, static_cast<int32_t>(10));
        } else if (ct.stable_streak >= CadenceTracker::kStableThreshold) {
            fg_cadence_adjust_us = std::max(fg_cadence_adjust_us - 1, static_cast<int32_t>(-4));
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
    cadence.Reset();
    fg_cadence_adjust_us = 0;
}

// ============================================================================
// Smooth Motion detection
// ============================================================================

static std::atomic<bool> s_smooth_motion{false};
static int64_t s_sm_check_qpc = 0;

static bool DetectSmoothMotion() {
    return GetModuleHandleW(L"NvPresent64.dll") != nullptr;
}

// ============================================================================
// VRR / GSync ceiling
// ============================================================================

static NvU32 ComputeVrrFloorIntervalUs(HWND hwnd) {
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
            if (GetModuleHandleW(L"nvngx_dlssg.dll"))  return 2;
            if (GetModuleHandleW(L"_nvngx_dlssg.dll")) return 2;
            if (GetModuleHandleW(L"sl.dlss_g.dll"))    return 2;
            if (GetModuleHandleW(L"dlss-g.dll"))       return 2;
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
    if (!latency_buf_) {
        latency_buf_ = new (std::nothrow) NvLatencyResult{};
    }
}

void UlLimiter::Shutdown() {
    if (dev_ && ReflexActive()) {
        const auto& gs = GetGameState();
        NvSleepParams p = {};
        p.version = NV_SLEEP_PARAMS_VER;
        if (gs.captured.load(std::memory_order_acquire)) {
            p.bLowLatencyMode = gs.low_latency.load(std::memory_order_relaxed) ? 1 : 0;
            p.bLowLatencyBoost = gs.boost.load(std::memory_order_relaxed) ? 1 : 0;
            p.bUseMarkersToOptimize = gs.use_markers.load(std::memory_order_relaxed) ? 1 : 0;
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
// ResetAdaptiveState — full reset on settings change or refocus (from v2)
// ============================================================================

void UlLimiter::ResetAdaptiveState() {
    gpu_predictor_.Reset();
    gpu_stats_ = GpuStats{};
    ptp_error_accum_us_ = 0.0f;
    ptp_correction_us_ = 0;
    ptp_sample_count_ = 0;
    last_ptp_present_ns_ = 0;
    last_pred_frame_id_ = 0;

    // Re-anchor the timing grid
    grid_epoch_ns_ = 0;
    grid_next_ns_ = 0;
    grid_interval_ns_ = 0;

    // Re-enter warmup
    warmup_start_qpc_ = ul_timing::NowQpc();
    warmup_done_ = false;

    last_sleep_valid_ = false;
    ul_log::Write("ResetAdaptiveState: full reset");
}

// ============================================================================
// Interval adjustment — VRR floor, FG offset, driver shave
// ============================================================================

static NvU32 AdjustIntervalUs(NvU32 raw, int fg_divisor, HWND hwnd,
                              const GpuStats& gs, int32_t ptp_correction_us) {
    if (raw == 0) return 0;

    // 1. VRR floor
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

    // 3. Queue pressure
    if (gs.valid && gs.queue_pressure && raw > 100)
        raw += 4;

    // 4. Adaptive GPU headroom
    if (gs.valid && gs.interval_adjust_us != 0) {
        int32_t adjusted = static_cast<int32_t>(raw) + gs.interval_adjust_us;
        if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
    }

    // 5. Present-to-present feedback correction
    if (ptp_correction_us != 0) {
        int32_t adjusted = static_cast<int32_t>(raw) + ptp_correction_us;
        if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
    }

    // 6. Driver shave
    if (raw > 50)
        raw -= 2;

    return raw;
}

// ============================================================================
// Sleep param helpers — build + changed-params optimization
// ============================================================================

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

    // --- PREDICTIVE SLEEP (v2 mode-dependent) ---
    // 1:1 → tighten based on predicted GPU time + safety margin
    // FG  → cadence-based stabilization (adjust to minimize output variance)
    // MFG → same as FG but more conservative (never tighten, only stabilize)
    if (raw_interval > 0 && gpu_predictor_.active) {
        bool fg = (DetectFGDivisor() > 1);

        if (!fg) {
            // 1:1 mode: tighten interval to predicted GPU time.
            // Only tighten (reduce) — we're a limiter, not an accelerator.
            // Only when enforcement site is SIM_START (GPU-bound).
            if (gpu_stats_.auto_site == SIM_START) {
                NvU32 predictive_interval = static_cast<NvU32>(
                    std::round(gpu_predictor_.predicted_us + gpu_predictor_.safety_margin_us));
                if (predictive_interval < raw_interval)
                    raw_interval = predictive_interval;
            }
        } else {
            // FG/MFG mode: apply cadence-based adjustment.
            // fg_cadence_adjust_us is positive (widen) when output is jittery,
            // negative (tighten) when output is confirmed smooth (2x FG only),
            // zero when holding steady.
            if (gpu_predictor_.fg_cadence_adjust_us != 0) {
                int32_t adjusted = static_cast<int32_t>(raw_interval)
                                 + gpu_predictor_.fg_cadence_adjust_us;
                if (adjusted > 50)
                    raw_interval = static_cast<NvU32>(adjusted);
            }
        }
    }

    p.minimumIntervalUs = AdjustIntervalUs(raw_interval, DetectFGDivisor(), hwnd_,
                                            gpu_stats_, ptp_correction_us_);

    return p;
}

bool UlLimiter::MaybeUpdateSleepMode(const NvSleepParams& p) {
    if (last_sleep_valid_ &&
        last_sleep_params_.bLowLatencyMode == p.bLowLatencyMode &&
        last_sleep_params_.bLowLatencyBoost == p.bLowLatencyBoost &&
        last_sleep_params_.bUseMarkersToOptimize == p.bUseMarkersToOptimize &&
        last_sleep_params_.minimumIntervalUs == p.minimumIntervalUs) {
        return false;
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

    NvStatus st;
    __try { st = InvokeGetLatency(dev_, latency_buf_); }
    __except(EXCEPTION_EXECUTE_HANDLER) { st = NV_NO_IMPL; }
    if (st != NV_OK) return;

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

        if (f.gpuActiveRenderTimeUs > 0 && f.gpuActiveRenderTimeUs < 200'000) {
            sum_gpu += static_cast<float>(f.gpuActiveRenderTimeUs);
            n_gpu++;
        }

        if (f.osRenderQueueStartTime > 0 && f.osRenderQueueEndTime > f.osRenderQueueStartTime) {
            float q_us = static_cast<float>(f.osRenderQueueEndTime - f.osRenderQueueStartTime);
            if (q_us < 500'000.0f) { sum_queue += q_us; n_queue++; }
        }

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
    if (target_interval_us > 0.0f) {
        gs.gpu_load_ratio = gs.avg_gpu_active_us / target_interval_us;

        if (gs.gpu_load_ratio < 0.70f)
            gs.interval_adjust_us = -3;
        else if (gs.gpu_load_ratio > 0.90f)
            gs.interval_adjust_us = static_cast<int32_t>(
                std::min(4.0f, (gs.gpu_load_ratio - 0.90f) * 40.0f));
        else
            gs.interval_adjust_us = 0;
    }

    // --- Feature #3: Auto enforcement site (v2 FG-aware) ---
    // When FG is active, bias toward PRESENT_FINISH to keep the interpolation
    // pipeline fed. Only use SIM_START if GPU load is genuinely low (<60%).
    if (fg_active) {
        if (gs.gpu_load_ratio < 0.60f)
            gs.auto_site = SIM_START;
        else
            gs.auto_site = PRESENT_FINISH;
    } else {
        if (gs.gpu_load_ratio > 0.85f)
            gs.auto_site = SIM_START;
        else if (gs.gpu_load_ratio < 0.65f)
            gs.auto_site = PRESENT_FINISH;
        // else: keep current value (hysteresis)
    }

    // --- Feature #4: Render queue depth monitoring ---
    if (target_interval_us > 0.0f)
        gs.queue_pressure = (gs.avg_queue_time_us > target_interval_us * 1.5f);
    else
        gs.queue_pressure = false;

    // --- Feature #5: Adaptive DLSSG pacing offset ---
    if (fg_active && n_fg > 0) {
        int32_t measured = static_cast<int32_t>(std::round(gs.avg_fg_overhead_us));
        measured = std::clamp(measured, 4, 60);
        gs.adaptive_fg_offset_us = measured;
    } else if (!fg_active) {
        gs.adaptive_fg_offset_us = -1;
    }

    // --- Feature #6: Predictive sleep + cadence tracking ---
    {
        auto& pred = gpu_predictor_;

        float recent_gpu[16] = {};
        uint64_t recent_ids[16] = {};
        int n_recent = 0;
        for (int i = 63; i >= 0 && n_recent < 16; i--) {
            auto& f = latency_buf_->frameReport[i];
            if (!f.frameID) continue;
            if (f.frameID <= last_pred_frame_id_) continue;
            if (f.gpuActiveRenderTimeUs > 0 && f.gpuActiveRenderTimeUs < 200'000) {
                recent_gpu[n_recent] = static_cast<float>(f.gpuActiveRenderTimeUs);
                recent_ids[n_recent] = f.frameID;
                n_recent++;
            }
        }

        // Sort chronologically (oldest first)
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

        if (n_recent > 0)
            last_pred_frame_id_ = recent_ids[n_recent - 1];

        if (n_recent > 0 && pred.predicted_us > 0.0f) {
            bool missed = recent_gpu[n_recent - 1] > (pred.predicted_us + pred.safety_margin_us);
            pred.RecordMiss(missed);
        }

        // Activate predictive sleep when we have enough data.
        // Under FG, always activate (cadence-based stabilization).
        // Under 1:1, only when GPU-bound (SIM_START).
        if (fg_active) {
            pred.active = (pred.count >= 4) && (pred.predicted_us > 0.0f)
                       && (target_interval_us > 0.0f);
        } else {
            pred.active = (gs.auto_site == SIM_START) && (pred.count >= 4)
                       && (pred.predicted_us > 0.0f) && (target_interval_us > 0.0f);
        }

        // Feed cadence tracker from presentEndTime values (v2 port).
        // Collect valid entries newer than last_fed_frame_id, sort, then feed.
        {
            struct CadenceSample { uint64_t id; uint64_t present_end; };
            CadenceSample csamples[64];
            int n_cs = 0;
            uint64_t last_fed = pred.cadence.last_fed_frame_id;
            for (int i = 0; i < 64; i++) {
                auto& f = latency_buf_->frameReport[i];
                if (f.frameID && f.presentEndTime && f.frameID > last_fed)
                    csamples[n_cs++] = { f.frameID, f.presentEndTime };
            }
            for (int i = 0; i < n_cs - 1; i++)
                for (int j = i + 1; j < n_cs; j++)
                    if (csamples[j].id < csamples[i].id)
                        std::swap(csamples[i], csamples[j]);
            for (int i = 0; i < n_cs; i++)
                pred.cadence.Feed(csamples[i].present_end);
            if (n_cs > 0)
                pred.cadence.last_fed_frame_id = csamples[n_cs - 1].id;
        }

        // Compute FG/MFG cadence response (stabilization adjustment)
        pred.UpdateCadenceResponse(fg_active, fg_active ? DetectFGDivisor() : 1,
                                   target_interval_us);
    }
}

int UlLimiter::ResolveEnforcementSite() const {
    if (gpu_stats_.valid)
        return gpu_stats_.auto_site;
    return PRESENT_FINISH;
}

// ============================================================================
// Reflex Sleep pacing
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

    bool rendered = g_ring[prev_m1].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
                  < g_ring[prev_m1].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed);
    if (!rendered) return;

    if (g_ring[prev].frame_id.load(std::memory_order_relaxed) != frame_id - max_q) return;

    int64_t start = ul_timing::NowNs();
    constexpr int64_t kTimeout = 50'000'000LL;

    auto still_waiting = [&]() {
        return g_ring[prev].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
             > g_ring[prev].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed);
    };

    while (still_waiting()) {
        if (ul_timing::NowNs() - start > kTimeout) return;
        int64_t remaining = kTimeout - (ul_timing::NowNs() - start);
        if (remaining < 1'000'000LL) break;
        int64_t wake = ul_timing::NowNs() + 500'000LL;
        ul_timing::SleepUntilNs(wake, htimer_queue_);
    }

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

    if (s_smooth_motion.load(std::memory_order_relaxed)) return;

    ExpandedSettings es = ExpandPreset();
    if (!es.use_marker_pacing) return;

    int site = ResolveEnforcementSite();

    size_t slot = static_cast<size_t>(frame_id % kRingSize);
    bool is_real_frame = (g_ring[slot].frame_id.load(std::memory_order_relaxed) == frame_id)
                      && (g_ring[slot].timestamp_ns[SIM_START].load(std::memory_order_relaxed) > 0);
    if (!is_real_frame) return;

    if (marker_type == PRESENT_BEGIN) {
        HandleQueuedFrames(frame_id, es.max_queued_frames);
        HandleDelayPresent(frame_id);
    }

    // v2 deferred enforcement: when site is SIM_START and queue_depth > 1
    // with FG active, defer sleep to PRESENT_BEGIN so the queue wait fires
    // first and the FG pipeline stays fed.
    int effective_site = site;
    if (site == SIM_START && es.max_queued_frames > 1 && DetectFGDivisor() > 1)
        effective_site = PRESENT_BEGIN;
    // Also defer when queue frames > 0 (original v1 behavior for non-FG)
    else if (site == SIM_START && es.max_queued_frames > 0)
        effective_site = PRESENT_BEGIN;

    if (marker_type == effective_site) {
        DoReflexSleep();
    }
}

// ============================================================================
// Timing fallback — QPC-based hard backstop
// ============================================================================

void UlLimiter::DoTimingFallback() {
    float target = g_cfg.fps_limit.load(std::memory_order_relaxed);
    if (target <= 0.0f) return;

    int64_t frame_ns = static_cast<int64_t>(1e9 / static_cast<double>(target));
    int64_t now = ul_timing::NowNs();

    if (grid_epoch_ns_ == 0) {
        grid_epoch_ns_ = now;
        grid_interval_ns_ = frame_ns;
        grid_next_ns_ = now + frame_ns;
        return;
    }

    grid_interval_ns_ = frame_ns;

    if (grid_next_ns_ <= now) {
        int64_t elapsed = now - grid_epoch_ns_;
        int64_t slots_elapsed = elapsed / grid_interval_ns_;
        grid_next_ns_ = grid_epoch_ns_ + (slots_elapsed + 1) * grid_interval_ns_;
        return;
    }

    ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);
    grid_next_ns_ += grid_interval_ns_;
}

// ============================================================================
// OnPresent — main entry from ReShade present callback
// ============================================================================

void UlLimiter::OnPresent() {
    frame_num_++;
    g_present_count.fetch_add(1, std::memory_order_relaxed);

    // Time-based warmup: 2 seconds from first present.
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

    // --- Settings change detection → full adaptive reset (from v2) ---
    {
        float cur_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
        int cur_vsync = g_cfg.vsync_override.load(std::memory_order_relaxed);
        bool cur_excl = g_cfg.exclusive_pacing.load(std::memory_order_relaxed);

        static float s_last_fps = 0.0f;
        static int s_last_vsync = -1;
        static bool s_last_excl = false;

        bool fps_changed = (s_last_fps != 0.0f && cur_fps != s_last_fps);
        bool vsync_changed = (s_last_vsync >= 0 && cur_vsync != s_last_vsync);
        bool excl_changed = (s_last_vsync >= 0 && cur_excl != s_last_excl);

        if (fps_changed || vsync_changed || excl_changed) {
            ResetAdaptiveState();
        }

        s_last_fps = cur_fps;
        s_last_vsync = cur_vsync;
        s_last_excl = cur_excl;
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
    if (frame_num_ % 2 == 0) {
        int prev_site = gpu_stats_.auto_site;
        UpdateGpuStats();
        if (gpu_stats_.valid && gpu_stats_.auto_site != prev_site) {
            ul_log::Write("Auto enforcement site: %s (GPU load %.0f%%)",
                          gpu_stats_.auto_site == SIM_START ? "SIM_START" : "PRESENT_FINISH",
                          gpu_stats_.gpu_load_ratio * 100.0f);
        }
    }

    // When Smooth Motion is active, use timing-only pacing.
    if (s_smooth_motion.load(std::memory_order_relaxed)) {
        DoTimingFallback();
        return;
    }

    // Present-to-present feedback loop.
    // Only run when FG is not active — with FG, cadence tracker handles pacing.
    if (ReflexActive() && dev_ && DetectFGDivisor() == 1) {
        float out_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
        if (out_fps > 0.0f) {
            float target_us = 1'000'000.0f / out_fps;
            int64_t now_ns = ul_timing::NowNs();

            if (last_ptp_present_ns_ > 0) {
                float actual_us = static_cast<float>(now_ns - last_ptp_present_ns_) / 1000.0f;
                if (actual_us > 0.0f && actual_us < target_us * 3.0f) {
                    ptp_error_accum_us_ += (actual_us - target_us);
                    ptp_sample_count_++;
                }
            }
            last_ptp_present_ns_ = now_ns;

            constexpr int kPtpUpdateInterval = 30;
            if (ptp_sample_count_ >= kPtpUpdateInterval) {
                float avg_error = ptp_error_accum_us_ / static_cast<float>(ptp_sample_count_);
                if (avg_error > 1.0f)
                    ptp_correction_us_ = std::max(ptp_correction_us_ - 1, -8);
                else if (avg_error < -1.0f)
                    ptp_correction_us_ = std::min(ptp_correction_us_ + 1, 8);

                ptp_error_accum_us_ = 0.0f;
                ptp_sample_count_ = 0;
            }
        }
    }

    // Reflex path
    if (ReflexActive() && dev_) {
        ExpandedSettings es = ExpandPreset();
        if (es.use_marker_pacing && g_game_uses_reflex.load(std::memory_order_relaxed)) {
            NvSleepParams p = BuildSleepParams();
            MaybeUpdateSleepMode(p);
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
