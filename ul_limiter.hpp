#pragma once
// Ultra Limiter — Frame limiter orchestrator
// Clean-room implementation from public API docs:
//   - NVAPI: SetSleepMode + Sleep for Reflex-based pacing, GetLatency for adaptive tuning
//   - Windows: QPC timing for fallback pacing
// Supports: Reflex sleep pacing, marker-based pacing, SL proxy pacing,
//           timing fallback, FG-aware rate division, adaptive interval,
//           auto enforcement site, render queue monitoring, adaptive FG offset.

#include "ul_config.hpp"
#include "ul_reflex.hpp"

#include <cstdint>
#include <windows.h>

// ============================================================================
// GPU statistics from GetLatency — drives adaptive features
// ============================================================================

struct GpuStats {
    // Rolling averages (exponential moving average, α = 0.1)
    float avg_gpu_active_us = 0.0f;     // gpuActiveRenderTimeUs
    float avg_queue_time_us = 0.0f;     // osRenderQueueEnd - osRenderQueueStart
    float avg_fg_overhead_us = 0.0f;    // presentEnd - gpuRenderEnd (when FG active)

    // Derived state
    float gpu_load_ratio = 0.0f;        // avg_gpu_active / target_interval (0..1+)

    // Auto enforcement site — stores a LatencyMarker enum value:
    //   SIM_START (0) when GPU-bound → lowest latency
    //   PRESENT_FINISH (5) when CPU-bound → best frame pacing
    int auto_site = 5;                  // default PRESENT_FINISH until we have data

    // Adaptive interval adjustment (µs added/subtracted from base interval)
    int32_t interval_adjust_us = 0;

    // Adaptive FG offset (replaces static kFGPacingOffsetUs when valid)
    int32_t adaptive_fg_offset_us = -1; // -1 = not yet computed, use static default

    // Queue pressure flag — true when render queue is growing
    bool queue_pressure = false;

    // Validity
    bool valid = false;
    int samples = 0;

    static constexpr float kEmaAlpha = 0.1f;
    static constexpr int kMinSamples = 8;  // need this many before trusting data
};

// ============================================================================
// Predictive sleep — GPU-aware wake scheduling
// ============================================================================
//
// Instead of a static minimumIntervalUs derived from fps_limit, predictive
// sleep computes a per-frame interval based on how long the GPU is expected
// to take. This maximizes GPU utilization (less idle time between render
// finish and present) while keeping frame times consistent.
//
// The predictor tracks recent GPU active render times from GetLatency and
// detects trends (3+ frames moving in the same direction). Single-frame
// spikes are ignored — the safety margin absorbs those. Only trends
// (camera pans, entering heavier areas) shift the prediction.
//
// An adaptive safety margin starts conservative (500µs) and tightens when
// predictions are accurate, widens when misses occur.
//
// Only active when GPU-bound (auto_site == SIM_START). When CPU-bound the
// GPU isn't the bottleneck, so predictive sleep wouldn't help.

struct GpuPredictor {
    // Recent GPU active render times (µs) — circular buffer from GetLatency
    static constexpr int kHistorySize = 8;
    float history[kHistorySize] = {};
    int head = 0;           // next write index
    int count = 0;          // how many valid entries (up to kHistorySize)

    // Trend detection: count of consecutive frames moving in the same direction
    int trend_dir = 0;      // +1 = rising, -1 = falling, 0 = flat
    int trend_len = 0;      // how many consecutive frames in this direction

    // Prediction output
    float predicted_us = 0.0f;  // predicted GPU time for next frame (µs)
    bool active = false;        // true when we have enough data and are GPU-bound

    // Adaptive safety margin (µs)
    // Starts at 500µs. Widens by 50µs per miss, tightens by 10µs per stable
    // window (30 frames with no miss). Clamped to [200, 2000] µs.
    float safety_margin_us = 500.0f;
    int frames_since_miss = 0;
    int miss_count = 0;

    static constexpr float kMinMargin = 200.0f;
    static constexpr float kMaxMargin = 2000.0f;
    static constexpr int kTrendThreshold = 3;  // frames before trend is trusted

    // Push a new GPU time sample and update the prediction.
    void Update(float gpu_active_us);

    // Record whether the last frame was a miss (actual > predicted + margin).
    // Called from UpdateGpuStats when new GetLatency data arrives.
    void RecordMiss(bool missed);

    // Reset state (e.g. on FPS limit change)
    void Reset();
};

class UlLimiter {
public:
    void Init();
    void Shutdown();

    // Connect to the D3D device and install Reflex hooks
    bool ConnectReflex(IUnknown* device);

    // Set the game window handle (needed for VRR monitor detection)
    void SetHwnd(HWND hwnd) { hwnd_ = hwnd; }

    // Called from ReShade present callback (pre-present)
    void OnPresent();

    // Called from Streamline proxy present detour
    void OnSLPresent();

    // Called from SetLatencyMarker detour via callback
    void OnMarker(int marker_type, uint64_t frame_id);

    // Read-only access to GPU stats for OSD display
    const GpuStats& GetGpuStats() const { return gpu_stats_; }

private:
    float ComputeRenderFps() const;
    int DetectFGDivisor() const;

    void DoReflexSleep();
    void DoTimingFallback();
    void HandleDelayPresent(uint64_t frame_id);
    void HandleQueuedFrames(uint64_t frame_id, int max_q);

    // Build a NvSleepParams from current config + game state.
    // Uses adaptive interval and FG offset when GPU stats are valid.
    NvSleepParams BuildSleepParams() const;

    // Only call SetSleepMode when params actually changed (SK-style optimization).
    // Returns true if params were sent to the driver.
    bool MaybeUpdateSleepMode(const NvSleepParams& p);

    // Poll GetLatency and update GPU stats (adaptive interval, enforcement site,
    // queue pressure, FG offset). Called every 2 frames from OnPresent.
    void UpdateGpuStats();

    // Determine the enforcement site for the current frame.
    // Returns SIM_START or PRESENT_FINISH based on auto-detection or config.
    int ResolveEnforcementSite() const;

    IUnknown* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    uint64_t frame_num_ = 0;

    // Reusable timer handles
    HANDLE htimer_delay_ = nullptr;
    HANDLE htimer_fallback_ = nullptr;
    HANDLE htimer_queue_ = nullptr;    // for hybrid sleep in HandleQueuedFrames

    // Fallback: phase-locked timing grid for consistent frame pacing (ns)
    // Instead of advancing target relative to previous target (which drifts),
    // we lock to a fixed grid: target = epoch + k * interval.
    int64_t grid_epoch_ns_ = 0;     // timestamp of the first frame on this grid
    int64_t grid_interval_ns_ = 0;  // current grid spacing (from fps_limit)
    int64_t grid_next_ns_ = 0;      // next grid slot to sleep until
    float last_fps_limit_ = 0.0f;   // detect FPS limit changes to reset grid

    // Present-to-present feedback loop — closed-loop correction for Reflex interval.
    // Measures actual present cadence vs target and nudges minimumIntervalUs to
    // compensate for systematic driver over/undershoot. Neither DC nor SK do this.
    float ptp_error_accum_us_ = 0.0f;  // accumulated signed error (µs)
    int32_t ptp_correction_us_ = 0;    // current correction applied to interval (clamped)
    int ptp_sample_count_ = 0;         // frames since last correction update
    int64_t last_ptp_present_ns_ = 0;  // previous present timestamp for P2P measurement

    // Changed-params optimization: last params sent to the driver.
    // Avoids calling SetSleepMode every frame when nothing changed.
    NvSleepParams last_sleep_params_{};
    bool last_sleep_valid_ = false;

    // GPU statistics from GetLatency — drives all adaptive features
    GpuStats gpu_stats_;
    GpuPredictor gpu_predictor_;  // predictive sleep — GPU-aware wake scheduling
    NvLatencyResult* latency_buf_ = nullptr;  // heap-allocated to avoid 10KB+ on stack
    uint64_t last_pred_frame_id_ = 0;         // last frameID fed to predictor (dedup)

    // Time-based warmup: 2 seconds from first present, consistent across frame rates
    int64_t warmup_start_qpc_ = 0;
    bool warmup_done_ = false;
    static constexpr int64_t kWarmupDurationSec = 2;  // seconds before limiting kicks in
};
