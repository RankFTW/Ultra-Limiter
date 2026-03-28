#pragma once
// ReLimiter — High-precision timing
// Clean-room implementation from Windows API documentation:
//   QueryPerformanceCounter/Frequency, CreateWaitableTimerEx,
//   ZwSetTimerResolution (ntdll, undocumented but widely known).

#include <windows.h>
#include <cstdint>

namespace ul_timing {

// Conversion factors — set by Init()
extern int64_t g_qpc_freq;      // QPC ticks per second
extern int64_t g_ns_per_tick;   // nanoseconds per QPC tick (integer approx, for diagnostics only)

// Must call once at startup. Returns false if QPC unavailable.
bool Init();

// Current time
int64_t NowQpc();
int64_t NowNs();

// Sleep until target_ns using high-resolution waitable timer + busy-wait tail.
// timer_handle is lazily created on first call and reused thereafter.
void SleepUntilNs(int64_t target_ns, HANDLE& timer_handle);

// Hook CreateWaitableTimer to promote all timers in the process to
// high-resolution. Improves timing precision for driver-internal timers.
// Requires MinHook to be initialized. Safe to call multiple times.
void InstallTimerPromotionHooks();
void RemoveTimerPromotionHooks();

}  // namespace ul_timing
