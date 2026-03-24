// ReLimiter — File logging implementation
// Uses raw Win32 file APIs to avoid CRT initialization issues in DllMain.

#include "ul_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ul_log {

static HANDLE s_hfile = INVALID_HANDLE_VALUE;
static LARGE_INTEGER s_start = {};
static LARGE_INTEGER s_freq = {};

static void DebugOut(const char* msg) {
    OutputDebugStringA("[UL] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

static void RawWrite(const char* str, int len) {
    if (s_hfile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(s_hfile, str, static_cast<DWORD>(len), &written, nullptr);
}

static void RawFlush() {
    if (s_hfile != INVALID_HANDLE_VALUE) FlushFileBuffers(s_hfile);
}

static bool TryOpen(const wchar_t* wpath) {
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        s_hfile = h;
        return true;
    }
    return false;
}

void Initialize(HMODULE addon_module) {
    if (s_hfile != INVALID_HANDLE_VALUE) return;

    wchar_t wpath[MAX_PATH] = {};

    // Try 1: next to the game executable (most user-visible location).
    // The addon DLL may be loaded from a proxy directory (e.g. REFramework's
    // _storage_ folder), so prefer the game exe directory.
    if (GetModuleFileNameW(nullptr, wpath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"ultra_limiter.log");
            if (TryOpen(wpath)) goto opened;
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "[UL] Log init: TryOpen(game dir) failed, err=%lu", GetLastError());
                OutputDebugStringA(dbg);
            }
        }
    }

    // Try 2: next to the addon DLL
    wpath[0] = L'\0';
    if (addon_module && GetModuleFileNameW(addon_module, wpath, MAX_PATH)) {
        {
            char apath[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, wpath, -1, apath, sizeof(apath), nullptr, nullptr);
            char dbg[512];
            snprintf(dbg, sizeof(dbg), "[UL] Log init: addon path = %s", apath);
            OutputDebugStringA(dbg);
        }
        wchar_t* dot = wcsrchr(wpath, L'.');
        if (dot) {
            wcscpy(dot, L".log");
            if (TryOpen(wpath)) goto opened;
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "[UL] Log init: TryOpen(addon) failed, err=%lu", GetLastError());
                OutputDebugStringA(dbg);
            }
        }
    } else {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[UL] Log init: GetModuleFileNameW failed, mod=%p err=%lu",
                 (void*)addon_module, GetLastError());
        OutputDebugStringA(dbg);
    }

    // Try 3: %TEMP%
    {
        wchar_t tmp[MAX_PATH] = {};
        DWORD len = GetTempPathW(MAX_PATH, tmp);
        if (len > 0 && len < MAX_PATH - 32) {
            wcscat(tmp, L"ultra_limiter.log");
            wcscpy(wpath, tmp);
            if (TryOpen(wpath)) goto opened;
            {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "[UL] Log init: TryOpen(TEMP) failed, err=%lu", GetLastError());
                OutputDebugStringA(dbg);
            }
        }
    }

    DebugOut("LOG INIT FAILED — could not open any log file");
    return;

opened:
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);

    char header[512];
    char apath[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, apath, sizeof(apath), nullptr, nullptr);
    int n = snprintf(header, sizeof(header), "=== ReLimiter Log ===\r\nLog file: %s\r\n", apath);
    if (n > 0) RawWrite(header, n);
    RawFlush();
    DebugOut("Log opened");
}

void Shutdown() {
    if (s_hfile != INVALID_HANDLE_VALUE) {
        Write("Shutdown");
        CloseHandle(s_hfile);
        s_hfile = INVALID_HANDLE_VALUE;
    }
}

void Write(const char* fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    int blen = vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    if (blen <= 0) return;

    DebugOut(body);

    if (s_hfile == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (s_freq.QuadPart > 0)
        ? static_cast<double>(now.QuadPart - s_start.QuadPart) / static_cast<double>(s_freq.QuadPart)
        : 0.0;

    char line[1100];
    int n = snprintf(line, sizeof(line), "[%10.4f] %s\r\n", elapsed, body);
    if (n > 0) RawWrite(line, n);
    RawFlush();
}

}  // namespace ul_log
