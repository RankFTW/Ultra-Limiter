# ReLimiter — Vulkan Behavior Reference

## Startup (DLL_PROCESS_ATTACH)

1. MinHook initialized
2. `VkReflexHookCreateDevice()` — hooks `vkCreateDevice` on `vulkan-1.dll` to inject `VK_NV_low_latency2` into the game's extension list
3. `HookVkGetDeviceProcAddr()` — hooks `vkGetDeviceProcAddr` on `vulkan-1.dll` to intercept the game's resolution of LL2 functions

## Device Creation (game calls vkCreateDevice)

4. `Hooked_vkCreateDevice` fires — checks if game already enables LL2 (native Reflex) or injects it. If native, sets `g_game_uses_reflex = true`
5. On success, calls `HookLL2Functions(device)` — uses the original (unhooked) `vkGetDeviceProcAddr` trampoline to resolve the real driver addresses of `vkSetLatencySleepModeNV`, `vkLatencySleepNV`, `vkSetLatencyMarkerNV`, then hooks all three via MinHook

## Game Calls LL2 Functions (native Reflex games like RDR2)

6. `Wrapped_vkSetLatencySleepModeNV` — sets `g_game_uses_reflex = true`, forwards to driver. Game controls its own sleep mode params
7. `Wrapped_vkLatencySleepNV` — forwards to driver. Keeps the driver's latency measurement pipeline coherent
8. `Wrapped_vkSetLatencyMarkerNV` — records timestamp in `g_ring`, does per-frame dedup, fires `s_vk_marker_cb` (= `OnMarkerCb`), forwards to driver. PRESENT_FINISH forwarded before callback, all others after

## Swapchain Init (OnInitSwapchain)

9. `VkReflex::Init(device)` — resolves LL2 function pointers using the unhooked trampoline (gets real driver functions, not our wrappers)
10. `VkReflex::AttachSwapchain(swapchain)` — creates timeline semaphore, marks active
11. `ConnectVulkanReflex` — attaches VkReflex to the limiter

## Per-Frame Pacing (OnPresent)

12. `DoOwnSleep()` always runs for Vulkan — both native and non-native
13. For native: builds sleep params, skips `SetSleepMode` (game handles it via wrapper), runs grid sleep for FPS cap enforcement
14. For non-native: builds sleep params, calls `vk_reflex_->SetSleepMode()` with our interval, runs grid sleep
15. Grid sleep: phase-locked timing grid with high-res waitable timer + busy-wait tail

## Marker-Based Pacing (OnMarker, native Reflex games only)

16. `Wrapped_vkSetLatencyMarkerNV` → `OnMarkerCb` → `s_limiter.OnMarker()` → `DoOwnSleep()` at the enforcement site
17. This means native Reflex games get DoOwnSleep called twice per frame — once from OnPresent (backstop) and once from OnMarker (enforcement site). The grid handles this gracefully — the second call either finds the grid slot already consumed or sleeps until the next one

## Pipeline Stats (UpdatePipelineStats)

18. `vk_reflex_->GetLatencyTimings()` called every frame — fills `NvLatencyResult` from `vkGetLatencyTimingsNV`
19. Feeds the same adaptive systems as DX: enforcement site selection, consistency buffer, boost controller, queue depth voting, PLL grid correction

## OSD GPU Metrics

20. `PollVkGpuLatency()` — only extracts `gpuActiveRenderTimeUs` (reliable). Render latency and present latency not populated (Vulkan timestamps unreliable when markers are intercepted). Those OSD lines auto-hide when zero

## Swapchain Destroy

21. `VkReflex::DetachSwapchain()` — destroys semaphore, marks inactive

## Shutdown

22. `VkReflexUnhookCreateDevice()` — removes vkCreateDevice and vkGetDeviceProcAddr hooks, plus the three LL2 function hooks

## Not Supported on Vulkan

- GSync detection (NVAPI surface handle acquisition fails — DX-only API)
- Frame splitting control (depends on GSync detection)
- Frame latency override / exclusive pacing (DXGI-only)
- VSync override (DXGI Present hook)
- Fake fullscreen (DXGI-only)
- NGX resolution hooks (NGX CreateFeature is DX-only — Vulkan games use a different NGX path not hooked yet)
