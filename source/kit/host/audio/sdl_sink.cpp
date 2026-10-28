// sdl_sink.cpp - the mixer's output on the default playback device, through
// an SDL3 audio stream. SDL converts the mixer's rate to the device's.
#include "sink.h"
#include "diagnostics.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <utility>
#include <string>
#include "../../platform/os.h"
#include "../../dx/host_api.h"
#include "../../mods/pop_mod_api.h"

namespace {
std::atomic<unsigned> diagnostic_sequence{0};
// How the mix is presented (POP_AUDIO_OUTPUT_*, POP_AUDIO_SWAP_*). Set by the
// game's options through core mods; read once per output period.
std::atomic<uint32_t> output_mode{POP_AUDIO_OUTPUT_MIXER}, output_flags{0};
const char *output_mode_name(uint32_t mode) {
    switch (mode) {
    case POP_AUDIO_OUTPUT_MONO:
        return "mono";
    case POP_AUDIO_OUTPUT_STEREO:
        return "stereo";
    case POP_AUDIO_OUTPUT_SURROUND51:
        return "5.1";
    }
    return "mixer layout";
}
static_assert(audio::present_mono == POP_AUDIO_OUTPUT_MONO &&
                  audio::present_stereo == POP_AUDIO_OUTPUT_STEREO &&
                  audio::present_51 == POP_AUDIO_OUTPUT_SURROUND51 &&
                  audio::present_swap_center_lfe == POP_AUDIO_SWAP_CENTER_LFE,
              "presentation values follow the mod API");
// The game's live output stream, for route logging on the main thread.
std::mutex active_stream_mutex;
SDL_AudioStream *active_stream = nullptr;
class SdlSink final : public AudioSink {
  public:
    ~SdlSink() override {
        stop();
    }
    bool start(double rate, const audio::Layout &layout,
               std::function<void(float *const *, uint32_t)> render) override {
        if (stream_)
            return true;
        if (!audio::output_map(layout, output_map_)) {
            fprintf(stderr, "[host] audio: unsupported or ambiguous speaker layout\n");
            return false;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[host] audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return false;
        }
        render_ = std::move(render);
        // RECOMP_AUDIO_FRAMES asks the device for that many frames per period
        // (SDL's default is the backend's: 480 on WASAPI, 1024 on Core Audio).
        // A diagnostic for crackle reports: a larger period gives the render
        // thread more slack at the cost of latency.
        if (const char *frames = recomp_env("AUDIO_FRAMES"); frames && *frames)
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
        // The backlog kept queued ahead of the device, in frames. A device
        // with a 10 ms period whose thread wakes 30 ms late (measured under
        // CrossOver) drains an exactly-filled stream and plays silence; a
        // 2048-frame backlog (43 ms at 48 kHz) rides that out. RECOMP_AUDIO_AHEAD
        // overrides it; 0 renders just in time, for A/B runs.
        ahead_frames_ = 2048;
        if (const char *ahead = recomp_env("AUDIO_AHEAD"); ahead && *ahead)
            ahead_frames_ = uint32_t(strtoul(ahead, nullptr, 10));
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_F32;
        channels_ = output_map_.count;
        present_map_ = output_map_;
        // A 5.1 mix on a device with fewer than six channels is folded here
        // (ITU, audio::present) rather than by SDL's generic matrix, which
        // attenuates the front pair of every stereo presentation.
        {
            SDL_AudioSpec preferred = {};
            int preferred_frames = 0;
            if (channels_ == 6 &&
                SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &preferred,
                                         &preferred_frames) &&
                preferred.channels > 0 && preferred.channels < 6) {
                channels_ = 2;
                present_map_.count = 2;
                present_map_.source = {0, 1};
                folded_device_ = true;
            }
        }
        spec.channels = channels_;
        spec.freq = int(rate);
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                            &SdlSink::pull, this);
        if (!stream_) {
            fprintf(stderr, "[host] audio: no playback device: %s\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec device = {};
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &frames);
        // CoreAudio can accept a six-channel AudioQueue on a stereo endpoint.
        // SDL reports that configured queue here, not physical speaker count.
        printf("[host] audio stream running: mixer %.0f Hz %u ch, configured output %d Hz %d ch, "
               "%d frames (physical speaker layout not inferred)\n",
               rate, channels_, device.freq, device.channels, frames);
        printf("[host] audio speaker map:");
        const auto &output_layout = channels_ == 6 ? audio::surround51 : audio::stereo;
        for (unsigned c = 0; c < channels_; ++c)
            printf(" %s=plane%u", audio::speaker_name(output_layout.speakers[c]),
                   present_map_.source[c]);
        printf(" (logical SDL layout; endpoint routing requires a speaker test)\n");
        printf("[host] audio presentation: %s%s\n",
               output_mode_name(output_mode.load(std::memory_order_relaxed)),
               folded_device_ ? "; the 5.1 mix is folded to stereo (ITU) for this 2-channel device"
                              : "");
        fflush(stdout);
        period_ms_ = frames > 0 && device.freq > 0 ? 1000.0 * frames / device.freq : 0;
        start_diagnostics(rate, device);
        {
            std::lock_guard<std::mutex> lock(active_stream_mutex);
            active_stream = stream_;
        }
        SDL_ResumeAudioStreamDevice(stream_);
        sdl_audio_log_route("stream opened", 0);
        return true;
    }
    void pause(bool paused) override {
        if (!stream_)
            return;
        if (paused)
            SDL_PauseAudioStreamDevice(stream_);
        else
            SDL_ResumeAudioStreamDevice(stream_);
    }
    void stop() override {
        if (!stream_)
            return;
        // Removing the callback waits for an in-flight callback to finish.
        // Destroying the stream/device then joins the producer before reporting.
        if (mute_)
            SDL_PauseAudioStreamDevice(stream_); // nothing renders once the mute is removed
        if (diagnostic_file_)
            SDL_SetAudioPostmixCallback(SDL_GetAudioStreamDevice(stream_), nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lock(active_stream_mutex);
            if (active_stream == stream_)
                active_stream = nullptr;
            SDL_DestroyAudioStream(stream_);
        }
        stream_ = nullptr;
        finish_diagnostics();
        report();
    }
    // Evidence for crackle reports: a pull that arrives later than 1.5 device
    // periods after the previous one is a gap the device filled with silence
    // (or stale data) before we could render.
    void report() {
        printf("[host] audio sink: %llu pulls, %llu late (>1.5 periods), %llu starved, max gap "
               "%.1f ms, max render %.1f ms, period %.1f ms, ahead %u frames\n",
               (unsigned long long)pulls_, (unsigned long long)late_, (unsigned long long)starved_,
               max_gap_ms_, max_render_ms_, period_ms_, ahead_frames_);
        fflush(stdout);
    }
    bool running() const override {
        return stream_ != nullptr;
    }

  private:
    // Explicit private diagnostic path, created exclusively. Default gameplay
    // has no meters/postmix callback and retains its existing SDL fast path.
    void start_diagnostics(double rate, const SDL_AudioSpec &device) {
        const char *path = recomp_env("AUDIO_DIAGNOSTICS");
        if (!path || !*path)
            return;
        // The host replaces its stereo sink when a discrete source arrives.
        // Preserve both segments instead of losing surround evidence or
        // reopening/truncating a previous run's file.
        const unsigned sequence = diagnostic_sequence.fetch_add(1, std::memory_order_relaxed);
        const std::string segment =
            sequence ? std::string(path) + "." + std::to_string(sequence) : std::string(path);
        diagnostic_file_ = fopen(segment.c_str(), "wx");
        if (!diagnostic_file_) {
            fprintf(stderr, "[host] audio diagnostics: cannot create a new evidence file\n");
            return;
        }
        mix_meter_ = std::make_unique<audio::diagnostic::Meter>();
        post_meter_ = std::make_unique<audio::diagnostic::Meter>();
        diagnostic_rate_ = unsigned(rate);
        diagnostic_channels_ = unsigned(device.channels);
        // TEST ONLY (host_tests --audio-device-modes): silence the device after
        // metering, so a real-device conversion check is inaudible.
        const char *mute = recomp_env("AUDIO_DIAGNOSTICS_MUTE");
        mute_ = mute && !strcmp(mute, "1");
        for (unsigned c = 0; c < audio::diagnostic::max_channels; ++c)
            diagnostic_map_[c] = int(c);
        int count = 0;
        int *map = SDL_GetAudioDeviceChannelMap(SDL_GetAudioStreamDevice(stream_), &count);
        if (map) {
            for (unsigned c = 0; c < audio::diagnostic::max_channels; ++c)
                diagnostic_map_[c] = int(c) < count ? map[c] : -1;
            SDL_free(map);
        }
        const bool installed =
            SDL_SetAudioPostmixCallback(SDL_GetAudioStreamDevice(stream_), &SdlSink::postmix, this);
        fprintf(diagnostic_file_,
                "{\"diagnostic\":\"audio-boundaries-v1\",\"requested_channels\":%u,"
                "\"requested_rate\":%u,\"requested_format\":\"float32\","
                "\"configured_channels\":%d,\"configured_rate\":%d,"
                "\"configured_sdl_format\":%u,\"postmix_installed\":%s,\"muted_after_meter\":%s,"
                "\"role_map_validity\":\"startup configuration; rerun after device change\","
                "\"boundary_limit\":\"SDL postmix before backend submission, not physical output; "
                "CoreAudio endpoint conversion and the receiver are downstream\","
                "\"application_RL_RR\":\"absent from this 2/6-channel mix, not measured zero\"}\n",
                channels_, diagnostic_rate_, device.channels, device.freq, unsigned(device.format),
                installed ? "true" : "false", mute_ ? "true" : "false");
        fflush(diagnostic_file_);
        if (!installed)
            fprintf(stderr, "[host] audio diagnostics: postmix unavailable: %s\n", SDL_GetError());
    }
    static void SDLCALL postmix(void *userdata, const SDL_AudioSpec *spec, float *buffer,
                                int bytes) {
        auto *self = static_cast<SdlSink *>(userdata);
        if (spec->channels > 0 && spec->freq > 0 && bytes >= 0 && spec->format == SDL_AUDIO_F32)
            self->post_meter_->observe(buffer, unsigned(bytes) / (unsigned(spec->channels) * 4),
                                       unsigned(spec->channels), unsigned(spec->freq));
        if (self->mute_ && bytes > 0)
            memset(buffer, 0, size_t(bytes));
    }
    void finish_diagnostics() {
        if (!diagnostic_file_)
            return;
        std::array<int, audio::diagnostic::max_channels> identity{0, 1, 2, 3, 4, 5, 6, 7};
        mix_meter_->report(diagnostic_file_, "host-mix-before-SDL", channels_, identity);
        post_meter_->report(diagnostic_file_, "SDL-postmix-before-backend", diagnostic_channels_,
                            diagnostic_map_);
        if (fclose(diagnostic_file_) != 0)
            fprintf(stderr, "[host] audio diagnostics: evidence file write failed\n");
        diagnostic_file_ = nullptr;
        mix_meter_.reset();
        post_meter_.reset();
    }
    static void SDLCALL pull(void *userdata, SDL_AudioStream *stream, int additional, int) {
        auto *self = static_cast<SdlSink *>(userdata);
        const uint32_t needed = uint32_t(additional) / (self->channels_ * sizeof(float));
        const uint32_t queued =
            uint32_t(SDL_GetAudioStreamQueued(stream)) / (self->channels_ * sizeof(float));
        if (queued == 0 && self->pulls_ > 0)
            ++self->starved_; // the device drained everything we had queued
        // Top the backlog up, and never hand back less than the device asked for.
        uint32_t frames = needed;
        if (self->ahead_frames_ > queued && self->ahead_frames_ - queued > frames)
            frames = self->ahead_frames_ - queued;
        if (!frames)
            return;
        const double now = double(SDL_GetTicksNS()) * 1e-6;
        if (self->last_pull_ms_ > 0) {
            const double gap = now - self->last_pull_ms_;
            if (gap > self->max_gap_ms_)
                self->max_gap_ms_ = gap;
            if (self->period_ms_ > 0 && gap > 1.5 * self->period_ms_)
                ++self->late_;
        }
        self->last_pull_ms_ = now;
        ++self->pulls_;
        // A line every 30 s as well as at exit, so a run that is killed or a
        // player's console still shows the numbers.
        if (self->report_ms_ == 0)
            self->report_ms_ = now;
        else if (now - self->report_ms_ >= 30000.0) {
            self->report_ms_ = now;
            self->report();
        }
        struct RenderTimer {
            SdlSink *s;
            double t0;
            ~RenderTimer() {
                const double ms = double(SDL_GetTicksNS()) * 1e-6 - t0;
                if (ms > s->max_render_ms_)
                    s->max_render_ms_ = ms;
            }
        } timer{self, now};
        float *planes[6];
        for (unsigned c = 0; c < 6; ++c) {
            self->planes_[c].assign(frames, 0.0f);
            planes[c] = self->planes_[c].data();
        }
        self->render_(planes, frames);
        self->present(planes, frames);
        self->interleaved_.resize(size_t(frames) * self->channels_);
        audio::interleave(self->present_map_, planes, frames, self->interleaved_.data());
        if (self->mix_meter_)
            self->mix_meter_->observe(self->interleaved_.data(), frames, self->channels_,
                                      self->diagnostic_rate_);
        SDL_PutAudioStreamData(stream, self->interleaved_.data(),
                               int(frames * self->channels_ * sizeof(float)));
    }
    // Applies the requested presentation to the mixed planes, in place.
    void present(float **planes, uint32_t frames) {
        audio::present(output_mode.load(std::memory_order_relaxed),
                       output_flags.load(std::memory_order_relaxed), folded_device_,
                       output_map_.count, planes, frames);
    }
    SDL_AudioStream *stream_ = nullptr;
    audio::OutputMap present_map_;
    bool folded_device_ = false;
    double period_ms_ = 0, last_pull_ms_ = 0, max_gap_ms_ = 0, max_render_ms_ = 0;
    uint64_t pulls_ = 0, late_ = 0, starved_ = 0;
    uint32_t ahead_frames_ = 2048;
    double report_ms_ = 0;
    std::function<void(float *const *, uint32_t)> render_;
    unsigned channels_ = 2;
    audio::OutputMap output_map_;
    std::vector<float> planes_[6], interleaved_;
    FILE *diagnostic_file_ = nullptr;
    bool mute_ = false;
    unsigned diagnostic_channels_ = 0, diagnostic_rate_ = 0;
    std::array<int, audio::diagnostic::max_channels> diagnostic_map_{};
    std::unique_ptr<audio::diagnostic::Meter> mix_meter_, post_meter_;
};
} // namespace

namespace {
void print_map(SDL_AudioDeviceID id) {
    int count = 0;
    if (int *map = SDL_GetAudioDeviceChannelMap(id, &count)) {
        printf(" map");
        for (int c = 0; c < count; ++c)
            printf(" %d", map[c]);
        SDL_free(map);
    } else {
        printf(" map identity");
    }
}
// "'name' 48000 Hz 2 ch 512 frames" for an opened (current format) or
// unopened (preferred format) device, or why it could not be read.
void print_device(SDL_AudioDeviceID id) {
    SDL_AudioSpec spec = {};
    int frames = 0;
    const char *name = SDL_GetAudioDeviceName(id);
    if (!SDL_GetAudioDeviceFormat(id, &spec, &frames)) {
        printf("'%s' format unavailable (%s)", name ? name : "?", SDL_GetError());
        return;
    }
    printf("'%s' %d Hz %d ch %d frames", name ? name : "?", spec.freq, spec.channels, frames);
}
} // namespace

void sdl_audio_log_route(const char *why, uint32_t which) {
    if (!SDL_WasInit(SDL_INIT_AUDIO))
        return;
    printf("[host] audio route (%s):", why);
    if (which) {
        printf(" event device #%u ", unsigned(which));
        print_device(SDL_AudioDeviceID(which));
        printf(";");
    }
    printf(" default ");
    print_device(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    print_map(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    {
        std::lock_guard<std::mutex> lock(active_stream_mutex);
        if (!active_stream) {
            printf("; no game stream open");
        } else {
            // The logical device of the game's stream reports the physical
            // device it is bound to now and that device's current format.
            const SDL_AudioDeviceID logical = SDL_GetAudioStreamDevice(active_stream);
            printf("; game stream on #%u -> ", unsigned(logical));
            print_device(logical);
            print_map(logical);
            SDL_AudioSpec in = {}, out = {};
            if (SDL_GetAudioStreamFormat(active_stream, &in, &out))
                printf("; stream in %d Hz %d ch -> out %d Hz %d ch", in.freq, in.channels, out.freq,
                       out.channels);
        }
    }
    printf(" (SDL converts the game's logical roles; physical routing needs a speaker test)\n");
    fflush(stdout);
}

int host_audio_set_output(uint32_t mode, uint32_t flags) {
    const uint32_t old_mode = output_mode.exchange(mode, std::memory_order_relaxed);
    const uint32_t old_flags = output_flags.exchange(flags, std::memory_order_relaxed);
    if (old_mode != mode || old_flags != flags) {
        const bool dolby = flags & POP_AUDIO_DOLBY_DIGITAL;
        printf("[host] audio presentation: %s%s%s (was %s%s%s)\n", output_mode_name(mode),
               dolby ? ", Dolby Digital" : "",
               !dolby && (flags & POP_AUDIO_SWAP_CENTER_LFE)
                   ? ", centre/LFE exchanged for this endpoint"
                   : "",
               output_mode_name(old_mode),
               (old_flags & POP_AUDIO_DOLBY_DIGITAL) ? ", Dolby Digital" : "",
               !(old_flags & POP_AUDIO_DOLBY_DIGITAL) && (old_flags & POP_AUDIO_SWAP_CENTER_LFE)
                   ? ", centre/LFE exchanged"
                   : "");
        fflush(stdout);
        if ((old_flags ^ flags) & POP_AUDIO_DOLBY_DIGITAL)
            audio_mixer_transport_changed();
    }
    return 1;
}
void host_audio_get_output(uint32_t *mode, uint32_t *flags) {
    if (mode)
        *mode = output_mode.load(std::memory_order_relaxed);
    if (flags)
        *flags = output_flags.load(std::memory_order_relaxed);
}

std::unique_ptr<AudioSink> make_sdl_sink() {
    return std::make_unique<SdlSink>();
}
