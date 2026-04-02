// ReLimiter — FPS-ratio-based frame generation tier monitor
// Clean-room implementation from public API documentation:
//   No external APIs used — pure arithmetic on measured FPS values.

#include "ul_fg_monitor.hpp"

namespace ul_fg_monitor {

static std::atomic<int> s_tier{0};
static const char*      s_mult = "";
static int              s_update_count = 0;  // number of valid Update() calls

// Lock-in state: once a tier is detected, hold it until a different
// tier has been observed for kConfirmFrames consecutive updates.
static int s_pending_tier = 0;
static int s_confirm_count = 0;
static constexpr int kFirstDetectFrames = 30;   // immediate-ish first detection
static constexpr int kTierChangeFrames  = 120;  // ~1-2s sustained for tier changes

void Update(int raw_tier) {
    if (raw_tier < 0 || raw_tier > 6) return;
    s_update_count++;

    int current = s_tier.load(std::memory_order_relaxed);

    // First detection: accept after a short confirmation window
    if (current == 0 && raw_tier > 0) {
        if (raw_tier == s_pending_tier) {
            s_confirm_count++;
            if (s_confirm_count >= kFirstDetectFrames) {
                s_tier.store(raw_tier, std::memory_order_relaxed);
                s_pending_tier = 0;
                s_confirm_count = 0;
            }
        } else {
            s_pending_tier = raw_tier;
            s_confirm_count = 1;
        }
    }
    // Same as current: reset pending
    else if (raw_tier == current) {
        s_pending_tier = 0;
        s_confirm_count = 0;
    }
    // Different tier: require sustained confirmation (120 frames)
    else {
        if (raw_tier == s_pending_tier) {
            s_confirm_count++;
            if (s_confirm_count >= kTierChangeFrames) {
                s_tier.store(raw_tier, std::memory_order_relaxed);
                s_pending_tier = 0;
                s_confirm_count = 0;
            }
        } else {
            s_pending_tier = raw_tier;
            s_confirm_count = 1;
        }
    }

    // Update display string
    int t = s_tier.load(std::memory_order_relaxed);
    switch (t) {
        case 6: s_mult = " 6x"; break;
        case 5: s_mult = " 5x"; break;
        case 4: s_mult = " 4x"; break;
        case 3: s_mult = " 3x"; break;
        case 2: s_mult = " 2x"; break;
        default: s_mult = ""; break;
    }
}

int GetTier() {
    return s_tier.load(std::memory_order_relaxed);
}

void Reset() {
    s_tier.store(0, std::memory_order_relaxed);
    s_mult = "";
    s_pending_tier = 0;
    s_confirm_count = 0;
    s_update_count = 0;
}

bool HasData() {
    return s_update_count >= kFirstDetectFrames;
}

const char* GetMultiplierString() {
    return s_mult;
}

}  // namespace ul_fg_monitor
