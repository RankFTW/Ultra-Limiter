#pragma once
// ReLimiter — FPS-ratio-based frame generation tier monitor
// Clean-room implementation from public API documentation:
//   No external APIs used — pure arithmetic on measured FPS values.

#include <atomic>

namespace ul_fg_monitor {

// Feed the computed FG multiplier each frame.
// raw_tier: 0 = no FG, 2-6 = multiplier (from output_fps / real_fps).
// First detection confirms after 30 frames. Tier changes require 120 frames.
void Update(int raw_tier);

// Read the current FPS-based FG tier (0 = no FG, 2–6 = multiplier).
int  GetTier();

// Reset to tier 0 (e.g. on background→foreground transition).
void Reset();

// True once enough updates have been processed to trust the tier value.
bool HasData();

// Return the multiplier suffix string for OSD display (" 2x", " 3x", etc.)
// Returns "" when tier < 2.
const char* GetMultiplierString();

}  // namespace ul_fg_monitor
