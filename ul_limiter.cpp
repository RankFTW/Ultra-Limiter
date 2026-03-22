// Ultra Limiter — Frame limiter implementation
// Clean-room from public API docs:
//   - NVAPI SDK (MIT): NvAPI_D3D_SetSleepMode params, NvAPI_D3D_Sleep
//   - Windows: GetModuleHandleW for DLL detection, QPC for timing
// No code from Display Commander or any other project.

#include "ul_limiter.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"

#include <algorithm>
#include <cmath>

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
            // Check if DLSS Frame Generation DLL is loaded
            if (GetModuleHandleW(L"nvngx_dlssg.dll")) return 2;
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

void UlLimiter::Init() {}

void UlLimiter::Shutdown() {
    // Restore game's sleep mode before unhooking
    if (dev_ && ReflexActive()) {
        NvSleepParams p = {};
        p.version = NV_SLEEP_PARAMS_VER;
        InvokeSetSleepMode(dev_, &p);
    }
    TeardownReflexHooks();
    dev_ = nullptr;

    if (htimer_delay_) { CloseHandle(htimer_delay_); htimer_delay_ = nullptr; }
    if (htimer_fallback_) { CloseHandle(htimer_fallback_); htimer_fallback_ = nullptr; }
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
// Reflex Sleep pacing — configure SetSleepMode then call Sleep
// ============================================================================

void UlLimiter::DoReflexSleep() {
    if (!ReflexActive() || !dev_) return;

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

    // Frame rate interval
    p.minimumIntervalUs = (render_fps > 0.0f)
        ? static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(render_fps)))
        : 0;

    InvokeSetSleepMode(dev_, &p);
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

    int64_t start = ul_timing::NowNs();
    constexpr int64_t kTimeout = 50'000'000LL;  // 50ms

    while (g_ring[prev].timestamp_ns[SIM_START].load(std::memory_order_relaxed)
           > g_ring[prev].timestamp_ns[PRESENT_BEGIN].load(std::memory_order_relaxed)) {
        if (ul_timing::NowNs() - start > kTimeout) break;
        YieldProcessor();
    }
}

// ============================================================================
// Marker-based pacing — called from SetLatencyMarker detour
// ============================================================================

void UlLimiter::OnMarker(int marker_type, uint64_t frame_id) {
    if (!ReflexActive() || !dev_) return;
    if (frame_num_ < kWarmup) return;

    // When Smooth Motion is active, all pacing is handled by DoTimingFallback
    // in OnPresent. Reflex Sleep in marker callbacks would conflict with the
    // driver's frame interpolation.
    if (s_smooth_motion.load(std::memory_order_relaxed)) return;

    ExpandedSettings es = ExpandPreset();
    if (!es.use_marker_pacing) return;

    bool sim_only = es.sim_start_only && (es.max_queued_frames == 0);

    if (sim_only) {
        if (marker_type == SIM_START) DoReflexSleep();
    } else {
        // Only pace on real (non-generated) frames.
        // Generated frames (from DLSS FG / smooth motion) fire PRESENT markers
        // but never SIM_START. Check the ring buffer to see if this frame has a
        // SIM_START timestamp — if not, it's a generated frame and we skip pacing.
        size_t slot = static_cast<size_t>(frame_id % kRingSize);
        bool is_real_frame = (g_ring[slot].frame_id.load(std::memory_order_relaxed) == frame_id)
                          && (g_ring[slot].timestamp_ns[SIM_START].load(std::memory_order_relaxed) > 0);

        if (marker_type == PRESENT_BEGIN && is_real_frame) {
            HandleQueuedFrames(frame_id, es.max_queued_frames);
            HandleDelayPresent(frame_id);
        }
        if (marker_type == PRESENT_FINISH && is_real_frame) {
            DoReflexSleep();
        }
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
    int64_t now = ul_timing::NowNs();

    if (next_target_ns_ <= 0) next_target_ns_ = now;
    next_target_ns_ += frame_ns;

    // If behind, reset to avoid burst catch-up
    if (next_target_ns_ < now) next_target_ns_ = now + frame_ns;

    ul_timing::SleepUntilNs(next_target_ns_, htimer_fallback_);
}

// ============================================================================
// OnPresent — main entry from ReShade present callback
// ============================================================================

void UlLimiter::OnPresent() {
    frame_num_++;

    if (frame_num_ < kWarmup) {
        if (frame_num_ == 1) ul_log::Write("OnPresent: warmup (%llu frames)", kWarmup);
        return;
    }
    if (frame_num_ == kWarmup) ul_log::Write("OnPresent: warmup done, limiting active");

    // Periodically check for Smooth Motion (driver-level FG)
    int64_t now_qpc = ul_timing::NowQpc();
    if (now_qpc - s_sm_check_qpc > ul_timing::g_qpc_freq * 2) {
        bool prev = s_smooth_motion.load(std::memory_order_relaxed);
        bool cur = DetectSmoothMotion();
        if (cur != prev) {
            s_smooth_motion.store(cur, std::memory_order_relaxed);
            ul_log::Write("Smooth Motion: %s", cur ? "detected" : "not detected");
        }
        s_sm_check_qpc = now_qpc;
    }

    // When Smooth Motion is active, Reflex Sleep conflicts with the driver's
    // frame interpolation timing. Use timing-only pacing instead.
    if (s_smooth_motion.load(std::memory_order_relaxed)) {
        DoTimingFallback();
        return;
    }

    // Reflex path
    if (ReflexActive() && dev_) {
        ExpandedSettings es = ExpandPreset();
        if (es.use_marker_pacing && g_game_uses_reflex.load(std::memory_order_relaxed)) {
            // Marker pacing handles sleep in OnMarker — only update sleep mode params here.
            // Do NOT call InvokeSleep, that would double-pace.
            float fps = ComputeRenderFps();
            const auto& gs = GetGameState();
            NvSleepParams p = {};
            p.version = NV_SLEEP_PARAMS_VER;
            p.bLowLatencyMode = 1;

            BoostMode bm = g_cfg.boost.load(std::memory_order_relaxed);
            if (bm == BoostMode::ForceOn) p.bLowLatencyBoost = 1;
            else if (bm == BoostMode::ForceOff) p.bLowLatencyBoost = 0;
            else if (gs.captured.load(std::memory_order_acquire))
                p.bLowLatencyBoost = gs.boost.load(std::memory_order_relaxed) ? 1 : 0;

            if (gs.captured.load(std::memory_order_acquire))
                p.bUseMarkersToOptimize = gs.use_markers.load(std::memory_order_relaxed) ? 1 : 0;

            p.minimumIntervalUs = (fps > 0.0f)
                ? static_cast<NvU32>(std::round(1e6 / static_cast<double>(fps))) : 0;

            InvokeSetSleepMode(dev_, &p);
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
