// Direct USA media decoder + real device clock, silence only. Frame delivery
// is measured here, not on-screen presentation or physical speaker routing.
#include "../timed_player.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static unsigned checks = 0, failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "--silent") && strcmp(argv[1], "--silent-once") &&
                      strcmp(argv[1], "--silent-pcm") && strcmp(argv[1], "--silent-pcm-once"))) {
        fprintf(stderr, "usage: timed_player_probe --silent[-pcm][-once] source delivery.csv\n");
        return 2;
    }
    const bool once = strstr(argv[1], "once") != nullptr;
    const bool preserve = strstr(argv[1], "pcm") != nullptr;
    mf::TimedPlayer player;
    std::string why;
    if (!player.open(argv[2], true, &why,
                     preserve ? mf::TimedPlayer::AudioTiming::PreserveDecodedSamples
                              : mf::TimedPlayer::AudioTiming::StrictTimestampEdits)) {
        fprintf(stderr, "open failed: %s\n", why.c_str());
        return 1;
    }
    FILE *csv = fopen(argv[3], "wx");
    if (!csv)
        return 2;
    fputs("epoch,pts,duration,clock,late,width,height,decode_errors,consumed,rebases\n", csv);
    fflush(csv);
    unsigned epoch = 1;
    double first_pts[2]{-1, -1};
    uint32_t first_pixel[2]{};
    auto deliver = [&](mf::VideoFrame &&frame) {
        if (first_pts[epoch - 1] < 0) {
            first_pts[epoch - 1] = frame.pts;
            first_pixel[epoch - 1] = frame.argb[frame.argb.size() / 2];
        }
        fprintf(csv, "%u,%.9f,%.9f,%.9f,%.9f,%d,%d,%u,%.9f,%llu\n", epoch, frame.pts,
                frame.duration, player.stats().time, player.stats().time - frame.pts, frame.width,
                frame.height, frame.decode_errors, player.stats().consumed_time,
                (unsigned long long)player.stats().clock_rebases);
        if (!(player.stats().video_delivered % 30))
            fflush(csv);
    };
    const uint64_t deadline = os_monotonic_ns() + 240000000000ull;
    bool did_pause = false, did_reset = false;
    while (os_monotonic_ns() < deadline) {
        auto state = player.step(deliver);
        if (state == mf::TimedPlayer::Status::Error) {
            fprintf(stderr, "player error: %s\n", player.error().c_str());
            break;
        }
        if (!once && !did_pause && player.stats().time >= .4) {
            CHECK(player.pause(true));
            const auto frozen = player.stats();
            for (unsigned i = 0; i < 20; ++i) {
                CHECK(player.step(deliver) == mf::TimedPlayer::Status::Paused);
                os_sleep_us(10000);
            }
            CHECK(player.stats().time == frozen.time);
            CHECK(player.stats().video_delivered == frozen.video_delivered);
            CHECK(player.pause(false));
            did_pause = true;
        }
        if (!once && !did_reset && player.stats().time >= .8) {
            CHECK(player.reset());
            CHECK(player.stats().time == 0 && player.stats().video_delivered == 0);
            epoch = 2;
            did_reset = true;
        }
        if (state == mf::TimedPlayer::Status::Ended)
            break;
        os_sleep_us(2000);
    }
    fclose(csv);
    const auto stats = player.stats();
    CHECK(player.status() == mf::TimedPlayer::Status::Ended);
    if (!once) {
        CHECK(did_pause && did_reset);
        CHECK(first_pts[0] >= 0 && first_pts[0] == first_pts[1]);
        CHECK(first_pixel[0] == first_pixel[1]);
    }
    // Every native timeline break must have been re-anchored, never ignored.
    CHECK(stats.native_discontinuities == stats.clock_rebases && stats.peak_video_queue <= 8);
    CHECK(stats.video_delivered > 1 || stats.audio_source_frames > 1);
    if (preserve) {
        CHECK(stats.audio_source_frames == stats.audio_submitted_frames);
        CHECK(stats.source_pcm_hash == stats.submitted_pcm_hash);
        CHECK(stats.audio_overlap_frames == 0);
    }
    player.close();
    CHECK(player.status() == mf::TimedPlayer::Status::Closed);
    printf("{\"checks\":%u,\"failures\":%u,\"video_delivered\":%llu,\"video_late\":%llu,"
           "\"decode_errors\":%llu,\"max_late_seconds\":%.9f,\"time\":%.9f,"
           "\"source_audio_frames\":%llu,\"silence_frames\":%llu,\"overlap_frames\":%llu,"
           "\"timestamp_adjustments\":%llu,\"rate\":%u,\"channels\":%u,"
           "\"submitted_audio_frames\":%llu,\"preroll_frames\":%llu,\"audio_origin\":%.9f,"
           "\"max_audio_anchor_residual\":%.9f,\"native_discontinuities\":%llu",
           checks, failures, (unsigned long long)stats.video_delivered,
           (unsigned long long)stats.video_late, (unsigned long long)stats.video_decode_errors,
           stats.max_video_lateness, stats.time, (unsigned long long)stats.audio_source_frames,
           (unsigned long long)stats.audio_silence_frames,
           (unsigned long long)stats.audio_overlap_frames,
           (unsigned long long)stats.audio_timestamp_adjustments, stats.rate, stats.channels,
           (unsigned long long)stats.audio_submitted_frames,
           (unsigned long long)stats.audio_preroll_frames, stats.audio_origin,
           stats.max_audio_anchor_residual, (unsigned long long)stats.native_discontinuities);
    printf(",\"clock_rebases\":%llu,\"clock_resyncs\":%llu,\"max_clock_resync\":%.6f",
           (unsigned long long)stats.clock_rebases, (unsigned long long)stats.clock_resyncs,
           stats.max_clock_resync);
    printf(",\"native_clock_corrections\":%llu,\"max_native_clock_correction\":%.9f",
           (unsigned long long)stats.native_clock_corrections, stats.max_native_clock_correction);
    printf(",\"return_lead_ms\":%.3f,\"lead_source\":%d,\"route_notes\":%u",
           stats.return_lead * 1000, stats.lead_source, stats.route_notes);
    if (stats.route_notes)
        printf(
            ",\"start_route\":{\"name\":\"%s\",\"uid\":\"%s\",\"default\":%d,\"device_rate\":%.0f,"
            "\"device_channels\":%u,\"nominal\":%.0f,\"io\":%u,\"latency\":%u}",
            stats.routes[0].route.name, stats.routes[0].route.uid,
            stats.routes[0].route.follows_default, stats.routes[0].route.device_rate,
            stats.routes[0].route.device_channels, stats.routes[0].route.nominal_rate,
            stats.routes[0].route.io_frames, stats.routes[0].route.latency_frames);
    printf(",\"source_hashes\":[");
    for (unsigned c = 0; c < stats.channels; ++c)
        printf("%s\"%08x\"", c ? "," : "", stats.source_pcm_hash[c]);
    printf("],\"payload_hashes_before_probe_mute\":[");
    for (unsigned c = 0; c < stats.channels; ++c)
        printf("%s\"%08x\"", c ? "," : "", stats.submitted_pcm_hash[c]);
    printf("]}\n");
    return failures ? 1 : 0;
}
