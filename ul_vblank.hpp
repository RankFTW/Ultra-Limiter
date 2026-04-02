#pragma once
// ReLimiter — VBlank monitor via D3DKMTWaitForVerticalBlankEvent
// Uses the public Windows D3DKMT API (gdi32.dll) to get exact vblank
// timestamps from the display hardware. Runs a background thread that
// wakes on every vblank and records the QPC timestamp.
//
// DX only — on Vulkan, VK_EXT_present_timing handles vblank alignment.

#include <atomic>
#include <cstdint>
#include <windows.h>

struct VBlankMonitor {
    // Initialize from HWND — opens D3DKMT adapter, spawns background thread.
    // Returns true if the vblank thread started successfully.
    bool Init(HWND hwnd);

    // Stop the background thread and close the adapter handle.
    void Shutdown();

    // Returns the QPC-nanosecond timestamp of the most recent vblank.
    // Returns 0 if the monitor hasn't started or no vblank has been recorded.
    int64_t GetLastVBlankNs() const {
        return last_vblank_ns_.load(std::memory_order_relaxed);
    }

    // Monotonic counter — increments on each vblank.
    uint64_t GetVBlankCount() const {
        return vblank_count_.load(std::memory_order_relaxed);
    }

    // True when the background thread is running and producing timestamps.
    bool IsActive() const {
        return active_.load(std::memory_order_relaxed);
    }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);

    std::atomic<int64_t>  last_vblank_ns_{0};
    std::atomic<uint64_t> vblank_count_{0};
    std::atomic<bool>     active_{false};
    std::atomic<bool>     stop_{false};

    HANDLE thread_handle_ = nullptr;
    uint32_t adapter_handle_ = 0;   // D3DKMT_HANDLE (UINT)
    uint32_t vidpn_source_id_ = 0;  // VidPnSourceId
};
