// Ultra Limiter — High-precision timing implementation
// Clean-room from Windows API docs:
//   - QueryPerformanceCounter / QueryPerformanceFrequency (MSDN)
//   - CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+, MSDN)
//   - ZwSetTimerResolution / ZwQueryTimerResolution (ntdll, well-known)
// No code from any other frame limiter project.

#include "ul_timing.hpp"
#include "ul_log.hpp"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((long)0x00000000L)
#endif

namespace ul_timing {

int64_t g_qpc_freq = 10'000'000;   // default 10 MHz
int64_t g_ns_per_tick = 100;        // default 100 ns/tick

// Kernel timer resolution in 100-ns units (from ZwQueryTimerResolution)
static int64_t s_kern_timer_100ns = 0;

bool Init() {
    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        ul_log::Write("ul_timing::Init: QPC not available");
        return false;
    }

    g_qpc_freq = freq.QuadPart;
    // Integer division — loses sub-nanosecond precision but avoids float
    g_ns_per_tick = 1'000'000'000LL / g_qpc_freq;
    if (g_ns_per_tick == 0) g_ns_per_tick = 1;  // freq > 1 GHz edge case

    ul_log::Write("ul_timing::Init: freq=%lld ns_per_tick=%lld", g_qpc_freq, g_ns_per_tick);

    // Request maximum kernel timer resolution via ntdll
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        using ZwQueryFn = long(__stdcall*)(unsigned long*, unsigned long*, unsigned long*);
        using ZwSetFn = long(__stdcall*)(unsigned long, unsigned char, unsigned long*);

        auto fnQuery = reinterpret_cast<ZwQueryFn>(GetProcAddress(ntdll, "ZwQueryTimerResolution"));
        auto fnSet = reinterpret_cast<ZwSetFn>(GetProcAddress(ntdll, "ZwSetTimerResolution"));

        if (fnQuery && fnSet) {
            unsigned long res_min, res_max, res_cur;
            if (fnQuery(&res_min, &res_max, &res_cur) == STATUS_SUCCESS) {
                ul_log::Write("ul_timing::Init: kernel timer min=%lu max=%lu cur=%lu (100ns)",
                              res_min, res_max, res_cur);
                // Request the finest resolution the system supports
                fnSet(res_max, TRUE, &res_cur);
                s_kern_timer_100ns = static_cast<int64_t>(res_cur);
                ul_log::Write("ul_timing::Init: after ZwSet cur=%lu", res_cur);
            }
        }
    }
    return true;
}

int64_t NowQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t NowNs() {
    return NowQpc() * g_ns_per_tick;
}

void SleepUntilNs(int64_t target_ns, HANDLE& timer_handle) {
    // Convert target to QPC ticks
    int64_t target_qpc = target_ns / g_ns_per_tick;
    int64_t now = NowQpc();
    if (target_qpc <= now) return;

    // Create high-resolution waitable timer on first use
    if (timer_handle == nullptr) {
        timer_handle = CreateWaitableTimerEx(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_handle)
            timer_handle = CreateWaitableTimer(nullptr, FALSE, nullptr);
    }

    int64_t remaining = target_qpc - now;

    // Convert kernel timer resolution from 100-ns units to QPC ticks
    int64_t kern_res_qpc = (s_kern_timer_100ns > 0)
        ? (s_kern_timer_100ns * g_qpc_freq / 10'000'000LL)
        : 0;

    // Use kernel timer for the bulk, leaving ~2 timer resolutions for busy-wait
    if (timer_handle && kern_res_qpc > 0 && remaining > static_cast<int64_t>(2.5 * kern_res_qpc)) {
        int64_t sleep_qpc = remaining - 2 * kern_res_qpc;
        // SetWaitableTimer takes 100-ns units, negative = relative
        int64_t sleep_100ns = sleep_qpc * 10'000'000LL / g_qpc_freq;
        LARGE_INTEGER due;
        due.QuadPart = -sleep_100ns;
        if (SetWaitableTimer(timer_handle, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_handle, INFINITE);
    }

    // Busy-wait for the final stretch — sub-millisecond precision
    while (NowQpc() < target_qpc)
        YieldProcessor();
}

}  // namespace ul_timing
