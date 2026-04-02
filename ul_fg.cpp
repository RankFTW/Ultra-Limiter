// ReLimiter — DLSS Frame Generation detection
// FG state is set by the slDLSSGSetOptions hook.

#include "ul_fg.hpp"

std::atomic<bool> g_fg_active{false};
std::atomic<int>  g_fg_multiplier{1};
