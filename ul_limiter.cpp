// ReLimiter — Frame limiter implementation
// Clean-room from public API docs:
//   - NVAPI SDK (MIT): NvAPI_D3D_SetSleepMode params, NvAPI_D3D_Sleep,
//     NV_GET_ADAPTIVE_SYNC_DATA, NV_SET_ADAPTIVE_SYNC_DATA, NV_GET_VRR_INFO
//   - Windows: GetModuleHandleW for DLL detection, QPC for timing,
//              EnumDisplaySettings / MonitorFromWindow for refresh rate
// No code from any other project.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ul_limiter.hpp"
#include "ul_log.hpp"
#include "ul_fg.hpp"
#include "ul_timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dxgi.h>
#include <memory>
#include <new>

// ============================================================================
// IsRealFrame — true when a GetLatency frame report represents a real rendered
// frame rather than a generated (FG) frame.
//
// Generated frames either have no sim phase at all (simStartTime == 0) or
// duplicate the previous real frame's sim timestamps (simEndTime <= simStartTime).
// Real frames always have a distinct, non-zero sim phase.
// ============================================================================

static inline bool IsRealFrame(const NvLatencyFrameReport& f) {
    return f.simStartTime > 0 && f.simEndTime > f.simStartTime;
}

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
    cadence.Reset();
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

    // First pass: compute raw stddev for outlier detection
    float var_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float d = deltas_us[(head - count + i + kWindowSize) % kWindowSize] - mean_delta_us;
        var_sum += d * d;
    }
    float raw_stddev = std::sqrt(var_sum / static_cast<float>(count));

    // Second pass: exclude outliers > 3σ from mean, recompute stats
    // This prevents a single anomalous frame from polluting the window.
    float threshold = raw_stddev * 3.0f;
    if (threshold > 0.0f && count >= 8) {
        float filt_sum = 0.0f;
        int filt_count = 0;
        for (int i = 0; i < count; i++) {
            float v = deltas_us[(head - count + i + kWindowSize) % kWindowSize];
            if (std::abs(v - mean_delta_us) <= threshold) {
                filt_sum += v;
                filt_count++;
            }
        }
        // Only use filtered stats if we didn't reject too many samples
        // (rejecting > 25% suggests real instability, not outliers)
        if (filt_count >= count * 3 / 4 && filt_count >= 4) {
            float filt_mean = filt_sum / static_cast<float>(filt_count);
            float filt_var = 0.0f;
            for (int i = 0; i < count; i++) {
                float v = deltas_us[(head - count + i + kWindowSize) % kWindowSize];
                if (std::abs(v - mean_delta_us) <= threshold) {
                    float d = v - filt_mean;
                    filt_var += d * d;
                }
            }
            mean_delta_us = filt_mean;
            variance_us2 = filt_var / static_cast<float>(filt_count);
            stddev_us = std::sqrt(variance_us2);
            return;
        }
    }

    // Fallback: use unfiltered stats (small window or no outliers)
    variance_us2 = var_sum / static_cast<float>(count);
    stddev_us = std::sqrt(variance_us2);
}

void CadenceTracker::Reset() {
    head = 0; count = 0; last_present_time = 0; last_fed_frame_id = 0;
    variance_us2 = 0; stddev_us = 0; mean_delta_us = 0;
}

float CadenceTracker::ComputeCV() const {
    return (count >= 4 && mean_delta_us > 0.0f) ? stddev_us / mean_delta_us : 0.0f;
}

// ============================================================================
// QPCCadenceMonitor — local QPC present-to-present variance (QPC Brake signal)
// ============================================================================

void QPCCadenceMonitor::Feed(int64_t now_qpc) {
    if (last_qpc != 0 && now_qpc > last_qpc) {
        int64_t delta = now_qpc - last_qpc;
        float delta_us = static_cast<float>(
            static_cast<double>(delta) * 1000000.0 / static_cast<double>(ul_timing::g_qpc_freq));
        deltas_us[head] = delta_us;
        head = (head + 1) % kWindowSize;
        if (count < kWindowSize) count++;
    }
    last_qpc = now_qpc;
}

float QPCCadenceMonitor::ComputeCV() const {
    if (count < 4) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < count; i++)
        sum += deltas_us[(head - count + i + kWindowSize) % kWindowSize];
    float mean = sum / static_cast<float>(count);
    if (mean == 0.0f) return 0.0f;
    float var_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float d = deltas_us[(head - count + i + kWindowSize) % kWindowSize] - mean;
        var_sum += d * d;
    }
    float stddev = std::sqrt(var_sum / static_cast<float>(count));
    return stddev / mean;
}

float QPCCadenceMonitor::ComputeStddev() const {
    if (count < 4) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < count; i++)
        sum += deltas_us[(head - count + i + kWindowSize) % kWindowSize];
    float mean = sum / static_cast<float>(count);
    if (mean == 0.0f) return 0.0f;
    float var_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float d = deltas_us[(head - count + i + kWindowSize) % kWindowSize] - mean;
        var_sum += d * d;
    }
    return std::sqrt(var_sum / static_cast<float>(count));
}

void QPCCadenceMonitor::Reset() {
    last_qpc = 0;
    head = 0;
    count = 0;
    std::memset(deltas_us, 0, sizeof(deltas_us));
}

// ============================================================================
// ConsistencyBuffer — two-mode state machine for adaptive consistency buffer
// ============================================================================

void ConsistencyBuffer::SelectTier(int fg_divisor) {
    if (fg_divisor >= 4)
        active_params = kTier4xPlusMFG;
    else if (fg_divisor >= 3)
        active_params = kTier3xMFG;
    else if (fg_divisor == 2)
        active_params = kTier2xFG;
    else
        active_params = kTier1x1;
}

void ConsistencyBuffer::Reset(int fg_divisor) {
    SelectTier(fg_divisor);
    mode = Mode::Stabilize;
    buffer_us = active_params.initial_buffer_us;
    consecutive_stable_ticks = 0;
}

void ConsistencyBuffer::Tick(float cadence_stddev_us, float qpc_cv, float qpc_stddev_us,
                             float vrr_proximity, bool fg_active,
                             float render_interval_us) {
    // QPC brake: immediate STABILIZE on QPC variance spike.
    //
    // Under FG the QPC monitor measures present-to-present (output) intervals,
    // but the alternating real/generated cadence inflates the raw CV.  Instead
    // of disabling the brake, re-normalise: compare QPC stddev against the
    // render interval so it still catches real submission jitter without
    // false-triggering on the normal FG interleaving pattern.
    float effective_cv = qpc_cv;
    if (fg_active && render_interval_us > 0.0f && qpc_stddev_us > 0.0f) {
        effective_cv = qpc_stddev_us / render_interval_us;
    }
    if (effective_cv > active_params.qpc_brake_threshold_cv) {
        mode = Mode::Stabilize;
        consecutive_stable_ticks = 0;
    }

    if (mode == Mode::Stabilize) {
        if (cadence_stddev_us > active_params.instability_threshold_us) {
            // Unstable: increase buffer
            buffer_us += active_params.stabilize_step_us;
            consecutive_stable_ticks = 0;
        } else if (cadence_stddev_us < active_params.stability_threshold_us) {
            // Stable tick
            consecutive_stable_ticks++;
            if (consecutive_stable_ticks >= active_params.stable_ticks_to_tighten) {
                mode = Mode::Tighten;
                consecutive_stable_ticks = 0;
            }
        } else {
            // In between thresholds: reset stable counter but don't increase buffer
            consecutive_stable_ticks = 0;
        }
    } else {
        // TIGHTEN mode
        if (cadence_stddev_us > active_params.instability_threshold_us) {
            // Instability detected: transition to STABILIZE
            mode = Mode::Stabilize;
            consecutive_stable_ticks = 0;
        } else {
            // Stable: decrease buffer with VRR proximity scaling
            int32_t effective_step = active_params.tighten_step_us;
            if (vrr_proximity > 0.90f) {
                effective_step = effective_step / 2;  // halve near VRR ceiling
            }
            buffer_us -= effective_step;
        }
    }

    // Final clamp
    if (buffer_us < active_params.min_buffer_us) buffer_us = active_params.min_buffer_us;
    if (buffer_us > active_params.max_buffer_us) buffer_us = active_params.max_buffer_us;
}

// ============================================================================
// DiagCSVLogger — expanded per-frame diagnostic CSV (18 columns)
// ============================================================================

void DiagCSVLogger::Open(HMODULE addon_module) {
    if (!addon_module) {
        ul_log::Write("DiagCSVLogger: null module handle, disabling");
        enabled = false;
        return;
    }

    // Try game exe directory first (same as INI path), fall back to addon DLL directory
    wchar_t wpath[MAX_PATH] = {};
    bool found = false;

    // Attempt 1: next to game exe
    if (GetModuleFileNameW(nullptr, wpath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"relimiter_diagnostics.csv");
            file = _wfopen(wpath, L"w");
            if (file) found = true;
        }
    }

    // Attempt 2: next to addon DLL
    if (!found) {
        wpath[0] = L'\0';
        if (GetModuleFileNameW(addon_module, wpath, MAX_PATH)) {
            wchar_t* slash = wcsrchr(wpath, L'\\');
            if (slash)
                wcscpy(slash + 1, L"relimiter_diagnostics.csv");
            else
                wcscpy(wpath, L"relimiter_diagnostics.csv");
            file = _wfopen(wpath, L"w");
            if (file) found = true;
        }
    }

    if (!file) {
        ul_log::Write("DiagCSVLogger: failed to open CSV file (err=%lu), disabling",
                      GetLastError());
        enabled = false;
        return;
    }

    fprintf(file, "timestamp,smoothness,cadence_stddev_us,cadence_mean_delta_us,"
                  "cadence_cv,qpc_cv,qpc_cv_render,gsync_active,cb_mode,cb_buffer_us,"
                  "pred_gpu_us,pred_sim_us,pred_fg_us,enforcement_site,"
                  "queue_depth,fg_tier,vrr_proximity,gpu_load_ratio,final_interval_us\n");
    fflush(file);
    header_written = true;
    enabled = true;
}

void DiagCSVLogger::WriteRow(int64_t timestamp_qpc,
                             float smoothness,
                             float cadence_stddev_us,
                             float cadence_mean_delta_us,
                             float cadence_cv,
                             float qpc_cv,
                             float qpc_cv_render,
                             bool gsync_active,
                             ConsistencyBuffer::Mode cb_mode,
                             int32_t cb_buffer_us,
                             float pred_gpu_us,
                             float pred_sim_us,
                             float pred_fg_us,
                             int enforcement_site,
                             int queue_depth,
                             int fg_tier,
                             float vrr_proximity,
                             float gpu_load_ratio,
                             NvU32 final_interval_us) {
    if (!enabled || !file) return;

    const char* mode_str = (cb_mode == ConsistencyBuffer::Mode::Tighten) ? "TIGHTEN" : "STABILIZE";
    fprintf(file, "%lld,%.2f,%.2f,%.2f,%.6f,%.6f,%.6f,%d,%s,%d,%.2f,%.2f,%.2f,%d,%d,%d,%.4f,%.4f,%u\n",
            static_cast<long long>(timestamp_qpc),
            smoothness,
            cadence_stddev_us,
            cadence_mean_delta_us,
            cadence_cv,
            qpc_cv,
            qpc_cv_render,
            gsync_active ? 1 : 0,
            mode_str,
            cb_buffer_us,
            pred_gpu_us,
            pred_sim_us,
            pred_fg_us,
            enforcement_site,
            queue_depth,
            fg_tier,
            vrr_proximity,
            gpu_load_ratio,
            final_interval_us);
    fflush(file);
}

void DiagCSVLogger::Close() {
    if (file) {
        fclose(file);
        file = nullptr;
    }
    enabled = false;
    header_written = false;
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
// GSyncDetector — runtime GSync/VRR active detection (64-bit only)
// ============================================================================

bool GSyncDetector::Init() {
    NvQueryInterface_fn qi = GetNvapiQi();
    if (!qi) {
        ul_log::Write("GSyncDetector::Init: NVAPI QI not available");
        return false;
    }

    NvU32 vrr_id  = FindFuncId("NvAPI_Disp_GetVRRInfo");
    NvU32 name_id = FindFuncId("NvAPI_DISP_GetDisplayIdByDisplayName");

    if (!vrr_id || !name_id) {
        ul_log::Write("GSyncDetector::Init: function IDs not found in table "
                      "(GetVRRInfo=0x%08X, GetDisplayIdByName=0x%08X)", vrr_id, name_id);
        return false;
    }

    get_vrr_info    = reinterpret_cast<GetVRRInfo_fn>(qi(vrr_id));
    get_display_id  = reinterpret_cast<GetDisplayIdByName_fn>(qi(name_id));

    resolved = (get_vrr_info != nullptr && get_display_id != nullptr);
    if (!resolved) {
        ul_log::Write("GSyncDetector::Init: failed to resolve function pointers "
                      "(GetVRRInfo=%p, GetDisplayIdByName=%p)",
                      reinterpret_cast<void*>(get_vrr_info),
                      reinterpret_cast<void*>(get_display_id));
    } else {
        ul_log::Write("GSyncDetector::Init: resolved OK");
    }
    return resolved;
}

bool GSyncDetector::ResolveDisplayId(HWND hwnd) {
    if (!resolved || !get_display_id) return false;
    if (!hwnd) return false;

    // HWND → HMONITOR → GDI device name → NVAPI display ID
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon) {
        ul_log::Write("GSyncDetector::ResolveDisplayId: MonitorFromWindow failed");
        return false;
    }

    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hmon, &mi)) {
        ul_log::Write("GSyncDetector::ResolveDisplayId: GetMonitorInfoA failed");
        return false;
    }

    // NvAPI_DISP_GetDisplayIdByDisplayName expects the GDI device name
    // exactly as returned by GetMonitorInfoA (e.g. "\\.\DISPLAY1")
    NvU32 id = 0;
    NvStatus st = get_display_id(mi.szDevice, &id);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("GSyncDetector::ResolveDisplayId: GetDisplayIdByDisplayName "
                          "failed for '%s' (status=%d)", mi.szDevice, st);
            error_logged = true;
        }
        return false;
    }

    display_id = id;
    error_logged = false;  // reset so Poll errors are reported fresh
    ul_log::Write("GSyncDetector::ResolveDisplayId: '%s' -> display ID 0x%08X",
                  mi.szDevice, display_id);
    return true;
}

bool GSyncDetector::Poll(int64_t now_qpc) {
    if (!resolved || !get_vrr_info || display_id == 0) {
        gsync_active = false;
        return false;
    }

    // Only poll every 2 seconds
    if (last_poll_qpc != 0 &&
        (now_qpc - last_poll_qpc) < ul_timing::g_qpc_freq * 2) {
        return gsync_active;
    }

    last_poll_qpc = now_qpc;

    NV_GET_VRR_INFO vrr_info = {};
    vrr_info.version = NV_GET_VRR_INFO_VER;

    NvStatus st = get_vrr_info(display_id, &vrr_info);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("GSyncDetector::Poll: GetVRRInfo failed (status=%d)", st);
            error_logged = true;
        }
        gsync_active = false;
        return false;
    }

    bool prev = gsync_active;
    // Check both bIsVRREnabled (driver-level VRR enablement) and
    // bIsDisplayInVRRMode (display hardware actually in VRR mode).
    // Some configurations (e.g. DLSS FG + Streamline, borderless windowed)
    // may only set one of these flags.
    gsync_active = (vrr_info.bIsVRREnabled != 0 || vrr_info.bIsDisplayInVRRMode != 0);

    // Log all VRR bits on first successful poll and on state transitions
    if (prev != gsync_active || !first_poll_logged) {
        ul_log::Write("GSyncDetector::Poll: enabled=%d possible=%d requested=%d "
                      "indicator=%d displayInVRR=%d -> gsync_active=%d",
                      vrr_info.bIsVRREnabled, vrr_info.bIsVRRPossible,
                      vrr_info.bIsVRRRequested, vrr_info.bIsVRRIndicatorEnabled,
                      vrr_info.bIsDisplayInVRRMode, gsync_active ? 1 : 0);
        first_poll_logged = true;
    }
    return gsync_active;
}

// ============================================================================
// FrameSplitController — disable/restore NVIDIA driver frame splitting
// ============================================================================

bool FrameSplitController::Init() {
    NvQueryInterface_fn qi = GetNvapiQi();
    if (!qi) {
        ul_log::Write("FrameSplitController::Init: NVAPI QI not available");
        return false;
    }

    NvU32 get_id  = FindFuncId("NvAPI_DISP_GetAdaptiveSyncData");
    NvU32 set_id  = FindFuncId("NvAPI_DISP_SetAdaptiveSyncData");
    NvU32 name_id = FindFuncId("NvAPI_DISP_GetDisplayIdByDisplayName");

    if (!get_id || !set_id || !name_id) {
        ul_log::Write("FrameSplitController::Init: function IDs not found in table");
        return false;
    }

    get_adaptive_sync = reinterpret_cast<GetAdaptiveSyncData_fn>(qi(get_id));
    set_adaptive_sync = reinterpret_cast<SetAdaptiveSyncData_fn>(qi(set_id));
    get_display_id    = reinterpret_cast<GetDisplayIdByName_fn>(qi(name_id));

    resolved = (get_adaptive_sync != nullptr && set_adaptive_sync != nullptr
                && get_display_id != nullptr);
    if (!resolved) {
        ul_log::Write("FrameSplitController::Init: failed to resolve function pointers");
    } else {
        ul_log::Write("FrameSplitController::Init: resolved OK");
    }
    return resolved;
}

bool FrameSplitController::ResolveDisplayId(HWND hwnd) {
    if (!resolved || !get_display_id) return false;
    if (!hwnd) return false;

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::ResolveDisplayId: MonitorFromWindow failed");
            error_logged = true;
        }
        return false;
    }

    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hmon, &mi)) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::ResolveDisplayId: GetMonitorInfoA failed");
            error_logged = true;
        }
        return false;
    }

    NvU32 id = 0;
    NvStatus st = get_display_id(mi.szDevice, &id);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::ResolveDisplayId: "
                          "GetDisplayIdByDisplayName failed for '%s' (status=%d)",
                          mi.szDevice, st);
            error_logged = true;
        }
        return false;
    }

    display_id = id;
    error_logged = false;
    ul_log::Write("FrameSplitController::ResolveDisplayId: '%s' -> display ID 0x%08X",
                  mi.szDevice, display_id);
    return true;
}

bool FrameSplitController::Disable() {
    if (!resolved || !get_adaptive_sync || !set_adaptive_sync) return false;
    if (display_id == 0) return false;
    if (disabled) return true;

    NV_GET_ADAPTIVE_SYNC_DATA get_data = {};
    get_data.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;

    NvStatus st = get_adaptive_sync(display_id, &get_data);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::Disable: GetAdaptiveSyncData failed (status=%d)", st);
            error_logged = true;
        }
        saved_valid = false;
        return false;
    }

    // Save original state for restore
    memcpy(&saved_data, &get_data, sizeof(get_data));
    saved_valid = true;

    NV_SET_ADAPTIVE_SYNC_DATA set_data = {};
    set_data.version               = NV_SET_ADAPTIVE_SYNC_DATA_VER;
    set_data.maxFrameInterval      = get_data.maxFrameInterval;
    set_data.bDisableAdaptiveSync  = get_data.bDisableAdaptiveSync;
    set_data.bDisableFrameSplitting = 1;
    // maxFrameIntervalNs = 0 → EDID default ("don't change").
    // The GET struct doesn't return maxFrameIntervalNs (it has lastFlipTimeStamp
    // at that offset instead), so we can't read-modify-write it. Passing 0
    // tells the driver to keep whatever ns-level interval is already configured.
    set_data.maxFrameIntervalNs    = 0;

    st = set_adaptive_sync(display_id, &set_data);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::Disable: SetAdaptiveSyncData failed (status=%d)", st);
            error_logged = true;
        }
        return false;
    }

    disabled = true;
    ul_log::Write("FrameSplitController::Disable: frame splitting disabled");
    return true;
}

bool FrameSplitController::Restore() {
    if (!resolved || !set_adaptive_sync) return false;
    if (!disabled) return true;
    if (!saved_valid) return false;

    NV_GET_ADAPTIVE_SYNC_DATA& saved = saved_data;

    NV_SET_ADAPTIVE_SYNC_DATA set_data = {};
    set_data.version                = NV_SET_ADAPTIVE_SYNC_DATA_VER;
    set_data.maxFrameInterval       = saved.maxFrameInterval;
    set_data.bDisableAdaptiveSync   = saved.bDisableAdaptiveSync;
    set_data.bDisableFrameSplitting = saved.bDisableFrameSplitting;

    NvStatus st = set_adaptive_sync(display_id, &set_data);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::Restore: SetAdaptiveSyncData failed (status=%d)", st);
            error_logged = true;
        }
        disabled = false;
        return false;
    }

    disabled = false;
    ul_log::Write("FrameSplitController::Restore: frame splitting restored");
    return true;
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

    // Invalidate cache on: timer expiry or vsync override change.
    bool stale = (now_qpc - s_last_check_qpc > ul_timing::g_qpc_freq * 2)
              || (ovr != s_last_vsync_override);
    if (!stale) return s_cached;
    s_last_check_qpc = now_qpc;
    s_last_vsync_override = ovr;

    if (ovr == 2) { s_cached = 0; return 0; }

    // QueryDisplayConfig — DX path.
    // Uses DISPLAYCONFIG_VIDEO_SIGNAL_INFO::vSyncFreq which reports the actual
    // display signal timing — works correctly under GSync/VRR where
    // EnumDisplaySettings may return dmDisplayFrequency=0.
    // Same approach as SpecialK and DisplayCommander.
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) { s_cached = 0; return 0; }

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hm) { s_cached = 0; return 0; }

    // Get the GDI device name for this monitor
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, &mi)) { s_cached = 0; return 0; }

    // Try QueryDisplayConfig first — accurate under VRR
    {
        UINT32 path_count = 0, mode_count = 0;
        LONG buf_res = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (buf_res == ERROR_SUCCESS && path_count > 0 && mode_count > 0) {
            auto paths = std::make_unique<DISPLAYCONFIG_PATH_INFO[]>(path_count);
            auto modes = std::make_unique<DISPLAYCONFIG_MODE_INFO[]>(mode_count);
            LONG qdc_res = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.get(),
                                              &mode_count, modes.get(), nullptr);
            if (qdc_res == ERROR_SUCCESS) {
                bool found_monitor = false;
                for (UINT32 i = 0; i < path_count; i++) {
                    if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE))
                        continue;

                    // Match this path to our monitor via GDI device name
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src_name = {};
                    src_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src_name.header.size = sizeof(src_name);
                    src_name.header.adapterId = paths[i].sourceInfo.adapterId;
                    src_name.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src_name.header) != ERROR_SUCCESS)
                        continue;

                    // Compare wide GDI name against our ANSI name
                    char src_name_a[64] = {};
                    WideCharToMultiByte(CP_ACP, 0, src_name.viewGdiDeviceName, -1,
                                        src_name_a, sizeof(src_name_a), nullptr, nullptr);
                    if (strcmp(src_name_a, mi.szDevice) != 0)
                        continue;

                    found_monitor = true;

                    // Found our monitor — get target mode for vSyncFreq
                    int mode_idx = (paths[i].flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)
                        ? paths[i].targetInfo.targetModeInfoIdx
                        : paths[i].targetInfo.modeInfoIdx;
                    if (mode_idx < 0 || static_cast<UINT32>(mode_idx) >= mode_count) {
                        static bool s_log_idx = false;
                        if (!s_log_idx) {
                            ul_log::Write("VrrFloor: monitor found but mode_idx out of range (%d, count=%u)",
                                           mode_idx, mode_count);
                            s_log_idx = true;
                        }
                        continue;
                    }
                    if (modes[mode_idx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                        static bool s_log_type = false;
                        if (!s_log_type) {
                            ul_log::Write("VrrFloor: monitor found but mode type=%d (expected TARGET=2)",
                                           modes[mode_idx].infoType);
                            s_log_type = true;
                        }
                        continue;
                    }

                    auto& vsig = modes[mode_idx].targetMode.targetVideoSignalInfo;
                    if (vsig.vSyncFreq.Numerator > 0 && vsig.vSyncFreq.Denominator > 0) {
                        double hz = static_cast<double>(vsig.vSyncFreq.Numerator)
                                  / static_cast<double>(vsig.vSyncFreq.Denominator);
                        if (hz >= 30.0) {
                            double ceiling_fps = 3600.0 * hz / (hz + 3600.0);
                            if (ceiling_fps >= 10.0) {
                                ceiling_fps *= 0.995;
                                s_cached = static_cast<NvU32>(std::round(1'000'000.0 / ceiling_fps));
                                return s_cached;
                            }
                        }
                    }
                    // vSyncFreq was 0 or hz < 30
                    static bool s_log_freq = false;
                    if (!s_log_freq) {
                        ul_log::Write("VrrFloor: monitor found, vSyncFreq=%u/%u (unusable)",
                                       vsig.vSyncFreq.Numerator, vsig.vSyncFreq.Denominator);
                        s_log_freq = true;
                    }
                    break;  // found our monitor, stop searching
                }

                if (!found_monitor) {
                    static bool s_log_nomatch = false;
                    if (!s_log_nomatch) {
                        ul_log::Write("VrrFloor: QueryDisplayConfig OK (%u paths) but no match for '%s'",
                                       path_count, mi.szDevice);
                        s_log_nomatch = true;
                    }
                }
            } else {
                static bool s_log_qdc = false;
                if (!s_log_qdc) {
                    ul_log::Write("VrrFloor: QueryDisplayConfig failed (%ld)", qdc_res);
                    s_log_qdc = true;
                }
            }
        } else {
            static bool s_log_buf = false;
            if (!s_log_buf) {
                ul_log::Write("VrrFloor: GetDisplayConfigBufferSizes failed (res=%ld, paths=%u, modes=%u)",
                               buf_res, path_count, mode_count);
                s_log_buf = true;
            }
        }
    }

    // Last resort: EnumDisplaySettings (may return 0 under VRR)
    {
        DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            double hz = static_cast<double>(dm.dmDisplayFrequency);
            if (hz >= 30.0) {
                double ceiling_fps = 3600.0 * hz / (hz + 3600.0);
                if (ceiling_fps >= 10.0) {
                    ceiling_fps *= 0.995;
                    s_cached = static_cast<NvU32>(std::round(1'000'000.0 / ceiling_fps));
                    return s_cached;
                }
            }
        }
    }

    s_cached = 0;
    return 0;
}

// ============================================================================
// VRR proximity — how close the target interval is to the VRR ceiling
// ============================================================================

static float ComputeVrrProximity(HWND hwnd, NvU32 target_interval_us) {
    NvU32 vrr_floor = ComputeVrrFloorIntervalUs(hwnd);
    if (vrr_floor == 0 || target_interval_us == 0)
        return 0.0f;
    float proximity = static_cast<float>(vrr_floor) / static_cast<float>(target_interval_us);
    return std::clamp(proximity, 0.0f, 1.0f);
}

// ============================================================================
// FG pacing offset (static fallback)
// ============================================================================

static constexpr NvU32 kFGPacingOffsetUs = 24;

// ============================================================================
// FG detection — always auto
// ============================================================================

int UlLimiter::DetectFGDivisor() const {
    return cached_fg_divisor_;
}

int UlLimiter::ComputeFGDivisorRaw() const {
    // Streamline slDLSSGSetOptions hook is the primary source of truth for DLSS FG.
    // Reads numFramesToGenerate directly from the Streamline DLSS-G API.
    int fg_mult = g_fg_multiplier.load(std::memory_order_relaxed);
    if (g_fg_active.load(std::memory_order_relaxed) && fg_mult >= 2)
        return fg_mult;

    // DMFG hint from game's requested frame latency.
    // ONLY used when no user-space FG DLL is present — this is the signal
    // for driver-side DMFG (RTX 50 series) which has no DLL.
    bool fg_dll = GetModuleHandleW(L"nvngx_dlssg.dll")
              || GetModuleHandleW(L"_nvngx_dlssg.dll")
              || GetModuleHandleW(L"sl.dlss_g.dll")
              || GetModuleHandleW(L"dlss-g.dll");
    if (!fg_dll) {
        UINT game_lat = GetGameRequestedLatency();
        if (game_lat >= 3) {
            int lat_hint = static_cast<int>(game_lat);
            if (lat_hint > 6) lat_hint = 6;
            return lat_hint;
        }
    }

    return 1;
}

void UlLimiter::CacheFGDivisor() {
    cached_fg_divisor_ = ComputeFGDivisorRaw();
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

void UlLimiter::Init(HMODULE addon_module) {
    if (!latency_buf_)
        latency_buf_ = new (std::nothrow) NvLatencyResult{};

    if (g_cfg.csv_diagnostics.load()) {
        diag_csv_logger_.Open(addon_module);
    }
}

void UlLimiter::Shutdown() {
    diag_csv_logger_.Close();
    vblank_monitor_.Shutdown();

    // Restore frame splitting state before shutdown
    frame_split_ctrl_.Restore();

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
    dxgi_sc_ = nullptr;
    last_sleep_valid_ = false;

    if (htimer_delay_) { CloseHandle(htimer_delay_); htimer_delay_ = nullptr; }
    if (htimer_fallback_) { CloseHandle(htimer_fallback_); htimer_fallback_ = nullptr; }
    if (htimer_queue_) { CloseHandle(htimer_queue_); htimer_queue_ = nullptr; }
    if (htimer_bg_) { CloseHandle(htimer_bg_); htimer_bg_ = nullptr; }

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

    // Init GSync detection and frame splitting control now that NVAPI is loaded
    if (!gsync_detector_.resolved)
        gsync_detector_.Init();
    if (!frame_split_ctrl_.resolved)
        frame_split_ctrl_.Init();

    // Start VBlank monitor for DX PLL — exact hardware vblank timestamps.
    if (hwnd_ && !vblank_monitor_.IsActive()) {
        vblank_monitor_.Init(hwnd_);
    }

    return true;
}


void UlLimiter::ResetAdaptiveState() {
    pipeline_predictor_.Reset();
    pipeline_stats_ = PipelineStats{};
    boost_ctrl_.Reset();
    qpc_monitor_.Reset();
    cached_fg_divisor_ = ComputeFGDivisorRaw();  // refresh before Reset uses it
    consistency_buf_.Reset(cached_fg_divisor_);
    gpu_load_gate_open_ = false;
    last_fg_tier_ = 0;
    pending_fg_tier_ = 0;
    fg_tier_confirm_ticks_ = 0;
    ptp_error_accum_us_ = 0.0f;
    ptp_correction_us_ = 0;
    ptp_sample_count_ = 0;
    last_ptp_present_ns_ = 0;
    last_pred_frame_id_ = 0;
    gpu_overload_mode_ = false;
    gpu_overload_count_ = 0;
    gpu_recover_count_ = 0;
    pll_smoothed_error_ns_ = 0.0f;
    last_pll_frame_id_ = 0;
    pll_error_variance_ns2_ = 0.0f;
    pll_sample_count_ = 0;
    pll_converged_ = false;
    pll_marker_present_ns_.store(0, std::memory_order_relaxed);
    pll_marker_frame_id_.store(0, std::memory_order_relaxed);
    last_pll_marker_frame_id_ = 0;

    // Reset voting windows
    site_vote_head_ = 0;
    site_vote_count_ = 0;
    gate_vote_head_ = 0;
    gate_vote_count_ = 0;

    // Reset DXGI frame stats tracking
    last_dxgi_sync_qpc_ = 0;
    last_dxgi_present_count_ = 0;
    last_real_present_count_ = 0;
    last_pll_vblank_count_ = 0;
    // Note: dxgi_sc_ is NOT cleared here — it's still valid across resets.
    // Only cleared in Shutdown or when the swapchain is destroyed.

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
// Interval adjustment — VRR floor, consistency buffer, FG offset, driver shave
// ============================================================================

static NvU32 AdjustIntervalUs(NvU32 raw, int fg_divisor, HWND hwnd,
                              const PipelineStats& ps, int32_t ptp_correction_us,
                              int32_t consistency_buffer_us,
                              bool gsync_active) {
    if (raw == 0) return 0;

    // Only apply VRR floor when GSync is active
    if (gsync_active) {
        NvU32 vrr_floor = ComputeVrrFloorIntervalUs(hwnd);
        if (vrr_floor > 0 && raw < vrr_floor)
            raw = vrr_floor;
    }

    if (fg_divisor > 1) {
        // FG path: consistency buffer + FG overhead offset.
        // Cap the combined adjustment to prevent over-padding — the consistency
        // buffer's max for the current tier is the ceiling for total FG adjustment.
        int32_t fg_adj = consistency_buffer_us;
        if (ps.adaptive_fg_offset_us > 0)
            fg_adj += ps.adaptive_fg_offset_us;

        // Clamp: don't let FG offset push total beyond what the consistency
        // buffer considers safe for this tier.
        if (fg_adj > consistency_buffer_us && consistency_buffer_us > 0) {
            // The consistency buffer already accounts for cadence variance.
            // Allow the FG offset to add at most half the buffer's current value
            // on top, preventing runaway stacking.
            int32_t max_total = consistency_buffer_us + (consistency_buffer_us / 2);
            if (fg_adj > max_total) fg_adj = max_total;
        }

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
        // Consistency buffer: independent additive term
        if (consistency_buffer_us != 0) {
            int32_t adjusted = static_cast<int32_t>(raw) + consistency_buffer_us;
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

    // Predictive sleep — 1:1 path only, when GPU has headroom.
    // Only shorten the interval when enforcement actually happens at
    // SIM_START (queue_depth <= 1).  When queue_depth > 1, OnMarker
    // redirects enforcement to PRESENT_BEGIN, where a shorter interval
    // directly reduces frame-to-frame time and causes FPS overshoot.
    bool fg = (DetectFGDivisor() > 1);
    bool marker_pacing = g_game_uses_reflex.load(std::memory_order_relaxed);
    int eff_queue = pipeline_stats_.pacing.queue_depth;
    if (raw_interval > 0 && pipeline_predictor_.active && marker_pacing
        && !(pipeline_stats_.valid && pipeline_stats_.gpu_load_ratio > 1.0f)) {
        if (!fg) {
            if (pipeline_stats_.auto_site == SIM_START && eff_queue <= 1) {
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
                                            consistency_buf_.buffer_us,
                                            gsync_detector_.gsync_active
                                            );
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

    bool ok = false;
    {
        NvSleepParams copy = p;
        NvStatus st = InvokeSetSleepMode(dev_, &copy);
        ok = (st == NV_OK);
    }

    if (ok) { last_sleep_params_ = p; last_sleep_valid_ = true; }
    return ok;
}

// Invoke the driver's sleep function (returns near-instantly when we already
// consumed the wait via our grid). Keeps the driver's internal state coherent.
void UlLimiter::InvokeReflexSleep() {
    if (ReflexActive() && dev_)
        InvokeSleep(dev_);
}

// ============================================================================
// Pipeline stats from GetLatency — adaptive interval, enforcement site, etc.
// ============================================================================

void UlLimiter::UpdatePipelineStats() {
    if (!latency_buf_) return;

    // Try GetLatency — may return empty reports for Streamline games
    // where the device registration is owned by Streamline's interposer.
    bool has_getlatency_data = false;
    {
        if (ReflexActive() && dev_) {
            memset(latency_buf_, 0, sizeof(*latency_buf_));
            latency_buf_->version = NV_LATENCY_RESULT_VER;

            NvStatus st;
            __try { st = InvokeGetLatency(dev_, latency_buf_); }
            __except(EXCEPTION_EXECUTE_HANDLER) { st = NV_NO_IMPL; }

            if (st == NV_OK) {
                for (int i = 0; i < 64; i++) {
                    if (latency_buf_->frameReport[i].frameID) {
                        has_getlatency_data = true;
                        break;
                    }
                }
            }

            // One-shot diagnostic
            {
                static bool s_gl_logged = false;
                static int s_gl_empty_count = 0;
                if (!s_gl_logged) {
                    s_gl_empty_count++;
                    if (has_getlatency_data) {
                        ul_log::Write("GetLatency: data available (after %d empty polls)", s_gl_empty_count);
                        s_gl_logged = true;
                    } else if (s_gl_empty_count == 300) {
                        ul_log::Write("GetLatency: still empty after %d polls — Streamline device mismatch, using marker fallback", s_gl_empty_count);
                        s_gl_logged = true;
                    }
                }
            }
        }
    }

    // Scan the most recent valid reports (up to 16) — only when GetLatency has data
    float sum_gpu = 0, sum_queue = 0, sum_fg = 0;
    float sum_sim = 0, sum_submit = 0, sum_driver = 0;
    float sum_idle_gap = 0, sum_input_disp = 0;
    int n_gpu = 0, n_queue = 0, n_fg = 0;
    int n_sim = 0, n_submit = 0, n_driver = 0;
    int n_idle_gap = 0, n_input_disp = 0;
    float target_interval_us = 0.0f;
    float target_load_ratio = 0.0f;  // target-based GPU load for site selection + load gate
    {
        float rfps = ComputeRenderFps();
        if (rfps > 0.0f) target_interval_us = 1'000'000.0f / rfps;
    }

    bool fg_active = (DetectFGDivisor() > 1);

    struct GapFrame { uint64_t id; uint64_t render_start; uint64_t render_end; float gpu_active; };
    GapFrame gap_frames[16] = {};
    int n_gap_frames = 0;

    if (has_getlatency_data) {
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
        if (fg_active && f.gpuRenderEndTime > 0 && f.presentEndTime > f.gpuRenderEndTime
            && IsRealFrame(f)) {
            float fg_us = static_cast<float>(f.presentEndTime - f.gpuRenderEndTime);
            if (fg_us > 0.0f && fg_us < 200'000.0f) { sum_fg += fg_us; n_fg++; }
        }
        if (IsRealFrame(f)) {
            float sim_us = static_cast<float>(f.simEndTime - f.simStartTime);
            if (sim_us < 200'000.0f) { sum_sim += sim_us; n_sim++; }
        }        if (f.renderSubmitStartTime > 0 && f.renderSubmitEndTime > f.renderSubmitStartTime) {
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
    } // end if (has_getlatency_data)

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
    if (!has_getlatency_data) {
        // Streamline fallback: no GetLatency data, but we can still track cadence
        // from marker timestamps. Set valid after enough marker-based cadence samples.
        if (pipeline_predictor_.cadence.count >= PipelineStats::kMinSamples)
            ps.valid = true;
        // Set target_load_ratio to a neutral value so adaptive systems don't
        // make decisions based on missing GPU data.
        target_load_ratio = 0.75f;  // neutral — neither overloaded nor idle
    } else {
        if (ps.samples < PipelineStats::kMinSamples) return;
        ps.valid = true;
    }

    if (has_getlatency_data) {
        ps.avg_pipeline_total_us = ps.avg_sim_us + ps.avg_render_submit_us
                                 + ps.avg_driver_us + ps.avg_queue_us + ps.avg_gpu_active_us;
    }

    // Adaptive interval adjustment (requires GetLatency GPU data)
    if (has_getlatency_data && target_interval_us > 0.0f) {
        ps.gpu_load_ratio = ps.avg_gpu_active_us / target_interval_us;
        ps.pipeline_load_ratio = ps.avg_pipeline_total_us / target_interval_us;

        // --- GPU overload mode detection ---
        // When the GPU can't sustain the target render rate, switch the interval
        // basis to actual render FPS so all adaptive systems get sane inputs.
        // Only evaluate when GPU load gate is open (gameplay, not menus/loading).
        if (gpu_load_gate_open_) {
            // Compute load against the USER's original target interval.
            // gpu_overload_mode_ is now metrics-only (no pacing effect),
            // so target_interval_us always reflects the user's target.
            // user_load drives both entry (>1.05) and recovery (<0.95).
            float user_target_fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
            int div = DetectFGDivisor();
            if (div > 1) user_target_fps /= static_cast<float>(div);
            float user_interval_us = (user_target_fps > 0.0f)
                ? 1'000'000.0f / user_target_fps : target_interval_us;
            float user_load = ps.avg_gpu_active_us / user_interval_us;

            if (!gpu_overload_mode_) {
                if (user_load > 1.05f) {
                    gpu_overload_count_++;
                    gpu_recover_count_ = 0;
                    if (gpu_overload_count_ >= kOverloadThreshold) {
                        gpu_overload_mode_ = true;
                        ul_log::Write("GPU overload: load=%.2f (metrics only, pacing unchanged)", user_load);
                    }
                } else {
                    gpu_overload_count_ = 0;
                }
            } else {
                if (user_load < 0.95f) {
                    gpu_recover_count_++;
                    gpu_overload_count_ = 0;
                    if (gpu_recover_count_ >= kOverloadThreshold) {
                        gpu_overload_mode_ = false;
                        ul_log::Write("GPU recovered: load=%.2f", user_load);
                    }
                } else {
                    gpu_recover_count_ = 0;
                }

                // Recompute load ratio against actual interval for adaptive systems
                // (interval adjustment, consistency buffer). Enforcement site selection
                // uses the original target-based ratio to avoid flip-flopping.
                if (pipeline_predictor_.cadence.mean_delta_us > 0.0f) {
                    ps.gpu_load_ratio = ps.avg_gpu_active_us / pipeline_predictor_.cadence.mean_delta_us;
                    ps.pipeline_load_ratio = ps.avg_pipeline_total_us / pipeline_predictor_.cadence.mean_delta_us;
                }
            }
        }

        // Save target-based load ratio for enforcement site and load gate.
        // In overload mode, gpu_load_ratio has been recomputed against actual
        // cadence (for adaptive systems), but site selection and the load gate
        // should use the original target to avoid oscillation.
        target_load_ratio = gpu_overload_mode_
            ? (ps.avg_gpu_active_us / target_interval_us)
            : ps.gpu_load_ratio;

        if (ps.gpu_load_ratio < 0.70f)
            ps.interval_adjust_us = -3;
        else if (ps.gpu_load_ratio > 0.90f)
            ps.interval_adjust_us = static_cast<int32_t>(
                std::min(4.0f, (ps.gpu_load_ratio - 0.90f) * 40.0f));
        else
            ps.interval_adjust_us = 0;
    }

    // Bottleneck detection (40% threshold) — requires GetLatency data
    if (has_getlatency_data && ps.avg_pipeline_total_us > 0.0f) {
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

    // Auto enforcement site — voting window prevents oscillation from
    // transient GPU load changes. Each frame votes for a site, and the
    // site only switches when a supermajority (70%) of the window agrees.
    {
        int suggested_site = ps.auto_site;  // keep current as default

        if (fg_active) {
            if (target_load_ratio < 0.55f)
                suggested_site = SIM_START;
            else if (target_load_ratio > 0.70f)
                suggested_site = PRESENT_FINISH;
        } else if (ps.bottleneck == Bottleneck::Gpu)
            suggested_site = SIM_START;
        else if (ps.bottleneck == Bottleneck::CpuSim || ps.bottleneck == Bottleneck::CpuSubmit)
            suggested_site = PRESENT_FINISH;
        else {
            if (target_load_ratio > 0.85f)
                suggested_site = SIM_START;
            else if (target_load_ratio < 0.65f)
                suggested_site = PRESENT_FINISH;
        }

        // Record vote
        site_votes_[site_vote_head_] = suggested_site;
        site_vote_head_ = (site_vote_head_ + 1) % kSiteVoteWindow;
        if (site_vote_count_ < kSiteVoteWindow) site_vote_count_++;

        // Count votes — only switch when 70% agree on a different site.
        // Before the window fills, use the suggested site directly.
        if (site_vote_count_ < kSiteVoteWindow / 2) {
            ps.auto_site = suggested_site;
        } else {
            int sim_votes = 0, pf_votes = 0;
            for (int i = 0; i < site_vote_count_; i++) {
                int idx = (site_vote_head_ - site_vote_count_ + i + kSiteVoteWindow) % kSiteVoteWindow;
                if (site_votes_[idx] == SIM_START) sim_votes++;
                else if (site_votes_[idx] == PRESENT_FINISH) pf_votes++;
            }
            int threshold = static_cast<int>(site_vote_count_ * 0.70f);
            if (sim_votes >= threshold)
                ps.auto_site = SIM_START;
            else if (pf_votes >= threshold)
                ps.auto_site = PRESENT_FINISH;
            // else: keep current site (no supermajority)
        }
    }

    // Render queue depth monitoring
    if (target_interval_us > 0.0f)
        ps.queue_pressure = (ps.avg_queue_us > target_interval_us * 1.5f);
    else
        ps.queue_pressure = false;

    // Adaptive DLSSG pacing offset (with rate-of-change clamping)
    if (fg_active && n_fg > 0) {
        int32_t measured = static_cast<int32_t>(std::round(ps.avg_fg_overhead_us));
        measured = std::clamp(measured, 4, 60);

        // Clamp rate of change to ±2µs per tick to prevent oscillation
        // under GPU-bound conditions where FG overhead fluctuates.
        if (ps.prev_adaptive_fg_offset_us >= 0) {
            int32_t delta = measured - ps.prev_adaptive_fg_offset_us;
            constexpr int32_t kMaxDelta = 2;
            if (delta > kMaxDelta) measured = ps.prev_adaptive_fg_offset_us + kMaxDelta;
            else if (delta < -kMaxDelta) measured = ps.prev_adaptive_fg_offset_us - kMaxDelta;
            measured = std::clamp(measured, 4, 60);
        }
        ps.prev_adaptive_fg_offset_us = measured;
        ps.adaptive_fg_offset_us = measured;
    } else if (!fg_active) {
        ps.adaptive_fg_offset_us = -1;
        ps.prev_adaptive_fg_offset_us = -1;
    }

    // Predictive sleep — pipeline-aware wake scheduling (requires GetLatency data)
    if (has_getlatency_data) {
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
            if (IsRealFrame(f)) {
                float sim = static_cast<float>(f.simEndTime - f.simStartTime);
                if (sim < 200'000.0f) s.sim_us = sim;
            }
            if (fg_active && f.gpuRenderEndTime > 0 && f.presentEndTime > f.gpuRenderEndTime
                && IsRealFrame(f)) {
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
    } // end if (has_getlatency_data) — predictive sleep

    // Feed cadence tracker — works with both GetLatency and marker fallback.
    {
        auto& cadence = pipeline_predictor_.cadence;
        if (has_getlatency_data) {
            // Real frames only — generated frames have no sim phase.
            struct CadenceSample { uint64_t id; uint64_t present_end; };
            CadenceSample csamples[64];
            int n_cs = 0;
            uint64_t last_fed = cadence.last_fed_frame_id;
            for (int i = 0; i < 64; i++) {
                auto& f = latency_buf_->frameReport[i];
                if (!f.frameID || !f.presentEndTime || f.frameID <= last_fed) continue;
                if (!IsRealFrame(f)) continue;
                csamples[n_cs++] = { f.frameID, f.presentEndTime };
            }
            for (int i = 0; i < n_cs - 1; i++)
                for (int j = i + 1; j < n_cs; j++)
                    if (csamples[j].id < csamples[i].id)
                        std::swap(csamples[i], csamples[j]);
            for (int i = 0; i < n_cs; i++)
                cadence.Feed(csamples[i].present_end);
            if (n_cs > 0)
                cadence.last_fed_frame_id = csamples[n_cs - 1].id;
        } else {
            // Streamline fallback: feed cadence from PRESENT_FINISH marker timestamps.
            uint64_t marker_fid = pll_marker_frame_id_.load(std::memory_order_relaxed);
            int64_t  marker_ns  = pll_marker_present_ns_.load(std::memory_order_relaxed);
            if (marker_fid > cadence.last_fed_frame_id && marker_ns > 0) {
                cadence.Feed(static_cast<uint64_t>(marker_ns / 1000));
                cadence.last_fed_frame_id = marker_fid;
            }
        }
        cadence.ComputeStats();
    }

    // --- PLL: phase-locked grid correction from display feedback ---
    // Use the most recent presentEndTime to measure phase error between
    // our grid and actual display timing. Nudge grid epoch to converge.
    if (grid_interval_ns_ > 0 && grid_epoch_ns_ > 0) {
        int64_t actual_ns = 0;
        uint64_t feedback_id = 0;
        bool high_precision_feedback = false;
        bool used_marker = false;
        bool used_dxgi = false;
        bool used_vblank = false;

        // Priority 1 (DX only): VBlank monitor — exact hardware vblank
        // timestamps from D3DKMTWaitForVerticalBlankEvent. Zero jitter,
        // zero delay, always in QPC domain. Best possible DX PLL source.
        if (vblank_monitor_.IsActive()) {
            uint64_t vb_count = vblank_monitor_.GetVBlankCount();
            if (vb_count > last_pll_vblank_count_) {
                last_pll_vblank_count_ = vb_count;
                actual_ns = vblank_monitor_.GetLastVBlankNs();
                feedback_id = vb_count;
                high_precision_feedback = true;
                used_vblank = true;
            }
        }

        // PLL feedback priority depends on whether Streamline is present.
        // Without Streamline: DXGI GetFrameStatistics is the best source —
        // hardware vblank timestamps, no CPU scheduling jitter. Markers are
        // CPU-timestamped and jitter by ±several ms under load.
        // With Streamline: DXGI may be a proxy swapchain with unreliable
        // stats. Markers (from PCL hook) are more reliable in that case.
        //
        // In both cases, the marker's "same render cycle" detection still
        // runs to prevent duplicate PLL corrections on generated frames.
        bool prefer_dxgi = dxgi_sc_
            && (GetModuleHandleW(L"sl.interposer.dll") == nullptr);

        // Always check marker state for render-cycle dedup, even when DXGI
        // is preferred. This prevents DXGI from firing on generated frame
        // presents within the same render cycle.
        bool marker_fresh = false;   // true when a NEW marker arrived this present
        bool marker_stale = false;   // true when we're in the same render cycle (generated frame)
        {
            uint64_t marker_fid = pll_marker_frame_id_.load(std::memory_order_relaxed);
            int64_t  marker_ns  = pll_marker_present_ns_.load(std::memory_order_relaxed);
            if (marker_fid > last_pll_marker_frame_id_ && marker_ns > 0) {
                last_pll_marker_frame_id_ = marker_fid;
                marker_fresh = true;

                if (actual_ns == 0 && !prefer_dxgi) {
                    // Streamline present: use marker as primary PLL source
                    // (only if no higher-priority source already provided data)
                    actual_ns   = marker_ns;
                    feedback_id = marker_fid;
                    high_precision_feedback = true;
                    used_marker = true;
                }
            } else if (actual_ns == 0 && marker_fid == last_pll_marker_frame_id_ && marker_ns > 0) {
                // Same render cycle (generated frame) — suppress lower-priority sources.
                // Only if no higher-priority source (vblank monitor) already provided data.
                marker_stale = true;
                actual_ns = marker_ns;
                used_marker = true;
            }
        }

        // DXGI GetFrameStatistics — direct display vblank timing.
        // Primary source on DX without Streamline (hardware timestamps).
        // Fallback source on DX with Streamline (proxy swapchain risk).
        // Skipped on generated frames (marker_stale).
        if (actual_ns == 0 && dxgi_sc_ && !marker_stale) {
            IDXGISwapChain* sc = reinterpret_cast<IDXGISwapChain*>(dxgi_sc_);
            DXGI_FRAME_STATISTICS stats = {};
            if (SUCCEEDED(sc->GetFrameStatistics(&stats)) && stats.SyncQPCTime.QuadPart > 0) {
                UINT ref_count = 0;
                if (fg_active) {
                    ref_count = last_real_present_count_;
                } else {
                    sc->GetLastPresentCount(&ref_count);
                }
                if (ref_count > last_dxgi_present_count_
                    && stats.PresentCount >= ref_count) {
                    last_dxgi_present_count_ = ref_count;
                    actual_ns = static_cast<int64_t>(
                        static_cast<double>(stats.SyncQPCTime.QuadPart) * 1e9
                        / static_cast<double>(ul_timing::g_qpc_freq));
                    feedback_id = ref_count;
                    high_precision_feedback = true;
                    used_dxgi = true;
                }
            }
        }

        // Marker fallback — when DXGI is preferred but didn't produce data
        // (e.g., GetFrameStatistics failed, or present count didn't advance),
        // fall back to the marker timestamp if we have one this cycle.
        if (actual_ns == 0 && prefer_dxgi && marker_fresh) {
            int64_t marker_ns = pll_marker_present_ns_.load(std::memory_order_relaxed);
            if (marker_ns > 0) {
                actual_ns   = marker_ns;
                feedback_id = pll_marker_frame_id_.load(std::memory_order_relaxed);
                high_precision_feedback = true;
                used_marker = true;
            }
        }

        // Fallback 2: GetLatencyTimings presentEndTime — real frames only.
        // When FG is active, this is the preferred DX feedback source since
        // GetLatency reports are filtered to real frames via IsRealFrame,
        // avoiding the generated-frame vblank misalignment of DXGI stats.
        if (actual_ns == 0) {
            // One-shot validation: confirm GetLatency timestamps are in the QPC
            // time domain. presentEndTime should be within a few seconds of NowQpc().
            // If the difference is large, the driver is using a different clock
            // (e.g. GPU hardware counter) and the conversion would be garbage.
            static int s_latency_domain_state = 0;  // 0=unknown, 1=valid, -1=invalid
            if (s_latency_domain_state == 0) {
                // Find any recent frame with a presentEndTime
                for (int i = 63; i >= 0; i--) {
                    auto& f = latency_buf_->frameReport[i];
                    if (f.presentEndTime == 0) continue;
                    int64_t now_qpc_ticks = ul_timing::NowQpc();
                    int64_t delta_ticks = now_qpc_ticks
                                       - static_cast<int64_t>(f.presentEndTime);
                    // Allow up to 10 seconds of delta (report delay + warmup)
                    int64_t max_delta = ul_timing::g_qpc_freq * 10;
                    if (std::abs(delta_ticks) < max_delta) {
                        s_latency_domain_state = 1;
                        ul_log::Write("PLL: GetLatency timestamps validated "
                                      "(delta=%.1fms, QPC domain confirmed)",
                                      static_cast<float>(delta_ticks) * 1000.0f
                                      / static_cast<float>(ul_timing::g_qpc_freq));
                    } else {
                        s_latency_domain_state = -1;
                        ul_log::Write("PLL: GetLatency timestamps INVALID "
                                      "(delta=%.1fs — not QPC domain, fallback disabled)",
                                      static_cast<float>(delta_ticks)
                                      / static_cast<float>(ul_timing::g_qpc_freq));
                    }
                    break;
                }
            }

            if (s_latency_domain_state == 1) {
                uint64_t newest_id = 0;
                uint64_t newest_present = 0;
                for (int i = 0; i < 64; i++) {
                    auto& f = latency_buf_->frameReport[i];
                    // Real frames only — generated frames have no sim phase
                    if (!IsRealFrame(f)) continue;
                    if (f.frameID > newest_id && f.presentEndTime > 0) {
                        newest_id = f.frameID;
                        newest_present = f.presentEndTime;
                    }
                }
                if (newest_id > last_pll_frame_id_ && newest_present > 0) {
                    feedback_id = newest_id;
                    actual_ns = static_cast<int64_t>(
                        static_cast<double>(newest_present) * 1e9
                        / static_cast<double>(ul_timing::g_qpc_freq));
                }
            }
        }

        // Gate: only process new feedback (dedup).
        // Vblank monitor has its own dedup (last_pll_vblank_count_), so don't
        // update last_pll_frame_id_ from vblank counts — they're in a different
        // ID space and would permanently block marker/DXGI sources.
        bool new_feedback = false;
        if (used_vblank && actual_ns > 0) {
            new_feedback = true;  // already deduped by last_pll_vblank_count_
        } else if (feedback_id > last_pll_frame_id_ && actual_ns > 0) {
            last_pll_frame_id_ = feedback_id;
            new_feedback = true;
        }

        if (new_feedback) {

            // Measure phase error at the vblank level, not the grid level.
            // With high vblank:grid ratios (e.g. 12:1 at 165Hz/41FPS), measuring
            // against the grid slot causes ±interval/2 ambiguity — the frame can
            // land on any of ~12 vblanks within one grid interval, making the
            // "nearest grid slot" meaningless.
            //
            // Instead: find the nearest vblank boundary to actual_ns.
            // The vblank period is known from the VRR floor interval.
            // Phase error = distance from nearest vblank = always in [-vblank/2, +vblank/2].
            // Correction still applies to grid_epoch_ns_ — a small epoch shift
            // moves the grid so the next real frame lands closer to a vblank.
            //
            // With VRR/GSync active, the display dynamically matches the render rate.
            // The effective vblank period equals the render interval (grid_interval_ns_),
            // not the monitor's maximum refresh rate. Use the grid interval directly.
            int64_t vblank_ns = 0;
            if (gsync_detector_.gsync_active) {
                // VRR active on DX: display locks to render rate
                vblank_ns = grid_interval_ns_;
            }
            if (vblank_ns == 0 && hwnd_) {
                NvU32 floor_us = ComputeVrrFloorIntervalUs(hwnd_);
                if (floor_us > 0)
                    vblank_ns = static_cast<int64_t>(floor_us) * 1000LL;
            }

            int64_t phase_error_ns = 0;
            if (vblank_ns > 0) {
                // Nearest vblank to actual_ns
                int64_t vblank_slot = (actual_ns / vblank_ns) * vblank_ns;
                phase_error_ns = actual_ns - vblank_slot;
                // Wrap to [-vblank/2, +vblank/2]
                if (phase_error_ns > vblank_ns / 2)
                    phase_error_ns -= vblank_ns;
            } else {
                // No vblank info — fall back to grid-level measurement with tight threshold
                int64_t elapsed = actual_ns - grid_epoch_ns_;
                int64_t expected_slot = (elapsed / grid_interval_ns_) * grid_interval_ns_
                                      + grid_epoch_ns_;
                phase_error_ns = actual_ns - expected_slot;
                if (phase_error_ns > grid_interval_ns_ / 2)
                    phase_error_ns -= grid_interval_ns_;
                else if (phase_error_ns < -grid_interval_ns_ / 2)
                    phase_error_ns += grid_interval_ns_;
            }

            // Three-tier gate for phase error samples:
            //   |error| <= vblank/4  → normal sample, accumulate into EMA
            //   |error| > vblank/4 && <= vblank/2 → noisy, skip sample, keep state
            //   |error| > vblank/2  → genuine discontinuity (mode change, clock jump), reset
            //
            // The old binary gate (pass/nuke) destroyed all PLL state on every
            // outlier. Marker feedback jitters by ±half a frame under CPU load,
            // producing frequent outliers that kept the PLL in permanent reset.
            // The three-tier approach lets the EMA survive normal jitter while
            // still resetting on actual discontinuities.
            int64_t gate_normal = vblank_ns > 0 ? vblank_ns / 4 : 1'000'000LL;
            int64_t gate_reset  = vblank_ns > 0 ? vblank_ns / 2 : 2'000'000LL;
            int64_t abs_error = std::abs(phase_error_ns);

            if (abs_error <= gate_normal) {
                // Normal sample — accumulate
                float alpha = high_precision_feedback ? 0.12f : kPllAlpha;
                float gain  = high_precision_feedback ? 0.20f : kPllCorrectionGain;

                pll_smoothed_error_ns_ = pll_smoothed_error_ns_ * (1.0f - alpha)
                                       + static_cast<float>(phase_error_ns) * alpha;

                float err_f = static_cast<float>(phase_error_ns);
                pll_error_variance_ns2_ = pll_error_variance_ns2_ * 0.95f
                                        + err_f * err_f * 0.05f;
                pll_sample_count_++;

                float noise_threshold = vblank_ns > 0
                    ? static_cast<float>(vblank_ns) * 0.25f
                    : 1'500'000.0f;
                float error_std = std::sqrt(pll_error_variance_ns2_);
                pll_converged_ = (pll_sample_count_ >= 20)
                              && (error_std < noise_threshold);

                if (pll_converged_) {
                    int64_t correction = static_cast<int64_t>(
                        pll_smoothed_error_ns_ * gain);
                    if (correction != 0)
                        grid_epoch_ns_ += correction;
                }
            } else if (abs_error > gate_reset) {
                // Genuine discontinuity — reset
                pll_smoothed_error_ns_ = 0.0f;
                pll_error_variance_ns2_ = 0.0f;
                pll_sample_count_ = 0;
                pll_converged_ = false;
            }
            // else: noisy sample (between vblank/4 and vblank/2) — skip, keep state

            // Diagnostics — one-shot on first feedback, then every 30 seconds
            static bool s_pll_first = true;
            static int64_t s_pll_last_log_qpc = 0;
            int64_t now_qpc = ul_timing::NowQpc();
            bool periodic = (now_qpc - s_pll_last_log_qpc) > ul_timing::g_qpc_freq * 30;
            if (s_pll_first || periodic) {
                const char* src = used_vblank ? "D3DKMT(vblank)"
                    : !high_precision_feedback ? "GetLatency"
                    : used_marker ? "marker(PF)"
                    : used_dxgi ? "DXGI(real)" : "unknown";
                const char* state = (abs_error > gate_reset) ? "reset(discontinuity)"
                    : (abs_error > gate_normal) ? "skipped(noisy)"
                    : pll_converged_ ? "correcting"
                    : "acquiring";
                float error_std = std::sqrt(pll_error_variance_ns2_);
                ul_log::Write("PLL: src=%s vblank=%.0fus phase_err=%+.0fus "
                              "smoothed=%+.0fus noise_std=%.0fus samples=%d %s",
                              src,
                              vblank_ns / 1000.0f,
                              static_cast<float>(phase_error_ns) / 1000.0f,
                              pll_smoothed_error_ns_ / 1000.0f,
                              error_std / 1000.0f,
                              pll_sample_count_,
                              state);
                s_pll_last_log_qpc = now_qpc;
                s_pll_first = false;
            }
        }
    }

    // --- GPU Load Gate (voting window) ---
    // When GPU load is below 50%, freeze ConsistencyBuffer tick only.
    // Voting window prevents gate flapping when load hovers near 50%.
    {
        int gate_vote = (target_load_ratio >= 0.50f) ? 1 : 0;
        gate_votes_[gate_vote_head_] = gate_vote;
        gate_vote_head_ = (gate_vote_head_ + 1) % kGateVoteWindow;
        if (gate_vote_count_ < kGateVoteWindow) gate_vote_count_++;

        bool was_gate_open = gpu_load_gate_open_;

        // Gate opens when 60% of votes say open, closes when 70% say closed.
        // Asymmetric thresholds: easier to open (enter gameplay) than close
        // (transition to menu), reducing false closes during brief load dips.
        // Before the window fills, use direct evaluation.
        if (gate_vote_count_ < kGateVoteWindow / 2) {
            gpu_load_gate_open_ = (target_load_ratio >= 0.50f);
        } else {
            int open_votes = 0;
            for (int i = 0; i < gate_vote_count_; i++) {
                int idx = (gate_vote_head_ - gate_vote_count_ + i + kGateVoteWindow) % kGateVoteWindow;
                open_votes += gate_votes_[idx];
            }
            float open_ratio = static_cast<float>(open_votes) / static_cast<float>(gate_vote_count_);
            if (!gpu_load_gate_open_ && open_ratio >= 0.60f)
                gpu_load_gate_open_ = true;
            else if (gpu_load_gate_open_ && open_ratio < 0.30f)
                gpu_load_gate_open_ = false;
        }

        // Flush overload state when transitioning from gameplay to menu/loading
        if (was_gate_open && !gpu_load_gate_open_) {
            gpu_overload_mode_ = false;
            gpu_overload_count_ = 0;
            gpu_recover_count_ = 0;
        }
    }

    if (gpu_load_gate_open_) {
        // --- FG tier change detection with hysteresis ---
        // DetectFGDivisor() returns the cached per-frame value (DLL presence,
        // FPS-ratio monitor, latency hint). FPS-based tier override is already
        // folded into ComputeFGDivisorRaw().
        int fg_tier = DetectFGDivisor();
        is_dmfg_ = (fg_tier >= 3 && !GetModuleHandleW(L"nvngx_dlssg.dll")
                                  && !GetModuleHandleW(L"_nvngx_dlssg.dll")
                                  && !GetModuleHandleW(L"sl.dlss_g.dll")
                                  && !GetModuleHandleW(L"dlss-g.dll")
                                  && !GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll")
                                  && !GetModuleHandleW(L"ffx_framegeneration.dll"));

        // Hysteresis: require kFGTierConfirmThreshold consecutive frames at
        // the new tier before propagating. Prevents rapid oscillation when
        // the FPS ratio hovers near a tier boundary (e.g. 2.5x → 2↔3).
        // First detection (last_fg_tier_ == 0 → nonzero) is immediate.
        if (fg_tier != last_fg_tier_) {
            if (last_fg_tier_ == 0 && fg_tier > 0) {
                // First detection: accept immediately
                pending_fg_tier_ = 0;
                fg_tier_confirm_ticks_ = 0;
            } else if (fg_tier == pending_fg_tier_) {
                fg_tier_confirm_ticks_++;
            } else {
                pending_fg_tier_ = fg_tier;
                fg_tier_confirm_ticks_ = 1;
            }

            bool confirmed = (last_fg_tier_ == 0 && fg_tier > 0)
                          || (fg_tier_confirm_ticks_ >= kFGTierConfirmThreshold);

            if (confirmed) {
                // Check if the consistency buffer tuning tier actually changes.
                // DMFG can oscillate between 4x/5x/6x — all use kTier4xPlusMFG.
                // Also 3x↔4x transitions: compare the full tuning struct
                // (all 9 fields) to avoid unnecessary resets.
                ConsistencyBuffer::TuningParams prev_params = consistency_buf_.active_params;
                consistency_buf_.SelectTier(fg_tier);
                bool tier_params_changed =
                    std::memcmp(&consistency_buf_.active_params, &prev_params,
                                sizeof(ConsistencyBuffer::TuningParams)) != 0;

                if (tier_params_changed) {
                    consistency_buf_.Reset(fg_tier);
                }

                // Flush GPU overload state — FG state change fundamentally changes
                // what "target render FPS" means, so overload detection from the
                // previous FG state is invalid.
                gpu_overload_mode_ = false;
                gpu_overload_count_ = 0;
                gpu_recover_count_ = 0;
                pipeline_predictor_.cadence.Reset();

                // Snap grid to new interval immediately — don't EMA-drift from
                // the old FG rate to the new one over 20 frames.
                grid_epoch_ns_ = 0;
                grid_next_ns_ = 0;
                grid_interval_ns_ = 0;

                // Reset PLL — stale phase errors from the old FG rate would
                // corrupt the EMA and cause the PLL to apply wrong corrections.
                pll_smoothed_error_ns_ = 0.0f;
                pll_error_variance_ns2_ = 0.0f;
                pll_sample_count_ = 0;
                pll_converged_ = false;

                last_fg_tier_ = fg_tier;
                pending_fg_tier_ = 0;
                fg_tier_confirm_ticks_ = 0;
            }
        } else {
            // Same tier as current: reset pending
            pending_fg_tier_ = 0;
            fg_tier_confirm_ticks_ = 0;
        }

        // --- GSync active detection ---
        if (gsync_detector_.display_id == 0 && hwnd_)
            gsync_detector_.ResolveDisplayId(hwnd_);
        gsync_detector_.Poll(ul_timing::NowQpc());

        // --- Frame splitting control ---
        if (gsync_detector_.gsync_active && fg_tier > 1) {
            if (frame_split_ctrl_.display_id == 0 && hwnd_)
                frame_split_ctrl_.ResolveDisplayId(hwnd_);
            frame_split_ctrl_.Disable();
        } else {
            frame_split_ctrl_.Restore();
        }

        // --- VRR proximity ---
        NvU32 target_iv_us = 0;
        {
            float rfps = ComputeRenderFps();
            if (rfps > 0.0f)
                target_iv_us = static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(rfps)));
        }
        float vrr_proximity = ComputeVrrProximity(hwnd_, target_iv_us);

        // Gate VRR proximity: when GSync is not active, disable VRR-specific behavior
        if (!gsync_detector_.gsync_active)
            vrr_proximity = 0.0f;

        // --- Cadence stddev_us with 8-sample minimum gate ---
        float cadence_stddev_us = (pipeline_predictor_.cadence.count >= 8)
            ? pipeline_predictor_.cadence.stddev_us : 0.0f;

        // --- QPC CV + stddev ---
        float qpc_cv = qpc_monitor_.ComputeCV();
        float qpc_stddev_us = qpc_monitor_.ComputeStddev();

        // Render interval from cadence tracker (real frames only via GetLatency)
        float render_interval_us = (pipeline_predictor_.cadence.count >= 8)
            ? pipeline_predictor_.cadence.mean_delta_us : 0.0f;

        // --- Tick consistency buffer state machine ---
        consistency_buf_.Tick(cadence_stddev_us, qpc_cv, qpc_stddev_us,
                             vrr_proximity, fg_tier > 1, render_interval_us);

        // --- Expanded diagnostic CSV logging ---
        if (g_cfg.csv_diagnostics.load(std::memory_order_relaxed) && diag_csv_logger_.enabled) {
            NvU32 diag_final_interval_us = 0;
            {
                float rfps = ComputeRenderFps();
                if (rfps > 0.0f) {
                    NvU32 raw = static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(rfps)));
                    diag_final_interval_us = AdjustIntervalUs(raw, fg_tier, hwnd_,
                                                              ps, ptp_correction_us_,
                                                              consistency_buf_.buffer_us,
                                                              gsync_detector_.gsync_active
                                                              );
                }
            }
            float diag_cv = (pipeline_predictor_.cadence.count >= 8)
                ? pipeline_predictor_.cadence.ComputeCV() : 0.0f;
            float smoothness = 100.0f - (diag_cv * 1000.0f);
            if (smoothness < 0.0f) smoothness = 0.0f;
            if (smoothness > 100.0f) smoothness = 100.0f;

            // Compute render-relative QPC CV for diagnostics (same logic as
            // ConsistencyBuffer::Tick uses for the brake decision).
            float qpc_cv_render = qpc_cv;
            if (fg_tier > 1 && render_interval_us > 0.0f && qpc_stddev_us > 0.0f)
                qpc_cv_render = qpc_stddev_us / render_interval_us;


            diag_csv_logger_.WriteRow(
                ul_timing::NowQpc(),
                smoothness,
                cadence_stddev_us,
                (pipeline_predictor_.cadence.count >= 8)
                    ? pipeline_predictor_.cadence.mean_delta_us : 0.0f,
                diag_cv,
                qpc_cv,
                qpc_cv_render,
                gsync_detector_.gsync_active,
                consistency_buf_.mode,
                consistency_buf_.buffer_us,
                pipeline_predictor_.gpu.predicted_us,
                pipeline_predictor_.sim.predicted_us,
                pipeline_predictor_.fg.predicted_us,
                ResolveEnforcementSite(),
                ps.pacing.queue_depth,
                fg_tier,
                vrr_proximity,
                ps.gpu_load_ratio,
                diag_final_interval_us);
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

        // When FG is active but the FG overhead predictor has no data
        // (e.g. frame splitting disabled -> presentEndTime ≈ gpuRenderEndTime),
        // predicted_total underestimates the actual cycle. Stale miss_count
        // from before the transition inflates miss_rate and locks queue_depth=3.
        // Suppress predictor-based queue depth decisions in this state.
        bool predictor_valid = !fg_active || (pipeline_predictor_.fg.count > 0);

        int suggested = 1;
        if (!predictor_valid) {
            // FG active but no FG overhead data (e.g. frame splitting disabled).
            // Predictor underestimates cycle time; stale miss_count inflates
            // miss_rate. Use a safe default rather than predictor-driven depth.
            suggested = 2;
        } else if (margin < 400.0f && miss_rate < 0.05f)
            suggested = 1;
        else if (margin > 1500.0f)
            suggested = 3;
        else if (margin > 800.0f || miss_rate > 0.10f)
            suggested = 2;

        // FG floor: when the GPU has no meaningful safety margin, FG needs
        // deeper queuing to absorb cadence variance from the generation
        // pipeline.  When margin is healthy the limiter controls cadence
        // and q=1 is fine.  Reuses the same margin threshold (400µs) that
        // the base suggestion logic considers "tight".
        if (fg_active && margin < 400.0f && suggested < 2)
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
                        // Demote by one step only — avoid large jumps (e.g. 3→1)
                        // that cause cadence disruption under FG.
                        dp.queue_depth = current - 1;
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
    // DX path: NVAPI
    if (!ReflexActive() || !dev_) return;
    NvSleepParams p = BuildSleepParams();
    MaybeUpdateSleepMode(p);
    InvokeSleep(dev_);
}

// ============================================================================
// Own-the-sleep unified pacer — replaces dual DoReflexSleep + DoTimingFallback
// ============================================================================
//
// Single pacer: our high-res timers handle all sleep timing via the
// phase-locked grid. Reflex receives the real interval for driver state
// management (queue management, boost, markers) but InvokeSleep returns
// near-instantly since we already consumed the wait time.

void UlLimiter::DoOwnSleep(int fg_div) {
    NvSleepParams p = BuildSleepParams();
    if (p.minimumIntervalUs == 0) return;

    // --- Unified phase-locked grid ---
    // minimumIntervalUs is the render interval. When fg_div > 1 and the caller
    // is OnPresent (fires for real + generated frames), divide to get the
    // output interval so generated frames pass through at the correct rate.
    int64_t frame_ns = static_cast<int64_t>(p.minimumIntervalUs) * 1000LL;
    if (fg_div > 1)
        frame_ns /= fg_div;
    int64_t now = ul_timing::NowNs();

    if (grid_epoch_ns_ == 0) {
        grid_epoch_ns_ = now;
        grid_interval_ns_ = frame_ns;
        grid_next_ns_ = now + frame_ns;
    } else {
        constexpr float kGridAlpha = 0.05f;
        grid_interval_ns_ = static_cast<int64_t>(
            grid_interval_ns_ * (1.0 - kGridAlpha) + frame_ns * kGridAlpha);

        if (grid_next_ns_ > now) {
            ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);
            grid_next_ns_ += grid_interval_ns_;
        } else {
            // Frame arrived late — missed the grid slot.
            // Advance from now to avoid the long+short frame pair that epoch
            // snapping creates when a single frame runs late.
            grid_next_ns_ = now + grid_interval_ns_;
        }
    }

    // --- Reflex passthrough (unified) ---
    __try {
        MaybeUpdateSleepMode(p);
        InvokeReflexSleep();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Silently absorb — the grid sleep already paced the frame.
    }
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
    bool has_reflex = (ReflexActive() && dev_);
    if (!has_reflex) return;
    if (!warmup_done_) return;
    if (is_background_) return;

    if (s_smooth_motion.load(std::memory_order_relaxed)) return;

    // Capture QPC timestamp at PRESENT_FINISH for PLL feedback — before any
    // early returns so it fires regardless of is_real_frame or enforcement site.
    if (marker_type == PRESENT_FINISH) {
        pll_marker_present_ns_.store(ul_timing::NowNs(), std::memory_order_relaxed);
        pll_marker_frame_id_.store(frame_id, std::memory_order_relaxed);
        if (dxgi_sc_) {
            IDXGISwapChain* sc = reinterpret_cast<IDXGISwapChain*>(dxgi_sc_);
            UINT cnt = 0;
            if (SUCCEEDED(sc->GetLastPresentCount(&cnt)) && cnt > 0)
                last_real_present_count_ = cnt;
        }
        // One-shot diagnostic: log the first PRESENT_FINISH frame_id after
        // enforcement switches to PRESENT_FINISH, to verify IDs are advancing.
        static int64_t s_pf_site_switch_qpc = 0;
        static bool s_pf_logged = false;
        int64_t now_qpc = ul_timing::NowQpc();
        if (!s_pf_logged && pipeline_stats_.auto_site == PRESENT_FINISH) {
            if (s_pf_site_switch_qpc == 0) s_pf_site_switch_qpc = now_qpc;
            if (now_qpc - s_pf_site_switch_qpc > ul_timing::g_qpc_freq * 2) {
                ul_log::Write("PLL marker(PF) check: frame_id=%llu last_marker_id=%llu "
                              "marker_ns=%lld now_ns=%lld",
                              frame_id, last_pll_marker_frame_id_,
                              pll_marker_present_ns_.load(std::memory_order_relaxed),
                              ul_timing::NowNs());
                s_pf_logged = true;
            }
        }
    }

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

    if (marker_type == effective_site) {
        DoOwnSleep();
    }
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

    // Smooth Motion doubles output at the driver level — pace real frames
    // at half the target so total output (real + interpolated) hits the cap.
    if (s_smooth_motion.load(std::memory_order_relaxed))
        target *= 0.5f;

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
        grid_next_ns_ = now + grid_interval_ns_;
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

        // Log pacing mode summary
        {
            bool dx = ReflexActive() && dev_;
            bool native = g_game_uses_reflex.load(std::memory_order_relaxed);
            bool sl = GetModuleHandleW(L"sl.interposer.dll") != nullptr;
            const char* api = dx ? "DX" : "None";
            const char* reflex = native ? "native" : dx ? "injected" : "none";
            int fg = DetectFGDivisor();
            float fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
            ul_log::Write("OnPresent: warmup done (frame %llu) — API=%s, Reflex=%s, "
                          "Streamline=%s, FG=%dx, target=%.0f FPS",
                          frame_num_, api, reflex, sl ? "yes" : "no", fg, fps);
        }

        // Flush defaults so new config keys appear in the INI (one-shot, deferred from init)
        static bool s_settings_flushed = false;
        if (!s_settings_flushed) {
            SaveSettings();
            s_settings_flushed = true;
        }

        // Re-init VBlank monitor if it died during startup (transient D3DKMT
        // failures during display mode changes).
        if (hwnd_ && !vblank_monitor_.IsActive()) {
            vblank_monitor_.Init(hwnd_);
        }
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
            // Background refocus — full reset of adaptive state.
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
                if (grid_next_ns_ > now_ns) {
                    // Cap sleep to one background interval to prevent blocking
                    // the render thread when alt-tabbing back into the game.
                    // Without this, grid_next_ns_ can drift far ahead and
                    // SleepUntilNs blocks indefinitely on refocus.
                    int64_t max_wake = now_ns + bg_ns;
                    int64_t wake = (grid_next_ns_ < max_wake) ? grid_next_ns_ : max_wake;
                    ul_timing::SleepUntilNs(wake, htimer_bg_);
                }
                grid_next_ns_ = now_ns + bg_ns;
            }
            return;
        }
    }

    // Feed QPC timestamp to local brake signal monitor.
    // When FG is active, OnPresent fires for every output frame (real + generated).
    // Feeding all of them produces alternating ~render_interval / ~0.5ms deltas,
    // giving qpc_cv > 1.0 which permanently triggers the ConsistencyBuffer brake.
    // Only feed on real-frame presents: every fg_divisor-th present.
    // Use last_fg_tier_ (confirmed from previous frame) — cached_fg_divisor_ isn't
    // set yet this frame.
    {
        int div = (last_fg_tier_ > 1) ? last_fg_tier_ : 1;
        if (div <= 1 || (frame_num_ % static_cast<uint64_t>(div)) == 0)
            qpc_monitor_.Feed(now_qpc);
    }

    // Cache FG divisor once per frame — all subsequent DetectFGDivisor() calls
    // return this value, ensuring consistency across BuildSleepParams,
    // ComputeRenderFps, UpdatePipelineStats, and the FG tier change logic.
    CacheFGDivisor();

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

    // Poll GetLatency every frame
    {
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
    // Skip when DXGI frame stats are active —
    // the PLL handles timing correction from direct display feedback,
    // and PTP's interval adjustments would fight the PLL's epoch corrections.
    bool any_reflex = (ReflexActive() && dev_);
    bool has_display_feedback = (dxgi_sc_ != nullptr);
    if (any_reflex && DetectFGDivisor() == 1 && !has_display_feedback) {
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

    // Reflex path — DX only.
    // Native Reflex (markers flow): OnMarker calls DoOwnSleep at enforcement site.
    // Non-native Reflex (no markers): DoOwnSleep here in OnPresent.
    // No Reflex: timing grid only.
    bool has_reflex = (ReflexActive() && dev_);
    if (has_reflex) {
        if (g_game_uses_reflex.load(std::memory_order_relaxed)) {
            // DX native Reflex: marker pacing — OnMarker calls DoOwnSleep
            // at enforcement site. Just update sleep mode params here.
            NvSleepParams p = BuildSleepParams();
            MaybeUpdateSleepMode(p);
        } else {
            // Non-native: DoOwnSleep from OnPresent.
            // On DX, OnPresent only fires for real frames — fg_div stays 1.
            DoOwnSleep();
        }
    } else {
        // Non-Reflex games — timing grid is the only pacer
        DoTimingFallback();
    }
}

// ============================================================================
// Streamline proxy present
// ============================================================================

void UlLimiter::OnSLPresent() {
    if (!g_cfg.use_sl_proxy.load(std::memory_order_relaxed)) return;
    if (!ReflexActive() || !dev_) return;
    DoOwnSleep();
}
