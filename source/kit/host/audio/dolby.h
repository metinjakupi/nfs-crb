// dolby.h - the parts of the Dolby Digital output that need no device: the
// IEC 61937 burst an AC-3 frame travels in over HDMI/S/PDIF, and the movie bus
// that mixes timed-media PCM into the encoded output while the host owns the
// device exclusively (a bitstream cannot be mixed with other PCM downstream).
#pragma once
#include "../../platform/os.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace audio::dolby {
constexpr uint32_t rate = 48000;
constexpr uint32_t frames_per_burst = 1536;              // one AC-3 frame
constexpr size_t burst_bytes = frames_per_burst * 2 * 2; // carried as 2 ch x 16-bit PCM

// IEC 61937-3 burst for one AC-3 frame: Pa=F872 Pb=4E1F Pc=type 1 | bsmod<<8,
// Pd=length in bits, as little-endian 16-bit samples, then the frame's bytes
// as 16-bit words (AC-3 is big-endian, so each pair is swapped into the
// little-endian sample), zero padded to the 1536-frame repetition period.
// Returns burst_bytes, or 0 when `frame` is not a whole AC-3 frame that fits.
inline size_t wrap_ac3(const uint8_t *frame, size_t bytes, uint8_t *burst) {
    if (!frame || bytes < 6 || bytes > burst_bytes - 8 || frame[0] != 0x0B || frame[1] != 0x77)
        return 0;
    const unsigned bsmod = frame[5] & 7u;
    auto put = [burst](size_t at, uint16_t v) {
        burst[at] = uint8_t(v);
        burst[at + 1] = uint8_t(v >> 8);
    };
    put(0, 0xF872);
    put(2, 0x4E1F);
    put(4, uint16_t(0x0001u | (bsmod << 8)));
    put(6, uint16_t(bytes * 8));
    size_t i = 0;
    for (; i + 1 < bytes; i += 2) {
        burst[8 + i] = frame[i + 1];
        burst[9 + i] = frame[i];
    }
    if (i < bytes) { // odd length: the last word's low byte is padding
        burst[8 + i] = 0;
        burst[9 + i] = frame[i];
        i += 2;
    }
    memset(burst + 8 + i, 0, burst_bytes - 8 - i);
    return burst_bytes;
}

// One timed-media output (OsAudioOutput semantics) mixed into the encoded
// output at 48 kHz. The owner thread writes interleaved PCM16 at the movie's
// rate; the encoder thread mixes. The clock reports what has been mixed minus
// what is still waiting in the encoded pipeline (the sink's lead), so a movie
// keeps its A/V anchoring. Capacity equals the platform queue's (8 x 2048).
class MovieBus {
  public:
    static constexpr uint32_t capacity = 8 * 2048;
    MovieBus(uint32_t source_rate, unsigned channels)
        : rate_(source_rate), channels_(channels), ring_(size_t(capacity) * channels) {}
    unsigned channels() const {
        return channels_;
    }
    uint32_t source_rate() const {
        return rate_;
    }
    // Owner thread. Copies up to 2048 frames; 0 when full (backpressure).
    int write(const int16_t *pcm, uint32_t frames) {
        if (!pcm || !frames)
            return -1;
        if (frames > 2048)
            frames = 2048;
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ + frames > capacity)
            return 0;
        for (uint32_t f = 0; f < frames; ++f) {
            const size_t at = ((head_ + f) % capacity) * channels_;
            for (unsigned c = 0; c < channels_; ++c)
                ring_[at + c] = pcm[size_t(f) * channels_ + c] * (1.0f / 32768.0f);
        }
        head_ = (head_ + frames) % capacity;
        count_ += frames;
        enqueued_ += frames;
        return int(frames);
    }
    int start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enqueued_)
            return -1;
        started_ = true;
        paused_ = false;
        return 0;
    }
    int pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = true;
        return 0;
    }
    int set_gain(float gain) {
        if (!std::isfinite(gain) || gain < 0 || gain > 1)
            return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        gain_ = gain;
        return 0;
    }
    float gain() {
        std::lock_guard<std::mutex> lock(mutex_);
        return gain_;
    }
    int reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = tail_ = count_ = 0;
        enqueued_ = consumed_ = retired_ = 0;
        pos_ = 0;
        history_count_ = history_head_ = 0;
        base_position_ = 0;
        last_position_ = 0;
        started_ = false;
        paused_ = true;
        ++epoch_;
        return 0;
    }
    // Presentation clock. `played` bursts of the encoded stream have been
    // handed to the device completely; the next one, burst `played`, has been
    // playing for `fraction` (0..1) of its length. The mix records how far this
    // output's read position (source frames) got in each encoded burst, so its
    // time is the position reached by bursts already played plus that fraction
    // of the one playing; never less than reported before, within an epoch.
    void clock(uint64_t played, double fraction, OsAudioClock *out) {
        std::lock_guard<std::mutex> lock(mutex_);
        *out = {};
        out->enqueued_frames = enqueued_;
        out->returned_frames = consumed_;
        out->epoch = epoch_;
        out->paused = paused_;
        out->running = started_ && !paused_;
        double position = base_position_;
        for (unsigned i = 0; i < history_count_; ++i) {
            const Burst &b = history_[(history_head_ + i) % history_size];
            if (b.index < played) {
                position = b.after;
            } else {
                if (b.index == played) {
                    const double f = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
                    position = b.before + f * (b.after - b.before);
                }
                break;
            }
        }
        if (position < last_position_)
            position = last_position_;
        last_position_ = position;
        out->sample_time = position;
        out->valid = started_ ? 1 : 0;
        out->native_status = 0;
    }
    // Encoder thread: adds `frames` 48 kHz frames of this output into the L R
    // C LFE Ls Rs planes (a stereo source goes to L/R), Lanczos-3 resampled,
    // for encoded burst `burst` (increasing). Stops at the end of what was
    // written: a tail plays out completely (missing look-ahead reads as
    // silence), and an underrun resumes where it stopped, never repeating.
    void mix(float *const *planes, uint32_t frames, uint64_t burst = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || paused_)
            return;
        const double before = double(retired_) + pos_;
        const double step = double(rate_) / double(rate);
        for (uint32_t i = 0; i < frames; ++i) {
            const double base = std::floor(pos_);
            if (base >= double(count_))
                break;
            float value[6] = {};
            const double frac = pos_ - base;
            for (int k = -2; k <= 3; ++k) {
                const double at = base + k;
                if (at < 0 || at >= double(count_))
                    continue;
                const float w = lanczos(float(frac - k));
                const size_t idx = ((tail_ + size_t(at)) % capacity) * channels_;
                for (unsigned c = 0; c < channels_; ++c)
                    value[c] += w * ring_[idx + c];
            }
            for (unsigned c = 0; c < channels_ && c < 6; ++c)
                planes[c][i] += gain_ * value[c];
            pos_ += step;
            // Retire frames no later kernel can reach (index < floor(pos) - 2).
            while (pos_ >= 3.0 && count_ > 0) {
                tail_ = (tail_ + 1) % capacity;
                --count_;
                ++retired_;
                pos_ -= 1.0;
            }
        }
        const double after = double(retired_) + pos_;
        consumed_ = retired_ + uint64_t(std::min(std::floor(pos_), double(count_)));
        if (after == before)
            return;
        if (history_count_ == history_size) { // the oldest has long been played
            base_position_ = history_[history_head_].after;
            history_head_ = (history_head_ + 1) % history_size;
            --history_count_;
        }
        history_[(history_head_ + history_count_) % history_size] = {burst, before, after};
        ++history_count_;
    }

  private:
    static float lanczos(float x) {
        if (x == 0.0f)
            return 1.0f;
        if (x <= -3.0f || x >= 3.0f)
            return 0.0f;
        const float px = 3.14159265358979f * x;
        return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
    }
    std::mutex mutex_;
    const uint32_t rate_;
    const unsigned channels_;
    std::vector<float> ring_;
    uint32_t head_ = 0, tail_ = 0, count_ = 0;
    uint64_t enqueued_ = 0, consumed_ = 0, retired_ = 0, epoch_ = 1;
    double pos_ = 0; // read position relative to tail_, in source frames
    struct Burst {
        uint64_t index = 0;
        double before = 0, after = 0; // read position, source frames
    };
    static constexpr unsigned history_size = 32; // far more than the sink keeps ahead
    Burst history_[history_size];
    unsigned history_head_ = 0, history_count_ = 0;
    double base_position_ = 0; // position reached by bursts dropped from the history
    double last_position_ = 0;
    bool started_ = false, paused_ = true;
    float gain_ = 1.0f;
};
} // namespace audio::dolby
