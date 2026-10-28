// Isolated timed-media player. One caller owns decoding and output submission;
// the device callback only returns buffers. No guest or GPU calls here.
#pragma once
#include "mf_media.h"
#include "presentation_clock.h"
#include "../platform/os.h"

#include <deque>
#include <array>
#include <functional>
#include <optional>

namespace mf {
class TimedPlayer {
  public:
    enum class Status { Closed, Priming, Playing, Paused, Draining, Ended, Error };
    // Both are diagnostic policies until source A/V comparisons are accepted.
    enum class AudioTiming { StrictTimestampEdits, PreserveDecodedSamples };
    struct Stats {
        uint64_t video_delivered = 0, video_late = 0, video_decode_errors = 0;
        uint64_t audio_source_frames = 0, audio_silence_frames = 0, audio_overlap_frames = 0;
        uint64_t audio_timestamp_adjustments = 0, native_discontinuities = 0;
        // Native timeline breaks survived by re-anchoring (device/route change),
        // and forward resyncs to the audio the device actually kept playing.
        uint64_t clock_rebases = 0, clock_resyncs = 0;
        double max_clock_resync = 0; // seconds of audio CoreAudio dropped, as skipped
        uint64_t audio_submitted_frames = 0, audio_preroll_frames = 0;
        uint64_t native_clock_corrections = 0;
        double max_native_clock_correction = 0;
        double audio_origin = 0, max_audio_anchor_residual = 0;
        std::array<uint32_t, 6> source_pcm_hash{}, submitted_pcm_hash{};
        size_t peak_video_queue = 0;
        double time = 0, max_video_lateness = 0;
        double consumed_time = 0; // audio handed back by the queue, seconds (diagnostic)
        uint32_t rate = 0, channels = 0;
        // What the device queue receives: channels, and how the source was
        // presented (host audio policy at open): 0 as decoded, 1 folded to
        // stereo, 2 folded to mono, 3 discrete with centre/LFE exchanged.
        uint32_t output_channels = 0, presentation = 0;
        // How far a buffer return leads presentation, and where that estimate
        // came from. Used only after a timeline break (see update_clock).
        enum LeadSource { LeadNone, LeadLearned, LeadDefault, LeadAdjusted };
        double return_lead = 0;
        int lead_source = LeadNone;
        // The queue's actual device when playback started (index 0) and after
        // each handled timeline break. Diagnostics for route/format evidence.
        struct RouteNote {
            double media = 0; // player time when observed
            uint64_t discontinuities = 0;
            double lead = 0; // return lead in use after this note
            int lead_source = LeadNone;
            int status = -1; // os_audio_output_route result
            OsAudioRoute route{};
        };
        std::array<RouteNote, 9> routes{};
        uint32_t route_notes = 0;
    };
    ~TimedPlayer();
    TimedPlayer() = default;
    TimedPlayer(const TimedPlayer &) = delete;
    TimedPlayer &operator=(const TimedPlayer &) = delete;
    // silence is only for unattended clock/decoder probes. Real playback keeps
    // all decoded channels. Each delivered frame includes its source PTS.
    bool open(const std::string &path, bool silence, std::string *why = nullptr,
              AudioTiming timing = AudioTiming::StrictTimestampEdits,
              VideoColorSelector color_selector = nullptr);
    void close();
    // How the next opened movie's audio is presented (values of
    // POP_AUDIO_OUTPUT_*: 0 as decoded, 1 mono, 2 stereo, 3 discrete 5.1),
    // and whether discrete 5.1 exchanges centre and LFE for its endpoint.
    // Set by the host before open(); probes leave it as decoded.
    void set_output_policy(uint32_t mode, bool swap_center_lfe) {
        policy_mode_ = mode;
        policy_swap_ = swap_center_lfe;
    }
    bool reset();
    bool pause(bool paused);
    // Set before the first step or during playback. Preserved through reset;
    // applies to the device queue so already-enqueued samples obey mute too.
    bool set_gain(float gain);
    Status step(const std::function<void(VideoFrame &&)> &deliver);
    Status status() const {
        return status_;
    }
    const Stats &stats() const {
        return stats_;
    }
    const std::string &error() const {
        return error_;
    }

  private:
    bool reject(const std::string &why);
    bool prepare_audio(AudioFrame &&frame);
    bool finish_audio_preroll();
    bool submit_audio();
    bool update_clock();
    void note_route(double media, uint64_t discontinuities);
    TimedMedia media_;
    VideoColorSelector color_selector_ = nullptr;
    PresentationClock presentation_clock_;
    OsAudioOutput *output_ = nullptr;
    Status status_ = Status::Closed, before_pause_ = Status::Closed;
    Stats stats_;
    std::string path_, error_;
    std::deque<VideoFrame> videos_;     // at most eight; decoder has another four
    std::deque<AudioFrame> preroll_;    // at most64 frames / 8MiB before a reliable origin
    std::vector<double> audio_anchors_; // at most three decoded-frame anchors
    uint64_t preroll_samples_ = 0;
    size_t preroll_bytes_ = 0;
    AudioTiming audio_timing_ = AudioTiming::StrictTimestampEdits;
    bool audio_origin_known_ = false;
    std::optional<AudioFrame> audio_;
    size_t audio_offset_ = 0;
    uint64_t gap_frames_ = 0, scheduled_ = 0;
    uint64_t host_anchor_ = 0, pause_host_ = 0, clock_progress_host_ = 0;
    double clock_progress_sample_ = -1;
    // A route/device change (TV wake, eARC renegotiation, output switch)
    // breaks the AudioQueue timeline; CoreAudio then discards audio queued for
    // the old route and renders its own silence while the native sample time
    // keeps advancing (measured with a render tap: work/claude-bridge/evidence/
    // 006-007). Returned-buffer frames stay continuous and jump by the discarded
    // amount. After a break, media time is therefore anchored to each buffer
    // return (minus the return lead learned during normal playback) and only
    // interpolated by sample time for at most one ordinary return step.
    static constexpr uint64_t max_clock_rebases_ = 8;
    uint64_t handled_discontinuities_ = 0;
    bool rebase_pending_ = false, rebased_ = false, lead_known_ = false;
    double rebase_media_ = 0, rebase_sample_ = 0, return_lead_ = 0;
    double last_consumed_ = -1, anchor_consumed_ = -1, anchor_sample_ = 0;
    // Interpolation past the latest return is bounded by the ordinary return
    // step (one to four buffers); anything larger is audio discarded by a break.
    static constexpr double kMaxReturnStepFrames = 8192;
    double return_step_ = 0;
    // Device I/O period of the route the lead belongs to (0 unknown), refreshed
    // about once a second while the lead is being learned.
    double lead_period_ = 0;
    uint64_t period_checked_host_ = 0;
    double media_anchor_ = 0, video_end_ = 0, previous_video_pts_ = -1;
    bool silence_ = false, has_audio_ = false, started_ = false, audio_tail_ = false;
    float gain_ = 1;
    unsigned map_[6]{};
    unsigned out_channels_ = 0; // channels written to the device queue
    uint32_t policy_mode_ = 0;
    bool policy_swap_ = false;
};
} // namespace mf
