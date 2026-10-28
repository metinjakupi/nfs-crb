// Logical speaker positions at the mixer/output boundary. A count alone is
// insufficient: callers must say which plane belongs to which speaker.
#pragma once
#include <array>
#include <stddef.h>
#include <stdint.h>

namespace audio {
enum class Speaker : uint8_t { Left, Right, Center, Lfe, LeftSurround, RightSurround };
struct Layout {
    unsigned count = 0;
    std::array<Speaker, 6> speakers = {};
};
inline constexpr Layout stereo{2, {Speaker::Left, Speaker::Right}};
inline constexpr Layout surround51{6,
                                   {Speaker::Left, Speaker::Right, Speaker::Center, Speaker::Lfe,
                                    Speaker::LeftSurround, Speaker::RightSurround}};

inline const char *speaker_name(Speaker speaker) {
    switch (speaker) {
    case Speaker::Left:
        return "L";
    case Speaker::Right:
        return "R";
    case Speaker::Center:
        return "C";
    case Speaker::Lfe:
        return "LFE";
    case Speaker::LeftSurround:
        return "Ls";
    case Speaker::RightSurround:
        return "Rs";
    }
    return "?";
}

// Output slots are SDL's documented L/R[/C/LFE/BL/BR] order. In a 5.1 mix
// BL/BR are the sole surround pair; the pinned CoreAudio backend tags that
// stream DVD_12 (L/R/C/LFE/Ls/Rs). This is not a 7.1 rear/side collapse.
struct OutputMap {
    unsigned count = 0;
    std::array<unsigned, 6> source = {};
};
inline bool output_map(const Layout &input, OutputMap &map) {
    map = {};
    if (input.count != 2 && input.count != 6)
        return false;
    const auto &output = input.count == 2 ? stereo : surround51;
    OutputMap candidate;
    candidate.count = input.count;
    for (unsigned dst = 0; dst < output.count; ++dst) {
        unsigned matches = 0;
        for (unsigned src = 0; src < input.count; ++src) {
            if (input.speakers[src] == output.speakers[dst]) {
                candidate.source[dst] = src;
                ++matches;
            }
        }
        if (matches != 1)
            return false; // missing/duplicated/unknown positions never become stereo
    }
    map = candidate;
    return true;
}

// How a mix is presented at the device boundary. Values equal the mod API's
// POP_AUDIO_OUTPUT_* and POP_AUDIO_SWAP_CENTER_LFE (checked in sdl_sink.cpp).
enum : uint32_t { present_mixer = 0, present_mono = 1, present_stereo = 2, present_51 = 3 };
enum : uint32_t { present_swap_center_lfe = 1 };

// Applies a presentation to the mixer's planes in place, before interleave.
// A six-plane (L R C LFE Ls Rs) mix is folded for Mono/Stereo or a two-channel
// device by ITU-R BS.775 (C and Ls/Rs at -3 dB into L/R, LFE omitted), summed
// at the game's gains and saturated at full scale like the mix itself - no
// normalisation or boost; planes 2..5 are cleared. Discrete 5.1 may exchange
// C and LFE (plane pointers) for an endpoint verified to swap them. A stereo
// mix only changes for Mono. Returns true when the planes were folded.
inline bool present(uint32_t mode, uint32_t flags, bool two_channel_device, unsigned mix_channels,
                    float **p, uint32_t frames) {
    if (mix_channels == 6) {
        const bool mono = mode == present_mono;
        if (two_channel_device || mono || mode == present_stereo) {
            const float k = 0.70710678f;
            for (uint32_t i = 0; i < frames; ++i) {
                float l = p[0][i] + k * p[2][i] + k * p[4][i];
                float r = p[1][i] + k * p[2][i] + k * p[5][i];
                if (mono)
                    l = r = 0.5f * (l + r);
                p[0][i] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
                p[1][i] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
                p[2][i] = p[3][i] = p[4][i] = p[5][i] = 0.0f;
            }
            return true;
        }
        if (flags & present_swap_center_lfe) {
            float *c = p[2];
            p[2] = p[3];
            p[3] = c;
        }
    } else if (mode == present_mono) {
        for (uint32_t i = 0; i < frames; ++i)
            p[0][i] = p[1][i] = 0.5f * (p[0][i] + p[1][i]);
    }
    return false;
}

// The map is validated when opening a stream, outside the audio callback.
inline void interleave(const OutputMap &map, float *const *planes, uint32_t frames, float *out) {
    for (uint32_t i = 0; i < frames; ++i)
        for (unsigned dst = 0; dst < map.count; ++dst)
            out[size_t(i) * map.count + dst] = planes[map.source[dst]][i];
}
} // namespace audio
