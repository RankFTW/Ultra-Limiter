// Ultra Limiter — File logging implementation
// Clean-room: uses only standard C stdio and Windows QueryPerformanceCounter.

#include "ul_log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ul_log {

static FILE* s_fp = nullptr;
static LARGE_INTEGER s_start = {};
static LARGE_INTEGER s_freq = {};

void Initialize(HMODULE addon_module) {
    if (s_fp) return;

    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(addon_module, path, sizeof(path))) return;

    // Replace extension with .log
    char* dot = strrchr(path, '.');
    if (dot && static_cast<size_t>(sizeof(path) - (dot - path)) >= 5)
        strcpy(dot, ".log");
    else
        return;

    s_fp = fopen(path, "w");
    if (!s_fp) return;

    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);

    fprintf(s_fp, "=== Ultra Limiter Log ===\nLog file: %s\n", path);
    fflush(s_fp);
}

void Shutdown() {
    if (s_fp) {
        Write("Shutdown");
        fclose(s_fp);
        s_fp = nullptr;
    }
}

void Write(const char* fmt, ...) {
    if (!s_fp) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (s_freq.QuadPart > 0)
        ? static_cast<double>(now.QuadPart - s_start.QuadPart) / static_cast<double>(s_freq.QuadPart)
        : 0.0;

    fprintf(s_fp, "[%10.4f] ", elapsed);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_fp, fmt, args);
    va_end(args);

    fprintf(s_fp, "\n");
    fflush(s_fp);
}

}  // namespace ul_log
