// ReLimiter — Frame limiter implementation
// Clean-room from public API docs:
//   - NVAPI SDK (MIT): NvAPI_D3D_SetSleepMode params, NvAPI_D3D_Sleep
//   - Windows: GetModuleHandleW for DLL detection, QPC for timing,
//              EnumDisplaySettings / MonitorFromWindow for refresh rate
// No code from Display Commander or any other project.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ul_fg_monitor.hpp"
#include "ul_limiter.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"
#ifdef _WIN64
#include "ul_vk_reflex.hpp"
#define VK_REFLEX_ACTIVE() (vk_reflex_ && vk_reflex_->IsActive())
#else
#define VK_REFLEX_ACTIVE() (false)
#endif

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

#ifdef _WIN64
bool GSyncDetector::Init() {
    NvQueryInterface_fn qi = GetNvapiQi();
    if (!qi) {
        ul_log::Write("GSyncDetector::Init: NVAPI QI not available");
        return false;
    }

    NvU32 goh_id = FindFuncId("NvAPI_D3D_GetObjectHandleForResource");
    NvU32 gsa_id = FindFuncId("NvAPI_D3D_IsGSyncActive");

    if (!goh_id || !gsa_id) {
        ul_log::Write("GSyncDetector::Init: function IDs not found in table");
        return false;
    }

    get_object_handle = reinterpret_cast<GetObjectHandle_fn>(qi(goh_id));
    is_gsync_active   = reinterpret_cast<IsGSyncActive_fn>(qi(gsa_id));

    resolved = (get_object_handle != nullptr && is_gsync_active != nullptr);
    if (!resolved) {
        ul_log::Write("GSyncDetector::Init: failed to resolve function pointers "
                      "(GetObjectHandle=%p, IsGSyncActive=%p)",
                      reinterpret_cast<void*>(get_object_handle),
                      reinterpret_cast<void*>(is_gsync_active));
    } else {
        ul_log::Write("GSyncDetector::Init: resolved OK");
    }
    return resolved;
}

bool GSyncDetector::AcquireSurfaceHandle(IUnknown* device, IUnknown* back_buffer) {
    if (!resolved || !get_object_handle || !device || !back_buffer) return false;

    // Reset error flag on each attempt (swapchain re-init should retry cleanly)
    error_logged = false;

    NvU32 handle = 0;
    NvStatus st = get_object_handle(device, back_buffer, &handle);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("GSyncDetector::AcquireSurfaceHandle: failed (status=%d)", st);
            error_logged = true;
        }
        surface_handle = 0;
        return false;
    }
    surface_handle = handle;
    ul_log::Write("GSyncDetector::AcquireSurfaceHandle: handle=0x%08X", handle);
    return true;
}

bool GSyncDetector::Poll(IUnknown* device, int64_t now_qpc) {
    if (!resolved || !is_gsync_active || surface_handle == 0) {
        gsync_active = false;
        return false;
    }

    // Only poll every 2 seconds
    if (last_poll_qpc != 0 &&
        (now_qpc - last_poll_qpc) < ul_timing::g_qpc_freq * 2) {
        return gsync_active;
    }

    last_poll_qpc = now_qpc;

    NvU32 active = 0;
    NvStatus st = is_gsync_active(device, surface_handle, &active);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("GSyncDetector::Poll: IsGSyncActive failed (status=%d)", st);
            error_logged = true;
        }
        gsync_active = false;
        return false;
    }

    gsync_active = (active != 0);
    return gsync_active;
}
#endif

// ============================================================================
// FrameSplitController — disable/restore NVIDIA driver frame splitting
// ============================================================================

#ifdef _WIN64
bool FrameSplitController::Init() {
    NvQueryInterface_fn qi = GetNvapiQi();
    if (!qi) {
        ul_log::Write("FrameSplitController::Init: NVAPI QI not available");
        return false;
    }

    NvU32 get_id = FindFuncId("NvAPI_DISP_GetAdaptiveSyncData");
    NvU32 set_id = FindFuncId("NvAPI_DISP_SetAdaptiveSyncData");
    NvU32 enum_id = FindFuncId("NvAPI_EnumNvidiaDisplayHandle");

    if (!get_id || !set_id || !enum_id) {
        ul_log::Write("FrameSplitController::Init: function IDs not found in table");
        return false;
    }

    get_adaptive_sync = reinterpret_cast<GetAdaptiveSyncData_fn>(qi(get_id));
    set_adaptive_sync = reinterpret_cast<SetAdaptiveSyncData_fn>(qi(set_id));
    enum_display      = reinterpret_cast<EnumDisplayHandle_fn>(qi(enum_id));

    resolved = (get_adaptive_sync != nullptr && set_adaptive_sync != nullptr
                && enum_display != nullptr);
    if (!resolved) {
        ul_log::Write("FrameSplitController::Init: failed to resolve function pointers");
    } else {
        ul_log::Write("FrameSplitController::Init: resolved OK");
    }
    return resolved;
}

bool FrameSplitController::ResolveDisplayId(HWND hwnd) {
    if (!resolved || !enum_display) return false;
    if (!hwnd) return false;

    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::ResolveDisplayId: MonitorFromWindow failed");
            error_logged = true;
        }
        return false;
    }

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::ResolveDisplayId: GetMonitorInfoW failed");
            error_logged = true;
        }
        return false;
    }

    // Enumerate NVAPI displays and match by device name
    for (NvU32 i = 0; i < 32; i++) {
        NvU32 handle = 0;
        NvStatus st = enum_display(i, &handle);
        if (st != NV_OK) break;  // no more displays

        display_id = handle;
        ul_log::Write("FrameSplitController::ResolveDisplayId: using display handle 0x%08X", handle);
        return true;
    }

    if (!error_logged) {
        ul_log::Write("FrameSplitController::ResolveDisplayId: no NVAPI display found");
        error_logged = true;
    }
    return false;
}

bool FrameSplitController::Disable() {
    if (!resolved || !get_adaptive_sync || !set_adaptive_sync) return false;
    if (display_id == 0) return false;
    if (disabled) return true;  // already disabled

    // Save original state
    memset(saved_data, 0, sizeof(saved_data));
    // NV_GET_ADAPTIVE_SYNC_DATA structure: version at offset 0 (NvU32)
    // Version = sizeof(struct) | (1 << 16)
    // Struct size is typically 12 bytes: version(4) + maxFrameInterval(4) + flags(4)
    NvU32 version = 12 | (1 << 16);
    memcpy(saved_data, &version, sizeof(version));

    NvStatus st = get_adaptive_sync(display_id, saved_data);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::Disable: GetAdaptiveSyncData failed (status=%d)", st);
            error_logged = true;
        }
        saved_valid = false;
        return false;
    }
    saved_valid = true;

    // Set frame splitting disabled: copy saved data, set bDisableFrameSplitting flag
    uint8_t set_data[64] = {};
    memcpy(set_data, saved_data, sizeof(saved_data));

    // The flags field is at offset 8 (after version + maxFrameInterval)
    // Bit 0 of flags = bDisableFrameSplitting
    NvU32 flags = 0;
    memcpy(&flags, &set_data[8], sizeof(flags));
    flags |= 1;  // set bDisableFrameSplitting = 1
    memcpy(&set_data[8], &flags, sizeof(flags));

    st = set_adaptive_sync(display_id, set_data);
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
    if (!disabled) return true;  // nothing to restore
    if (!saved_valid) return false;

    // Restore original state: clear bDisableFrameSplitting flag
    uint8_t restore_data[64] = {};
    memcpy(restore_data, saved_data, sizeof(saved_data));

    NvU32 flags = 0;
    memcpy(&flags, &restore_data[8], sizeof(flags));
    flags &= ~1u;  // clear bDisableFrameSplitting
    memcpy(&restore_data[8], &flags, sizeof(flags));

    NvStatus st = set_adaptive_sync(display_id, restore_data);
    if (st != NV_OK) {
        if (!error_logged) {
            ul_log::Write("FrameSplitController::Restore: SetAdaptiveSyncData failed (status=%d)", st);
            error_logged = true;
        }
        disabled = false;  // best-effort: mark as not disabled
        return false;
    }

    disabled = false;
    ul_log::Write("FrameSplitController::Restore: frame splitting restored");
    return true;
}
#endif

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
    bool fg_dll = GetModuleHandleW(L"nvngx_dlssg.dll")
              || GetModuleHandleW(L"_nvngx_dlssg.dll")
              || GetModuleHandleW(L"sl.dlss_g.dll")
              || GetModuleHandleW(L"dlss-g.dll");
    bool fsr_dll = GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll")
                || GetModuleHandleW(L"ffx_framegeneration.dll");

    // DMFG hint from game's requested frame latency.
    // ONLY used when no user-space FG DLL is present — this is the signal
    // for driver-side DMFG (RTX 50 series) which has no DLL.
    // When an FG DLL IS loaded, the game may still request high queue depths
    // (e.g. Crimson Desert requests 4 with standard 2x DLSS FG), so the
    // latency hint would misclassify 2x as 3x. Trust cadence detection instead.
    UINT game_lat = GetGameRequestedLatency();
    int lat_hint = 1;
    if (!fg_dll && !fsr_dll) {
        if (game_lat >= 3) {
            lat_hint = static_cast<int>(game_lat);
            if (lat_hint > 6) lat_hint = 6;
        }
    }

    // FPS-based monitor is ground truth for runtime FG state.
    // It detects FG on/off from the output/native FPS ratio.
    // When it has data, trust it over DLL presence (DLLs stay loaded
    // even when the game disables FG, e.g. in menus).
    int fps_tier = ul_fg_monitor::GetTier();
    if (fps_tier >= 2) return fps_tier;
    if (fps_tier == 0 && ul_fg_monitor::HasData()) return 1;  // confirmed no FG

    // No FPS-based data yet — fall back to static detection.
    // DMFG latency hint (no DLL case)
    if (lat_hint > 1) return lat_hint;

    // DLL loaded = assume 2x until FPS monitor confirms otherwise
    if (fg_dll || fsr_dll) return 2;

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

void UlLimiter::Init(HMODULE addon_module) {
    if (!latency_buf_)
        latency_buf_ = new (std::nothrow) NvLatencyResult{};

    if (g_cfg.csv_diagnostics.load()) {
        diag_csv_logger_.Open(addon_module);
    }
}

void UlLimiter::Shutdown() {
    diag_csv_logger_.Close();

    // Restore frame splitting state before shutdown
#ifdef _WIN64
    frame_split_ctrl_.Restore();
#endif

    // Vulkan cleanup
#ifdef _WIN64
    if (vk_reflex_) {
        vk_reflex_->Shutdown();
        vk_reflex_ = nullptr;
    }
#endif

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

#ifdef _WIN64
    // Init GSync detection and frame splitting control now that NVAPI is loaded
    if (!gsync_detector_.resolved)
        gsync_detector_.Init();
    if (!frame_split_ctrl_.resolved)
        frame_split_ctrl_.Init();
#endif

    return true;
}

#ifdef _WIN64
void UlLimiter::ConnectVulkanReflex(VkReflex* vk) {
    vk_reflex_ = vk;
    ul_log::Write("ConnectVulkanReflex: Vulkan Reflex backend attached (active=%d)",
                  vk ? vk->IsActive() : 0);
}

void UlLimiter::AcquireGSyncSurface(IUnknown* device, IUnknown* back_buffer) {
    gsync_detector_.AcquireSurfaceHandle(device, back_buffer);
}
#endif

void UlLimiter::ResetAdaptiveState() {
    pipeline_predictor_.Reset();
    pipeline_stats_ = PipelineStats{};
    boost_ctrl_.Reset();
    qpc_monitor_.Reset();
    consistency_buf_.Reset(DetectFGDivisor());
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
        // FG path: consistency buffer + FG overhead offset
        int32_t fg_adj = consistency_buffer_us;
        if (ps.adaptive_fg_offset_us > 0)
            fg_adj += ps.adaptive_fg_offset_us;

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
#ifdef _WIN64
                                            gsync_detector_.gsync_active
#else
                                            false
#endif
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
#ifdef _WIN64
    if (VK_REFLEX_ACTIVE()) {
        memset(latency_buf_, 0, sizeof(*latency_buf_));
        latency_buf_->version = NV_LATENCY_RESULT_VER;
        if (!vk_reflex_->GetLatencyTimings(latency_buf_)) return;
    }
    // DX path
    else
#endif
    {
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
    float target_load_ratio = 0.0f;  // target-based GPU load for site selection + load gate
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
        // Hysteresis band: only switch when gpu_load crosses 0.55 or 0.70
        // to avoid oscillation when hovering near the boundary.
        if (target_load_ratio < 0.55f)
            ps.auto_site = SIM_START;
        else if (target_load_ratio > 0.70f)
            ps.auto_site = PRESENT_FINISH;
        // else: keep previous site (dead zone)
    } else if (ps.bottleneck == Bottleneck::Gpu)
        ps.auto_site = SIM_START;
    else if (ps.bottleneck == Bottleneck::CpuSim || ps.bottleneck == Bottleneck::CpuSubmit)
        ps.auto_site = PRESENT_FINISH;
    else {
        if (target_load_ratio > 0.85f)
            ps.auto_site = SIM_START;
        else if (target_load_ratio < 0.65f)
            ps.auto_site = PRESENT_FINISH;
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

        // Cadence response and unified FG adjustment removed — replaced by ConsistencyBuffer
        // ComputeStats so CV is fresh for the consistency buffer tick
        pred.cadence.ComputeStats();
    }

    // --- PLL: phase-locked grid correction from display feedback ---
    // Use the most recent presentEndTime to measure phase error between
    // our grid and actual display timing. Nudge grid epoch to converge.
    if (grid_interval_ns_ > 0 && grid_epoch_ns_ > 0) {
        uint64_t newest_id = 0;
        uint64_t newest_present = 0;
        for (int i = 0; i < 64; i++) {
            auto& f = latency_buf_->frameReport[i];
            if (f.frameID > newest_id && f.presentEndTime > 0) {
                newest_id = f.frameID;
                newest_present = f.presentEndTime;
            }
        }

        if (newest_id > last_pll_frame_id_ && newest_present > 0) {
            last_pll_frame_id_ = newest_id;

            // Convert presentEndTime (QPC ticks) to nanoseconds
            int64_t actual_ns = static_cast<int64_t>(
                static_cast<double>(newest_present) * 1e9
                / static_cast<double>(ul_timing::g_qpc_freq));

            // Nearest grid slot to the actual present time
            int64_t elapsed = actual_ns - grid_epoch_ns_;
            int64_t expected_slot = (elapsed / grid_interval_ns_) * grid_interval_ns_
                                  + grid_epoch_ns_;
            int64_t phase_error_ns = actual_ns - expected_slot;

            // Wrap to [-interval/2, +interval/2] for slot ambiguity
            if (phase_error_ns > grid_interval_ns_ / 2)
                phase_error_ns -= grid_interval_ns_;
            else if (phase_error_ns < -grid_interval_ns_ / 2)
                phase_error_ns += grid_interval_ns_;

            // EMA smooth the error
            pll_smoothed_error_ns_ = pll_smoothed_error_ns_ * (1.0f - kPllAlpha)
                                   + static_cast<float>(phase_error_ns) * kPllAlpha;

            // Nudge grid epoch
            int64_t correction = static_cast<int64_t>(
                pll_smoothed_error_ns_ * kPllCorrectionGain);
            if (correction != 0)
                grid_epoch_ns_ += correction;
        }
    }

    // --- GPU Load Gate ---
    // When GPU load is below 50%, freeze ConsistencyBuffer tick only.
    // DynamicPacing and BoostController continue running as before.
    // Use target-based load ratio for the gate — in overload mode the
    // cadence-based ratio is artificially low and would falsely close the gate.
    bool was_gate_open = gpu_load_gate_open_;
    gpu_load_gate_open_ = (target_load_ratio >= 0.50f);

    // Flush overload state when transitioning from gameplay to menu/loading
    // (gate closing). Overload detection from gameplay is invalid in menus.
    if (was_gate_open && !gpu_load_gate_open_) {
        gpu_overload_mode_ = false;
        gpu_overload_count_ = 0;
        gpu_recover_count_ = 0;
    }

    if (gpu_load_gate_open_) {
        // --- FG tier change detection ---
        // DetectFGDivisor() returns the latency-hint-aware base tier (DLL
        // presence for standard FG, latency hint for DMFG).
        // g_fps_fg_tier (from main.cpp) provides the FPS-based multiplier
        // computed from output_fps / native_fps — immune to GPU load
        // inflating cadence mean_delta.  Prefer it when available.
        int fg_tier = DetectFGDivisor();
        int dmfg_floor = fg_tier;
        is_dmfg_ = (dmfg_floor >= 3);

        // FPS-based tier override (DLL-based FG only — DMFG uses latency hint)
        int fps_tier = ul_fg_monitor::GetTier();
        if (!is_dmfg_ && fps_tier >= 2) {
            fg_tier = fps_tier;
        }
        if (fg_tier != last_fg_tier_) {
            // Only reset the consistency buffer when the tuning params actually
            // change. DMFG can oscillate between 4x/5x/6x frequently — all use
            // kTier4xPlusMFG, so resetting on every shift would cause repeated
            // buffer spikes for no benefit.
            ConsistencyBuffer::TuningParams prev_params = consistency_buf_.active_params;
            consistency_buf_.SelectTier(fg_tier);
            if (consistency_buf_.active_params.initial_buffer_us != prev_params.initial_buffer_us
                || consistency_buf_.active_params.max_buffer_us != prev_params.max_buffer_us) {
                consistency_buf_.Reset(fg_tier);
            }

            // Flush GPU overload state — FG state change fundamentally changes
            // what "target render FPS" means, so overload detection from the
            // previous FG state is invalid.
            gpu_overload_mode_ = false;
            gpu_overload_count_ = 0;
            gpu_recover_count_ = 0;
            pipeline_predictor_.cadence.Reset();

            last_fg_tier_ = fg_tier;
        }

        // --- GSync active detection ---
#ifdef _WIN64
        gsync_detector_.Poll(dev_, ul_timing::NowQpc());

        // --- Frame splitting control ---
        if (gsync_detector_.gsync_active && fg_tier > 1) {
            if (frame_split_ctrl_.display_id == 0 && hwnd_)
                frame_split_ctrl_.ResolveDisplayId(hwnd_);
            frame_split_ctrl_.Disable();
        } else {
            frame_split_ctrl_.Restore();
        }
#endif

        // --- VRR proximity ---
        NvU32 target_iv_us = 0;
        {
            float rfps = ComputeRenderFps();
            if (rfps > 0.0f)
                target_iv_us = static_cast<NvU32>(std::round(1'000'000.0 / static_cast<double>(rfps)));
        }
        float vrr_proximity = ComputeVrrProximity(hwnd_, target_iv_us);

        // Gate VRR proximity: when GSync is not active, disable VRR-specific behavior
#ifdef _WIN64
        if (!gsync_detector_.gsync_active)
            vrr_proximity = 0.0f;
#endif

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
#ifdef _WIN64
                                                              gsync_detector_.gsync_active
#else
                                                              false
#endif
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
#ifdef _WIN64
                gsync_detector_.gsync_active,
#else
                false,
#endif
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

        int suggested = 1;
        if (margin < 400.0f && miss_rate < 0.05f)
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
    // Vulkan path: use VK_NV_low_latency2
#ifdef _WIN64
    if (VK_REFLEX_ACTIVE()) {
        // When the game uses native Reflex (calls vkSetLatencySleepModeNV /
        // vkLatencySleepNV itself), do NOT call SetSleepMode or Sleep — our
        // calls would conflict with the game's own sleep cycle and crash.
        // The game handles its own pacing; we just provide timing backstop.
        if (g_game_uses_reflex.load(std::memory_order_relaxed))
            return;

        // Non-native: update driver hints only, no semaphore wait.
        // DoTimingFallback handles actual pacing.
        NvSleepParams p = BuildSleepParams();
        vk_reflex_->SetSleepMode(
            p.bLowLatencyMode != 0,
            p.bLowLatencyBoost != 0,
            p.minimumIntervalUs);
        return;
    }
#endif

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

void UlLimiter::DoOwnSleep() {
    // Vulkan path: update driver hints, then use our grid for timing
#ifdef _WIN64
    if (VK_REFLEX_ACTIVE()) {
        // For native Reflex games, SetSleepMode is handled by the game
        // (forwarded to driver via our wrapper). For non-native, we set it here.
        bool native = g_game_uses_reflex.load(std::memory_order_relaxed);

        NvSleepParams p = BuildSleepParams();
        if (p.minimumIntervalUs == 0) return;

        if (!native) {
            vk_reflex_->SetSleepMode(
                p.bLowLatencyMode != 0,
                p.bLowLatencyBoost != 0,
                p.minimumIntervalUs);
        }

        // Grid sleep with our timers.
        //
        // Two intervals are in play:
        //   SetSleepMode (above): render rate (p.minimumIntervalUs) — tells
        //     the driver how fast we're rendering. Unchanged.
        //   Grid interval: output rate (render interval / fg_divisor) — paces
        //     the present callbacks, which fire for every frame including
        //     generated ones. Without this division, generated frames get
        //     blocked by the render-rate grid.
        //
        // DX path stays unchanged — on DX, OnPresent only fires for real
        // frames, so the grid interval matches the render rate.
        int fg_div = DetectFGDivisor();
        int64_t grid_ns = static_cast<int64_t>(p.minimumIntervalUs) * 1000LL;
        if (fg_div > 1)
            grid_ns /= fg_div;  // output rate for grid only
        int64_t now = ul_timing::NowNs();

        if (grid_epoch_ns_ == 0) {
            grid_epoch_ns_ = now;
            grid_interval_ns_ = grid_ns;
            grid_next_ns_ = now + grid_ns;
        } else {
            constexpr float kGridAlpha = 0.05f;
            grid_interval_ns_ = static_cast<int64_t>(
                grid_interval_ns_ * (1.0 - kGridAlpha) + grid_ns * kGridAlpha);
            if (grid_next_ns_ > now) {
                ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);
                grid_next_ns_ += grid_interval_ns_;
            } else {
                int64_t elapsed = now - grid_epoch_ns_;
                int64_t slots = elapsed / grid_interval_ns_;
                grid_next_ns_ = grid_epoch_ns_ + (slots + 1) * grid_interval_ns_;
            }
        }

        // Vulkan Sleep passthrough — keeps the driver's semaphore-based frame
        // tracking alive. Only needed for non-native games; native games
        // already call vkLatencySleepNV themselves (forwarded by our wrapper),
        // so a second Sleep would double-signal the semaphore.
        if (!native) {
            __try {
                vk_reflex_->Sleep();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                // Absorb — grid already paced the frame.
            }
        }

        return;  // Vulkan path complete — no DX passthrough
    }
#endif

    // DX path
    if (!ReflexActive() || !dev_) return;

    // Build params — real interval for both grid and driver hint
    NvSleepParams p = BuildSleepParams();
    if (p.minimumIntervalUs == 0) return;

    // Convert adjusted interval to nanoseconds for the grid
    int64_t frame_ns = static_cast<int64_t>(p.minimumIntervalUs) * 1000LL;
    int64_t now = ul_timing::NowNs();

    // --- Phase-locked grid (DX path) ---
    if (grid_epoch_ns_ == 0) {
        // First frame — establish grid, no sleep
        grid_epoch_ns_ = now;
        grid_interval_ns_ = frame_ns;
        grid_next_ns_ = now + frame_ns;
    } else {
        // Smooth the grid interval to prevent sawtooth from frame-to-frame
        // micro-oscillations in adaptive corrections. Sustained changes
        // (consistency buffer, FG offset) still converge in ~20 frames.
        constexpr float kGridAlpha = 0.05f;
        grid_interval_ns_ = static_cast<int64_t>(
            grid_interval_ns_ * (1.0 - kGridAlpha) + frame_ns * kGridAlpha);

        if (grid_next_ns_ > now) {
            // Frame arrived early — sleep until grid slot
            ul_timing::SleepUntilNs(grid_next_ns_, htimer_fallback_);
            grid_next_ns_ += grid_interval_ns_;
        } else {
            // Frame arrived late — snap grid forward to next slot ahead of now
            int64_t elapsed = now - grid_epoch_ns_;
            int64_t slots = elapsed / grid_interval_ns_;
            grid_next_ns_ = grid_epoch_ns_ + (slots + 1) * grid_interval_ns_;
        }
    }

    // --- Reflex passthrough ---
    // Pass real interval so driver maintains correct internal state.
    // InvokeSleep returns near-instantly since we already consumed the wait.
    // SEH guard: device/swapchain can be in a transitional state during
    // alt-tab or swapchain recreation — protect against driver crashes.
    __try {
        MaybeUpdateSleepMode(p);
        InvokeSleep(dev_);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Silently absorb — the grid sleep already paced the frame.
        // The driver will recover on the next valid call.
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
    bool has_reflex = (ReflexActive() && dev_) || VK_REFLEX_ACTIVE();
    if (!has_reflex) return;
    if (!warmup_done_) return;
    if (is_background_) return;

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
        DoOwnSleep();
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

        // Flush defaults so new config keys appear in the INI (one-shot, deferred from init)
        static bool s_settings_flushed = false;
        if (!s_settings_flushed) {
            SaveSettings();
            s_settings_flushed = true;
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
            // Clear stale FPS-based FG tier — background limiter distorts
            // the output/native FPS ratio.  Falls back to DLL-based detection
            // until the ratio stabilizes after warmup.
            ul_fg_monitor::Reset();
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

    // Feed QPC timestamp to local brake signal monitor
    qpc_monitor_.Feed(now_qpc);

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
    bool any_reflex = (ReflexActive() && dev_) || VK_REFLEX_ACTIVE();
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
    // DoOwnSleep handles all timing via our grid, then passes through to
    // Reflex for driver state management. No separate DoTimingFallback needed.
    bool vk_active = VK_REFLEX_ACTIVE();
    if (vk_active) {
#ifdef _WIN64
        // Both native and non-native Vulkan Reflex: DoOwnSleep handles grid
        // sleep for FPS cap enforcement. For native games, it skips SetSleepMode
        // (the game's own calls are forwarded to the driver by our wrapper).
        // For non-native, it also sets our sleep mode params.
        DoOwnSleep();
#endif
    } else if (ReflexActive() && dev_) {
        if (g_game_uses_reflex.load(std::memory_order_relaxed)) {
            // Marker pacing — OnMarker calls DoOwnSleep at enforcement site.
            // Just update sleep mode params here (no sleep, no grid).
            NvSleepParams p = BuildSleepParams();
            MaybeUpdateSleepMode(p);
        } else {
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
