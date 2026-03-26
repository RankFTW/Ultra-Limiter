#pragma once
// ReLimiter — Frame limiter orchestrator
// Clean-room implementation from public API docs:
//   - NVAPI: SetSleepMode + Sleep for Reflex-based pacing, GetLatency for adaptive tuning
//   - Windows: QPC timing for fallback pacing
// Supports: Reflex sleep pacing, marker-based pacing, SL proxy pacing,
//           timing fallback, FG-aware rate division, adaptive interval,
//           auto enforcement site, render queue monitoring, adaptive FG offset,
//           pipeline-aware predictive sleep, bottleneck detection, dynamic
//           queue depth, dynamic Boost, cadence-based FG/MFG stabilization.

#include "ul_config.hpp"
#include "ul_reflex.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <windows.h>

// Forward declaration
#ifdef _WIN64
class VkReflex;
#endif

// ============================================================================
// Bottleneck identification
// ============================================================================

enum class Bottleneck : uint8_t {
    None = 0,
    Gpu,
    CpuSim,
    CpuSubmit,
    Driver,
    Queue,
};

// ============================================================================
// Dynamic queue depth — voting-window based adaptation
// ============================================================================

struct DynamicPacing {
    bool use_markers = true;
    int queue_depth = 1;       // 1-3, dynamically chosen
    int64_t last_change_qpc = 0;

    static constexpr int kMaxVoteWindow = 240;
    static constexpr int kMinVoteWindow = 30;
    static constexpr float kTargetWindowSeconds = 2.0f;
    int vote_history[kMaxVoteWindow] = {};
    int vote_head = 0;
    int vote_count = 0;
    int vote_window_size = 60;

    static constexpr int64_t kHysteresisSeconds = 4;

    void ResizeWindow(float fps_limit) {
        int polls_per_sec = static_cast<int>(fps_limit / 2.0f);
        vote_window_size = (std::max)(kMinVoteWindow,
                           (std::min)(kMaxVoteWindow,
                           static_cast<int>(polls_per_sec * kTargetWindowSeconds)));
        vote_head = 0;
        vote_count = 0;
    }

    void Reset() {
        queue_depth = 1;
        last_change_qpc = 0;
        vote_head = 0;
        vote_count = 0;
    }
};

// ============================================================================
// Pipeline statistics from GetLatency — drives all adaptive features
// ============================================================================

struct PipelineStats {
    // Per-stage EMAs (µs)
    float avg_sim_us = 0.0f;
    float avg_render_submit_us = 0.0f;
    float avg_driver_us = 0.0f;
    float avg_queue_us = 0.0f;
    float avg_gpu_active_us = 0.0f;
    float avg_fg_overhead_us = 0.0f;

    // Pipeline-level metrics
    float avg_gpu_idle_gap_us = 0.0f;
    float avg_input_to_display_us = 0.0f;
    float avg_pipeline_total_us = 0.0f;

    // Derived state
    float gpu_load_ratio = 0.0f;
    float pipeline_load_ratio = 0.0f;

    int auto_site = 5;  // PRESENT_FINISH default
    int32_t interval_adjust_us = 0;
    int32_t adaptive_fg_offset_us = -1;
    bool queue_pressure = false;

    Bottleneck bottleneck = Bottleneck::None;
    DynamicPacing pacing;

    bool valid = false;
    int samples = 0;

    static constexpr float kEmaAlpha = 0.1f;
    static constexpr int kMinSamples = 8;
};

// ============================================================================
// StagePredictor — trend-aware per-stage time prediction
// ============================================================================

struct StagePredictor {
    static constexpr int kHistorySize = 8;
    float history[kHistorySize] = {};
    int head = 0;
    int count = 0;

    int trend_dir = 0;
    int trend_len = 0;

    float predicted_us = 0.0f;

    static constexpr int kTrendThreshold = 3;

    void Update(float sample_us);
    void Reset();
};

// ============================================================================
// CadenceTracker — output present-to-present variance measurement
// ============================================================================

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

    void Feed(uint64_t present_end_time);
    void ComputeStats();
    float ComputeCV() const;
    void Reset();
};

// ============================================================================
// QPCCadenceMonitor — local QPC present-to-present variance (QPC Brake signal)
// ============================================================================

struct QPCCadenceMonitor {
    static constexpr int kWindowSize = 16;
    int64_t last_qpc = 0;
    float deltas_us[kWindowSize] = {};
    int head = 0;
    int count = 0;

    void Feed(int64_t now_qpc);
    float ComputeCV() const;
    void Reset();
};

// ============================================================================
// ConsistencyBuffer — two-mode state machine for adaptive consistency buffer
// ============================================================================

struct ConsistencyBuffer {
    enum class Mode : uint8_t { Tighten, Stabilize };

    Mode mode = Mode::Stabilize;
    int32_t buffer_us = 0;
    int consecutive_stable_ticks = 0;

    struct TuningParams {
        int32_t initial_buffer_us;
        int32_t max_buffer_us;
        int32_t stabilize_step_us;
        int32_t tighten_step_us;
        float stability_threshold_cv;
        float instability_threshold_cv;
        int stable_ticks_to_tighten;
        float qpc_brake_threshold_cv;
    };

    static constexpr TuningParams kTier1x1      = {4,  20, 2, 1, 0.03f, 0.08f, 15, 0.12f};
    static constexpr TuningParams kTier2xFG     = {12, 50, 4, 2, 0.03f, 0.08f, 12, 0.10f};
    static constexpr TuningParams kTier3xMFG    = {20, 80, 6, 3, 0.04f, 0.10f, 10, 0.10f};
    static constexpr TuningParams kTier4xPlusMFG = {30, 120, 8, 4, 0.05f, 0.12f, 8, 0.10f};

    TuningParams active_params = kTier1x1;

    void Tick(float cadence_cv, float qpc_cv, float vrr_proximity);
    void SelectTier(int fg_divisor);
    void Reset(int fg_divisor);
};

// ============================================================================
// CSVLogger — diagnostic CSV writer for consistency buffer state
// ============================================================================

struct CSVLogger {
    FILE* file = nullptr;
    bool enabled = false;
    bool header_written = false;

    void Open(HMODULE addon_module);
    void WriteRow(int64_t timestamp_qpc,
                  ConsistencyBuffer::Mode mode,
                  int32_t buffer_us,
                  float cadence_cv,
                  float qpc_cv,
                  float gpu_load_ratio,
                  int fg_tier,
                  float vrr_proximity,
                  NvU32 final_interval_us);
    void Close();
};

struct PipelinePredictor {
    StagePredictor gpu;
    StagePredictor sim;
    StagePredictor fg;

    float predicted_total_us = 0.0f;
    bool active = false;

    float safety_margin_us = 500.0f;
    int frames_since_miss = 0;
    int miss_count = 0;

    static constexpr float kMinMargin = 200.0f;
    static constexpr float kMaxMargin = 2000.0f;

    CadenceTracker cadence;

    void UpdatePrediction(bool fg_active);
    void RecordMiss(bool missed);
    void Reset();
};

// ============================================================================
// BoostController — dynamic Reflex Boost management
// ============================================================================

struct BoostController {
    enum class Decision : uint8_t { On, Off, Defer };

    Decision current = Decision::Defer;
    int64_t last_change_qpc = 0;

    float avg_gpu_after_idle_us = 0.0f;
    float avg_gpu_steady_us = 0.0f;
    int idle_frame_count = 0;
    int steady_frame_count = 0;
    int feed_count = 0;

    bool thermal_suspect = false;
    int64_t thermal_suspect_start_qpc = 0;

    static constexpr int64_t kHysteresisSeconds = 6;
    static constexpr float kIdleGapThresholdUs = 2000.0f;
    static constexpr float kRampThreshold = 0.10f;

    Decision Evaluate(const PipelineStats& ps);
    void FeedIdleGap(float gpu_idle_gap_us, float gpu_active_us);
    void CheckThermal(const StagePredictor& gpu_pred, const StagePredictor& sim_pred);
    void Reset();
};

class UlLimiter {
public:
    void Init(HMODULE addon_module = nullptr);
    void Shutdown();

    bool ConnectReflex(IUnknown* device);
#ifdef _WIN64
    void ConnectVulkanReflex(VkReflex* vk);
#endif
    void SetHwnd(HWND hwnd) { hwnd_ = hwnd; }

    void OnPresent();
    void OnSLPresent();
    void OnMarker(int marker_type, uint64_t frame_id);

    const PipelineStats& GetPipelineStats() const { return pipeline_stats_; }

private:
    float ComputeRenderFps() const;
    int DetectFGDivisor() const;

    void DoReflexSleep();
    void DoTimingFallback();
    void HandleDelayPresent(uint64_t frame_id);
    void HandleQueuedFrames(uint64_t frame_id, int max_q);

    NvSleepParams BuildSleepParams() const;
    bool MaybeUpdateSleepMode(const NvSleepParams& p);
    void UpdatePipelineStats();
    void ResetAdaptiveState();
    int ResolveEnforcementSite() const;

    IUnknown* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    uint64_t frame_num_ = 0;
#ifdef _WIN64
    VkReflex* vk_reflex_ = nullptr;  // non-null when Vulkan backend is active
#endif

    HANDLE htimer_delay_ = nullptr;
    HANDLE htimer_fallback_ = nullptr;
    HANDLE htimer_queue_ = nullptr;

    int64_t grid_epoch_ns_ = 0;
    int64_t grid_interval_ns_ = 0;
    int64_t grid_next_ns_ = 0;
    float last_fps_limit_ = 0.0f;

    float ptp_error_accum_us_ = 0.0f;
    int32_t ptp_correction_us_ = 0;
    int ptp_sample_count_ = 0;
    int64_t last_ptp_present_ns_ = 0;

    NvSleepParams last_sleep_params_{};
    bool last_sleep_valid_ = false;

    PipelineStats pipeline_stats_;
    PipelinePredictor pipeline_predictor_;
    BoostController boost_ctrl_;
    NvLatencyResult* latency_buf_ = nullptr;
    uint64_t last_pred_frame_id_ = 0;

    // Adaptive consistency buffer components
    QPCCadenceMonitor qpc_monitor_;
    ConsistencyBuffer consistency_buf_;
    bool gpu_load_gate_open_ = false;
    int last_fg_tier_ = 0;
    int pending_fg_tier_ = 0;
    int fg_tier_confirm_ticks_ = 0;
    CSVLogger csv_logger_;

    int64_t warmup_start_qpc_ = 0;
    bool warmup_done_ = false;
    static constexpr int64_t kWarmupDurationSec = 2;

    bool is_background_ = false;
};
