// Opt-in, bounded audio evidence. This never changes samples, the mixer layout,
// gain or routing. The caller serializes access and reports after callbacks stop.
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace audio::diagnostic {
constexpr unsigned max_channels = 8;
// One hour of one-second windows: a listening session, not just its start.
constexpr unsigned max_windows = 3600;

// SDL's documented order, corroborated by its CoreAudio DVD_12/WAVE_7_1
// queue tags. These are application roles, not a physical device's order.
inline const char *sdl_role(unsigned channels, int slot) {
    static constexpr const char *stereo[] = {"FL", "FR"};
    static constexpr const char *six[] = {"FL", "FR", "C", "LFE", "SL", "SR"};
    static constexpr const char *eight[] = {"FL", "FR", "C", "LFE", "RL", "RR", "SL", "SR"};
    if (slot < 0 || unsigned(slot) >= channels)
        return "UNKNOWN";
    if (channels == 2)
        return stereo[slot];
    if (channels == 6)
        return six[slot];
    if (channels == 8)
        return eight[slot];
    return "UNKNOWN";
}

struct Window {
    unsigned channels = 0, rate = 0;
    uint64_t frames = 0;
    std::array<double, max_channels> square_sum{}, peak{};
    std::array<uint64_t, max_channels> nonfinite{};
};

// Up to an hour of one-second statistics, with no allocation, locking or
// I/O on the callback. Excess/unsupported samples are counted, never mislabeled.
struct Meter {
    std::array<Window, max_windows> windows{};
    unsigned count = 0;
    uint64_t omitted_frames = 0;

    void observe(const float *interleaved, uint32_t frames, unsigned channels, unsigned rate) {
        if (!interleaved || channels == 0 || channels > max_channels || rate == 0) {
            omitted_frames += frames;
            return;
        }
        for (uint32_t i = 0; i < frames; ++i) {
            if (!count || windows[count - 1].frames == windows[count - 1].rate ||
                windows[count - 1].channels != channels || windows[count - 1].rate != rate) {
                if (count == max_windows) {
                    omitted_frames += frames - i;
                    return;
                }
                auto &w = windows[count++];
                w.channels = channels;
                w.rate = rate;
            }
            auto &w = windows[count - 1];
            ++w.frames;
            for (unsigned c = 0; c < channels; ++c) {
                const double sample = interleaved[size_t(i) * channels + c];
                if (!std::isfinite(sample)) {
                    ++w.nonfinite[c];
                    continue;
                }
                w.peak[c] = std::max(w.peak[c], std::abs(sample));
                w.square_sum[c] += sample * sample;
            }
        }
    }

    // A backend channel map maps output slots to SDL role indices. If the
    // configured channel count changes, the startup map cannot label that data.
    // RMS excludes nonfinite samples and records their count separately.
    void report(FILE *file, const char *boundary, unsigned mapped_channels,
                const std::array<int, max_channels> &map) const {
        for (unsigned i = 0; i < count; ++i) {
            const auto &w = windows[i];
            fprintf(file,
                    "{\"boundary\":\"%s\",\"window\":%u,\"rate\":%u,\"format\":\"float32\","
                    "\"frames\":%llu,\"channels\":[",
                    boundary, i, w.rate, (unsigned long long)w.frames);
            for (unsigned c = 0; c < w.channels; ++c) {
                const uint64_t finite = w.frames - w.nonfinite[c];
                const double rms = finite ? std::sqrt(w.square_sum[c] / finite) : 0;
                fprintf(file,
                        "%s{\"slot\":%u,\"role\":\"%s\",\"peak\":%.9g,\"rms\":%.9g,"
                        "\"nonfinite\":%llu}",
                        c ? "," : "", c,
                        w.channels == mapped_channels ? sdl_role(w.channels, map[c]) : "UNKNOWN",
                        w.peak[c], rms, (unsigned long long)w.nonfinite[c]);
            }
            fprintf(file, "]}\n");
        }
        fprintf(file, "{\"boundary\":\"%s\",\"omitted_frames\":%llu}\n", boundary,
                (unsigned long long)omitted_frames);
    }
};

// Diagnostic-only channel sequence: FL FR C LFE SL SR RL RR. The eight-channel
// SDL queue has its rear pair before its side pair, so sequence != memory order.
inline constexpr unsigned test_slots[] = {0, 1, 2, 3, 6, 7, 4, 5};
inline constexpr unsigned test_rate = 48000, test_slot_frames = 72000;
inline constexpr unsigned test_frames = test_slot_frames * 8;
inline void channel_test(float *out, uint32_t first, uint32_t frames) {
    std::fill(out, out + size_t(frames) * 8, 0.0f);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t absolute = uint64_t(first) + i;
        if (absolute >= test_frames)
            break;
        const unsigned slot = unsigned(absolute / test_slot_frames);
        const unsigned phase = unsigned(absolute % test_slot_frames);
        if (phase >= test_rate)
            continue;
        const double fade = std::min(1.0, double(std::min(phase, test_rate - 1 - phase)) / 960);
        const double hz = test_slots[slot] == 3 ? 60.0 : 440.0;
        out[size_t(i) * 8 + test_slots[slot]] =
            float(0.08 * fade * std::sin(6.2831853071795864769 * hz * phase / test_rate));
    }
}
} // namespace audio::diagnostic
