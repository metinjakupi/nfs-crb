// sink.h - where the mixer's output goes. One implementation opens the
// platform's audio device through SDL; the other renders on demand for tests
// and the headless hosts.
#pragma once
#include <stdint.h>

#include <functional>
#include <memory>
#include "layout.h"

struct AudioSink {
    virtual ~AudioSink() = default;
    // Starts pulling: `render` is called on the sink's thread with
    // deinterleaved float buffers to fill. `rate` is the rate the mixer
    // renders at; the sink converts to the device's own if they differ.
    virtual bool start(double rate, const audio::Layout &layout,
                       std::function<void(float *const *channels, uint32_t frames)> render) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    // Stop pulling from the device without tearing the stream down; resume later.
    virtual void pause(bool paused) = 0;
    // True for a sink that sends an encoded bitstream (it owns the device).
    virtual bool bitstream() const {
        return false;
    }
};
std::unique_ptr<AudioSink> make_sdl_sink();
// Dolby Digital 5.1 bitstream on the default device (dolby_sink.cpp); null on
// platforms without it. start() fails, leaving the device untouched, when the
// device cannot take a Dolby Digital bitstream.
std::unique_ptr<AudioSink> make_dolby_sink();
// The output transport changed (host_audio_set_output's Dolby flag): the mixer
// reopens its sink with the requested one. Main/any thread; takes the API lock.
void audio_mixer_transport_changed();
// Logs, for an SDL audio device event: the event's own device (`which`, 0 if
// none) with its format, the current default playback device, and the game's
// active stream - the physical device it is bound to right now, that device's
// current format and channel map, and the stream's input/output formats.
// The game's stream follows the default device, and SDL moves it silently when
// a TV/receiver appears or a device changes format. Main thread only; reads
// the active stream under a lock and never touches the mixer.
void sdl_audio_log_route(const char *why, uint32_t which);
