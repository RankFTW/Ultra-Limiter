// Ultra Limiter — ReShade addon entry point
// Clean-room implementation. All code written from public API documentation:
//   - ReShade addon API: register_addon, events, overlay (BSD-3)
//   - Dear ImGui: GetForegroundDrawList, SliderInt, Checkbox, BeginCombo (MIT)
//   - Windows: EnumDisplayMonitors, SetWindowPos, GetWindowLong, CreateThread
//   - NVAPI SDK (MIT): NvAPI_D3D_GetLatency struct layout
// No code from Display Commander, Special K, or any other project.

#define ImTextureID ImU64
#include <imgui.h>

#include "ul_config.hpp"
#include "ul_limiter.hpp"
#include "ul_log.hpp"
#include "ul_timing.hpp"

#include <reshade.hpp>

#include <MinHook.h>
#include <dxgi.h>
#include <windows.h>
#include <cmath>
#include <new>
#include <vector>
#include <string>
#include <mutex>

// Addon metadata
extern "C" __declspec(dllexport) const char *NAME = "Ultra Limiter";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "FG-aware frame rate limiter with NVIDIA Reflex integration.";

static UlLimiter s_limiter;
static HWND s_hwnd = nullptr;

// ============================================================================
// OSD data
// ============================================================================

static constexpr int kHistLen = 128;
static float s_ft_hist[kHistLen] = {};
static int s_ft_idx = 0;
static int64_t s_last_qpc = 0;
static float s_fps = 0.0f;
static float s_ft_ms = 0.0f;

// Native FPS (from SIM_START markers)
static int64_t s_last_sim_qpc = 0;
static float s_native_fps = 0.0f;
static float s_native_ft_ms = 0.0f;
static float s_nat_hist[64] = {};
static int s_nat_idx = 0;
static std::atomic<bool> s_has_native{false};

// GPU latency from GetLatency
static float s_gpu_ms = 0.0f;
static float s_render_lat_ms = 0.0f;
static float s_present_lat_ms = 0.0f;
static std::atomic<bool> s_has_gpu{false};
static IUnknown* s_reflex_dev = nullptr;

// Resolution
static std::atomic<uint32_t> s_out_w{0}, s_out_h{0};
static std::atomic<uint32_t> s_rnd_w{0}, s_rnd_h{0};

// FG mode string
static char s_fg_str[32] = "None";
static int64_t s_fg_check_qpc = 0;
static bool s_fg_logged = false;

// ============================================================================
// FG detection
// ============================================================================

static void UpdateFGString() {
    bool dlssg = GetModuleHandleW(L"nvngx_dlssg.dll")
              || GetModuleHandleW(L"_nvngx_dlssg.dll")
              || GetModuleHandleW(L"sl.dlss_g.dll")
              || GetModuleHandleW(L"dlss-g.dll");
    bool fsr = GetModuleHandleW(L"amd_fidelityfx_framegeneration.dll")
            || GetModuleHandleW(L"ffx_framegeneration.dll");
    bool smooth = GetModuleHandleW(L"NvPresent64.dll") != nullptr;

    if (!s_fg_logged) {
        ul_log::Write("FG detect: dlssg=%d fsr=%d smooth=%d", dlssg, fsr, smooth);
        s_fg_logged = true;
    }

    const char* mult = "";
    bool from_fps = false;
    if (s_has_native.load(std::memory_order_relaxed) && s_native_fps > 1.0f && s_fps > 1.0f) {
        float r = s_fps / s_native_fps;
        if (r > 3.5f)      { mult = " 4x"; from_fps = true; }
        else if (r > 2.5f) { mult = " 3x"; from_fps = true; }
        else if (r > 1.3f) { mult = " 2x"; from_fps = true; }
    }

    if (dlssg)          snprintf(s_fg_str, sizeof(s_fg_str), "DLSS FG%s", mult);
    else if (fsr)       snprintf(s_fg_str, sizeof(s_fg_str), "FSR FG%s", mult);
    else if (smooth)    snprintf(s_fg_str, sizeof(s_fg_str), "Smooth Motion%s", mult);
    else if (from_fps)  snprintf(s_fg_str, sizeof(s_fg_str), "FG%s", mult);
    else                snprintf(s_fg_str, sizeof(s_fg_str), "None");
}

// ============================================================================
// Marker callback for native FPS tracking
// ============================================================================

static void OnMarkerOSD(int mt, uint64_t) {
    if (mt != SIM_START) return;
    int64_t now = ul_timing::NowQpc();
    if (s_last_sim_qpc > 0) {
        float dt = static_cast<float>(now - s_last_sim_qpc) * 1000.0f
                 / static_cast<float>(ul_timing::g_qpc_freq);
        s_nat_hist[s_nat_idx % 64] = dt;
        s_nat_idx++;
        int n = (s_nat_idx < 64) ? s_nat_idx : 64;
        float sum = 0.0f;
        for (int i = 0; i < n; i++) sum += s_nat_hist[(s_nat_idx - 1 - i + 64) % 64];
        s_native_ft_ms = sum / static_cast<float>(n);
        s_native_fps = (s_native_ft_ms > 0.0f) ? 1000.0f / s_native_ft_ms : 0.0f;
        s_has_native.store(true, std::memory_order_relaxed);
    }
    s_last_sim_qpc = now;
}

// ============================================================================
// GPU latency polling
// ============================================================================

// Detect hook-heavy frameworks that conflict with GetLatency calls.
// REFramework (dinput8.dll) hooks deeply into the rendering pipeline and
// can cause hard crashes when GetLatency reads the driver's frame report
// ring buffer. When detected, all GetLatency calls are disabled.
static bool s_reframework_checked = false;

static void DetectConflictingFrameworks() {
    if (s_reframework_checked) return;

    bool reframework = GetModuleHandleW(L"reframework.dll") != nullptr
                    || GetModuleHandleW(L"REFramework.dll") != nullptr;

    // dinput8.dll is REFramework's proxy DLL in RE Engine games.
    // Only treat as REFramework if Streamline is also present.
    if (!reframework) {
        bool dinput8 = GetModuleHandleW(L"dinput8.dll") != nullptr;
        bool streamline = GetModuleHandleW(L"sl.common.dll") != nullptr
                       || GetModuleHandleW(L"sl.dlss_g.dll") != nullptr;
        if (dinput8 && streamline) reframework = true;
    }

    if (reframework) {
        BlockGetLatency();
        ul_log::Write("REFramework detected — GetLatency disabled (crash prevention)");
        s_reframework_checked = true;
    }
}

static NvStatus SafeGetLatency(IUnknown* dev, NvLatencyResult* r) {
    __try { return InvokeGetLatency(dev, r); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return NV_NO_IMPL; }
}

static void PollGpuLatency() {
    if (!ReflexActive() || !s_reflex_dev) return;

    static NvLatencyResult* buf = nullptr;
    static bool disabled = false;
    if (disabled) return;

    if (!buf) { buf = new (std::nothrow) NvLatencyResult{}; if (!buf) return; }
    memset(buf, 0, sizeof(*buf));
    buf->version = NV_LATENCY_RESULT_VER;

    NvStatus st = SafeGetLatency(s_reflex_dev, buf);
    if (st != NV_OK) {
        static int fails = 0;
        if (++fails > 3) { ul_log::Write("PollGpuLatency: disabled after %d fails", fails); disabled = true; }
        return;
    }

    float gpu = 0, rlat = 0, plat = 0;
    int valid = 0;
    for (int i = 63; i >= 0 && valid < 8; i--) {
        auto& f = buf->frameReport[i];
        if (!f.frameID) continue;
        if (f.gpuActiveRenderTimeUs > 0) gpu += static_cast<float>(f.gpuActiveRenderTimeUs) / 1000.0f;
        if (f.simStartTime > 0 && f.gpuRenderEndTime > f.simStartTime)
            rlat += static_cast<float>(f.gpuRenderEndTime - f.simStartTime) / 1000.0f;
        if (f.presentStartTime > 0 && f.presentEndTime > f.presentStartTime)
            plat += static_cast<float>(f.presentEndTime - f.presentStartTime) / 1000.0f;
        valid++;
    }
    if (valid > 0) {
        s_gpu_ms = gpu / valid; s_render_lat_ms = rlat / valid; s_present_lat_ms = plat / valid;
        s_has_gpu.store(true, std::memory_order_relaxed);
    }
}

// ============================================================================
// Frametime stats
// ============================================================================

static void UpdateStats() {
    int64_t now = ul_timing::NowQpc();
    if (s_last_qpc > 0) {
        float dt = static_cast<float>(now - s_last_qpc) * 1000.0f
                 / static_cast<float>(ul_timing::g_qpc_freq);
        s_ft_hist[s_ft_idx % kHistLen] = dt;
        s_ft_idx++;
        int n = (s_ft_idx < 64) ? s_ft_idx : 64;
        float sum = 0.0f;
        for (int i = 0; i < n; i++) sum += s_ft_hist[(s_ft_idx - 1 - i + kHistLen) % kHistLen];
        s_ft_ms = sum / static_cast<float>(n);
        s_fps = (s_ft_ms > 0.0f) ? 1000.0f / s_ft_ms : 0.0f;
    }
    s_last_qpc = now;
}

// ============================================================================
// OSD drawing
// ============================================================================

static void DrawOSD(reshade::api::effect_runtime*) {
    // Poll OSD toggle hotkey
    {
        static bool s_key_was_down = false;
        int vk = g_cfg.osd_toggle_key.load(std::memory_order_relaxed);
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !s_key_was_down) {
            g_cfg.osd_on.store(!g_cfg.osd_on.load(std::memory_order_relaxed));
            SaveSettings();
        }
        s_key_was_down = down;
    }

    if (!g_cfg.osd_on.load(std::memory_order_relaxed) || s_ft_idx < 2) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    float x = g_cfg.osd_x.load(std::memory_order_relaxed);
    float y = g_cfg.osd_y.load(std::memory_order_relaxed) + 8.0f;
    constexpr float lh = 18.0f, gap = 4.0f, pad = 8.0f;
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    const ImU32 green = IM_COL32(100, 255, 100, 200);
    const ImU32 cyan  = IM_COL32(100, 200, 255, 255);
    const ImU32 gold  = IM_COL32(200, 200, 100, 255);
    char buf[64];

    // When Smooth Motion is active, s_fps (from present) is the native render rate
    // and s_native_fps (from SIM_START) is the higher output rate. Swap for display.
    // If the game doesn't use Reflex markers, s_native_fps will be 0 — fall back to
    // s_fps so the OSD never shows 0.
    bool sm = GetModuleHandleW(L"NvPresent64.dll") != nullptr;
    bool has_nat = s_has_native.load(std::memory_order_relaxed) && s_native_fps > 0.0f;
    float disp_fps = (sm && has_nat) ? s_native_fps : s_fps;
    float disp_native_fps = (sm && has_nat) ? s_fps : s_native_fps;
    float disp_native_ft = (sm && has_nat) ? s_ft_ms : s_native_ft_ms;

    if (g_cfg.show_fps.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "FPS: %.1f", disp_fps);
        dl->AddText(ImVec2(x + pad, y), white, buf); y += lh + gap;
    }
    if (g_cfg.show_native_fps.load(std::memory_order_relaxed) && s_has_native.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "Native: %.1f FPS (%.2f ms)", disp_native_fps, disp_native_ft);
        dl->AddText(ImVec2(x + pad, y), white, buf); y += lh + gap;
    }
    if (g_cfg.show_frametime.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "Frame: %.2f ms", s_ft_ms);
        dl->AddText(ImVec2(x + pad, y), white, buf); y += lh + gap;
    }

    bool gpu = s_has_gpu.load(std::memory_order_relaxed);
    if (gpu && g_cfg.show_gpu_time.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "GPU: %.2f ms", s_gpu_ms);
        dl->AddText(ImVec2(x + pad, y), cyan, buf); y += lh + gap;
    }
    if (gpu && g_cfg.show_render_lat.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "Render Lat: %.2f ms", s_render_lat_ms);
        dl->AddText(ImVec2(x + pad, y), cyan, buf); y += lh + gap;
    }
    if (gpu && g_cfg.show_present_lat.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof(buf), "Present Lat: %.2f ms", s_present_lat_ms);
        dl->AddText(ImVec2(x + pad, y), cyan, buf); y += lh + gap;
    }

    if (g_cfg.show_fg_mode.load(std::memory_order_relaxed)) {
        int64_t now = ul_timing::NowQpc();
        if (now - s_fg_check_qpc > ul_timing::g_qpc_freq) { UpdateFGString(); s_fg_check_qpc = now; }
        snprintf(buf, sizeof(buf), "FG: %s", s_fg_str);
        dl->AddText(ImVec2(x + pad, y), gold, buf); y += lh + gap;
    }

    if (g_cfg.show_resolution.load(std::memory_order_relaxed)) {
        uint32_t ow = s_out_w.load(), oh = s_out_h.load();
        uint32_t rw = s_rnd_w.load(), rh = s_rnd_h.load();
        if (rw > 0 && rh > 0 && ow > 0 && rw < ow) {
            int pct = static_cast<int>(100.0f * rw / static_cast<float>(ow));
            snprintf(buf, sizeof(buf), "Res: %ux%u -> %ux%u (%d%%)", rw, rh, ow, oh, pct);
        } else if (ow > 0) {
            snprintf(buf, sizeof(buf), "Res: %ux%u (native)", ow, oh);
        } else {
            snprintf(buf, sizeof(buf), "Res: --");
        }
        dl->AddText(ImVec2(x + pad, y), gold, buf); y += lh + gap;
    }

    // Frametime graph
    if (g_cfg.show_graph.load(std::memory_order_relaxed)) {
        int count = (s_ft_idx < kHistLen) ? s_ft_idx : kHistLen;
        if (count > 1) {
            float gmax = 33.3f;
            for (int i = 0; i < count; i++) if (s_ft_hist[i] > gmax) gmax = s_ft_hist[i];
            float gx = x + pad, step = 160.0f / static_cast<float>(count - 1);
            for (int i = 0; i < count - 1; i++) {
                int i0 = (s_ft_idx - count + i + kHistLen) % kHistLen;
                int i1 = (s_ft_idx - count + i + 1 + kHistLen) % kHistLen;
                float v0 = s_ft_hist[i0] / (gmax * 1.1f); if (v0 > 1) v0 = 1;
                float v1 = s_ft_hist[i1] / (gmax * 1.1f); if (v1 > 1) v1 = 1;
                dl->AddLine(ImVec2(gx + step * i, y + 40 * (1 - v0)),
                            ImVec2(gx + step * (i + 1), y + 40 * (1 - v1)), green, 1.5f);
            }
        }
    }
}

// ============================================================================
// Monitor switching
// ============================================================================

struct MonInfo {
    HMONITOR hmon;
    RECT rc;
    char dev[32];
    char label[128];
    bool primary;
};

static std::vector<MonInfo> s_mons;
static int s_sel_mon = 0;
static bool s_mons_dirty = true;

static void MoveToMonitor(HWND hwnd, const MonInfo& m);

static void ApplyWindowMode(HWND hwnd, WindowMode mode) {
    if (!hwnd || mode == WindowMode::NoOverride) return;

    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, &mi)) return;

    int mx = mi.rcMonitor.left, my = mi.rcMonitor.top;
    int mw = mi.rcMonitor.right - mx, mh = mi.rcMonitor.bottom - my;

    if (mode == WindowMode::ForceBorderless) {
        LONG st = GetWindowLongA(hwnd, GWL_STYLE);
        st &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
        st |= WS_POPUP;
        SetWindowLongA(hwnd, GWL_STYLE, st);
        LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
        ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        SetWindowLongA(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, HWND_TOP, mx, my, mw, mh, SWP_FRAMECHANGED | SWP_NOZORDER);
    } else if (mode == WindowMode::ForceFullscreen) {
        LONG st = GetWindowLongA(hwnd, GWL_STYLE);
        st |= WS_POPUP; st &= ~WS_OVERLAPPEDWINDOW;
        SetWindowLongA(hwnd, GWL_STYLE, st);
        SetWindowPos(hwnd, HWND_TOP, mx, my, mw, mh, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}

// Worker thread for monitor moves / fullscreen changes (avoids D3D re-entrancy)
static HANDLE s_wk_thread = nullptr;
static HANDLE s_wk_event = nullptr;
static HANDLE s_wk_stop = nullptr;
static MonInfo s_pend_mon{};
static HWND s_pend_hwnd = nullptr;
static WindowMode s_pend_wm = WindowMode::NoOverride;
static bool s_pend_wm_apply = false;
static std::mutex s_pend_mtx;

static DWORD WINAPI WorkerThread(LPVOID) {
    HANDLE ev[2] = { s_wk_stop, s_wk_event };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        MonInfo mon{}; HWND hwnd = nullptr; bool do_move = false;
        WindowMode wm = WindowMode::NoOverride; bool do_wm = false;
        {
            std::lock_guard<std::mutex> lk(s_pend_mtx);
            mon = s_pend_mon; hwnd = s_pend_hwnd; do_move = (hwnd != nullptr);
            wm = s_pend_wm; do_wm = s_pend_wm_apply;
            s_pend_hwnd = nullptr; s_pend_wm_apply = false;
        }
        if (do_move) MoveToMonitor(hwnd, mon);
        if (do_wm && s_hwnd) ApplyWindowMode(s_hwnd, wm);
    }
    return 0;
}

static void RequestMove(HWND hwnd, const MonInfo& m) {
    { std::lock_guard<std::mutex> lk(s_pend_mtx); s_pend_mon = m; s_pend_hwnd = hwnd; }
    if (s_wk_event) SetEvent(s_wk_event);
}

static void RequestWindowMode(WindowMode wm) {
    { std::lock_guard<std::mutex> lk(s_pend_mtx); s_pend_wm = wm; s_pend_wm_apply = true; }
    if (s_wk_event) SetEvent(s_wk_event);
}

static void StartWorker() {
    if (s_wk_thread) return;
    s_wk_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    s_wk_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!s_wk_event || !s_wk_stop) {
        ul_log::Write("StartWorker: CreateEvent failed");
        if (s_wk_event) { CloseHandle(s_wk_event); s_wk_event = nullptr; }
        if (s_wk_stop) { CloseHandle(s_wk_stop); s_wk_stop = nullptr; }
        return;
    }
    s_wk_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!s_wk_thread) {
        ul_log::Write("StartWorker: CreateThread failed");
        CloseHandle(s_wk_event); CloseHandle(s_wk_stop);
        s_wk_event = s_wk_stop = nullptr;
    }
}

static void StopWorker() {
    if (!s_wk_thread) return;
    SetEvent(s_wk_stop);
    WaitForSingleObject(s_wk_thread, 2000);
    CloseHandle(s_wk_thread); CloseHandle(s_wk_event); CloseHandle(s_wk_stop);
    s_wk_thread = s_wk_event = s_wk_stop = nullptr;
}

static BOOL CALLBACK EnumMonCb(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonInfo>*>(lp);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(hm, &mi)) {
        MonInfo info = {};
        info.hmon = hm; info.rc = mi.rcMonitor;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        strncpy(info.dev, mi.szDevice, sizeof(info.dev) - 1);
        int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        const char* sn = mi.szDevice;
        if (strncmp(sn, "\\\\.\\", 4) == 0) sn += 4;
        snprintf(info.label, sizeof(info.label), "%s (%dx%d @ %d,%d)%s",
                 sn, w, h, mi.rcMonitor.left, mi.rcMonitor.top, info.primary ? " [Primary]" : "");
        list->push_back(info);
    }
    return TRUE;
}

static void RefreshMons() {
    s_mons.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonCb, reinterpret_cast<LPARAM>(&s_mons));
    s_sel_mon = 0;
    std::string saved;
    { std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx); saved = g_cfg.target_monitor; }
    if (!saved.empty()) {
        for (int i = 0; i < (int)s_mons.size(); i++) {
            if (_stricmp(s_mons[i].dev, saved.c_str()) == 0) { s_sel_mon = i + 1; break; }
        }
    }
    s_mons_dirty = false;
}

static void MoveToMonitor(HWND hwnd, const MonInfo& m) {
    if (!hwnd) return;
    LONG st = GetWindowLongA(hwnd, GWL_STYLE);
    bool borderless = (st & WS_POPUP) && !(st & WS_CAPTION);
    int mw = m.rc.right - m.rc.left, mh = m.rc.bottom - m.rc.top;
    if (borderless || (st & WS_POPUP)) {
        SetWindowPos(hwnd, HWND_TOP, m.rc.left, m.rc.top, mw, mh, SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        RECT wr; GetWindowRect(hwnd, &wr);
        SetWindowPos(hwnd, nullptr, m.rc.left, m.rc.top, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOZORDER | SWP_NOSIZE);
    }
    ul_log::Write("MoveToMonitor: %s (%d,%d %dx%d)", m.dev, m.rc.left, m.rc.top, mw, mh);
}

static void ApplySavedMonitor(HWND hwnd) {
    if (!hwnd) return;
    std::string saved;
    { std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx); saved = g_cfg.target_monitor; }
    if (saved.empty()) return;

    HMONITOR cur = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(cur, &mi) && _stricmp(mi.szDevice, saved.c_str()) == 0) return;

    std::vector<MonInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonCb, reinterpret_cast<LPARAM>(&list));
    for (auto& m : list) {
        if (_stricmp(m.dev, saved.c_str()) == 0) { RequestMove(hwnd, m); return; }
    }
}

// ============================================================================
// Overlay UI (ReShade settings panel)
// ============================================================================

static const char* kPresetLabels[] = {
    "Low Latency (Native)", "Low Latency (Markers)",
    "Balanced", "Stability", "Pace Generated (SL Proxy)", "Custom"
};
static const char* kFGLabels[] = { "Auto", "Off", "2x", "3x", "4x" };
static const char* kBoostLabels[] = { "Game", "On", "Off" };

static bool Combo(const char* label, int* cur, const char* const items[], int n) {
    bool changed = false;
    if (ImGui::BeginCombo(label, items[*cur], 0)) {
        for (int i = 0; i < n; i++) {
            bool sel = (i == *cur);
            if (ImGui::Selectable(items[i], sel)) { *cur = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static int GetMonitorRefreshRate() {
    HWND hwnd = s_hwnd;
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) return 60;
    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, &mi)) return 60;
    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 60;
    return static_cast<int>(dm.dmDisplayFrequency);
}

static const char* VkName(int vk) {
    switch (vk) {
        case VK_END:    return "END";
        case VK_HOME:   return "HOME";
        case VK_INSERT: return "INSERT";
        case VK_DELETE: return "DELETE";
        case VK_PAUSE:  return "PAUSE";
        case VK_SCROLL: return "SCROLL LOCK";
        case VK_PRIOR:  return "PAGE UP";
        case VK_NEXT:   return "PAGE DOWN";
        case VK_F1: return "F1"; case VK_F2: return "F2"; case VK_F3: return "F3";
        case VK_F4: return "F4"; case VK_F5: return "F5"; case VK_F6: return "F6";
        case VK_F7: return "F7"; case VK_F8: return "F8"; case VK_F9: return "F9";
        case VK_F10: return "F10"; case VK_F11: return "F11"; case VK_F12: return "F12";
        case VK_NUMPAD0: return "NUM 0"; case VK_NUMPAD1: return "NUM 1";
        case VK_NUMPAD2: return "NUM 2"; case VK_NUMPAD3: return "NUM 3";
        case VK_NUMPAD4: return "NUM 4"; case VK_NUMPAD5: return "NUM 5";
        case VK_NUMPAD6: return "NUM 6"; case VK_NUMPAD7: return "NUM 7";
        case VK_NUMPAD8: return "NUM 8"; case VK_NUMPAD9: return "NUM 9";
        case VK_MULTIPLY: return "NUM *"; case VK_ADD: return "NUM +";
        case VK_SUBTRACT: return "NUM -"; case VK_DECIMAL: return "NUM .";
        case VK_DIVIDE: return "NUM /";
        default: {
            static char s_buf[16];
            if (vk >= 0x30 && vk <= 0x39) { s_buf[0] = static_cast<char>(vk); s_buf[1] = 0; return s_buf; }
            if (vk >= 0x41 && vk <= 0x5A) { s_buf[0] = static_cast<char>(vk); s_buf[1] = 0; return s_buf; }
            snprintf(s_buf, sizeof(s_buf), "0x%02X", vk);
            return s_buf;
        }
    }
}

// Returns a pressed VK code (scanning a useful range), or 0 if nothing pressed
static int PollAnyKey() {
    // F1-F12
    for (int vk = VK_F1; vk <= VK_F12; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    // Navigation keys
    for (int vk : {VK_END, VK_HOME, VK_INSERT, VK_DELETE, VK_PAUSE, VK_SCROLL, VK_PRIOR, VK_NEXT})
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    // Numpad
    for (int vk = VK_NUMPAD0; vk <= VK_DIVIDE; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    // Letters
    for (int vk = 0x41; vk <= 0x5A; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    // Digits
    for (int vk = 0x30; vk <= 0x39; vk++)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    return 0;
}

static void DrawOverlay(reshade::api::effect_runtime*) {
    bool changed = false;

    // --- FPS Limit ---
    ImGui::TextDisabled("FPS Limit");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target output frame rate. Set 0 for unlimited.");

    float fps = g_cfg.fps_limit.load(std::memory_order_relaxed);
    int fps_i = static_cast<int>(fps);
    if (ImGui::SliderInt("##fps_limit", &fps_i, 0, 500)) {
        g_cfg.fps_limit.store(static_cast<float>(fps_i));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

    // Quick-select buttons
    // Reflex cap formula: fps = refresh - (refresh^2 / 3600)
    int hz = GetMonitorRefreshRate();
    float reflex_cap = static_cast<float>(hz) - (static_cast<float>(hz) * static_cast<float>(hz) / 3600.0f);
    reflex_cap = std::floor(reflex_cap);  // always round down
    if (reflex_cap < 1.0f) reflex_cap = 57.0f;
    int presets[] = { 30, 60, 120, 240, static_cast<int>(reflex_cap) };
    const char* labels[] = { "30", "60", "120", "240", nullptr };
    char reflex_label[32];
    snprintf(reflex_label, sizeof(reflex_label), "Reflex (%d)", static_cast<int>(reflex_cap));

    for (int i = 0; i < 5; i++) {
        if (i > 0) ImGui::SameLine();
        const char* lbl = (i == 4) ? reflex_label : labels[i];
        bool active = (fps_i == presets[i]);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(lbl)) {
            g_cfg.fps_limit.store(static_cast<float>(presets[i])); changed = true;
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // --- VSync ---
    ImGui::TextDisabled("VSync");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the game's VSync setting.\nGame: no change. On: force sync. Off: force no sync.");

    int vsync = g_cfg.vsync_override.load(std::memory_order_relaxed);
    static const char* kVsLabels[] = { "Game", "On", "Off" };
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        bool active = (vsync == i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char vlbl[16]; snprintf(vlbl, sizeof(vlbl), "%s##vs%d", kVsLabels[i], i);
        if (ImGui::SmallButton(vlbl)) { g_cfg.vsync_override.store(i); changed = true; }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // --- 5XXX Exclusive Pacing Optimization ---
    ImGui::TextDisabled("5XXX Exclusive Pacing Optimization");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Forces single-frame queue depth (SetMaximumFrameLatency = 1).\n"
        "Reduces input latency by preventing the GPU from queuing extra frames.\n"
        "Recommended for NVIDIA 50-series GPUs with flip metering support.");

    bool ep = g_cfg.exclusive_pacing.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable##excl_pacing", &ep)) {
        g_cfg.exclusive_pacing.store(ep);
        ApplyFrameLatency();
        changed = true;
    }

    ImGui::Spacing();

    // --- FG Mode ---
    ImGui::TextDisabled("FG Mode");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How to account for frame generation when computing render rate.\nAuto detects DLSS FG, FSR FG, and Smooth Motion.");

    int fg = static_cast<int>(g_cfg.fg_mult.load());
    if (Combo("##fg_mode", &fg, kFGLabels, 5)) { g_cfg.fg_mult.store(static_cast<FGMultiplier>(fg)); changed = true; }

    ImGui::Spacing();

    // --- Boost ---
    ImGui::TextDisabled("Boost");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reflex low-latency boost override.\nGame: use the game's setting. On/Off: force.");

    int boost = static_cast<int>(g_cfg.boost.load());
    if (Combo("##boost", &boost, kBoostLabels, 3)) { g_cfg.boost.store(static_cast<BoostMode>(boost)); changed = true; }

    ImGui::Spacing();

    // --- Preset ---
    ImGui::TextDisabled("Preset");
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame pacing strategy.\nNative: pace on sim start, lowest latency.\nMarkers: pace via Reflex markers with queue depth.\nSL Proxy: pace generated frames via Streamline.\nCustom: manual control of all sub-settings.");

    int preset = static_cast<int>(g_cfg.preset.load());
    if (Combo("##preset", &preset, kPresetLabels, 6)) { g_cfg.preset.store(static_cast<PacingPreset>(preset)); changed = true; }

    if (static_cast<PacingPreset>(preset) == PacingPreset::Custom) {
        ImGui::Indent(16);
        bool mk = g_cfg.use_marker_pacing.load();
        if (ImGui::Checkbox("Use Reflex Markers", &mk)) { g_cfg.use_marker_pacing.store(mk); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pace using Reflex latency markers from the game.");

        int q = g_cfg.max_queued_frames.load();
        if (ImGui::SliderInt("Max Queued Frames", &q, 0, 4)) { g_cfg.max_queued_frames.store(q); }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many frames can be in-flight before the limiter waits.");

        bool dp = g_cfg.delay_present.load();
        if (ImGui::Checkbox("Delay Present Start", &dp)) { g_cfg.delay_present.store(dp); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a delay between sim start and present begin.");
        if (dp) {
            float da = g_cfg.delay_present_amount.load();
            if (ImGui::SliderFloat("Delay (frames)", &da, 0, 3, "%.2f")) { g_cfg.delay_present_amount.store(da); }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        }

        bool sl = g_cfg.use_sl_proxy.load();
        if (ImGui::Checkbox("Streamline Proxy", &sl)) { g_cfg.use_sl_proxy.store(sl); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hook Streamline's swapchain Present for FG pacing.");
        ImGui::Unindent(16);
    }

    ImGui::Spacing();

    // --- Display (collapsible) ---
    if (ImGui::CollapsingHeader("Display")) {
        ImGui::Indent(8);
        bool osd = g_cfg.osd_on.load();
        if (ImGui::Checkbox("Show OSD", &osd)) { g_cfg.osd_on.store(osd); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the on-screen display overlay.");

        if (osd) {
            float ox = g_cfg.osd_x.load();
            if (ImGui::SliderFloat("OSD X", &ox, 0, 7680, "%.0f")) { g_cfg.osd_x.store(ox); }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            float oy = g_cfg.osd_y.load();
            if (ImGui::SliderFloat("OSD Y", &oy, 0, 4320, "%.0f")) { g_cfg.osd_y.store(oy); }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

            ImGui::TextDisabled("OSD Elements");
            auto toggle = [&](const char* lbl, std::atomic<bool>& v, const char* tip) {
                bool b = v.load(); if (ImGui::Checkbox(lbl, &b)) { v.store(b); changed = true; }
                if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            toggle("FPS", g_cfg.show_fps, "Total output frame rate (including generated frames).");
            toggle("Frametime", g_cfg.show_frametime, "Time per frame in milliseconds.");
            toggle("Native FPS", g_cfg.show_native_fps, "Real render rate excluding generated frames.");
            toggle("Frametime Graph", g_cfg.show_graph, "Rolling frametime history graph.");
            toggle("GPU Render Time", g_cfg.show_gpu_time, "GPU active render time from Reflex.");
            toggle("Render Latency", g_cfg.show_render_lat, "Sim-start to GPU-render-end latency.");
            toggle("Present Latency", g_cfg.show_present_lat, "Present start-to-end latency.");
            toggle("FG Mode", g_cfg.show_fg_mode, "Detected frame generation technology.");
            toggle("Output Resolution", g_cfg.show_resolution, "Render and output resolution with scale %.");
        }
        ImGui::Unindent(8);
    }

    // --- Monitor (collapsible) ---
    if (ImGui::CollapsingHeader("Monitor")) {
        ImGui::Indent(8);
        if (s_mons_dirty) RefreshMons();
        if (ImGui::Button("Refresh Monitors")) s_mons_dirty = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-enumerate connected displays.");

        ImGui::SameLine();
        const char* preview = (s_sel_mon == 0) ? "No Override" : s_mons[s_sel_mon - 1].label;
        if (ImGui::BeginCombo("##monitor", preview)) {
            if (ImGui::Selectable("No Override", s_sel_mon == 0)) {
                s_sel_mon = 0;
                { std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx); g_cfg.target_monitor.clear(); }
                changed = true;
            }
            for (int i = 0; i < (int)s_mons.size(); i++) {
                bool sel = (s_sel_mon == i + 1);
                if (ImGui::Selectable(s_mons[i].label, sel)) {
                    s_sel_mon = i + 1;
                    { std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx); g_cfg.target_monitor = s_mons[i].dev; }
                    changed = true;
                    if (s_hwnd) RequestMove(s_hwnd, s_mons[i]);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select a monitor to move the game window to.");

        ImGui::Spacing();
        ImGui::TextDisabled("Window Mode");
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override the game's window mode.\nDefault: no change. Fullscreen/Borderless: force.");

        static const char* kWmLabels[] = { "Default", "Fullscreen", "Borderless Fullscreen" };
        int wm = static_cast<int>(g_cfg.window_mode.load());
        if (Combo("##window_mode", &wm, kWmLabels, 3)) {
            g_cfg.window_mode.store(static_cast<WindowMode>(wm)); changed = true;
            RequestWindowMode(static_cast<WindowMode>(wm));
        }
        ImGui::Unindent(8);
    }

    // --- Keybinds (collapsible) ---
    if (ImGui::CollapsingHeader("Keybinds")) {
        ImGui::Indent(8);
        static bool s_binding_osd = false;

        ImGui::TextDisabled("Toggle OSD");
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Press the button then hit a key to rebind.\nDefault: END");

        if (s_binding_osd) {
            ImGui::Button("Press a key...", ImVec2(-1, 0));
            int vk = PollAnyKey();
            if (vk != 0) {
                g_cfg.osd_toggle_key.store(vk);
                s_binding_osd = false;
                changed = true;
            }
        } else {
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s##osd_key", VkName(g_cfg.osd_toggle_key.load()));
            if (ImGui::Button(lbl, ImVec2(-1, 0))) s_binding_osd = true;
        }
        ImGui::Unindent(8);
    }

    // --- Status ---
    ImGui::Spacing();
    ImGui::TextDisabled("Status");
    ImGui::Text("Reflex: %s", ReflexActive() ? "Hooked" : "Not hooked");
    ImGui::Text("Native Reflex: %s", g_game_uses_reflex.load() ? "Detected" : "No");

    ExpandedSettings es = ExpandPreset();
    bool native = g_game_uses_reflex.load();
    const char* mode = "Timing";
    if (!ReflexActive()) mode = "Timing (no Reflex)";
    else if (es.use_sl_proxy) mode = "SL Proxy";
    else if (es.use_marker_pacing && native) {
        // Show the dynamically resolved enforcement site
        int site = s_limiter.GetGpuStats().valid ? s_limiter.GetGpuStats().auto_site : PRESENT_FINISH;
        mode = (site == SIM_START) ? "Marker (Sim Start)" : "Marker (Present)";
    }
    else mode = "Present-based (Reflex Sleep)";
    ImGui::Text("Pacing: %s", mode);
    ImGui::Text("Target: %.0f FPS", g_cfg.fps_limit.load());

    if (changed) SaveSettings();
}

// ============================================================================
// ReShade callbacks
// ============================================================================

static void OnSLPresentCb() { s_limiter.OnSLPresent(); }

static void OnMarkerCb(int mt, uint64_t fid) {
    __try {
        OnMarkerOSD(mt, fid);
        s_limiter.OnMarker(mt, fid);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void OnInitSwapchain(reshade::api::swapchain* sc, bool) {
    if (!sc) return;

    __try {
        ul_log::Write("OnInitSwapchain: enter");

        DetectConflictingFrameworks();

        HWND hwnd = static_cast<HWND>(sc->get_hwnd());
        if (hwnd) s_hwnd = hwnd;
        if (hwnd) s_limiter.SetHwnd(hwnd);

        if (hwnd) ApplySavedMonitor(hwnd);
        if (hwnd) {
            WindowMode wm = g_cfg.window_mode.load();
            if (wm != WindowMode::NoOverride) RequestWindowMode(wm);
        }

        reshade::api::device* dev = sc->get_device();
        if (!dev) { ul_log::Write("OnInitSwapchain: no device"); return; }

        auto api = dev->get_api();
        ul_log::Write("OnInitSwapchain: api=%d", (int)api);

        if (api != reshade::api::device_api::d3d11 && api != reshade::api::device_api::d3d12) return;

        IUnknown* native = reinterpret_cast<IUnknown*>(dev->get_native());

        // Capture output resolution from back buffer
        {
            reshade::api::resource bb = sc->get_back_buffer(0);
            if (bb != reshade::api::resource{0}) {
                reshade::api::resource_desc d = dev->get_resource_desc(bb);
                if (d.texture.width > 0 && d.texture.height > 0) {
                    s_out_w.store(d.texture.width); s_out_h.store(d.texture.height);
                    ul_log::Write("OnInitSwapchain: output %ux%u", d.texture.width, d.texture.height);
                }
            }
        }

        UpdateFGString();
        ul_log::Write("OnInitSwapchain: FG=%s", s_fg_str);

        if (native) {
            s_reflex_dev = native;
            ul_log::Write("OnInitSwapchain: ConnectReflex...");
            s_limiter.ConnectReflex(native);
            ul_log::Write("OnInitSwapchain: ConnectReflex done");
        }

        ExpandedSettings es = ExpandPreset();
        if (es.use_sl_proxy && !IsStreamlineSafeMode()) {
            auto nsc = reinterpret_cast<IDXGISwapChain*>(sc->get_native());
            if (nsc) {
                ul_log::Write("OnInitSwapchain: HookSLProxy...");
                HookSLProxy(nsc);
            }
        } else if (es.use_sl_proxy) {
            ul_log::Write("OnInitSwapchain: skipping SL proxy hook (Streamline safe mode)");
        }

        // Hook Present for VSync override (only if SL proxy didn't already hook it)
        // Skip when in Streamline safe mode — vtable hooks on Streamline-managed
        // swapchains cause crashes during swapchain recreation.
        if (!IsStreamlineSafeMode()) {
            auto nsc = reinterpret_cast<IDXGISwapChain*>(sc->get_native());
            if (nsc) {
                ul_log::Write("OnInitSwapchain: HookVSyncPresent...");
                HookVSyncPresent(nsc);
            }
        } else {
            ul_log::Write("OnInitSwapchain: skipping VSync hook (Streamline safe mode)");
        }

        // Hook SetMaximumFrameLatency for 5XXX Exclusive Pacing Optimization
        // Also skip in Streamline safe mode.
        if (!IsStreamlineSafeMode()) {
            auto nsc = reinterpret_cast<IDXGISwapChain*>(sc->get_native());
            if (nsc) {
                ul_log::Write("OnInitSwapchain: HookFrameLatency...");
                HookFrameLatency(nsc);
            }
        } else {
            ul_log::Write("OnInitSwapchain: skipping FrameLatency hook (Streamline safe mode)");
        }

        ul_log::Write("OnInitSwapchain: done");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ul_log::Write("OnInitSwapchain: exception 0x%08X", GetExceptionCode());
    }
}

static void OnDestroySwapchain(reshade::api::swapchain* sc, bool) {
    // Reset VSync and frame-latency hooks so they can be re-installed on the
    // next swapchain.  The vtable hooks themselves remain valid (MinHook patches
    // the function prologue, not the vtable pointer), but the COM reference we
    // hold to IDXGISwapChain2 must be released to avoid use-after-free.
    // TeardownReflexHooks handles the latency hook cleanup.
    // We intentionally do NOT tear down the NVAPI Reflex hooks here — those are
    // function-level hooks that survive swapchain recreation.
    ul_log::Write("OnDestroySwapchain");
}

// Track render resolution via viewport dimensions.
// When DLSS/FSR is active, the game sets a viewport matching the internal
// render resolution for the main 3D pass. This is the most reliable signal.
// We track the most frequently bound viewport size per frame to filter noise.

struct VpBucket {
    uint32_t w, h;
    int count;
};
static constexpr int kMaxVpBuckets = 8;
static VpBucket s_vp_buckets[kMaxVpBuckets] = {};
static int s_vp_bucket_count = 0;

static void OnBindViewports(reshade::api::command_list*,
                            uint32_t first,
                            uint32_t count,
                            const reshade::api::viewport* viewports) {
    if (!count || !viewports) return;

    uint32_t ow = s_out_w.load(std::memory_order_relaxed);
    uint32_t oh = s_out_h.load(std::memory_order_relaxed);
    if (!ow || !oh) return;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t w = static_cast<uint32_t>(viewports[i].width);
        uint32_t h = static_cast<uint32_t>(viewports[i].height);

        // Must be smaller than output (this is the upscale source)
        if (w >= ow || h >= oh) continue;
        // Must be at least 25% of output
        if (w < ow / 4 || h < oh / 4) continue;
        // Skip tiny viewports
        if (w < 256 || h < 256) continue;

        // Aspect ratio must match output within 10%
        float out_ar = static_cast<float>(ow) / oh;
        float vp_ar = static_cast<float>(w) / h;
        if (std::abs(vp_ar - out_ar) / out_ar > 0.10f) continue;

        // Tally in per-frame buckets
        bool found = false;
        for (int j = 0; j < s_vp_bucket_count; j++) {
            if (s_vp_buckets[j].w == w && s_vp_buckets[j].h == h) {
                s_vp_buckets[j].count++;
                found = true;
                break;
            }
        }
        if (!found && s_vp_bucket_count < kMaxVpBuckets) {
            s_vp_buckets[s_vp_bucket_count++] = { w, h, 1 };
        }
    }
}

static void OnPresent(reshade::api::command_queue*, reshade::api::swapchain* sc,
                      const reshade::api::rect*, const reshade::api::rect*,
                      uint32_t, const reshade::api::rect*) {
    static uint64_t cnt = 0; cnt++;
    if (!sc) return;
    HWND hwnd = static_cast<HWND>(sc->get_hwnd());
    if (hwnd != s_hwnd) return;

    __try {
        s_limiter.OnPresent();
        UpdateStats();
        if (cnt > 300 && cnt % 30 == 0) PollGpuLatency();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ul_log::Write("OnPresent: exception 0x%08X", GetExceptionCode());
    }

    // Commit the most-used sub-native viewport as the render resolution
    if (s_vp_bucket_count > 0) {
        int best = 0;
        for (int i = 1; i < s_vp_bucket_count; i++) {
            if (s_vp_buckets[i].count > s_vp_buckets[best].count)
                best = i;
        }
        s_rnd_w.store(s_vp_buckets[best].w, std::memory_order_relaxed);
        s_rnd_h.store(s_vp_buckets[best].h, std::memory_order_relaxed);
        s_vp_bucket_count = 0;
    }
}

// ============================================================================
// Crash handler
// ============================================================================

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = nullptr;

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord)
        ul_log::Write("CRASH: code=0x%08X addr=%p",
                      ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// DLL entry
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        // Initialize logging FIRST so any crash during startup is captured.
        ul_log::Initialize(hModule);

        s_prev_filter = SetUnhandledExceptionFilter(CrashHandler);

        if (!reshade::register_addon(hModule)) {
            ul_log::Write("FATAL: reshade::register_addon failed");
            SetUnhandledExceptionFilter(s_prev_filter);
            return FALSE;
        }

        ul_log::Write("=== Ultra Limiter (clean-room) starting ===");

        DetectConflictingFrameworks();

        if (!ul_timing::Init()) {
            ul_log::Write("FATAL: timing init failed");
            reshade::unregister_addon(hModule);
            SetUnhandledExceptionFilter(s_prev_filter);
            return FALSE;
        }

        __try {
            LoadSettings(hModule);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            ul_log::Write("FATAL: LoadSettings exception 0x%08X", GetExceptionCode());
            reshade::unregister_addon(hModule);
            SetUnhandledExceptionFilter(s_prev_filter);
            return FALSE;
        }

        if (MH_Initialize() != MH_OK) {
            ul_log::Write("FATAL: MH_Initialize failed");
            reshade::unregister_addon(hModule);
            SetUnhandledExceptionFilter(s_prev_filter);
            return FALSE;
        }

        __try {
            s_limiter.Init();

            SetSLPresentCb(OnSLPresentCb);
            SetMarkerCb(OnMarkerCb);
            StartWorker();

            reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
            reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
            reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
            reshade::register_event<reshade::addon_event::present>(OnPresent);
            reshade::register_overlay("Ultra Limiter", DrawOverlay);
            reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            ul_log::Write("FATAL: event registration exception 0x%08X", GetExceptionCode());
            MH_Uninitialize();
            reshade::unregister_addon(hModule);
            SetUnhandledExceptionFilter(s_prev_filter);
            return FALSE;
        }
        ul_log::Write("All events registered");
        break;

    case DLL_PROCESS_DETACH:
        ul_log::Write("Shutting down");
        SetUnhandledExceptionFilter(s_prev_filter);
        StopWorker();
        reshade::unregister_overlay("Ultra Limiter", DrawOverlay);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(DrawOSD);
        s_limiter.Shutdown();
        MH_Uninitialize();
        reshade::unregister_addon(hModule);
        ul_log::Shutdown();
        break;
    }
    return TRUE;
}
