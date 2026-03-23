#pragma once
// Ultra Limiter — Configuration
// Clean-room implementation. Settings designed from the feature requirements:
//   FPS limiting, FG-aware pacing, Reflex integration, OSD, monitor, fullscreen.
// INI persistence via Windows WritePrivateProfileString / GetPrivateProfileString.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// --- Enums ---

// How to handle frame generation when computing the real render rate
enum class FGMultiplier : uint8_t {
    Auto = 0,  // Detect from loaded DLLs
    Off,       // No FG — target FPS = user FPS
    X2,        // 2x FG (e.g. DLSS-G)
    X3,        // 3x MFG
    X4,        // 4x MFG
};

// Reflex boost override
enum class BoostMode : uint8_t {
    GameDefault = 0,  // Pass through whatever the game sets
    ForceOn,
    ForceOff,
};

// Window mode override
enum class WindowMode : uint8_t {
    NoOverride = 0,
    ForceFullscreen,
    ForceBorderless,
};

// Pacing strategy preset
enum class PacingPreset : uint8_t {
    NativePacing = 0,   // Pace on SIMULATION_START only, no queue limit
    MarkerLowLat,       // Pace via Reflex markers, max queued = 1
    MarkerBalanced,     // Pace via Reflex markers, max queued = 2
    MarkerStability,    // Pace via Reflex markers, max queued = 3
    StreamlineProxy,    // Pace generated frames via SL proxy hook
    Custom,             // User controls all sub-settings
};

// --- Main config struct ---

struct UlConfig {
    // Core limiter
    std::atomic<float> fps_limit{60.0f};           // 0 = unlimited
    std::atomic<FGMultiplier> fg_mult{FGMultiplier::Auto};
    std::atomic<BoostMode> boost{BoostMode::GameDefault};
    std::atomic<PacingPreset> preset{PacingPreset::NativePacing};

    // Custom sub-settings (only active when preset == Custom)
    std::atomic<bool> use_marker_pacing{true};
    std::atomic<int> max_queued_frames{0};
    std::atomic<bool> delay_present{false};
    std::atomic<float> delay_present_amount{1.0f};  // in frame-times
    std::atomic<bool> use_sl_proxy{false};

    // OSD
    std::atomic<bool> osd_on{true};
    std::atomic<float> osd_x{10.0f};
    std::atomic<float> osd_y{10.0f};
    std::atomic<bool> show_fps{true};
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
    // When enabled, overrides IDXGISwapChain2::SetMaximumFrameLatency to 1,
    // forcing single-frame queue depth for lowest input latency on NVIDIA 5XXX.
    std::atomic<bool> exclusive_pacing{false};

    // Keybinds (virtual key codes)
    std::atomic<int> osd_toggle_key{VK_END};  // default: END
};

extern UlConfig g_cfg;

// Resolved settings after expanding the preset
struct ExpandedSettings {
    bool use_marker_pacing;
    int max_queued_frames;
    bool delay_present;
    bool use_sl_proxy;
};

ExpandedSettings ExpandPreset();

// Load from INI next to addon DLL. Creates default INI if missing.
void LoadSettings(HMODULE addon_module);

// Save current settings to INI.
void SaveSettings();
