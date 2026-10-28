#pragma once

#include "d3d9_volume.h"
#include <array>
#include <cstddef>

// Narrow, allocation-free DDS view for linear BGRA8/XRGB8 volume inputs.
// No decompression, mip generation, pitch guessing or format substitution.
// The caller retains the input bytes; every admitted mip has an exact extent.
namespace ddsvolume {
struct Level {
    uint32_t width = 0, height = 0, depth = 0, row_pitch = 0, slice_pitch = 0;
    size_t offset = 0, bytes = 0;
};
struct View {
    uint32_t width = 0, height = 0, depth = 0, levels = 0, format = 0;
    std::array<Level, 10> mip{};
};
inline bool parse(const uint8_t *data, size_t size, View &out) {
    out = {};
    if (!data || size < 128)
        return false;
    auto u32 = [&](unsigned word) {
        const uint8_t *p = data + 4 * word;
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    if (u32(0) != 0x20534444 || u32(1) != 124 || u32(19) != 32)
        return false;
    const uint32_t flags = u32(2), caps = u32(27);
    if ((flags & 0x801007) != 0x801007 || (flags & ~0x82100fu) || (caps & 0x1008) != 0x1008 ||
        (caps & ~0x40100au) || u32(28) != 0x200000 || u32(29) || u32(30) || u32(31))
        return false;
    View v;
    v.width = u32(4);
    v.height = u32(3);
    v.depth = u32(6);
    v.levels = flags & 0x20000 ? u32(7) : 1;
    if (!(flags & 0x20000) && u32(7) > 1)
        return false;
    if (!v.levels || v.levels > v.mip.size() ||
        !d9volume::extent(v.width, v.height, v.depth, v.levels) ||
        (v.levels > 1 && !(caps & 0x400000)) || ((flags & 8) ? u32(5) != v.width * 4 : u32(5) != 0))
        return false;
    // DDS_PIXELFORMAT masks specify little-endian BGRA memory order.
    if (u32(21) || u32(22) != 32 || u32(23) != 0xff0000 || u32(24) != 0xff00 || u32(25) != 0xff)
        return false;
    if (u32(20) == 0x41 && u32(26) == 0xff000000)
        v.format = 21;
    else if (u32(20) == 0x40 && u32(26) == 0)
        v.format = 22;
    else
        return false;
    size_t offset = 128;
    for (uint32_t i = 0; i < v.levels; ++i) {
        auto &m = v.mip[i];
        m.width = std::max(v.width >> i, 1u);
        m.height = std::max(v.height >> i, 1u);
        m.depth = std::max(v.depth >> i, 1u);
        m.row_pitch = m.width * 4;
        m.slice_pitch = m.row_pitch * m.height;
        m.bytes = size_t(m.slice_pitch) * m.depth;
        m.offset = offset;
        if (m.bytes > size - offset)
            return false;
        offset += m.bytes;
    }
    if (offset != size)
        return false;
    out = v;
    return true;
}
} // namespace ddsvolume
