#include "video_frame.h"
#include <algorithm>
#include <cmath>

// BT.601 studio range: Y=16 is black, Y=235 is white, and chroma is
// centred on 128. Store guest little-endian pixels without alignment casts.
void video_frame_convert_row(uint8_t *dest, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                             uint32_t width, VideoSurfaceType type, unsigned chroma_shift) {
    for (uint32_t x = 0; x < width; ++x) {
        int c = (int)y[x] - 16, d = (int)u[x >> chroma_shift] - 128,
            e = (int)v[x >> chroma_shift] - 128;
        auto clip = [](int value) { return (uint32_t)std::clamp(value / 256, 0, 255); };
        uint32_t r = clip(298 * c + 409 * e + 128);
        uint32_t g = clip(298 * c - 100 * d - 208 * e + 128);
        uint32_t b = clip(298 * c + 516 * d + 128);
        if (type == VIDEO_XRGB8888) {
            *dest++ = (uint8_t)b;
            *dest++ = (uint8_t)g;
            *dest++ = (uint8_t)r;
            *dest++ = 0;
        } else {
            uint32_t pixel = type == VIDEO_RGB565 ? ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                                                  : ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            *dest++ = (uint8_t)pixel;
            *dest++ = (uint8_t)(pixel >> 8);
        }
    }
}

bool video_color_conversion_valid(const VideoColorConversion &c) {
    const float values[] = {c.y_offset, c.uv_offset, c.y_scale, c.v_to_r,
                            c.u_to_g,   c.v_to_g,    c.u_to_b};
    for (float value : values)
        if (!std::isfinite(value) || std::abs(value) > 65536)
            return false;
    return true;
}

// A separate entry point keeps legacy integer rounding byte-identical. The
// decoder validates coefficients once per frame, never in the inner loop.
void video_frame_convert_color_row(uint8_t *dest, const uint8_t *y, const uint8_t *u,
                                   const uint8_t *v, uint32_t width, const VideoColorConversion &c,
                                   unsigned chroma_shift) {
    auto channel = [](float value) {
        return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    for (uint32_t x = 0; x < width; ++x) {
        const float yy = float(y[x]) / 255.0f - c.y_offset;
        const float uu = float(u[x >> chroma_shift]) / 255.0f - c.uv_offset;
        const float vv = float(v[x >> chroma_shift]) / 255.0f - c.uv_offset;
        const float scaled_y = c.y_scale * yy;
        *dest++ = channel(scaled_y + uu * c.u_to_b);
        *dest++ = channel((scaled_y + uu * c.u_to_g) + vv * c.v_to_g);
        *dest++ = channel(scaled_y + vv * c.v_to_r);
        *dest++ = 0;
    }
}
