// Decoder-independent, limited-range BT.601 planar YUV row conversion.
#pragma once
#include <stdint.h>

enum VideoSurfaceType : uint32_t {
    VIDEO_XRGB8888 = 3,
    VIDEO_RGB555 = 9,
    VIDEO_RGB565 = 10,
};

// chroma_shift is 1 for YUV420P or 2 for YUV410P. The caller selects
// the chroma row for y >> chroma_shift and supplies width * bpp output bytes.
void video_frame_convert_row(uint8_t *dest, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                             uint32_t width, VideoSurfaceType type, unsigned chroma_shift = 1);

// Optional explicit YUV conversion for players with a verified source policy.
// Samples are normalized unsigned bytes; coefficients operate before RGB clamp.
// Legacy callers retain their original integer BT.601 conversion above.
struct VideoColorConversion {
    float y_offset = 0, uv_offset = 0;
    float y_scale = 0, v_to_r = 0, u_to_g = 0, v_to_g = 0, u_to_b = 0;
};
bool video_color_conversion_valid(const VideoColorConversion &conversion);
void video_frame_convert_color_row(uint8_t *dest, const uint8_t *y, const uint8_t *u,
                                   const uint8_t *v, uint32_t width,
                                   const VideoColorConversion &conversion,
                                   unsigned chroma_shift = 1);
