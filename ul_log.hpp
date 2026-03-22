#pragma once
// Ultra Limiter — File logging
// Clean-room implementation using standard C I/O and Windows QPC.

#include <windows.h>

namespace ul_log {

// Open log file next to the addon DLL (replaces .addon64 extension with .log).
void Initialize(HMODULE addon_module);

// Flush and close.
void Shutdown();

// Printf-style timestamped log line.
void Write(const char* fmt, ...);

}  // namespace ul_log
