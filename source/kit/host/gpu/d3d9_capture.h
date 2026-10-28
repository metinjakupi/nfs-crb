#pragma once
#include "d3d9_trace.h"
#include "../capture_marker.h"

namespace d9trace {
// Deliberately sampled, unfiltered FULL frames: 2 Hz, 16 samples = roughly
// two seconds preceding a trigger plus six seconds after it. No pixel readback.
// Screen recording must use the immutable R frame marker to identify samples.
// Unsampled frames have counts/context only: never infer absent draws from them.
struct CaptureControl {
    static constexpr uint32_t draw_capacity = 6144, event_capacity = 1024;
    static constexpr size_t history = 16, timeline_capacity = 2048;
    static constexpr uint64_t interval = 500000000, post = 6000000000;
    uint32_t state = capture_state::Disarmed, reason = 0;
    uint64_t id = 0, armed_ns = 0, trigger_ns = 0, trigger_frame = 0;
    uint64_t next_sample = 0, deadline = 0;
    bool next_detail = false;
    bool request(uint64_t now, uint64_t frame) {
        using namespace capture_state;
        if (state == Capturing || state == Saving) {
            reason = state == Capturing ? 1 : 2;
            return false;
        }
        if (state == Armed) {
            if (now - armed_ns < 2000000000) {
                reason = 6;
                return false;
            }
            ++id;
            state = Capturing;
            reason = 0;
            trigger_ns = now;
            trigger_frame = frame;
            deadline = now + post;
            next_sample = 0; // next WHOLE frame; not a partially recorded frame
            return true;
        }
        if (id >= 3) {
            reason = 3;
            return false;
        }
        state = Armed;
        reason = 0;
        armed_ns = now;
        next_sample = 0;
        return true;
    }
    bool frame(uint64_t now, bool detail) {
        using namespace capture_state;
        if (state == Capturing && detail && now >= deadline) {
            state = Saving;
            reason = 0;
            next_detail = false;
            return true;
        }
        next_detail = (state == Armed || state == Capturing) && now >= next_sample;
        if (next_detail)
            next_sample = now + interval;
        return false;
    }
    void saved(bool io_ok, bool complete) {
        state = io_ok && complete ? capture_state::Ready : capture_state::Incomplete;
        reason = !io_ok ? 5 : !complete ? 4 : 0;
    }
    CaptureMarker marker(uint64_t frame, uint64_t now, bool detailed) const {
        return {id, frame, now, state, reason, true, detailed};
    }
};
struct CaptureSummary {
    HostD9Accounting accounting{};
    uint64_t frame = 0, ns = 0, wall_us = 0;
    uint64_t draws_attempted = 0, events_attempted = 0;
    uint64_t draws_retained = 0, events_retained = 0, draws_dropped = 0, events_dropped = 0;
    Context context;
    bool detailed = false;
};
} // namespace d9trace
