#pragma once
// ReLimiter — Configuration
// Clean-room implementation. Settings designed from the feature requirements:
//   FPS limiting, FG-aware pacing, Reflex integration, OSD, monitor, fullscreen.
// INI persistence via Windows WritePrivateProfileString / GetPrivateProfileString.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// --- Enums ---

// Window mode override
enum class WindowMode : uint8_t {
    NoOverride = 0,
    ForceFullscreen,
    ForceBorderless,
};

// --- Main config struct ---

struct UlConfig {
    // Core limiter
    std::atomic<float> fps_limit{0.0f};            // 0 = unlimited
    std::atomic<float> bg_fps_limit{0.0f};         // 0 = disabled (use fps_limit/3 fallback)

    // Internal only (not exposed in UI)
    std::atomic<bool> use_sl_proxy{false};

    // OSD
    std::atomic<bool> osd_on{true};
    std::atomic<float> osd_x{10.0f};
    std::atomic<float> osd_y{10.0f};
    std::atomic<bool> show_fps{true};
    std::atomic<bool> show_1pct_low{true};
    std::atomic<bool> show_frametime{true};
    std::atomic<bool> show_native_fps{true};
    std::atomic<bool> show_graph{true};
    std::atomic<bool> show_gpu_time{true};
    std::atomic<bool> show_render_lat{true};
    std::atomic<bool> show_present_lat{true};
    std::atomic<bool> show_fg_mode{true};
    std::atomic<bool> show_resolution{true};

    // Monitor
    std::mutex monitor_mtx;
    std::string target_monitor;  // device name like \\.\DISPLAY1

    // Window mode
    std::atomic<WindowMode> window_mode{WindowMode::NoOverride};

    // VSync override: 0 = no override, 1 = force on, 2 = force off
    std::atomic<int> vsync_override{0};

    // 5XXX Exclusive Pacing Optimization (flip metering)
    std::atomic<bool> exclusive_pacing{false};

    // Fake Fullscreen — intercept exclusive fullscreen and convert to borderless window
    std::atomic<bool> fake_fullscreen{false};

    // Keybinds (virtual key codes)
    std::atomic<int> osd_toggle_key{VK_END};  // default: END

    // OSD background opacity (0 = invisible, 100 = solid black)
    std::atomic<int> osd_bg_opacity{0};

    // OSD drop shadow on text (true = enabled)
    std::atomic<bool> osd_drop_shadow{false};

    // OSD text brightness (0–100, default 100 = full white)
    std::atomic<int> osd_text_brightness{100};

    // OSD smoothness score display
    std::atomic<bool> show_smoothness{true};

    // OSD scale (100–300, default 100 = normal size)
    std::atomic<int> osd_scale{100};

    // Large frametime graph with labeled axis
    std::atomic<bool> show_big_graph{false};

    // Expanded per-frame diagnostic CSV
    std::atomic<bool> csv_diagnostics{false};
};

extern UlConfig g_cfg;

// Load from INI next to addon DLL. Creates default INI if missing.
void LoadSettings(HMODULE addon_module);

// Save current settings to INI.
void SaveSettings();
