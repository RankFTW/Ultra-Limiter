// ReLimiter — High-precision timing implementation
// Clean-room from Windows API docs:
//   - QueryPerformanceCounter / QueryPerformanceFrequency (MSDN)
//   - CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+, MSDN)
//   - ZwSetTimerResolution / ZwQueryTimerResolution (ntdll, well-known)
//   - SetThreadPriority / THREAD_PRIORITY_TIME_CRITICAL (MSDN)
// No code from any other frame limiter project.
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
    // Overflow-safe QPC-to-nanoseconds conversion.
    // Naive `qpc * 1e9 / freq` overflows int64 after ~292 years at 1 GHz QPC,
    // but `qpc * g_ns_per_tick` (the old code) loses precision when freq doesn't
    // divide 1e9 evenly (e.g. 24 MHz → g_ns_per_tick=41 instead of 41.667,
    // drifting ~0.27ms per 16ms frame).
    //
    // Split into whole-seconds + remainder to avoid both overflow and truncation:
    //   ns = (qpc / freq) * 1e9  +  (qpc % freq) * 1e9 / freq
    // The remainder term never exceeds freq * 1e9 which fits in int64 for any
    // realistic QPC frequency (up to ~9.2 GHz).
    int64_t qpc = NowQpc();
    int64_t sec = qpc / g_qpc_freq;
    int64_t rem = qpc % g_qpc_freq;
    return sec * 1'000'000'000LL + rem * 1'000'000'000LL / g_qpc_freq;
}

void SleepUntilNs(int64_t target_ns, HANDLE& timer_handle) {
    // Convert target_ns to QPC ticks using the same overflow-safe approach.
    // target_qpc = target_ns * freq / 1e9, split to avoid overflow.
    int64_t target_sec = target_ns / 1'000'000'000LL;
    int64_t target_rem = target_ns % 1'000'000'000LL;
    int64_t target_qpc = target_sec * g_qpc_freq + target_rem * g_qpc_freq / 1'000'000'000LL;

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

    // Busy-wait for the final stretch — sub-millisecond precision.
    // Temporarily boost thread priority to minimize OS preemption risk
    // during the critical timing window.
    int prev_priority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (NowQpc() < target_qpc)
        YieldProcessor();

    SetThreadPriority(GetCurrentThread(), prev_priority);
}

}  // namespace ul_timing

// ============================================================================
// Global timer promotion — upgrade all waitable timers to high-resolution
// ============================================================================
//
// Some drivers and middleware create standard waitable timers internally.
// On systems where the default timer resolution is ~15.6ms, these timers
// can introduce significant jitter. By hooking CreateWaitableTimer(Ex),
// we promote all timers in the process to high-resolution, ensuring
// consistent sub-millisecond precision throughout the rendering pipeline.

#include <MinHook.h>

namespace ul_timing {

// Original function pointers
using CreateWaitableTimerW_fn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using CreateWaitableTimerExW_fn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

static CreateWaitableTimerW_fn   s_orig_create_timer_w = nullptr;
static CreateWaitableTimerExW_fn s_orig_create_timer_ex_w = nullptr;
static bool s_timer_hooks_installed = false;

static HANDLE WINAPI Hook_CreateWaitableTimerW(
    LPSECURITY_ATTRIBUTES lpAttrs, BOOL bManualReset, LPCWSTR lpName)
{
    // Promote to high-resolution via CreateWaitableTimerExW
    DWORD flags = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    if (bManualReset) flags |= CREATE_WAITABLE_TIMER_MANUAL_RESET;

    HANDLE h = s_orig_create_timer_ex_w
        ? s_orig_create_timer_ex_w(lpAttrs, lpName, flags, TIMER_ALL_ACCESS)
        : nullptr;

    // Fallback if high-res not supported
    if (!h && s_orig_create_timer_w)
        h = s_orig_create_timer_w(lpAttrs, bManualReset, lpName);

    return h;
}

static HANDLE WINAPI Hook_CreateWaitableTimerExW(
    LPSECURITY_ATTRIBUTES lpAttrs, LPCWSTR lpName, DWORD dwFlags, DWORD dwAccess)
{
    // Add high-resolution flag if not already present
    dwFlags |= CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;

    return s_orig_create_timer_ex_w
        ? s_orig_create_timer_ex_w(lpAttrs, lpName, dwFlags, dwAccess)
        : nullptr;
}

void InstallTimerPromotionHooks() {
    if (s_timer_hooks_installed) return;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;

    void* pCreateW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerW"));
    void* pCreateExW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerExW"));

    if (!pCreateW || !pCreateExW) return;

    // Hook ExW first (needed by the W hook's promotion path)
    MH_STATUS st = MH_CreateHook(pCreateExW, reinterpret_cast<void*>(Hook_CreateWaitableTimerExW),
                                  reinterpret_cast<void**>(&s_orig_create_timer_ex_w));
    if (st != MH_OK) return;
    if (MH_EnableHook(pCreateExW) != MH_OK) { MH_RemoveHook(pCreateExW); return; }

    st = MH_CreateHook(pCreateW, reinterpret_cast<void*>(Hook_CreateWaitableTimerW),
                        reinterpret_cast<void**>(&s_orig_create_timer_w));
    if (st != MH_OK) { MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW); return; }
    if (MH_EnableHook(pCreateW) != MH_OK) {
        MH_RemoveHook(pCreateW);
        MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW);
        return;
    }

    s_timer_hooks_installed = true;
    ul_log::Write("Timer promotion hooks installed (all timers → high-resolution)");
}

void RemoveTimerPromotionHooks() {
    if (!s_timer_hooks_installed) return;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        void* pCreateW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerW"));
        void* pCreateExW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateWaitableTimerExW"));
        if (pCreateW) { MH_DisableHook(pCreateW); MH_RemoveHook(pCreateW); }
        if (pCreateExW) { MH_DisableHook(pCreateExW); MH_RemoveHook(pCreateExW); }
    }

    s_orig_create_timer_w = nullptr;
    s_orig_create_timer_ex_w = nullptr;
    s_timer_hooks_installed = false;
}

}  // namespace ul_timing
