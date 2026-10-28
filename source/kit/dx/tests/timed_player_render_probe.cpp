// Independent rendered-audio timing through the production TimedPlayer and
// os_audio path. A test-only post-effects tap (os_audio_render_probe_install)
// records which submitted frames CoreAudio actually renders, and when, then
// renders silence; the player's clock and video delivery times are recorded
// alongside. Optional in-process output changes toggle the default device's
// nominal rate (like a TV/eARC renegotiation) and are always restored.
// Never run by CTest: it changes a system audio setting while it runs.
#include "../timed_player.h"

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
AudioObjectID g_device = kAudioObjectUnknown;
Float64 g_original_rate = 0;
std::atomic<bool> g_changed{false};

bool get_rate(Float64 *rate) {
    const AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = sizeof *rate;
    return !AudioObjectGetPropertyData(g_device, &a, 0, nullptr, &size, rate);
}
bool set_rate(Float64 rate) {
    const AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    return !AudioObjectSetPropertyData(g_device, &a, 0, nullptr, sizeof rate, &rate);
}
UInt32 g_original_io = 0;
std::atomic<bool> g_io_changed{false};
bool set_io_frames(UInt32 frames) {
    const AudioObjectPropertyAddress a{kAudioDevicePropertyBufferFrameSize,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    return !AudioObjectSetPropertyData(g_device, &a, 0, nullptr, sizeof frames, &frames);
}
bool get_io_frames(UInt32 *frames) {
    const AudioObjectPropertyAddress a{kAudioDevicePropertyBufferFrameSize,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = sizeof *frames;
    return !AudioObjectGetPropertyData(g_device, &a, 0, nullptr, &size, frames);
}
void restore_io() {
    if (g_io_changed.exchange(false) && g_original_io)
        set_io_frames(g_original_io);
}
void restore_rate() {
    if (g_changed.exchange(false) && g_original_rate > 0)
        set_rate(g_original_rate);
}
void on_signal(int sig) {
    restore_rate();
    restore_io();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

struct Options {
    std::vector<double> flips; // player times; -1 = at playback start, -2 = while priming
    unsigned hold_ms = 400, cycles = 1;
    double pause_at = -1, pause_ms = 0, flip_in_pause = -1, stop_at = -1;
    bool tap = true, marker = false;
    unsigned marker_period_ms = 1000;
    double io_at = -1;
    unsigned io_frames = 0; // this process's I/O buffer size on the default device
};

struct Sample {
    uint64_t host;
    double time, consumed;
    uint64_t rebases, returned_note;
};
struct Delivery {
    uint64_t host;
    double pts, time;
};
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: timed_player_render_probe movie out-prefix [--flip=t[,t..]] "
                        "[--hold-ms=N] [--cycles=N] [--pause=t:ms] [--flip-in-pause] "
                        "[--stop=t] [--no-tap | --marker]\n");
        return 2;
    }
    Options opt;
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        if (!strncmp(a, "--flip=", 7)) {
            for (const char *p = a + 7; *p;) {
                char *end = nullptr;
                const double at = strtod(p, &end);
                if (end == p || (*end && *end != ',')) {
                    fprintf(stderr, "bad --flip list\n");
                    return 2;
                }
                opt.flips.push_back(at);
                p = *end ? end + 1 : end;
            }
        } else if (!strncmp(a, "--hold-ms=", 10)) {
            opt.hold_ms = unsigned(atoi(a + 10));
        } else if (!strncmp(a, "--cycles=", 9)) {
            opt.cycles = unsigned(atoi(a + 9));
        } else if (!strncmp(a, "--pause=", 8)) {
            opt.pause_at = strtod(a + 8, nullptr);
            if (const char *c = strchr(a, ':'))
                opt.pause_ms = strtod(c + 1, nullptr);
        } else if (!strcmp(a, "--flip-in-pause")) {
            opt.flip_in_pause = 1;
        } else if (!strcmp(a, "--marker")) {
            opt.tap = false; // production pipeline, metered markers at volume 0
            opt.marker = true;
        } else if (!strncmp(a, "--marker-period=", 16)) {
            opt.marker_period_ms = unsigned(atoi(a + 16));
        } else if (!strncmp(a, "--io-frames=", 12)) {
            // t:frames - change THIS process's I/O buffer size (a per-client HAL
            // setting) at player time t, restored at exit: a new route period.
            opt.io_at = strtod(a + 12, nullptr);
            if (const char *c = strchr(a, ':'))
                opt.io_frames = unsigned(atoi(c + 1));
        } else if (!strcmp(a, "--no-tap")) {
            opt.tap = false; // production queue pipeline; no rendered-audio records
        } else if (!strncmp(a, "--stop=", 7)) {
            opt.stop_at = strtod(a + 7, nullptr);
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    const AudioObjectPropertyAddress def{kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
    UInt32 size = sizeof g_device;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, nullptr, &size, &g_device) ||
        !get_rate(&g_original_rate)) {
        fprintf(stderr, "no default output device\n");
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::atexit(restore_rate);
    std::atexit(restore_io);
    get_io_frames(&g_original_io);
    bool io_done = false;
    // Main-thread events (negative "rate" = new I/O frame count), merged at the end.
    std::vector<std::pair<uint64_t, double>> io_log;
    const Float64 other = g_original_rate == 48000.0 ? 44100.0 : 48000.0;
    // One change request at a time; the worker does set/hold/restore cycles.
    std::atomic<int> requests{0}, changes_done{0};
    std::atomic<bool> quit{false};
    std::vector<std::pair<uint64_t, double>> change_log; // host, rate (worker only)
    change_log.reserve(64);
    std::thread worker([&] {
        while (!quit.load()) {
            if (requests.load() > changes_done.load()) {
                for (unsigned c = 0; c < opt.cycles; ++c) {
                    g_changed = true;
                    set_rate(other);
                    change_log.push_back({os_monotonic_ns(), other});
                    os_sleep_us(uint64_t(opt.hold_ms) * 1000);
                    set_rate(g_original_rate);
                    g_changed = false;
                    change_log.push_back({os_monotonic_ns(), g_original_rate});
                    if (c + 1 < opt.cycles)
                        os_sleep_us(uint64_t(opt.hold_ms) * 1000);
                }
                ++changes_done;
            }
            os_sleep_us(1000);
        }
    });

    if ((opt.tap && os_audio_render_probe_install(1u << 20)) ||
        (opt.marker && os_audio_marker_install(1u << 20, opt.marker_period_ms))) {
        fprintf(stderr, "render probe unavailable\n");
        quit = true;
        worker.join();
        return 1;
    }
    mf::TimedPlayer player;
    std::string why;
    if (!player.open(argv[1], true, &why, mf::TimedPlayer::AudioTiming::PreserveDecodedSamples)) {
        fprintf(stderr, "open failed: %s\n", why.c_str());
        quit = true;
        worker.join();
        return 1;
    }
    std::vector<Sample> samples;
    std::vector<Delivery> deliveries;
    samples.reserve(200000);
    deliveries.reserve(20000);
    auto deliver = [&](mf::VideoFrame &&frame) {
        deliveries.push_back({os_monotonic_ns(), frame.pts, player.stats().time});
    };
    size_t next_flip = 0;
    bool paused_done = false, stopped = false;
    const uint64_t deadline = os_monotonic_ns() + 300000000000ull;
    mf::TimedPlayer::Status state = player.status();
    while (os_monotonic_ns() < deadline) {
        state = player.step(deliver);
        const auto &st = player.stats();
        samples.push_back(
            {os_monotonic_ns(), st.time, st.consumed_time, st.clock_rebases, st.route_notes});
        if (state == mf::TimedPlayer::Status::Error || state == mf::TimedPlayer::Status::Ended)
            break;
        const bool playing = state == mf::TimedPlayer::Status::Playing;
        // -1: as soon as playback starts; -2: already while the player primes.
        const double flip_at = next_flip < opt.flips.size() ? opt.flips[next_flip] : 1e9;
        if ((flip_at <= -2) || (playing && (flip_at < 0 || st.time >= flip_at))) {
            ++requests;
            ++next_flip;
        }
        if (playing && !io_done && opt.io_at >= 0 && opt.io_frames && st.time >= opt.io_at) {
            io_done = true;
            g_io_changed = true;
            const bool ok = set_io_frames(UInt32(opt.io_frames));
            io_log.push_back({os_monotonic_ns(), ok ? -double(opt.io_frames) : -1.0});
        }
        if (playing && !paused_done && opt.pause_at >= 0 && st.time >= opt.pause_at) {
            paused_done = true;
            player.pause(true);
            if (opt.flip_in_pause > 0)
                ++requests;
            const uint64_t until = os_monotonic_ns() + uint64_t(opt.pause_ms * 1e6);
            while (os_monotonic_ns() < until) {
                player.step(deliver);
                os_sleep_us(2000);
            }
            while (requests.load() > changes_done.load())
                os_sleep_us(1000);
            player.pause(false);
        }
        if (opt.stop_at >= 0 && st.time >= opt.stop_at) {
            stopped = true;
            break;
        }
        os_sleep_us(2000);
    }
    const auto stats = player.stats();
    const std::string error = player.error();
    const auto final_state = state;
    player.close();
    while (requests.load() > changes_done.load())
        os_sleep_us(1000);
    quit = true;
    worker.join();
    restore_rate();
    restore_io();
    // A nominal-rate change completes asynchronously; wait up to 3 s for it.
    Float64 now_rate = 0;
    for (unsigned i = 0; i < 300; ++i) {
        get_rate(&now_rate);
        if (now_rate == g_original_rate)
            break;
        if (i == 150)
            set_rate(g_original_rate);
        os_sleep_us(10000);
    }

    const std::string prefix = argv[2];
    std::vector<OsAudioRenderRecord> records(1u << 20);
    const size_t total = os_audio_render_probe_read(records.data(), records.size());
    records.resize(std::min(total, records.size()));
    if (FILE *f = fopen((prefix + "-render.csv").c_str(), "wx")) {
        fputs("host_ns,stamp_host_ns,first,frames,output,sample_time\n", f);
        for (const auto &r : records)
            fprintf(f, "%llu,%llu,%llu,%u,%u,%.1f\n", (unsigned long long)r.host_ns,
                    (unsigned long long)r.stamp_host_ns, (unsigned long long)r.first, r.frames,
                    r.output, r.sample_time);
        fclose(f);
    }
    if (opt.marker) {
        std::vector<OsAudioMarkerEdge> edges(1u << 20);
        const size_t count = os_audio_marker_read(edges.data(), edges.size());
        edges.resize(std::min(count, edges.size()));
        if (FILE *f = fopen((prefix + "-markers.csv").c_str(), "wx")) {
            fputs("host_ns,sample_time,output,mask,level,edge,l0,l1,l2,l3,l4,l5\n", f);
            for (const auto &e : edges)
                fprintf(f, "%llu,%.1f,%u,%u,%.6f,%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
                        (unsigned long long)e.host_ns, e.sample_time, e.output, e.channel_mask,
                        e.level, e.edge, e.levels[0], e.levels[1], e.levels[2], e.levels[3],
                        e.levels[4], e.levels[5]);
            fclose(f);
        }
    }
    if (FILE *f = fopen((prefix + "-video.csv").c_str(), "wx")) {
        fputs("host_ns,pts,player_time\n", f);
        for (const auto &d : deliveries)
            fprintf(f, "%llu,%.9f,%.9f\n", (unsigned long long)d.host, d.pts, d.time);
        fclose(f);
    }
    if (FILE *f = fopen((prefix + "-clock.csv").c_str(), "wx")) {
        fputs("host_ns,player_time,consumed,rebases\n", f);
        for (const auto &s : samples)
            fprintf(f, "%llu,%.9f,%.9f,%llu\n", (unsigned long long)s.host, s.time, s.consumed,
                    (unsigned long long)s.rebases);
        fclose(f);
    }
    if (FILE *f = fopen((prefix + "-changes.csv").c_str(), "wx")) {
        fputs("host_ns,rate\n", f);
        change_log.insert(change_log.end(), io_log.begin(), io_log.end());
        std::sort(change_log.begin(), change_log.end());
        for (const auto &c : change_log)
            fprintf(f, "%llu,%.0f\n", (unsigned long long)c.first, c.second);
        fclose(f);
    }
    static const char *const status_names[] = {"closed",   "priming", "playing", "paused",
                                               "draining", "ended",   "error"};
    printf("{\"marker_period_ms\":%u,\"io_frames_original\":%u,",
           opt.marker ? opt.marker_period_ms : 0, unsigned(g_original_io));
    printf("\"state\":\"%s\",\"stopped\":%s,\"error\":\"%s\",\"rate\":%u,\"channels\":%u,"
           "\"time\":%.6f,\"video_delivered\":%llu,\"video_late\":%llu,\"max_late\":%.6f,"
           "\"submitted\":%llu,\"source\":%llu,\"native_discontinuities\":%llu,"
           "\"clock_rebases\":%llu,\"clock_resyncs\":%llu,\"max_clock_resync\":%.6f,"
           "\"return_lead_ms\":%.3f,\"lead_source\":%d,\"render_records\":%zu,"
           "\"device_rate_before\":%.0f,\"device_rate_after\":%.0f,\"changes\":%zu,\"routes\":[",
           status_names[int(final_state)], stopped ? "true" : "false", error.c_str(), stats.rate,
           stats.channels, stats.time, (unsigned long long)stats.video_delivered,
           (unsigned long long)stats.video_late, stats.max_video_lateness,
           (unsigned long long)stats.audio_submitted_frames,
           (unsigned long long)stats.audio_source_frames,
           (unsigned long long)stats.native_discontinuities,
           (unsigned long long)stats.clock_rebases, (unsigned long long)stats.clock_resyncs,
           stats.max_clock_resync, stats.return_lead * 1000, stats.lead_source, total,
           g_original_rate, now_rate, change_log.size());
    for (uint32_t i = 0; i < stats.route_notes; ++i) {
        const auto &n = stats.routes[i];
        printf("%s{\"media\":%.3f,\"breaks\":%llu,\"status\":%d,\"name\":\"%s\","
               "\"uid\":\"%s\",\"follows_default\":%d,\"device_rate\":%.0f,\"device_channels\":%u,"
               "\"nominal\":%.0f,\"io\":%u,\"latency\":%u,\"pipeline\":%u,\"lead_ms\":%.3f,"
               "\"lead_source\":%d}",
               i ? "," : "", n.media, (unsigned long long)n.discontinuities, n.status, n.route.name,
               n.route.uid, n.route.follows_default, n.route.device_rate, n.route.device_channels,
               n.route.nominal_rate, n.route.io_frames, n.route.latency_frames,
               n.route.pipeline_frames, n.lead * 1000, n.lead_source);
    }
    printf("]}\n");
    return now_rate == g_original_rate ? 0 : 3;
}
