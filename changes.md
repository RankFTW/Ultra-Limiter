## v2.5.5

Improved Vulkan Streamline compatibility, hardware-accurate display timing, and pacing stability.

> **Note**: NVIDIA driver **595.58 or newer** is required for best Vulkan performance and frame pacing.

### Vulkan Streamline Overhaul
- Completely reworked how ReLimiter integrates with Streamline (DLSS FG) on Vulkan games. Markers are now intercepted at the Streamline API level instead of patching low-level driver functions, fixing games where frame generation broke or markers stopped flowing after the first frame.
- ReLimiter now properly controls pacing in Vulkan+Streamline games — the game's sleep and interval settings are captured and managed on our schedule, preventing the driver from capping FPS independently.

### Hardware Display Timing (Vulkan)
- Added support for VK_EXT_present_timing, providing nanosecond-accurate display scanout feedback directly from the GPU hardware.
- Frame pacing now uses real display timing instead of estimated present times, resulting in smoother output cadence on supported hardware.
- VRR ceiling detection uses the display's actual refresh duration instead of Windows display settings, improving accuracy on variable refresh rate monitors.
- Frames are released slightly early to the driver's display scheduler for hardware-precise vblank alignment, reducing jitter from software busy-wait.

### Pacing Stability
- Fixed an issue where Streamline games on Vulkan could lock to ~15 FPS due to conflicting sleep calls.
- Low-latency extension is now always enabled for Vulkan games regardless of Streamline presence, improving compatibility.
- Hook state is refreshed on swapchain recreate to handle driver address changes after resolution or settings changes.

### Logging
- Improved diagnostic logging for Vulkan hooks with periodic status updates instead of one-shot messages, making it easier to troubleshoot issues.