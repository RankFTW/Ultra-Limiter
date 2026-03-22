#pragma once
// Ultra Limiter — Frame limiter orchestrator
// Clean-room implementation from public API docs:
//   - NVAPI: SetSleepMode + Sleep for Reflex-based pacing
//   - Windows: QPC timing for fallback pacing
// Supports: Reflex sleep pacing, marker-based pacing, SL proxy pacing,
//           timing fallback, FG-aware rate division.

#include "ul_config.hpp"
#include "ul_reflex.hpp"

#include <cstdint>
#include <windows.h>

class UlLimiter {
public:
    void Init();
    void Shutdown();

    // Connect to the D3D device and install Reflex hooks
    bool ConnectReflex(IUnknown* device);

    // Called from ReShade present callback (pre-present)
    void OnPresent();

    // Called from Streamline proxy present detour
    void OnSLPresent();

    // Called from SetLatencyMarker detour via callback
    void OnMarker(int marker_type, uint64_t frame_id);

private:
    float ComputeRenderFps() const;
    int DetectFGDivisor() const;

    void DoReflexSleep();
    void DoTimingFallback();
    void HandleDelayPresent(uint64_t frame_id);
    void HandleQueuedFrames(uint64_t frame_id, int max_q);

    IUnknown* dev_ = nullptr;
    uint64_t frame_num_ = 0;

    // Reusable timer handles
    HANDLE htimer_delay_ = nullptr;
    HANDLE htimer_fallback_ = nullptr;

    // Fallback: next target present time (ns)
    int64_t next_target_ns_ = 0;

    static constexpr uint64_t kWarmup = 300;
};
