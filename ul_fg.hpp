#pragma once
// ReLimiter — DLSS Frame Generation detection
// FG state is set by the slDLSSGSetOptions hook.

#include <atomic>
#include <cstdint>

// Is DLSS Frame Generation currently active?
extern std::atomic<bool> g_fg_active;

// Total FG multiplier: 1 = no FG, 2 = 2x, 3 = 3x, etc.
// Derived from DLSSGOptions.numFramesToGenerate + 1.
extern std::atomic<int>  g_fg_multiplier;
