#pragma once
// ReLimiter — Streamline PCL hooks for FG detection and marker interception.
// Works on both DX and Vulkan — hooks sl.interposer.dll exports.
// Written from public Streamline SDK headers (MIT license).

#include "ul_reflex.hpp"  // MarkerCb

// Hook Streamline's slPCLSetMarker, slReflexSleep, slReflexSetOptions,
// slGetFeatureFunction (for slDLSSGSetOptions/GetState interception).
// Call after sl.interposer.dll is loaded. Safe to call multiple times.
bool HookStreamlinePCL();

// Install LoadLibrary hooks to catch sl.interposer.dll loading early.
// When detected, immediately hooks slGetFeatureFunction for DLSSG detection.
// Call from DllMain (DLL_PROCESS_ATTACH). Safe to call multiple times.
void InstallLoadLibraryHooks();
void RemoveLoadLibraryHooks();

// Invoke the real Streamline sleep on our schedule (trampoline past our hook).
bool InvokeStreamlineSleep();

// Invoke the real Streamline SetOptions with our interval.
bool InvokeStreamlineSetOptions(uint32_t interval_us, bool low_latency, bool boost);

// Register marker callback for Streamline PCL path.
void SetStreamlineMarkerCb(MarkerCb cb);
