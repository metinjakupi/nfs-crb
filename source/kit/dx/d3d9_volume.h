#pragma once

#include <algorithm>
#include <cstdint>

// Initial sampled-volume subset. Keep guest allocation and the Metal upload
// contract in agreement; block compression, palettes and depth are not admitted.
namespace d9volume {
inline uint32_t pixel_bytes(uint32_t format) {
    switch (format) {
    case 21: // A8R8G8B8
    case 22: // X8R8G8B8
        return 4;
    case 28: // A8
    case 50: // L8
        return 1;
    default:
        return 0;
    }
}
inline uint32_t levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t n = 1;
    for (uint32_t v = std::max({w, h, d}); v > 1; v >>= 1)
        ++n;
    return n;
}
inline bool extent(uint32_t w, uint32_t h, uint32_t d, uint32_t count) {
    if (!w || !h || !d || w > 512 || h > 512 || d > 512 || count > levels(w, h, d))
        return false;
    // Budget both the canonical source and the worst admitted GPU conversion
    // (BGRA8), using 64-bit arithmetic before any allocation. 0 means full chain.
    uint64_t bytes = 0;
    const uint32_t n = count ? count : levels(w, h, d);
    for (uint32_t l = 0; l < n; ++l)
        bytes += uint64_t(std::max(w >> l, 1u)) * std::max(h >> l, 1u) * std::max(d >> l, 1u) * 4;
    return bytes <= 128u * 1024u * 1024u;
}
} // namespace d9volume
