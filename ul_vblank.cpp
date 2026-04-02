// ReLimiter — VBlank monitor via D3DKMTWaitForVerticalBlankEvent
// Clean-room from public Microsoft documentation:
//   - D3DKMTOpenAdapterFromHdc (gdi32.dll)
//   - D3DKMTWaitForVerticalBlankEvent (gdi32.dll)
//   - D3DKMTCloseAdapter (gdi32.dll)
// No code from any other project.

#include "ul_vblank.hpp"
#include "ul_timing.hpp"
#include "ul_log.hpp"

// D3DKMT structures — from public Microsoft headers (MIT)
// We define them inline to avoid pulling in d3dkmthk.h which
// requires the WDK. These are stable public API structures.

typedef UINT D3DKMT_HANDLE;

typedef struct _D3DKMT_OPENADAPTERFROMHDC {
    HDC            hDc;
    D3DKMT_HANDLE  hAdapter;
    LUID           AdapterLuid;
    UINT           VidPnSourceId;
} D3DKMT_OPENADAPTERFROMHDC;

typedef struct _D3DKMT_CLOSEADAPTER {
    D3DKMT_HANDLE hAdapter;
} D3DKMT_CLOSEADAPTER;

typedef struct _D3DKMT_WAITFORVERTICALBLANKEVENT {
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;        // optional, can be 0
    UINT          VidPnSourceId;
} D3DKMT_WAITFORVERTICALBLANKEVENT;

// Function pointer types
using PFN_D3DKMTOpenAdapterFromHdc          = LONG(WINAPI*)(D3DKMT_OPENADAPTERFROMHDC*);
using PFN_D3DKMTCloseAdapter                = LONG(WINAPI*)(const D3DKMT_CLOSEADAPTER*);
using PFN_D3DKMTWaitForVerticalBlankEvent   = LONG(WINAPI*)(const D3DKMT_WAITFORVERTICALBLANKEVENT*);

// STATUS_SUCCESS
constexpr LONG STATUS_SUCCESS_VAL = 0;

// Resolved once, used by all instances
static PFN_D3DKMTOpenAdapterFromHdc        s_pfn_OpenAdapter = nullptr;
static PFN_D3DKMTCloseAdapter              s_pfn_CloseAdapter = nullptr;
static PFN_D3DKMTWaitForVerticalBlankEvent s_pfn_WaitVBlank = nullptr;
static bool s_resolved = false;

static void ResolveD3DKMT() {
    if (s_resolved) return;
    s_resolved = true;

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) {
        ul_log::Write("VBlankMonitor: gdi32.dll not loaded");
        return;
    }
    s_pfn_OpenAdapter = reinterpret_cast<PFN_D3DKMTOpenAdapterFromHdc>(
        GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
    s_pfn_CloseAdapter = reinterpret_cast<PFN_D3DKMTCloseAdapter>(
        GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    s_pfn_WaitVBlank = reinterpret_cast<PFN_D3DKMTWaitForVerticalBlankEvent>(
        GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));

    if (!s_pfn_OpenAdapter || !s_pfn_CloseAdapter || !s_pfn_WaitVBlank) {
        ul_log::Write("VBlankMonitor: D3DKMT functions not found (Open=%p, Close=%p, Wait=%p)",
                       s_pfn_OpenAdapter, s_pfn_CloseAdapter, s_pfn_WaitVBlank);
        s_pfn_OpenAdapter = nullptr;
        s_pfn_CloseAdapter = nullptr;
        s_pfn_WaitVBlank = nullptr;
    }
}

bool VBlankMonitor::Init(HWND hwnd) {
    Shutdown();  // clean up any previous instance

    ResolveD3DKMT();
    if (!s_pfn_OpenAdapter || !s_pfn_WaitVBlank) {
        ul_log::Write("VBlankMonitor::Init: D3DKMT not available");
        return false;
    }

    if (!hwnd) {
        ul_log::Write("VBlankMonitor::Init: null HWND");
        return false;
    }

    // HWND → HMONITOR → GDI device name → HDC → D3DKMT adapter
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hmon) {
        ul_log::Write("VBlankMonitor::Init: MonitorFromWindow failed");
        return false;
    }

    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hmon, &mi)) {
        ul_log::Write("VBlankMonitor::Init: GetMonitorInfoA failed");
        return false;
    }

    HDC hdc = CreateDCA(mi.szDevice, mi.szDevice, nullptr, nullptr);
    if (!hdc) {
        ul_log::Write("VBlankMonitor::Init: CreateDCA failed for '%s'", mi.szDevice);
        return false;
    }

    D3DKMT_OPENADAPTERFROMHDC open = {};
    open.hDc = hdc;
    LONG status = s_pfn_OpenAdapter(&open);
    DeleteDC(hdc);

    if (status != STATUS_SUCCESS_VAL) {
        ul_log::Write("VBlankMonitor::Init: D3DKMTOpenAdapterFromHdc failed (status=%ld)", status);
        return false;
    }

    adapter_handle_ = open.hAdapter;
    vidpn_source_id_ = open.VidPnSourceId;
    stop_.store(false, std::memory_order_relaxed);

    thread_handle_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_handle_) {
        ul_log::Write("VBlankMonitor::Init: CreateThread failed");
        D3DKMT_CLOSEADAPTER close = { open.hAdapter };
        s_pfn_CloseAdapter(&close);
        adapter_handle_ = 0;
        return false;
    }

    // Lower thread priority — vblank monitoring shouldn't compete with game threads
    SetThreadPriority(thread_handle_, THREAD_PRIORITY_ABOVE_NORMAL);

    ul_log::Write("VBlankMonitor::Init: started (adapter=0x%X, vidpn=%u, device='%s')",
                   adapter_handle_, vidpn_source_id_, mi.szDevice);
    return true;
}

void VBlankMonitor::Shutdown() {
    if (thread_handle_) {
        stop_.store(true, std::memory_order_relaxed);
        // The thread blocks in D3DKMTWaitForVerticalBlankEvent which wakes
        // on every vblank (~6ms at 165Hz). It checks stop_ after each wake.
        // Worst case wait is one vblank period.
        WaitForSingleObject(thread_handle_, 500);
        CloseHandle(thread_handle_);
        thread_handle_ = nullptr;
    }

    if (adapter_handle_ && s_pfn_CloseAdapter) {
        D3DKMT_CLOSEADAPTER close = { adapter_handle_ };
        s_pfn_CloseAdapter(&close);
    }
    adapter_handle_ = 0;
    vidpn_source_id_ = 0;
    active_.store(false, std::memory_order_relaxed);
    last_vblank_ns_.store(0, std::memory_order_relaxed);
    vblank_count_.store(0, std::memory_order_relaxed);
}

DWORD WINAPI VBlankMonitor::ThreadProc(LPVOID param) {
    auto* self = static_cast<VBlankMonitor*>(param);

    D3DKMT_WAITFORVERTICALBLANKEVENT wait = {};
    wait.hAdapter = self->adapter_handle_;
    wait.hDevice = 0;
    wait.VidPnSourceId = self->vidpn_source_id_;

    // Retry logic: the first few calls can fail with WAIT_TIMEOUT (258)
    // during display mode changes at startup. Retry up to 10 times with
    // 100ms sleep between attempts before giving up.
    constexpr int kMaxRetries = 10;
    int retries = 0;

    while (!self->stop_.load(std::memory_order_relaxed)) {
        LONG status = s_pfn_WaitVBlank(&wait);
        if (status != STATUS_SUCCESS_VAL) {
            retries++;
            if (retries <= kMaxRetries) {
                // Transient failure — sleep and retry.
                // Display may be settling after a mode change.
                if (retries == 1) {
                    ul_log::Write("VBlankMonitor: wait failed (status=%ld), retrying (%d/%d)",
                                   status, retries, kMaxRetries);
                }
                Sleep(100);
                continue;
            }
            // Exhausted retries — adapter is genuinely invalid.
            ul_log::Write("VBlankMonitor: D3DKMTWaitForVerticalBlankEvent failed after %d retries (status=%ld), stopping",
                           kMaxRetries, status);
            break;
        }

        // Success — reset retry counter and mark active
        if (retries > 0) {
            ul_log::Write("VBlankMonitor: recovered after %d retries", retries);
            retries = 0;
        }
        if (!self->active_.load(std::memory_order_relaxed))
            self->active_.store(true, std::memory_order_relaxed);

        int64_t now_ns = ul_timing::NowNs();
        self->last_vblank_ns_.store(now_ns, std::memory_order_relaxed);
        self->vblank_count_.fetch_add(1, std::memory_order_relaxed);
    }

    self->active_.store(false, std::memory_order_relaxed);
    return 0;
}
