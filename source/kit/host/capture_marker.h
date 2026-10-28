#pragma once
#include <cstdint>

// Value copied with the immutable presented pixels, never a live renderer pointer.
struct CaptureMarker {
    uint64_t id = 0, render_frame = 0, monotonic_ns = 0;
    uint32_t state = 0, reason = 0;
    bool enabled = false, detailed = false;
};
namespace capture_state {
enum { Disarmed, Armed, Capturing, Saving, Ready, Incomplete };
inline const char *name(uint32_t s) {
    const char *names[] = {"DISARMED", "ARMED", "CAPTURING", "SAVING", "READY", "INCOMPLETE"};
    return s < 6 ? names[s] : "INVALID";
}
inline const char *reason(uint32_t r) {
    const char *names[] = {"F8 ARM / TRIGGER", "BUSY CAPTURING",  "BUSY SAVING",
                           "SESSION LIMIT 3",  "RECORD OVERFLOW", "SAVE FAILED",
                           "HISTORY WARMING"};
    return r < 7 ? names[r] : "INVALID";
}
} // namespace capture_state
