#pragma once
// Ultra Limiter — Frame limiter orchestrator
// Clean-room implementation from public API docs:
//   - NVAPI: SetSleepMode + Sleep for Reflex-based pacing, GetLatency for adaptive tuning
//   - Windows: QPC timing for fallback pacing
// Supports: Reflex sleep pacing, marker-based pacing, SL proxy pacing,
//           timing fallback, FG-aware rate division, adaptive interval,
//           auto enforcement site, render queue monitoring, adaptive FG offset,
//           cadence-based FG/MFG stabilization, pipeline-aware predictive sleep.

#include "ul_config.hpp"
#include "ul_reflex.hpp"

#include <algorithm>
#include <cstdint>
#include <windows.h>

// ============================================================================
// CadenceTracker — output present-to-present variance measurement
// ============================================================================
//
// Measures the actual output cadence (present-to-present deltas from
// GetLatency) and computes rolling standard deviation. This is the
// universal "pacing quality" signal that works across 1:1, FG, and MFG.
//
// Low variance = smooth output. High variance = jittery delivery.

struct CadenceTracker {
    static constexpr int kWindowSize = 32;
    float deltas_us[kWindowSize] = {};
    int head = 0;
    int count = 0;
    uint64_t last_present_time = 0;
    uint64_t last_fed_frame_id = 0;

    float variance_us2 = 0.0f;
    float stddev_us = 0.0f;
    float mean_delta_us = 0.0f;

    // Interval stabilization state (used under FG/MFG)
    float best_interval_us = 0.0f;
    float best_variance_us2 = 1e12f;
    int stable_streak = 0;
    int jitter_streak = 0;

    static constexpr float kLowVarianceRatio = 0.03f;   // 3% of target = smooth
    static constexpr float kHighVarianceRatio = 0.08f;   // 8% of target = jittery
    static constexpr int kStableThreshold = 15;
    static constexpr int kJitterThreshold = 5;

    void Feed(uint64_t present_end_time);
    void ComputeStats();
    void Reset();
};

// ============================================================================
// GPU statistics from GetLatency — drives all adaptive features
// ============================================================================

struct GpuStats {
    // Rolling averages (exponential moving average, α = 0.1)
    float avg_gpu_active_us = 0.0f;
    float avg_queue_time_us = 0.0f;
    float avg_fg_overhead_us = 0.0f;

    // Derived state
    float gpu_load_ratio = 0.0f;

    // Auto enforcement site — SIM_START (0) or PRESENT_FINISH (5)
    int auto_site = 5;  // default PRESENT_FINISH until we have data

    // Adaptive interval adjustment (µs added/subtracted from base interval)
    int32_t interval_adjust_us = 0;

    // Adaptive FG offset (replaces static kFGPacingOffsetUs when valid)
    int32_t adaptive_fg_offset_us = -1;

    // Queue pressure flag
    bool queue_pressure = false;

    // Validity
    bool valid = false;
    int samples = 0;

    static constexpr float kEmaAlpha = 0.1f;
    static constexpr int kMinSamples = 8;
};

// ============================================================================
// GpuPredictor — trend-aware GPU time prediction for predictive sleep
// ============================================================================

struct GpuPredictor {
    static constexpr int kHistorySize = 8;
    float history[kHistorySize] = {};
    int head = 0;
    int count = 0;

    int trend_dir = 0;
    int trend_len = 0;

    float predicted_us = 0.0f;
    bool active = false;

    float safety_margin_us = 500.0f;
    int frames_since_miss = 0;
    int miss_count = 0;

    static constexpr float kMinMargin = 200.0f;
    static constexpr float kMaxMargin = 2000.0f;
    static constexpr int kTrendThreshold = 3;

    // Output cadence tracking — universal pacing quality signal
    CadenceTracker cadence;

    // FG/MFG stabilization output: adjustment to apply to the base interval.
    // Positive = widen (back off), negative = tighten, 0 = hold.
    int32_t fg_cadence_adjust_us = 0;

    void Update(float gpu_active_us);
    void RecordMiss(bool missed);
    void UpdateCadenceResponse(bool fg_active, int fg_divisor, float target_interval_us);
    void Reset();
};

class UlLimiter {
public:
    void Init();
    void Shutdown();

    bool ConnectReflex(IUnknown* device);
    void SetHwnd(HWND hwnd) { hwnd_ = hwnd; }

    void OnPresent();
    void OnSLPresent();
    void OnMarker(int marker_type, uint64_t frame_id);

    const GpuStats& GetGpuStats() const { return gpu_stats_; }

private:
    float ComputeRenderFps() const;
    int DetectFGDivisor() const;

    void DoReflexSleep();
    void DoTimingFallback();
    void HandleDelayPresent(uint64_t frame_id);
    void HandleQueuedFrames(uint64_t frame_id, int max_q);

    NvSleepParams BuildSleepParams() const;
    bool MaybeUpdateSleepMode(const NvSleepParams& p);
    void UpdateGpuStats();
    int ResolveEnforcementSite() const;

    // Reset all adaptive state (predictors, EMAs, P2P, grid).
    // Called on settings change and on refocus from background.
    void ResetAdaptiveState();

    IUnknown* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    uint64_t frame_num_ = 0;

    HANDLE htimer_delay_ = nullptr;
    HANDLE htimer_fallback_ = nullptr;
    HANDLE htimer_queue_ = nullptr;

    // Phase-locked timing grid (ns)
    int64_t grid_epoch_ns_ = 0;
    int64_t grid_interval_ns_ = 0;
    int64_t grid_next_ns_ = 0;
    float last_fps_limit_ = 0.0f;

    // Present-to-present feedback loop
    float ptp_error_accum_us_ = 0.0f;
    int32_t ptp_correction_us_ = 0;
    int ptp_sample_count_ = 0;
    int64_t last_ptp_present_ns_ = 0;

    // Changed-params optimization
    NvSleepParams last_sleep_params_{};
    bool last_sleep_valid_ = false;

    // GPU statistics + predictor
    GpuStats gpu_stats_;
    GpuPredictor gpu_predictor_;
    NvLatencyResult* latency_buf_ = nullptr;
    uint64_t last_pred_frame_id_ = 0;

    // Time-based warmup
    int64_t warmup_start_qpc_ = 0;
    bool warmup_done_ = false;
    static constexpr int64_t kWarmupDurationSec = 2;
};
