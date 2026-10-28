// Explicit real-device probe: silence only, finite duration, no game/profile.
// Tests the OS timeline rather than assuming returned buffers have been heard.
#include "../os.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
        }                                                                                          \
    } while (0)

static int fill(OsAudioOutput *output) {
    const int16_t silence[2048 * 6]{};
    int total = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const int written = os_audio_output_write(output, silence, 2048);
        CHECK(written >= 0 && written <= 2048);
        if (written <= 0)
            return total;
        total += written;
    }
    CHECK(false); // bounded queue must exert backpressure
    return total;
}

static OsAudioClock observe(OsAudioOutput *output, const char *phase, uint32_t rate) {
    OsAudioClock clock{};
    os_audio_output_clock(output, &clock);
    printf("{\"phase\":\"%s\",\"rate\":%u,\"host_ns\":%llu,\"sample_time\":%.6f,"
           "\"enqueued\":%llu,\"epoch\":%llu,\"discontinuities\":%llu,"
           "\"valid\":%d,\"running\":%d,\"paused\":%d,\"native_status\":%d}\n",
           phase, rate, (unsigned long long)os_monotonic_ns(), clock.sample_time,
           (unsigned long long)clock.enqueued_frames, (unsigned long long)clock.epoch,
           (unsigned long long)clock.discontinuities, clock.valid, clock.running, clock.paused,
           clock.native_status);
    return clock;
}

static OsAudioClock run_for(OsAudioOutput *output, uint32_t rate, unsigned ms) {
    OsAudioClock last{};
    double previous = -1;
    const uint64_t end = os_monotonic_ns() + uint64_t(ms) * 1000000;
    while (os_monotonic_ns() < end) {
        fill(output);
        last = observe(output, "playing", rate);
        if (last.valid) {
            CHECK(last.sample_time >= previous);
            CHECK(last.sample_time <= double(last.enqueued_frames) + 1);
            previous = last.sample_time;
        }
        os_sleep_us(10000);
    }
    CHECK(previous > 0);
    return last;
}

static void test_output(uint32_t rate, OsAudioLayout layout) {
    int status = 0;
    OsAudioOutput *output = os_audio_output_open(rate, layout, &status);
    printf("{\"open_rate\":%u,\"layout\":%d,\"native_status\":%d}\n", rate, int(layout), status);
    CHECK(output != nullptr && status == 0);
    if (!output)
        return;
    CHECK(os_audio_output_start(output) == -1); // no data
    CHECK(fill(output) == 8 * 2048);
    const auto before = observe(output, "prebuffered", rate);
    CHECK(!before.valid && before.paused && before.enqueued_frames == 8 * 2048);
    os_sleep_us(100000);
    CHECK(!observe(output, "prebuffered-wait", rate).valid);
    CHECK(os_audio_output_start(output) == 0);
    auto playing = run_for(output, rate, 450);
    CHECK(playing.valid && playing.sample_time > double(rate) * .2);
    CHECK(os_audio_output_pause(output) == 0);
    os_sleep_us(50000); // allow a device period to settle
    const auto paused = observe(output, "paused", rate);
    os_sleep_us(200000);
    const auto held = observe(output, "paused-held", rate);
    CHECK(paused.valid && held.valid && held.paused);
    CHECK(std::abs(held.sample_time - paused.sample_time) <= 1);
    CHECK(held.enqueued_frames == paused.enqueued_frames);
    CHECK(os_audio_output_start(output) == 0);
    const auto resumed = run_for(output, rate, 300);
    CHECK(resumed.valid && resumed.sample_time > held.sample_time + double(rate) * .1);
    CHECK(os_audio_output_reset(output) == 0);
    const auto reset = observe(output, "reset", rate);
    CHECK(!reset.valid && reset.enqueued_frames == 0 && reset.epoch == before.epoch + 1);
    CHECK(fill(output) == 8 * 2048);
    CHECK(os_audio_output_start(output) == 0);
    const auto restarted = run_for(output, rate, 200);
    CHECK(restarted.valid && restarted.sample_time < resumed.sample_time);
    CHECK(restarted.epoch == reset.epoch);
    os_audio_output_close(output); // stop while active: no post-destruction callback
}

// Higher-frequency raw clock capture, deliberately without a monotonicity
// assertion or smoothing. Store bounded records in memory so file I/O cannot
// disturb the measured calls. This distinguishes device estimate corrections
// from decoder/sample-scheduling mistakes in the separate movie probe.
static void capture_timeline(uint32_t rate, OsAudioLayout layout, bool starve = false) {
    struct Record {
        uint64_t host;
        OsAudioClock clock;
    };
    std::vector<Record> records;
    records.reserve(32768);
    int error = 0;
    auto *output = os_audio_output_open(rate, layout, &error);
    CHECK(output && !error);
    if (!output)
        return;
    fill(output);
    CHECK(os_audio_output_start(output) == 0);
    const uint64_t begin = os_monotonic_ns();
    const uint64_t until = begin + (starve ? 3000000000ull : 20000000000ull);
    while (os_monotonic_ns() < until && records.size() + 2 <= records.capacity()) {
        OsAudioClock before{}, after{};
        os_audio_output_clock(output, &before);
        records.push_back({os_monotonic_ns(), before});
        // Deliberately stop producing silence long enough to drain the queue.
        // Observe the native discontinuity and sample coordinate on recovery.
        const auto elapsed = os_monotonic_ns() - begin;
        if (!starve || elapsed < 200000000ull || elapsed > 1200000000ull)
            fill(output);
        os_audio_output_clock(output, &after);
        records.push_back({os_monotonic_ns(), after});
        os_sleep_us(2000);
    }
    os_audio_output_close(output);
    for (const auto &record : records) {
        const auto &clock = record.clock;
        printf("{\"raw_rate\":%u,\"host_ns\":%llu,\"sample_time\":%.6f,"
               "\"enqueued\":%llu,\"valid\":%d,\"running\":%d,"
               "\"discontinuities\":%llu,\"status\":%d}\n",
               rate, (unsigned long long)record.host, clock.sample_time,
               (unsigned long long)clock.enqueued_frames, clock.valid, clock.running,
               (unsigned long long)clock.discontinuities, clock.native_status);
    }
}

static void test_gain(uint32_t rate, OsAudioLayout layout) {
    int error = 0;
    auto *output = os_audio_output_open(rate, layout, &error);
    CHECK(output && !error);
    if (!output)
        return;
    float actual = -1;
    CHECK(os_audio_output_set_gain(nullptr, .5f) == -1);
    CHECK(os_audio_output_get_gain(output, nullptr) == -1);
    CHECK(os_audio_output_set_gain(output, 0) == 0);
    CHECK(os_audio_output_get_gain(output, &actual) == 0 && actual == 0);
    CHECK(fill(output) == 8 * 2048);
    CHECK(os_audio_output_start(output) == 0);
    os_sleep_us(50000);
    for (float gain : {1.f, .5f, 0.f}) {
        CHECK(os_audio_output_set_gain(output, gain) == 0);
        CHECK(os_audio_output_get_gain(output, &actual) == 0 && std::abs(actual - gain) < .00001f);
    }
    for (float invalid : {-1.f, 1.001f, float(NAN), float(INFINITY)})
        CHECK(os_audio_output_set_gain(output, invalid) == -1);
    CHECK(os_audio_output_get_gain(output, &actual) == 0 && actual == 0);
    CHECK(os_audio_output_pause(output) == 0);
    CHECK(os_audio_output_set_gain(output, .25f) == 0);
    CHECK(os_audio_output_reset(output) == 0);
    CHECK(os_audio_output_get_gain(output, &actual) == 0 && std::abs(actual - .25f) < .00001f);
    os_audio_output_close(output);
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--silent") && strcmp(argv[1], "--silent-timeline") &&
                      strcmp(argv[1], "--silent-gain") && strcmp(argv[1], "--silent-underrun"))) {
        fprintf(
            stderr,
            "usage: audio_clock_probe --silent[-timeline|-gain|-underrun] (opens default audio device)\n");
        return 2;
    }
    if (!strcmp(argv[1], "--silent-underrun")) {
        capture_timeline(44100, OS_AUDIO_51, true);
        return failures ? 1 : 0;
    }
    if (!strcmp(argv[1], "--silent-gain")) {
        test_gain(48000, OS_AUDIO_STEREO);
        test_gain(44100, OS_AUDIO_51);
        printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (!strcmp(argv[1], "--silent-timeline")) {
        capture_timeline(48000, OS_AUDIO_STEREO);
        capture_timeline(44100, OS_AUDIO_51);
        printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
        return failures ? 1 : 0;
    }
    int error = 0;
    CHECK(os_audio_output_open(0, OS_AUDIO_STEREO, &error) == nullptr && error == -1);
    CHECK(os_audio_output_open(48000, static_cast<OsAudioLayout>(3), &error) == nullptr);
    OsAudioClock invalid{};
    CHECK(os_audio_output_clock(nullptr, &invalid) == -1 && !invalid.valid);
    os_audio_output_close(nullptr);
    test_output(48000, OS_AUDIO_STEREO);
    test_output(44100, OS_AUDIO_51);
    printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
    return failures ? 1 : 0;
}
