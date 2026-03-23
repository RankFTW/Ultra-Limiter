// Ultra Limiter — Configuration implementation
// Clean-room: uses only Windows INI APIs (GetPrivateProfileString/Int, WritePrivateProfileString).

#include "ul_config.hpp"
#include "ul_log.hpp"

#include <cstdio>
#include <cstring>

UlConfig g_cfg;
static char s_ini[MAX_PATH] = {};

// --- Helpers ---

static bool BuildIniPath(HMODULE mod, char* out, size_t sz) {
    wchar_t wpath[MAX_PATH] = {};

    // Try next to the game executable first.
    // The addon DLL may be loaded from a proxy directory (e.g. REFramework's
    // _storage_ folder), so the game exe directory is the most predictable
    // and user-visible location.
    if (GetModuleFileNameW(nullptr, wpath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"ultra_limiter.ini");
            HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, static_cast<int>(sz), nullptr, nullptr);
                OutputDebugStringA("[UL] INI path: next to game exe");
                return true;
            }
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "[UL] BuildIniPath: game dir write failed, err=%lu", GetLastError());
                OutputDebugStringA(dbg);
            }
        }
    }

    // Fallback: next to the addon DLL
    wpath[0] = L'\0';
    if (mod && GetModuleFileNameW(mod, wpath, MAX_PATH)) {
        wchar_t* dot = wcsrchr(wpath, L'.');
        if (dot) {
            wcscpy(dot, L".ini");
            HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, static_cast<int>(sz), nullptr, nullptr);
                OutputDebugStringA("[UL] INI path: next to addon DLL");
                return true;
            }
        }
    }
    OutputDebugStringA("[UL] BuildIniPath: all paths failed");
    return false;
}

static bool ReadBool(const char* path, const char* key, bool def) {
    char buf[16];
    GetPrivateProfileStringA("UltraLimiter", key, def ? "true" : "false", buf, sizeof(buf), path);
    return (_stricmp(buf, "true") == 0 || _stricmp(buf, "1") == 0 || _stricmp(buf, "yes") == 0);
}

static float ReadFloat(const char* path, const char* key, float def) {
    char buf[32], defstr[32];
    snprintf(defstr, sizeof(defstr), "%.2f", def);
    GetPrivateProfileStringA("UltraLimiter", key, defstr, buf, sizeof(buf), path);
    return static_cast<float>(atof(buf));
}

static void W(const char* key, const char* val) {
    WritePrivateProfileStringA("UltraLimiter", key, val, s_ini);
}

static void WBool(const char* key, bool v) { W(key, v ? "true" : "false"); }

static void WInt(const char* key, int v) {
    char buf[32]; snprintf(buf, sizeof(buf), "%d", v); W(key, buf);
}

static void WFloat(const char* key, float v) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.2f", v); W(key, buf);
}

// --- Preset expansion ---

ExpandedSettings ExpandPreset() {
    ExpandedSettings e{};
    PacingPreset p = g_cfg.preset.load(std::memory_order_relaxed);
    switch (p) {
        case PacingPreset::NativePacing:
            e.use_marker_pacing = true; e.max_queued_frames = 0;
            e.delay_present = false; e.use_sl_proxy = false;
            break;
        case PacingPreset::MarkerLowLat:
            e.use_marker_pacing = true; e.max_queued_frames = 1;
            e.delay_present = false; e.use_sl_proxy = false;
            break;
        case PacingPreset::MarkerBalanced:
            e.use_marker_pacing = true; e.max_queued_frames = 2;
            e.delay_present = false; e.use_sl_proxy = false;
            break;
        case PacingPreset::MarkerStability:
            e.use_marker_pacing = true; e.max_queued_frames = 3;
            e.delay_present = false; e.use_sl_proxy = false;
            break;
        case PacingPreset::StreamlineProxy:
            e.use_marker_pacing = false; e.max_queued_frames = 0;
            e.delay_present = false; e.use_sl_proxy = true;
            break;
        case PacingPreset::Custom:
            e.use_marker_pacing = g_cfg.use_marker_pacing.load(std::memory_order_relaxed);
            e.max_queued_frames = g_cfg.max_queued_frames.load(std::memory_order_relaxed);
            e.delay_present = g_cfg.delay_present.load(std::memory_order_relaxed);
            e.use_sl_proxy = g_cfg.use_sl_proxy.load(std::memory_order_relaxed);
            break;
    }
    return e;
}

// --- Default INI ---

static void WriteDefaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "[UltraLimiter]\n"
        "fps_limit=0\n"
        "fg_mult=auto\n"
        "boost=game\n"
        "preset=native_pacing\n"
        "use_marker_pacing=true\n"
        "max_queued_frames=0\n"
        "delay_present=false\n"
        "delay_present_amount=1.0\n"
        "use_sl_proxy=false\n"
        "osd_on=true\n"
        "osd_x=10.0\n"
        "osd_y=10.0\n"
        "show_fps=true\n"
        "show_frametime=true\n"
        "show_native_fps=true\n"
        "show_graph=true\n"
        "show_gpu_time=true\n"
        "show_render_lat=true\n"
        "show_present_lat=true\n"
        "show_fg_mode=true\n"
        "show_resolution=true\n"
        "target_monitor=\n"
        "window_mode=none\n"
        "vsync_override=0\n"
        "exclusive_pacing=false\n"
        "osd_toggle_key=35\n"
    );
    fclose(f);
}

// --- Load ---

void LoadSettings(HMODULE addon_module) {
    if (!BuildIniPath(addon_module, s_ini, sizeof(s_ini))) {
        ul_log::Write("LoadSettings: failed to build INI path");
        return;
    }
    ul_log::Write("LoadSettings: path=%s", s_ini);

    if (GetFileAttributesA(s_ini) == INVALID_FILE_ATTRIBUTES) {
        ul_log::Write("LoadSettings: creating defaults");
        WriteDefaults(s_ini);
        return;
    }

    const char* ini = s_ini;
    char buf[64];

    g_cfg.fps_limit.store(static_cast<float>(GetPrivateProfileIntA("UltraLimiter", "fps_limit", 0, ini)),
                          std::memory_order_relaxed);

    GetPrivateProfileStringA("UltraLimiter", "fg_mult", "auto", buf, sizeof(buf), ini);
    if (_stricmp(buf, "off") == 0)       g_cfg.fg_mult.store(FGMultiplier::Off);
    else if (_stricmp(buf, "2x") == 0)   g_cfg.fg_mult.store(FGMultiplier::X2);
    else if (_stricmp(buf, "3x") == 0)   g_cfg.fg_mult.store(FGMultiplier::X3);
    else if (_stricmp(buf, "4x") == 0)   g_cfg.fg_mult.store(FGMultiplier::X4);
    else                                  g_cfg.fg_mult.store(FGMultiplier::Auto);

    GetPrivateProfileStringA("UltraLimiter", "boost", "game", buf, sizeof(buf), ini);
    if (_stricmp(buf, "on") == 0)        g_cfg.boost.store(BoostMode::ForceOn);
    else if (_stricmp(buf, "off") == 0)  g_cfg.boost.store(BoostMode::ForceOff);
    else                                  g_cfg.boost.store(BoostMode::GameDefault);

    GetPrivateProfileStringA("UltraLimiter", "preset", "native_pacing", buf, sizeof(buf), ini);
    if (_stricmp(buf, "marker_lowlat") == 0)       g_cfg.preset.store(PacingPreset::MarkerLowLat);
    else if (_stricmp(buf, "marker_balanced") == 0) g_cfg.preset.store(PacingPreset::MarkerBalanced);
    else if (_stricmp(buf, "marker_stability") == 0)g_cfg.preset.store(PacingPreset::MarkerStability);
    else if (_stricmp(buf, "sl_proxy") == 0)        g_cfg.preset.store(PacingPreset::StreamlineProxy);
    else if (_stricmp(buf, "custom") == 0)          g_cfg.preset.store(PacingPreset::Custom);
    else                                             g_cfg.preset.store(PacingPreset::NativePacing);

    g_cfg.use_marker_pacing.store(ReadBool(ini, "use_marker_pacing", true));
    g_cfg.max_queued_frames.store(GetPrivateProfileIntA("UltraLimiter", "max_queued_frames", 0, ini));
    g_cfg.delay_present.store(ReadBool(ini, "delay_present", false));
    g_cfg.delay_present_amount.store(ReadFloat(ini, "delay_present_amount", 1.0f));
    g_cfg.use_sl_proxy.store(ReadBool(ini, "use_sl_proxy", false));

    g_cfg.osd_on.store(ReadBool(ini, "osd_on", true));
    g_cfg.osd_x.store(ReadFloat(ini, "osd_x", 10.0f));
    g_cfg.osd_y.store(ReadFloat(ini, "osd_y", 10.0f));
    g_cfg.show_fps.store(ReadBool(ini, "show_fps", true));
    g_cfg.show_frametime.store(ReadBool(ini, "show_frametime", true));
    g_cfg.show_native_fps.store(ReadBool(ini, "show_native_fps", true));
    g_cfg.show_graph.store(ReadBool(ini, "show_graph", true));
    g_cfg.show_gpu_time.store(ReadBool(ini, "show_gpu_time", true));
    g_cfg.show_render_lat.store(ReadBool(ini, "show_render_lat", true));
    g_cfg.show_present_lat.store(ReadBool(ini, "show_present_lat", true));
    g_cfg.show_fg_mode.store(ReadBool(ini, "show_fg_mode", true));
    g_cfg.show_resolution.store(ReadBool(ini, "show_resolution", true));

    {
        char mon[64] = {};
        GetPrivateProfileStringA("UltraLimiter", "target_monitor", "", mon, sizeof(mon), ini);
        std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx);
        g_cfg.target_monitor = mon;
    }

    GetPrivateProfileStringA("UltraLimiter", "window_mode", "none", buf, sizeof(buf), ini);
    if (_stricmp(buf, "fullscreen") == 0)      g_cfg.window_mode.store(WindowMode::ForceFullscreen);
    else if (_stricmp(buf, "borderless") == 0)  g_cfg.window_mode.store(WindowMode::ForceBorderless);
    else                                         g_cfg.window_mode.store(WindowMode::NoOverride);

    g_cfg.vsync_override.store(GetPrivateProfileIntA("UltraLimiter", "vsync_override", 0, ini));
    g_cfg.exclusive_pacing.store(ReadBool(ini, "exclusive_pacing", false));
    g_cfg.osd_toggle_key.store(GetPrivateProfileIntA("UltraLimiter", "osd_toggle_key", VK_END, ini));

    ul_log::Write("LoadSettings: fps=%.0f preset=%d fg=%d boost=%d",
                  g_cfg.fps_limit.load(), (int)g_cfg.preset.load(),
                  (int)g_cfg.fg_mult.load(), (int)g_cfg.boost.load());
}

// --- Save ---

void SaveSettings() {
    if (s_ini[0] == '\0') return;
    ul_log::Write("SaveSettings: writing %s", s_ini);

    WInt("fps_limit", static_cast<int>(g_cfg.fps_limit.load(std::memory_order_relaxed)));

    switch (g_cfg.fg_mult.load()) {
        case FGMultiplier::Off: W("fg_mult", "off"); break;
        case FGMultiplier::X2:  W("fg_mult", "2x"); break;
        case FGMultiplier::X3:  W("fg_mult", "3x"); break;
        case FGMultiplier::X4:  W("fg_mult", "4x"); break;
        default:                W("fg_mult", "auto"); break;
    }

    switch (g_cfg.boost.load()) {
        case BoostMode::ForceOn:  W("boost", "on"); break;
        case BoostMode::ForceOff: W("boost", "off"); break;
        default:                  W("boost", "game"); break;
    }

    switch (g_cfg.preset.load()) {
        case PacingPreset::NativePacing:    W("preset", "native_pacing"); break;
        case PacingPreset::MarkerLowLat:    W("preset", "marker_lowlat"); break;
        case PacingPreset::MarkerBalanced:  W("preset", "marker_balanced"); break;
        case PacingPreset::MarkerStability: W("preset", "marker_stability"); break;
        case PacingPreset::StreamlineProxy: W("preset", "sl_proxy"); break;
        case PacingPreset::Custom:          W("preset", "custom"); break;
    }

    WBool("use_marker_pacing", g_cfg.use_marker_pacing.load());
    WInt("max_queued_frames", g_cfg.max_queued_frames.load());
    WBool("delay_present", g_cfg.delay_present.load());
    WFloat("delay_present_amount", g_cfg.delay_present_amount.load());
    WBool("use_sl_proxy", g_cfg.use_sl_proxy.load());

    WBool("osd_on", g_cfg.osd_on.load());
    WFloat("osd_x", g_cfg.osd_x.load());
    WFloat("osd_y", g_cfg.osd_y.load());
    WBool("show_fps", g_cfg.show_fps.load());
    WBool("show_frametime", g_cfg.show_frametime.load());
    WBool("show_native_fps", g_cfg.show_native_fps.load());
    WBool("show_graph", g_cfg.show_graph.load());
    WBool("show_gpu_time", g_cfg.show_gpu_time.load());
    WBool("show_render_lat", g_cfg.show_render_lat.load());
    WBool("show_present_lat", g_cfg.show_present_lat.load());
    WBool("show_fg_mode", g_cfg.show_fg_mode.load());
    WBool("show_resolution", g_cfg.show_resolution.load());

    {
        std::lock_guard<std::mutex> lk(g_cfg.monitor_mtx);
        W("target_monitor", g_cfg.target_monitor.c_str());
    }

    switch (g_cfg.window_mode.load()) {
        case WindowMode::ForceFullscreen: W("window_mode", "fullscreen"); break;
        case WindowMode::ForceBorderless: W("window_mode", "borderless"); break;
        default:                          W("window_mode", "none"); break;
    }

    WInt("osd_toggle_key", g_cfg.osd_toggle_key.load());
    WInt("vsync_override", g_cfg.vsync_override.load());
    WBool("exclusive_pacing", g_cfg.exclusive_pacing.load());
}
