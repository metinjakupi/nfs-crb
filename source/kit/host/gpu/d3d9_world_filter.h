#pragma once

#include <algorithm>
#include <cstdint>

// Match the Metal backend's world-sampler heuristic. Do not override point,
// unmipmapped or clamped samplers used by post-processing, UI and shadows.
inline int d3d9_world_anisotropy(const uint32_t *s, int policy) {
    const bool filtered = s[5] >= 2 && s[5] <= 3 && s[6] >= 2 && s[6] <= 3;
    const bool world = filtered && s[7] == 2 && s[1] == 1 && s[2] == 1;
    const int game = (s[5] == 3 || s[6] == 3)
                         ? int(std::max<uint32_t>(1, std::min<uint32_t>(16, s[10])))
                         : 1;
    if (!world || policy <= 0)
        return game;
    return policy == 1 ? 1 : 1 << std::min(policy, 4);
}
