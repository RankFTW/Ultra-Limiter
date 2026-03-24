// ReLimiter — Frame limiter implementation
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
#include "ul_vk_reflex.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

// ============================================================================
// StagePredictor — trend-aware per-stage time prediction
// ============================================================================

void StagePredictor::Update(float sample_us) {
    if (sample_us <= 0.0f || sample_us > 200'000.0f) return;

    if (count >= 2) {
        int prev_idx = (head - 1 + kHistorySize) % kHistorySize;
        float prev = history[prev_idx];
        float delta = sample_us - prev;
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

    history[head] = sample_us;
    head = (head + 1) % kHistorySize;
    if (count < kHistorySize) count++;

    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + kHistorySize) % kHistorySize;
        float w = (i == 0) ? 4.0f : (i == 1) ? 3.0f : (i == 2) ? 2.0f : 1.0f;
        weighted_sum += history[idx] * w;
        weight_total += w;
    }
    predicted_us = weighted_sum / weight_total;

    if (trend_dir > 0 && trend_len >= kTrendThreshold && count >= 3) {
        int oldest_idx = (head - 1 - std::min(trend_len, count - 1) + kHistorySize) % kHistorySize;
        int newest_idx = (head - 1 + kHistorySize) % kHistorySize;
        float rise = history[newest_idx] - history[oldest_idx];
        float per_frame = rise / static_cast<float>(std::min(trend_len, count - 1));
        if (per_frame > 0.0f)
            predicted_us += per_frame;
    }
}

void StagePredictor::Reset() {
    head = 0; count = 0; trend_dir = 0; trend_len = 0; predicted_us = 0.0f;
}

// ============================================================================
// PipelinePredictor
// ============================================================================

void PipelinePredictor::UpdatePrediction(bool fg_active) {
    predicted_total_us = gpu.predicted_us + sim.predicted_us;
    if (fg_active && fg.count > 0)
        predicted_total_us += fg.predicted_us;
}

void PipelinePredictor::RecordMiss(bool missed) {
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

void PipelinePredictor::Reset() {
    gpu.Reset(); sim.Reset(); fg.Reset();
    predicted_total_us = 0.0f; active = false;
    safety_margin_us = 500.0f; frames_since_miss = 0; miss_count = 0;
    cadence.Reset(); fg_cadence_adjust_us = 0; fg_unified_adjust_us = 0;
}

// ============================================================================
// Unified FG adjustment
// ============================================================================

void PipelinePredictor::ComputeFGAdjustment(const FGPacingContext& ctx) {
    int32_t base = static_cast<int32_t>(std::round(ctx.fg_overhead_us));
    base = std::clamp(base, 4, 60);

    float low_thresh = ctx.output_interval_us * CadenceTracker::kLowVarianceRatio;
    float high_thresh = ctx.output_interval_us * CadenceTracker::kHighVarianceRatio;

    bool is_smooth = (ctx.cadence_stddev_us < low_thresh) && (cadence.count >= 8);
    bool is_jittery = (ctx.cadence_stddev_us > high_thresh) && (cadence.count >= 8);
    bool has_data = (cadence.count >= 8);

    int32_t delta = fg_cadence_adjust_us;

    if (has_data) {
        if (is_jittery && ctx.queue_stressed) {
            delta = std::min(delta + 3, static_cast<int32_t>(15));
        } else if (is_jittery) {
            delta = std::min(delta, static_cast<int32_t>(12));
        } else if (is_smooth && ctx.gpu_headroom > 0.30f && !ctx.queue_stressed) {
            if (!ctx.is_mfg)
                delta = std::max(delta - 1, static_cast<int32_t>(-4));
        } else if (is_smooth && ctx.gpu_headroom <= 0.30f) {
            if (delta < 0) delta = 0;
        }
        if (ctx.queue_stressed && delta < 0)
            delta = 0;
    }

    fg_unified_adjust_us = base + delta;
}

// ============================================================================
// CadenceTracker
// ============================================================================

void CadenceTracker::Feed(uint64_t present_end_time) {
    if (present_end_time == 0) return;
    if (last_present_time > 0 && present_end_time > last_present_time) {
        float delta_us = static_cast<float>(present_end_time - last_present_time);
        if (delta_us > 100.0f && delta_us < 200'000.0f) {
            deltas_us[head] = delta_us;
            head = (head + 1) % kWindowSize;
            if (count < kWindowSize) count++;
        }
    }
    last_present_time = present_end_time;
}

void CadenceTracker::ComputeStats() {
    if (count < 4) { variance_us2 = 0; stddev_us = 0; mean_delta_us = 0; return; }
    float sum = 0.0f;
    for (int i = 0; i < count; i++)
        sum += deltas_us[(head - count + i + kWindowSize) % kWindowSize];
    mean_delta_us = sum / static_cast<float>(count);
    float var_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float d = deltas_us[(head - count + i + kWindowSize) % kWindowSize] - mean_delta_us;
        var_sum += d * d;
    }
    variance_us2 = var_sum / static_cast<float>(count);
    stddev_us = std::sqrt(variance_us2);
}

void CadenceTracker::Reset() {
    head = 0; count = 0; last_present_time = 0; last_fed_frame_id = 0;
    variance_us2 = 0; stddev_us = 0; mean_delta_us = 0;
    best_interval_us = 0; best_variance_us2 = 1e12f;
    stable_streak = 0; jitter_streak = 0;
}

// ============================================================================
// PipelinePredictor — FG/MFG cadence response
// ============================================================================

void PipelinePredictor::UpdateCadenceResponse(bool fg_active, int fg_divisor,
                                               float target_interval_us) {
    if (!fg_active || target_interval_us <= 0.0f) {
        fg_cadence_adjust_us = 0;
        return;
    }

    auto& ct = cadence;
    ct.ComputeStats();
    if (ct.count < 8) { fg_cadence_adjust_us = 0; return; }

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

    if (is_smooth) { ct.stable_streak++; ct.jitter_streak = 0; }
    else if (is_jittery) { ct.jitter_streak++; ct.stable_streak = 0; }
    else {
        if (ct.stable_streak > 0) ct.stable_streak--;
        if (ct.jitter_streak > 0) ct.jitter_streak--;
    }

    if (ct.variance_us2 < ct.best_variance_us2) {
        ct.best_variance_us2 = ct.variance_us2;
        ct.best_interval_us = ct.mean_delta_us;
    }

    bool is_mfg = (effective_divisor >= 3);

    if (is_mfg) {
        if (ct.jitter_streak >= CadenceTracker::kJitterThreshold)
            fg_cadence_adjust_us = std::min(fg_cadence_adjust_us + 2, static_cast<int32_t>(12));
        else if (ct.stable_streak >= CadenceTracker::kStableThreshold * 2)
            fg_cadence_adjust_us = std::max(fg_cadence_adjust_us - 1, static_cast<int32_t>(0));
    } else {
        if (ct.jitter_streak >= CadenceTracker::kJitterThreshold)
            fg_cadence_adjust_us = std::min(fg_cadence_adjust_us + 2, static_cast<int32_t>(10));
        else if (ct.stable_streak >= CadenceTracker::kStableThreshold)
            fg_cadence_adjust_us = std::max(fg_cadence_adjust_us - 1, static_cast<int32_t>(-4));
    }
}

// ============================================================================
// BoostController — dynamic Reflex Boost management
// ============================================================================

BoostController::Decision BoostController::Evaluate(const PipelineStats& ps) {
    int64_t now_qpc = ul_timing::NowQpc();
    bool hysteresis_ok = (last_change_qpc == 0)
        || (now_qpc - last_change_qpc > ul_timing::g_qpc_freq * kHysteresisSeconds);
    if (!hysteresis_ok) return current;

    Decision next = Decision::Defer;
    if (ps.gpu_load_ratio > 0.85f)
        next = Decision::Off;
    else if (thermal_suspect)
        next = Decision::Off;
    else if (idle_frame_count >= 3
          && avg_gpu_steady_us > 0.0f
          && avg_gpu_after_idle_us > avg_gpu_steady_us * (1.0f + kRampThreshold)
          && !thermal_suspect)
        next = Decision::On;

    if (next != current) { current = next; last_change_qpc = now_qpc; }
    return current;
}

void BoostController::FeedIdleGap(float gpu_idle_gap_us, float gpu_active_us) {
    if (gpu_active_us <= 0.0f) return;
    constexpr float ema_a = 0.15f;
    constexpr int kDecayInterval = 120;
    if (++feed_count >= kDecayInterval) {
        idle_frame_count /= 2; steady_frame_count /= 2; feed_count = 0;
    }
    if (gpu_idle_gap_us > kIdleGapThresholdUs) {
        idle_frame_count++;
        avg_gpu_after_idle_us = (idle_frame_count > 1)
            ? avg_gpu_after_idle_us * (1.0f - ema_a) + gpu_active_us * ema_a : gpu_active_us;
    } else {
        steady_frame_count++;
        avg_gpu_steady_us = (steady_frame_count > 1)
            ? avg_gpu_steady_us * (1.0f - ema_a) + gpu_active_us * ema_a : gpu_active_us;
    }
}

void BoostController::CheckThermal(const StagePredictor& gpu_pred,
                                   const StagePredictor& sim_pred) {
    if (gpu_pred.trend_dir == 1 && gpu_pred.trend_len >= 3 && sim_pred.trend_dir <= 0) {
        if (thermal_suspect_start_qpc == 0) thermal_suspect_start_qpc = ul_timing::NowQpc();
        if (ul_timing::NowQpc() - thermal_suspect_start_qpc > ul_timing::g_qpc_freq * 3)
            thermal_suspect = true;
    } else {
        thermal_suspect = false; thermal_suspect_start_qpc = 0;
    }
}

void BoostController::Reset() {
    current = Decision::Defer; last_change_qpc = 0;
    avg_gpu_after_idle_us = 0; avg_gpu_steady_us = 0;
    idle_frame_count = 0; steady_frame_count = 0; feed_count = 0;
    thermal_suspect = false; thermal_suspect_start_qpc = 0;
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
// FG pacing offset (static fallback)
// ============================================================================

static constexpr NvU32 kFGPacingOffsetUs = 24;

// ============================================================================
// FG detection — always auto
// ============================================================================

int UlLimiter::DetectFGDivisor() const {
    if (GetModuleHandleW(L"nvngx_dlssg.dll"))  return 2;
    if (GetModuleHandleW(L"_nvngx_dlssg.dll")) return 2;
    if (GetModuleHandleW(L"sl.dlss_g.dll"))    return 2;
    if (GetModuleHandleW(L"dlss-g.dll"))        return 2;
    if (GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll")) return 2;
    if (GetModuleHandleW(L"ffx_framegeneration.dll"))            return 2;
    return 1;
}

// Compute the Reflex cap from the monitor refresh rate.
// Formula: fps = hz - (hz² / 3600), floored. Returns 0 on failure.
static float ComputeReflexCapFromMonitor(HWND hwnd) {
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) return 0.0f;

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hm) return 0.0f;

    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, &mi)) return 0.0f;

    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0.0f;

    float hz = static_cast<float>(dm.dmDisplayFrequency);
    if (hz < 30.0f) return 0.0f;

    float cap = hz - (hz * hz / 3600.0f);
    return (cap >= 10.0f) ? std::floor(cap) : 0.0f;
}

float UlLimiter::ComputeRenderFps() const {
    float target = g_cfg.fps_limit.load(std::memory_order_relaxed);

    // When unlimited (0) and FG is active, derive an effective target from
    // the monitor's Reflex cap so the pacing engine has a real value to work
    // with. Without this, minimumIntervalUs = 0 and the VRR ceiling, adaptive
    // interval, queue depth voting, and cadence tracking all get bypassed.
    if (target <= 0.0f) {
        int div = DetectFGDivisor();
        if (div <= 1) return 0.0f;  // no FG — truly unlimited
        float cap = ComputeReflexCapFromMonitor(hwnd_);
        if (cap <= 0.0f) return 0.0f;
        return cap / static_cast<float>(div);
    }

    int div = DetectFGDivisor();
    return (div > 1) ? target / static_cast<float>(div) : target;
}

// ============================================================================
// Lifecycle
// ============================================================================

void UlLimiter::Init() {
    if (!latency_buf_)
        latency_buf_ = new (std::nothrow) NvLatencyResult{};
}

void UlLimiter::Shutdown() {
    // Vulkan cleanup
    if (vk_reflex_) {
        vk_reflex_->Shutdown();
        vk_reflex_ = nullptr;
    }

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

void UlLimiter::ConnectVulkanReflex(VkReflex* vk) {
    vk_reflex_ = vk;
    ul_log::Write("ConnectVulkanReflex: Vulkan Reflex backend attached (active=%d)",
                  vk ? vk->IsActive() : 0);
}

void UlLimiter::ResetAdaptiveState() {
    pipeline_predictor_.Reset();
    pipeline_stats_ = PipelineStats{};
    boost_ctrl_.Reset();
    ptp_error_accum_us_ = 0.0f;
    ptp_correction_us_ = 0;
    ptp_sample_count_ = 0;
    last_ptp_present_ns_ = 0;
    last_pred_frame_id_ = 0;

    grid_epoch_ns_ = 0;
    grid_next_ns_ = 0;
    grid_interval_ns_ = 0;

    float fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
    if (fps > 0.0f)
        pipeline_stats_.pacing.ResizeWindow(fps);

    warmup_start_qpc_ = ul_timing::NowQpc();
    warmup_done_ = false;
    last_sleep_valid_ = false;
    ul_log::Write("ResetAdaptiveState: full reset");
}

// ============================================================================
// Interval adjustment — VRR floor, FG offset, driver shave
// ============================================================================

static NvU32 AdjustIntervalUs(NvU32 raw, int fg_divisor, HWND hwnd,
                              const PipelineStats& ps, int32_t ptp_correction_us,
                              int32_t fg_unified_adjust_us) {
    if (raw == 0) return 0;

    NvU32 vrr_floor = ComputeVrrFloorIntervalUs(hwnd);
    if (vrr_floor > 0 && raw < vrr_floor)
        raw = vrr_floor;

    if (fg_divisor > 1) {
        // FG path: single unified adjustment
        int32_t fg_adj = fg_unified_adjust_us;
        if (fg_adj == 0)
            fg_adj = static_cast<int32_t>(kFGPacingOffsetUs);

        if (fg_adj > 0) {
            raw += static_cast<NvU32>(fg_adj);
        } else {
            int32_t adjusted = static_cast<int32_t>(raw) + fg_adj;
            if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
        }
    } else {
        // 1:1 path: independent corrections
        if (ps.valid && ps.queue_pressure && raw > 100)
            raw += 4;
        if (ps.valid && ps.interval_adjust_us != 0) {
            int32_t adjusted = static_cast<int32_t>(raw) + ps.interval_adjust_us;
            if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
        }
        if (ptp_correction_us != 0) {
            int32_t adjusted = static_cast<int32_t>(raw) + ptp_correction_us;
            if (adjusted > 50) raw = static_cast<NvU32>(adjusted);
        }
    }

    if (raw > 50) raw -= 2;
    return raw;
}

// ============================================================================
// Sleep param helpers
// ============================================================================

NvSleepParams UlLimiter::BuildSleepParams() const {
    float render_fps = ComputeRenderFps();
    const auto& gs = GetGameState();

    NvSleepParams p = {};
    p.version = NV_SLEEP_PARAMS_VER;
    p.bLowLatencyMode = 1;

    // Dynamic Boost
    auto bd = boost_ctrl_.current;
    if (bd == BoostController::Decision::On)
        p.bLowLatencyBoost = 1;
    else if (bd == BoostController::Decision::Off)
        p.bLowLatencyBoost = 0;
    else {
        if (gs.captured.load(std::memory_order_acquire))
            p.bLowLatencyBoost = gs.boost.load(std::memory_order_relaxed) ? 1 : 0;
    }

    if (gs.captured.load(std::memory_order_acquire))
        p.bUseMarkersToOptimize = gs.use_markers.load(std::memory_order_relaxed) ? 1 : 0;

    NvU32 raw_interval = (render_fps > 0.0f)
        ? static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(render_fps)))
        : 0;

    // Predictive sleep — mode-dependent
    if (raw_interval > 0 && pipeline_predictor_.active) {
        bool fg = (DetectFGDivisor() > 1);
        if (!fg) {
            if (pipeline_stats_.auto_site == SIM_START) {
                NvU32 predictive_interval = static_cast<NvU32>(
                    std::round(pipeline_predictor_.predicted_total_us
                               + pipeline_predictor_.safety_margin_us));
                if (predictive_interval < raw_interval)
                    raw_interval = predictive_interval;
            }
        }
    }

    p.minimumIntervalUs = AdjustIntervalUs(raw_interval, DetectFGDivisor(), hwnd_,
                                            pipeline_stats_, ptp_correction_us_,
                                            pipeline_predictor_.fg_unified_adjust_us);
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
    if (st == NV_OK) { last_sleep_params_ = p; last_sleep_valid_ = true; }
    return (st == NV_OK);
}

// ============================================================================
// Pipeline stats from GetLatency — adaptive interval, enforcement site, etc.
// ============================================================================

void UlLimiter::UpdatePipelineStats() {
    if (!latency_buf_) return;

    // Vulkan path
    if (vk_reflex_ && vk_reflex_->IsActive()) {
        memset(latency_buf_, 0, sizeof(*latency_buf_));
        latency_buf_->version = NV_LATENCY_RESULT_VER;
        if (!vk_reflex_->GetLatencyTimings(latency_buf_)) return;
        // Fall through to shared analysis below
    }
    // DX path
    else {
        if (!ReflexActive() || !dev_) return;

        memset(latency_buf_, 0, sizeof(*latency_buf_));
        latency_buf_->version = NV_LATENCY_RESULT_VER;

        NvStatus st;
        __try { st = InvokeGetLatency(dev_, latency_buf_); }
        __except(EXCEPTION_EXECUTE_HANDLER) { st = NV_NO_IMPL; }
        if (st != NV_OK) return;
    }

    // Scan the most recent valid reports (up to 16)
    float sum_gpu = 0, sum_queue = 0, sum_fg = 0;
    float sum_sim = 0, sum_submit = 0, sum_driver = 0;
    float sum_idle_gap = 0, sum_input_disp = 0;
    int n_gpu = 0, n_queue = 0, n_fg = 0;
    int n_sim = 0, n_submit = 0, n_driver = 0;
    int n_idle_gap = 0, n_input_disp = 0;
    float target_interval_us = 0.0f;
    {
        float rfps = ComputeRenderFps();
        if (rfps > 0.0f) target_interval_us = 1'000'000.0f / rfps;
    }

    bool fg_active = (DetectFGDivisor() > 1);

    struct GapFrame { uint64_t id; uint64_t render_start; uint64_t render_end; float gpu_active; };
    GapFrame gap_frames[16] = {};
    int n_gap_frames = 0;

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
        if (f.simStartTime > 0 && f.simEndTime > f.simStartTime) {
            float sim_us = static_cast<float>(f.simEndTime - f.simStartTime);
            if (sim_us < 200'000.0f) { sum_sim += sim_us; n_sim++; }
        }
        if (f.renderSubmitStartTime > 0 && f.renderSubmitEndTime > f.renderSubmitStartTime) {
            float sub_us = static_cast<float>(f.renderSubmitEndTime - f.renderSubmitStartTime);
            if (sub_us < 200'000.0f) { sum_submit += sub_us; n_submit++; }
        }
        if (f.driverStartTime > 0 && f.driverEndTime > f.driverStartTime) {
            float drv_us = static_cast<float>(f.driverEndTime - f.driverStartTime);
            if (drv_us < 200'000.0f) { sum_driver += drv_us; n_driver++; }
        }
        if (f.inputSampleTime > 0 && f.presentEndTime > f.inputSampleTime) {
            float itd_us = static_cast<float>(f.presentEndTime - f.inputSampleTime);
            if (itd_us < 500'000.0f) { sum_input_disp += itd_us; n_input_disp++; }
        }
        if (f.gpuRenderStartTime > 0 && f.gpuRenderEndTime > 0 && n_gap_frames < 16) {
            gap_frames[n_gap_frames++] = {
                f.frameID, f.gpuRenderStartTime, f.gpuRenderEndTime,
                f.gpuActiveRenderTimeUs > 0 ? static_cast<float>(f.gpuActiveRenderTimeUs) : 0.0f
            };
        }
    }

    // Compute GPU idle gaps from chronologically sorted frames
    for (int i = 0; i < n_gap_frames - 1; i++)
        for (int j = i + 1; j < n_gap_frames; j++)
            if (gap_frames[j].id < gap_frames[i].id)
                std::swap(gap_frames[i], gap_frames[j]);
    for (int i = 1; i < n_gap_frames; i++) {
        if (gap_frames[i].render_start > gap_frames[i - 1].render_end) {
            float gap_us = static_cast<float>(gap_frames[i].render_start - gap_frames[i - 1].render_end);
            if (gap_us < 200'000.0f) {
                sum_idle_gap += gap_us;
                n_idle_gap++;
                boost_ctrl_.FeedIdleGap(gap_us, gap_frames[i].gpu_active);
            }
        }
    }

    // Update EMAs
    auto& ps = pipeline_stats_;
    constexpr float a = PipelineStats::kEmaAlpha;

    if (n_gpu > 0) {
        float cur = sum_gpu / static_cast<float>(n_gpu);
        ps.avg_gpu_active_us = ps.valid ? ps.avg_gpu_active_us * (1.0f - a) + cur * a : cur;
    }
    if (n_queue > 0) {
        float cur = sum_queue / static_cast<float>(n_queue);
        ps.avg_queue_us = ps.valid ? ps.avg_queue_us * (1.0f - a) + cur * a : cur;
    }
    if (n_fg > 0) {
        float cur = sum_fg / static_cast<float>(n_fg);
        ps.avg_fg_overhead_us = ps.valid ? ps.avg_fg_overhead_us * (1.0f - a) + cur * a : cur;
    }
    if (n_sim > 0) {
        float cur = sum_sim / static_cast<float>(n_sim);
        ps.avg_sim_us = ps.valid ? ps.avg_sim_us * (1.0f - a) + cur * a : cur;
    }
    if (n_submit > 0) {
        float cur = sum_submit / static_cast<float>(n_submit);
        ps.avg_render_submit_us = ps.valid ? ps.avg_render_submit_us * (1.0f - a) + cur * a : cur;
    }
    if (n_driver > 0) {
        float cur = sum_driver / static_cast<float>(n_driver);
        ps.avg_driver_us = ps.valid ? ps.avg_driver_us * (1.0f - a) + cur * a : cur;
    }
    if (n_idle_gap > 0) {
        float cur = sum_idle_gap / static_cast<float>(n_idle_gap);
        ps.avg_gpu_idle_gap_us = ps.valid ? ps.avg_gpu_idle_gap_us * (1.0f - a) + cur * a : cur;
    }
    if (n_input_disp > 0) {
        float cur = sum_input_disp / static_cast<float>(n_input_disp);
        ps.avg_input_to_display_us = ps.valid ? ps.avg_input_to_display_us * (1.0f - a) + cur * a : cur;
    }

    ps.samples += n_gpu;
    if (ps.samples < PipelineStats::kMinSamples) return;
    ps.valid = true;

    ps.avg_pipeline_total_us = ps.avg_sim_us + ps.avg_render_submit_us
                             + ps.avg_driver_us + ps.avg_queue_us + ps.avg_gpu_active_us;

    // Adaptive interval adjustment
    if (target_interval_us > 0.0f) {
        ps.gpu_load_ratio = ps.avg_gpu_active_us / target_interval_us;
        ps.pipeline_load_ratio = ps.avg_pipeline_total_us / target_interval_us;
        if (ps.gpu_load_ratio < 0.70f)
            ps.interval_adjust_us = -3;
        else if (ps.gpu_load_ratio > 0.90f)
            ps.interval_adjust_us = static_cast<int32_t>(
                std::min(4.0f, (ps.gpu_load_ratio - 0.90f) * 40.0f));
        else
            ps.interval_adjust_us = 0;
    }

    // Bottleneck detection (40% threshold)
    if (ps.avg_pipeline_total_us > 0.0f) {
        float stages[5] = {
            ps.avg_sim_us, ps.avg_render_submit_us, ps.avg_driver_us,
            ps.avg_queue_us, ps.avg_gpu_active_us
        };
        static constexpr Bottleneck stage_map[5] = {
            Bottleneck::CpuSim, Bottleneck::CpuSubmit, Bottleneck::Driver,
            Bottleneck::Queue, Bottleneck::Gpu
        };
        int max_idx = 0;
        for (int i = 1; i < 5; i++)
            if (stages[i] > stages[max_idx]) max_idx = i;
        if (stages[max_idx] > ps.avg_pipeline_total_us * 0.40f)
            ps.bottleneck = stage_map[max_idx];
        else
            ps.bottleneck = Bottleneck::None;
    }

    // Auto enforcement site (enhanced with bottleneck + FG awareness)
    if (fg_active) {
        if (ps.gpu_load_ratio < 0.60f)
            ps.auto_site = SIM_START;
        else
            ps.auto_site = PRESENT_FINISH;
    } else if (ps.bottleneck == Bottleneck::Gpu)
        ps.auto_site = SIM_START;
    else if (ps.bottleneck == Bottleneck::CpuSim || ps.bottleneck == Bottleneck::CpuSubmit)
        ps.auto_site = PRESENT_FINISH;
    else {
        if (ps.gpu_load_ratio > 0.85f)
            ps.auto_site = SIM_START;
        else if (ps.gpu_load_ratio < 0.65f)
            ps.auto_site = PRESENT_FINISH;
    }

    // Render queue depth monitoring
    if (target_interval_us > 0.0f)
        ps.queue_pressure = (ps.avg_queue_us > target_interval_us * 1.5f);
    else
        ps.queue_pressure = false;

    // Adaptive DLSSG pacing offset
    if (fg_active && n_fg > 0) {
        int32_t measured = static_cast<int32_t>(std::round(ps.avg_fg_overhead_us));
        measured = std::clamp(measured, 4, 60);
        ps.adaptive_fg_offset_us = measured;
    } else if (!fg_active) {
        ps.adaptive_fg_offset_us = -1;
    }

    // Predictive sleep — pipeline-aware wake scheduling
    {
        auto& pred = pipeline_predictor_;

        struct FrameSample { uint64_t id; float gpu_us; float sim_us; float fg_us; };
        FrameSample recent[16] = {};
        int n_recent = 0;

        for (int i = 63; i >= 0 && n_recent < 16; i--) {
            auto& f = latency_buf_->frameReport[i];
            if (!f.frameID || f.frameID <= last_pred_frame_id_) continue;

            FrameSample s = {};
            s.id = f.frameID;
            if (f.gpuActiveRenderTimeUs > 0 && f.gpuActiveRenderTimeUs < 200'000)
                s.gpu_us = static_cast<float>(f.gpuActiveRenderTimeUs);
            if (f.simStartTime > 0 && f.simEndTime > f.simStartTime) {
                float sim = static_cast<float>(f.simEndTime - f.simStartTime);
                if (sim < 200'000.0f) s.sim_us = sim;
            }
            if (fg_active && f.gpuRenderEndTime > 0 && f.presentEndTime > f.gpuRenderEndTime) {
                float fgo = static_cast<float>(f.presentEndTime - f.gpuRenderEndTime);
                if (fgo > 0.0f && fgo < 200'000.0f) s.fg_us = fgo;
            }
            if (s.gpu_us > 0.0f) recent[n_recent++] = s;
        }

        for (int i = 0; i < n_recent - 1; i++)
            for (int j = i + 1; j < n_recent; j++)
                if (recent[j].id < recent[i].id)
                    std::swap(recent[i], recent[j]);

        for (int i = 0; i < n_recent; i++) {
            pred.gpu.Update(recent[i].gpu_us);
            if (recent[i].sim_us > 0.0f) pred.sim.Update(recent[i].sim_us);
            if (recent[i].fg_us > 0.0f) pred.fg.Update(recent[i].fg_us);
        }
        if (n_recent > 0) last_pred_frame_id_ = recent[n_recent - 1].id;

        pred.UpdatePrediction(fg_active);

        if (n_recent > 0 && pred.predicted_total_us > 0.0f) {
            float actual = recent[n_recent - 1].gpu_us + recent[n_recent - 1].sim_us
                         + recent[n_recent - 1].fg_us;
            bool missed = actual > (pred.predicted_total_us + pred.safety_margin_us);
            pred.RecordMiss(missed);
        }

        pred.active = (pred.gpu.count >= 4)
                   && (pred.predicted_total_us > 0.0f) && (target_interval_us > 0.0f);

        // Feed cadence tracker
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

        pred.UpdateCadenceResponse(fg_active, fg_active ? DetectFGDivisor() : 1,
                                   target_interval_us);

        // Unified FG adjustment
        if (fg_active && target_interval_us > 0.0f) {
            int effective_divisor = 1;
            if (pred.cadence.mean_delta_us > 0.0f) {
                float ratio = target_interval_us / pred.cadence.mean_delta_us;
                if (ratio > 3.5f)      effective_divisor = 4;
                else if (ratio > 2.5f) effective_divisor = 3;
                else if (ratio > 1.3f) effective_divisor = 2;
            } else {
                effective_divisor = DetectFGDivisor();
            }

            FGPacingContext fctx;
            fctx.gpu_headroom = 1.0f - ps.gpu_load_ratio;
            fctx.queue_stressed = ps.queue_pressure;
            fctx.fg_overhead_us = ps.avg_fg_overhead_us;
            fctx.cadence_stddev_us = pred.cadence.stddev_us;
            fctx.output_interval_us = target_interval_us / static_cast<float>(effective_divisor);
            fctx.is_mfg = (effective_divisor >= 3);
            pred.ComputeFGAdjustment(fctx);
        } else {
            pred.fg_unified_adjust_us = 0;
        }
    }

    // Dynamic queue depth (voting window)
    {
        auto& dp = ps.pacing;
        float margin = pipeline_predictor_.safety_margin_us;
        float miss_rate = (pipeline_predictor_.gpu.count > 0)
            ? static_cast<float>(pipeline_predictor_.miss_count)
              / static_cast<float>((std::max)(pipeline_predictor_.gpu.count, 1))
            : 0.0f;

        int suggested = 1;
        if (margin < 400.0f && miss_rate < 0.05f)
            suggested = 1;
        else if (margin > 1500.0f)
            suggested = 3;
        else if (margin > 800.0f || miss_rate > 0.10f)
            suggested = 2;

        if (fg_active && suggested < 2)
            suggested = 2;
        if (ps.queue_pressure)
            suggested = (std::min)(suggested + 1, 3);

        dp.vote_history[dp.vote_head] = suggested;
        dp.vote_head = (dp.vote_head + 1) % dp.vote_window_size;
        if (dp.vote_count < dp.vote_window_size)
            dp.vote_count++;

        if (dp.vote_count >= dp.vote_window_size / 2) {
            int counts[4] = {};
            for (int i = 0; i < dp.vote_count; i++) {
                int idx = (dp.vote_head - dp.vote_count + i + DynamicPacing::kMaxVoteWindow)
                        % dp.vote_window_size;
                int v = dp.vote_history[idx];
                if (v >= 0 && v <= 3) counts[v]++;
            }

            int current = dp.queue_depth;
            int64_t now_qpc = ul_timing::NowQpc();
            bool hysteresis_ok = (dp.last_change_qpc == 0)
                || (now_qpc - dp.last_change_qpc > ul_timing::g_qpc_freq * DynamicPacing::kHysteresisSeconds);

            if (hysteresis_ok) {
                if (suggested > current) {
                    int votes_deeper = 0;
                    for (int d = current + 1; d <= 3; d++) votes_deeper += counts[d];
                    if (votes_deeper >= static_cast<int>(dp.vote_count * 0.60f)) {
                        int best = current + 1;
                        for (int d = current + 2; d <= 3; d++)
                            if (counts[d] > counts[best]) best = d;
                        dp.queue_depth = best;
                        dp.last_change_qpc = now_qpc;
                    }
                } else if (suggested < current) {
                    int votes_shallower = 0;
                    for (int d = 1; d < current; d++) votes_shallower += counts[d];
                    if (votes_shallower >= static_cast<int>(dp.vote_count * 0.80f)) {
                        int best = current - 1;
                        for (int d = 1; d < current - 1; d++)
                            if (counts[d] > counts[best]) best = d;
                        dp.queue_depth = (std::max)(1, best);
                        dp.last_change_qpc = now_qpc;
                    }
                }
            }
        }
    }

    // BoostController thermal check + evaluate
    boost_ctrl_.CheckThermal(pipeline_predictor_.gpu, pipeline_predictor_.sim);
    boost_ctrl_.Evaluate(ps);
}

// ============================================================================
// Enforcement site resolution
// ============================================================================

int UlLimiter::ResolveEnforcementSite() const {
    if (pipeline_stats_.valid)
        return pipeline_stats_.auto_site;
    return PRESENT_FINISH;
}

// ============================================================================
// Reflex Sleep pacing
// ============================================================================

void UlLimiter::DoReflexSleep() {
    // Vulkan path: use VK_NV_low_latency2
    if (vk_reflex_ && vk_reflex_->IsActive()) {
        NvSleepParams p = BuildSleepParams();
        // Always update the driver's low-latency mode / interval / boost hints
        vk_reflex_->SetSleepMode(
            p.bLowLatencyMode != 0,
            p.bLowLatencyBoost != 0,
            p.minimumIntervalUs);
        // Only block on the timeline semaphore when the game uses native Reflex
        // markers. Without markers the driver has no frame timing context, so
        // vkWaitSemaphores blocks for the wrong duration → low FPS.
        // In that case we let DoTimingFallback() handle the actual pacing.
        if (g_game_uses_reflex.load(std::memory_order_relaxed))
            vk_reflex_->Sleep();
        return;
    }

    // DX path: NVAPI
    if (!ReflexActive() || !dev_) return;
    NvSleepParams p = BuildSleepParams();
    MaybeUpdateSleepMode(p);
    InvokeSleep(dev_);
}

// ============================================================================
// Delay PRESENT_BEGIN — no-op (feature cut)
// ============================================================================

void UlLimiter::HandleDelayPresent(uint64_t /*frame_id*/) {
    // delay_present feature has been cut — this is a no-op.
}

// ============================================================================
// Wait for N-back frame to finish rendering (hybrid wait)
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
    constexpr int64_t kTimeout = 50'000'000LL;  // 50ms

    auto still_waiting = [&]() {
        return g_ring[prev].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
             > g_ring[prev].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed);
    };

    // Phase 1: yield via short timer sleeps (~0.5ms each)
    while (still_waiting()) {
        if (ul_timing::NowNs() - start > kTimeout) return;
        int64_t remaining = kTimeout - (ul_timing::NowNs() - start);
        if (remaining < 1'000'000LL) break;
        int64_t wake = ul_timing::NowNs() + 500'000LL;
        ul_timing::SleepUntilNs(wake, htimer_queue_);
    }

    // Phase 2: busy-wait for the final sub-millisecond
    while (still_waiting()) {
        if (ul_timing::NowNs() - start > kTimeout) break;
        YieldProcessor();
    }
}

// ============================================================================
// Marker-based pacing
// ============================================================================

void UlLimiter::OnMarker(int marker_type, uint64_t frame_id) {
    bool has_reflex = (ReflexActive() && dev_) || (vk_reflex_ && vk_reflex_->IsActive());
    if (!has_reflex) return;
    if (!warmup_done_) return;

    if (s_smooth_motion.load(std::memory_order_relaxed)) return;

    const auto& dp = pipeline_stats_.pacing;

    int site = ResolveEnforcementSite();

    size_t slot = static_cast<size_t>(frame_id % kRingSize);
    bool is_real_frame = (g_ring[slot].frame_id.load(std::memory_order_relaxed) == frame_id)
                      && (g_ring[slot].timestamp_ns[SIM_START].load(std::memory_order_relaxed) > 0);
    if (!is_real_frame) return;

    if (marker_type == PRESENT_BEGIN)
        HandleQueuedFrames(frame_id, dp.queue_depth);

    int effective_site = site;
    if (site == SIM_START && dp.queue_depth > 1)
        effective_site = PRESENT_BEGIN;

    if (marker_type == effective_site)
        DoReflexSleep();
}

// ============================================================================
// Timing fallback — QPC-based hard backstop
// ============================================================================

void UlLimiter::DoTimingFallback() {
    float target = g_cfg.fps_limit.load(std::memory_order_relaxed);

    // When unlimited and FG is active, use the Reflex cap as the backstop target
    if (target <= 0.0f) {
        int div = DetectFGDivisor();
        if (div > 1) {
            float cap = ComputeReflexCapFromMonitor(hwnd_);
            if (cap > 0.0f) target = cap;
        }
    }
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

    // --- Background limiter ---
    // When bg_fps_limit > 0, use that. Otherwise fall back to fps_limit/3 (min 10).
    // Freeze all adaptive evaluation while backgrounded. Full reset on refocus.
    {
        bool bg = (hwnd_ && GetForegroundWindow() != hwnd_);
        if (bg && !is_background_) {
            is_background_ = true;
            ul_log::Write("Background: entering background mode");
        } else if (!bg && is_background_) {
            is_background_ = false;
            ResetAdaptiveState();
            ul_log::Write("Background: refocused, full reset");
        }
        if (is_background_) {
            float bg_cfg = g_cfg.bg_fps_limit.load(std::memory_order_relaxed);
            float bg_fps;
            if (bg_cfg > 0.0f) {
                bg_fps = bg_cfg;
            } else {
                float main_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
                bg_fps = (main_fps > 0.0f) ? (std::max)(main_fps / 3.0f, 10.0f) : 10.0f;
            }
            int64_t bg_ns = static_cast<int64_t>(1e9 / static_cast<double>(bg_fps));
            int64_t now_ns = ul_timing::NowNs();
            if (grid_epoch_ns_ == 0) {
                grid_epoch_ns_ = now_ns;
                grid_interval_ns_ = bg_ns;
                grid_next_ns_ = now_ns + bg_ns;
            } else {
                grid_interval_ns_ = bg_ns;
                if (grid_next_ns_ > now_ns)
                    ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);
                grid_next_ns_ = now_ns + bg_ns;
            }
            return;
        }
    }

    // --- Settings change detection → global reset ---
    {
        float cur_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
        int cur_vsync = g_cfg.vsync_override.load(std::memory_order_relaxed);
        bool cur_excl = g_cfg.exclusive_pacing.load(std::memory_order_relaxed);

        static int s_last_vsync = -1;
        static bool s_last_excl = false;

        bool fps_changed = (last_fps_limit_ != 0.0f && cur_fps != last_fps_limit_);
        bool vsync_changed = (s_last_vsync >= 0 && cur_vsync != s_last_vsync);
        bool excl_changed = (s_last_vsync >= 0 && cur_excl != s_last_excl);

        if (fps_changed || vsync_changed || excl_changed)
            ResetAdaptiveState();

        last_fps_limit_ = cur_fps;
        s_last_vsync = cur_vsync;
        s_last_excl = cur_excl;

        if (pipeline_stats_.pacing.vote_window_size <= 0 && cur_fps > 0.0f)
            pipeline_stats_.pacing.ResizeWindow(cur_fps);
    }

    // Periodically check for Smooth Motion
    if (now_qpc - s_sm_check_qpc > ul_timing::g_qpc_freq * 2) {
        bool prev = s_smooth_motion.load(std::memory_order_relaxed);
        bool cur = DetectSmoothMotion();
        if (cur != prev) {
            s_smooth_motion.store(cur, std::memory_order_relaxed);
            ul_log::Write("Smooth Motion: %s", cur ? "detected" : "not detected");
        }
        s_sm_check_qpc = now_qpc;
    }

    // Poll GetLatency every 2 frames
    if (frame_num_ % 2 == 0) {
        int prev_site = pipeline_stats_.auto_site;
        UpdatePipelineStats();
        if (pipeline_stats_.valid && pipeline_stats_.auto_site != prev_site) {
            ul_log::Write("Auto enforcement site: %s (GPU load %.0f%%)",
                          pipeline_stats_.auto_site == SIM_START ? "SIM_START" : "PRESENT_FINISH",
                          pipeline_stats_.gpu_load_ratio * 100.0f);
        }
    }

    // Smooth Motion active → timing-only pacing
    if (s_smooth_motion.load(std::memory_order_relaxed)) {
        DoTimingFallback();
        return;
    }

    // Present-to-present feedback loop (1:1 only — not under FG)
    bool any_reflex = (ReflexActive() && dev_) || (vk_reflex_ && vk_reflex_->IsActive());
    if (any_reflex && DetectFGDivisor() == 1) {
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

    // Reflex path — DX or Vulkan
    bool vk_active = (vk_reflex_ && vk_reflex_->IsActive());
    bool vk_native = vk_active && g_game_uses_reflex.load(std::memory_order_relaxed);
    if (vk_active) {
        // Only call DoReflexSleep (SetSleepMode + Sleep) when the game uses
        // native Reflex markers. Without markers, SetSleepMode's halved
        // minimumIntervalUs causes the driver to throttle at render rate
        // instead of output rate. Let DoTimingFallback handle pacing instead.
        if (vk_native)
            DoReflexSleep();
    } else if (ReflexActive() && dev_) {
        if (g_game_uses_reflex.load(std::memory_order_relaxed)) {
            NvSleepParams p = BuildSleepParams();
            MaybeUpdateSleepMode(p);
        } else {
            DoReflexSleep();
        }
    }

    // Timing fallback as hard backstop
    // When Vulkan Reflex is active AND the game uses native markers,
    // vkLatencySleepNV + vkWaitSemaphores already blocks for the full
    // interval — skip DoTimingFallback to avoid double-sleep.
    // When VK Reflex is active but the game does NOT use native markers,
    // Sleep() was skipped above, so we need the timing fallback for pacing.
    if (!vk_native)
        DoTimingFallback();
}

// ============================================================================
// Streamline proxy present
// ============================================================================

void UlLimiter::OnSLPresent() {
    if (!g_cfg.use_sl_proxy.load(std::memory_order_relaxed)) return;
    if (!ReflexActive() || !dev_) return;
    DoReflexSleep();
}
