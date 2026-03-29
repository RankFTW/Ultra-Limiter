// ReLimiter - Configuration implementation
#include "ul_config.hpp"
#include "ul_log.hpp"
#include <cstdio>
#include <cstring>

UlConfig g_cfg;
static char s_ini[MAX_PATH] = {};

static bool BuildIniPath(HMODULE mod, char* out, size_t sz) {
    wchar_t wpath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, wpath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"relimiter.ini");
            HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, (int)sz, nullptr, nullptr);
                OutputDebugStringA("[UL] INI path: next to game exe");
                return true;
            }
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "[UL] BuildIniPath: game dir write failed, err=%lu", GetLastError());
            OutputDebugStringA(dbg);
        }
    }
    wpath[0] = L'\0';
    if (mod && GetModuleFileNameW(mod, wpath, MAX_PATH)) {
        wchar_t* dot = wcsrchr(wpath, L'.');
        if (dot) {
            wcscpy(dot, L".ini");
            HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, (int)sz, nullptr, nullptr);
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

static void WriteDefaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "[UltraLimiter]\n"
        "fps_limit=0\n"
        "bg_fps_limit=0\n"
        "osd_on=true\n"
        "osd_x=10.0\n"
        "osd_y=10.0\n"
        "show_fps=true\n"
        "show_1pct_low=true\n"
        "show_frametime=true\n"
        "show_native_fps=true\n"
        "show_graph=true\n"
        "show_gpu_time=true\n"
        "show_render_lat=true\n"
        "show_present_lat=true\n"
        "show_fg_mode=true\n"
        "target_monitor=\n"
        "window_mode=none\n"
        "vsync_override=0\n"
        "exclusive_pacing=false\n"
        "fake_fullscreen=false\n"
        "osd_toggle_key=35\n"
        "osd_bg_opacity=0\n"
        "osd_drop_shadow=false\n"
        "osd_text_brightness=100\n"
        "osd_scale=100\n"
        "show_smoothness=true\n"
        "show_big_graph=false\n"
        "show_native_graph=false\n"
        "csv_diagnostics=false\n"
    );
    fclose(f);
}
// Migrate ultra_limiter.ini → relimiter.ini if the old file exists.
// Checks both the game exe directory and the addon DLL directory.
static void MigrateOldIni(HMODULE mod) {
    wchar_t wpath[MAX_PATH] = {};

    // Check game exe directory
    if (GetModuleFileNameW(nullptr, wpath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) {
            // Build old path
            wcscpy(slash + 1, L"ultra_limiter.ini");
            if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
                // Old file exists — build new path in same directory
                wchar_t newpath[MAX_PATH] = {};
                wcsncpy(newpath, wpath, MAX_PATH);
                wchar_t* ns = wcsrchr(newpath, L'\\');
                if (ns) {
                    wcscpy(ns + 1, L"relimiter.ini");
                    // Only migrate if new file doesn't already exist
                    if (GetFileAttributesW(newpath) == INVALID_FILE_ATTRIBUTES) {
                        if (CopyFileW(wpath, newpath, TRUE))
                            OutputDebugStringA("[UL] Migrated ultra_limiter.ini -> relimiter.ini (game dir)");
                    }
                    DeleteFileW(wpath);
                    OutputDebugStringA("[UL] Deleted old ultra_limiter.ini (game dir)");
                }
            }
        }
    }

    // Check addon DLL directory
    if (mod) {
        wpath[0] = L'\0';
        if (GetModuleFileNameW(mod, wpath, MAX_PATH)) {
            wchar_t* slash = wcsrchr(wpath, L'\\');
            if (slash) {
                wcscpy(slash + 1, L"ultra_limiter.ini");
                if (GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
                    wchar_t newpath[MAX_PATH] = {};
                    wcsncpy(newpath, wpath, MAX_PATH);
                    wchar_t* ns = wcsrchr(newpath, L'\\');
                    if (ns) {
                        wcscpy(ns + 1, L"relimiter.ini");
                        if (GetFileAttributesW(newpath) == INVALID_FILE_ATTRIBUTES) {
                            if (CopyFileW(wpath, newpath, TRUE))
                                OutputDebugStringA("[UL] Migrated ultra_limiter.ini -> relimiter.ini (addon dir)");
                        }
                        DeleteFileW(wpath);
                        OutputDebugStringA("[UL] Deleted old ultra_limiter.ini (addon dir)");
                    }
                }
            }
        }
    }
}

void LoadSettings(HMODULE addon_module) {
    MigrateOldIni(addon_module);
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
    g_cfg.bg_fps_limit.store(static_cast<float>(GetPrivateProfileIntA("UltraLimiter", "bg_fps_limit", 0, ini)),
                              std::memory_order_relaxed);
    g_cfg.osd_on.store(ReadBool(ini, "osd_on", true));
    g_cfg.osd_x.store(ReadFloat(ini, "osd_x", 10.0f));
    g_cfg.osd_y.store(ReadFloat(ini, "osd_y", 10.0f));
    g_cfg.show_fps.store(ReadBool(ini, "show_fps", true));
    g_cfg.show_1pct_low.store(ReadBool(ini, "show_1pct_low", true));
    g_cfg.show_frametime.store(ReadBool(ini, "show_frametime", true));
    g_cfg.show_native_fps.store(ReadBool(ini, "show_native_fps", true));
    g_cfg.show_graph.store(ReadBool(ini, "show_graph", true));
    g_cfg.show_gpu_time.store(ReadBool(ini, "show_gpu_time", true));
    g_cfg.show_render_lat.store(ReadBool(ini, "show_render_lat", true));
    g_cfg.show_present_lat.store(ReadBool(ini, "show_present_lat", true));
    g_cfg.show_fg_mode.store(ReadBool(ini, "show_fg_mode", true));
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
    g_cfg.fake_fullscreen.store(ReadBool(ini, "fake_fullscreen", false));
    g_cfg.osd_toggle_key.store(GetPrivateProfileIntA("UltraLimiter", "osd_toggle_key", VK_END, ini));
    g_cfg.osd_bg_opacity.store(GetPrivateProfileIntA("UltraLimiter", "osd_bg_opacity", 0, ini));
    g_cfg.osd_drop_shadow.store(ReadBool(ini, "osd_drop_shadow", false));
    g_cfg.osd_text_brightness.store(GetPrivateProfileIntA("UltraLimiter", "osd_text_brightness", 100, ini));
    g_cfg.osd_scale.store(GetPrivateProfileIntA("UltraLimiter", "osd_scale", 100, ini));
    g_cfg.show_smoothness.store(ReadBool(ini, "show_smoothness", true));
    g_cfg.show_big_graph.store(ReadBool(ini, "show_big_graph", false));
    g_cfg.show_native_graph.store(ReadBool(ini, "show_native_graph", false));
    g_cfg.csv_diagnostics.store(ReadBool(ini, "csv_diagnostics", false));
    ul_log::Write("LoadSettings: fps=%.0f bg_fps=%.0f",
                  g_cfg.fps_limit.load(), g_cfg.bg_fps_limit.load());
}

void SaveSettings() {
    if (s_ini[0] == '\0') return;
    ul_log::Write("SaveSettings: writing %s", s_ini);
    WInt("fps_limit", static_cast<int>(g_cfg.fps_limit.load(std::memory_order_relaxed)));
    WInt("bg_fps_limit", static_cast<int>(g_cfg.bg_fps_limit.load(std::memory_order_relaxed)));
    WBool("osd_on", g_cfg.osd_on.load());
    WFloat("osd_x", g_cfg.osd_x.load());
    WFloat("osd_y", g_cfg.osd_y.load());
    WBool("show_fps", g_cfg.show_fps.load());
    WBool("show_1pct_low", g_cfg.show_1pct_low.load());
    WBool("show_frametime", g_cfg.show_frametime.load());
    WBool("show_native_fps", g_cfg.show_native_fps.load());
    WBool("show_graph", g_cfg.show_graph.load());
    WBool("show_gpu_time", g_cfg.show_gpu_time.load());
    WBool("show_render_lat", g_cfg.show_render_lat.load());
    WBool("show_present_lat", g_cfg.show_present_lat.load());
    WBool("show_fg_mode", g_cfg.show_fg_mode.load());
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
    WInt("osd_bg_opacity", g_cfg.osd_bg_opacity.load());
    WBool("osd_drop_shadow", g_cfg.osd_drop_shadow.load());
    WInt("osd_text_brightness", g_cfg.osd_text_brightness.load());
    WInt("osd_scale", g_cfg.osd_scale.load());
    WInt("vsync_override", g_cfg.vsync_override.load());
    WBool("exclusive_pacing", g_cfg.exclusive_pacing.load());
    WBool("fake_fullscreen", g_cfg.fake_fullscreen.load());
    WBool("show_smoothness", g_cfg.show_smoothness.load());
    WBool("show_big_graph", g_cfg.show_big_graph.load());
    WBool("show_native_graph", g_cfg.show_native_graph.load());
    WBool("csv_diagnostics", g_cfg.csv_diagnostics.load());
}