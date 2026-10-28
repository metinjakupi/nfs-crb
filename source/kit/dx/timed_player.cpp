#include "timed_player.h"
#include <algorithm>
#include <cmath>

namespace mf {
TimedPlayer::~TimedPlayer() {
    close();
}

void TimedPlayer::close() {
    os_audio_output_close(output_);
    output_ = nullptr;
    media_.close();
    audio_.reset();
    preroll_.clear();
    audio_anchors_.clear();
    videos_.clear();
    status_ = Status::Closed;
}

bool TimedPlayer::reject(const std::string &why) {
    error_ = why;
    status_ = Status::Error;
    // A failed/discontinuous output clock must not leave queued movie sound
    // playing while the video is frozen or silently fall back to a fake clock.
    os_audio_output_close(output_);
    output_ = nullptr;
    return false;
}

bool TimedPlayer::open(const std::string &path, bool silence, std::string *why, AudioTiming timing,
                       VideoColorSelector color_selector) {
    close();
    path_ = path;
    silence_ = silence;
    error_.clear();
    stats_ = {};
    presentation_clock_ = {};
    stats_.source_pcm_hash.fill(2166136261u);
    stats_.submitted_pcm_hash.fill(2166136261u);
    audio_timing_ = timing;
    color_selector_ = color_selector;
    audio_origin_known_ = false;
    preroll_samples_ = preroll_bytes_ = 0;
    started_ = audio_tail_ = false;
    audio_offset_ = 0;
    gap_frames_ = scheduled_ = host_anchor_ = pause_host_ = 0;
    clock_progress_host_ = 0;
    clock_progress_sample_ = -1;
    handled_discontinuities_ = 0;
    rebase_pending_ = rebased_ = lead_known_ = false;
    rebase_media_ = rebase_sample_ = return_lead_ = anchor_sample_ = return_step_ = 0;
    lead_period_ = 0;
    period_checked_host_ = 0;
    last_consumed_ = anchor_consumed_ = -1;
    media_anchor_ = video_end_ = 0;
    previous_video_pts_ = -1;
    if (!media_.open(path, {4, 32}, &error_, color_selector_)) {
        if (why)
            *why = error_;
        return reject(error_);
    }
    has_audio_ = media_.has_audio();
    status_ = Status::Priming;
    return true;
}

bool TimedPlayer::reset() {
    // Reopen also resets native buffer callbacks and codec priming. No sound or
    // timestamp from the previous loop is allowed into the new media epoch.
    const std::string path = path_;
    return open(path, silence_, nullptr, audio_timing_, color_selector_);
}

bool TimedPlayer::pause(bool paused) {
    if (status_ == Status::Closed || status_ == Status::Ended || status_ == Status::Error)
        return false;
    if (paused == (status_ == Status::Paused))
        return true;
    if (paused) {
        if (output_ && started_ && os_audio_output_pause(output_))
            return reject("movie audio device could not pause");
        pause_host_ = os_monotonic_ns();
        before_pause_ = status_;
        status_ = Status::Paused;
    } else {
        if (output_ && started_ && !audio_tail_ && os_audio_output_start(output_))
            return reject("movie audio device could not resume");
        if (host_anchor_)
            host_anchor_ += os_monotonic_ns() - pause_host_;
        clock_progress_host_ = os_monotonic_ns();
        status_ = before_pause_;
    }
    media_.pause(paused);
    return true;
}

bool TimedPlayer::set_gain(float gain) {
    if (!std::isfinite(gain) || gain < 0 || gain > 1)
        return false;
    if (output_ && os_audio_output_set_gain(output_, gain))
        return reject("movie audio device could not set volume");
    gain_ = gain;
    return true;
}

// WMA's early packet timestamps may describe codec priming rather than the
// first returned PCM sample. Buffer a small, bounded preroll and compare up to
// three explicit decoded-frame anchors with cumulative samples. A median avoids
// a single initial partial-frame timestamp dominating the whole soundtrack.
// This is an experimental reconstruction: retain residual diagnostics, never
// pretend the packet-derived timestamps themselves were changed at the decoder.
bool TimedPlayer::finish_audio_preroll() {
    if (audio_anchors_.empty())
        return reject("movie audio has no decoded-frame timestamp within bounded preroll");
    auto anchors = audio_anchors_;
    std::sort(anchors.begin(), anchors.end());
    double origin = anchors[anchors.size() / 2];
    if (origin < -.001 || !std::isfinite(origin))
        return reject("movie audio priming implies a negative timeline origin");
    origin = std::max(0., origin);
    stats_.audio_origin = std::round(origin * stats_.rate) / stats_.rate;
    stats_.audio_preroll_frames = preroll_.size();
    audio_origin_known_ = true;
    return true;
}

bool TimedPlayer::prepare_audio(AudioFrame &&frame) {
    if (!std::isfinite(frame.pts) || frame.pts < 0 || frame.pts > 24 * 3600 || frame.rate < 8000 ||
        frame.rate > 192000 || (frame.layout.size() != 2 && frame.layout.size() != 6))
        return reject("movie has unsupported audio timing or layout");
    // WAVE 5.1(back) carries the same two surround roles as a 5.1 speaker bed.
    // This explicit policy maps BL/BR to Ls/Rs; stereo is never upmixed to 5.1.
    bool found[6]{};
    for (unsigned c = 0; c < frame.layout.size(); ++c) {
        unsigned role = 6;
        switch (frame.layout[c]) {
        case AudioChannel::Left:
            role = 0;
            break;
        case AudioChannel::Right:
            role = 1;
            break;
        case AudioChannel::Center:
            role = 2;
            break;
        case AudioChannel::Lfe:
            role = 3;
            break;
        case AudioChannel::BackLeft:
        case AudioChannel::SideLeft:
            role = 4;
            break;
        case AudioChannel::BackRight:
        case AudioChannel::SideRight:
            role = 5;
            break;
        }
        if (role >= frame.layout.size() || found[role])
            return reject("movie audio speaker layout has missing or duplicate roles");
        found[role] = true;
        map_[role] = c;
    }
    if (!output_) {
        int error = 0;
        // The presentation policy at open (set_output_policy, from the
        // game's Audio Mode): Mono/Stereo fold a 5.1 movie like the game's own
        // mix; discrete 5.1 applies the endpoint's verified centre/LFE exchange.
        const bool six = frame.layout.size() == 6, mono = policy_mode_ == 1;
        const bool fold = six && (mono || policy_mode_ == 2);
        if (fold || (!six && mono))
            stats_.presentation = mono ? 2u : 1u;
        else if (six && policy_swap_)
            stats_.presentation = 3u;
        else
            stats_.presentation = 0u;
        out_channels_ = six && !fold ? 6u : 2u;
        stats_.output_channels = out_channels_;
        output_ = os_audio_output_open(uint32_t(frame.rate),
                                       out_channels_ == 6 ? OS_AUDIO_51 : OS_AUDIO_STEREO, &error);
        if (!output_)
            return reject("movie audio device could not open: " + std::to_string(error));
        if (os_audio_output_set_gain(output_, gain_))
            return reject("movie audio device could not initialize volume");
        stats_.rate = uint32_t(frame.rate);
        stats_.channels = uint32_t(frame.layout.size());
    } else if (stats_.rate != uint32_t(frame.rate) || stats_.channels != frame.layout.size()) {
        return reject("movie changes audio format during playback");
    }
    if (audio_timing_ == AudioTiming::PreserveDecodedSamples && !audio_origin_known_) {
        if (preroll_.size() >= 64 ||
            preroll_bytes_ + frame.pcm.size() * sizeof(int16_t) > 8u * 1024u * 1024u)
            return reject("movie audio preroll exceeds its bounded storage");
        if (frame.timestamp_origin == AudioFrame::TimestampOrigin::Frame)
            audio_anchors_.push_back(frame.pts - double(preroll_samples_) / frame.rate);
        preroll_samples_ += frame.samples();
        preroll_bytes_ += frame.pcm.size() * sizeof(int16_t);
        preroll_.push_back(std::move(frame));
        if (audio_anchors_.size() >= 3 || preroll_.size() == 64)
            return finish_audio_preroll();
        return true;
    }
    audio_offset_ = 0;
    if (audio_timing_ == AudioTiming::PreserveDecodedSamples) {
        const double expected =
            stats_.audio_origin + double(stats_.audio_source_frames) / frame.rate;
        if (frame.timestamp_origin == AudioFrame::TimestampOrigin::Frame) {
            const double residual = std::abs(frame.pts - expected);
            stats_.max_audio_anchor_residual = std::max(stats_.max_audio_anchor_residual, residual);
            // Do not flatten an unbounded/genuine discontinuity into continuous
            // PCM. 50ms is a diagnostic admission bound, NOT an A/V-sync pass:
            // corpus residuals and their locations still require review.
            if (residual > .050)
                return reject("movie audio anchor differs by more than 50ms; preserve-PCM policy "
                              "needs review");
        }
        if (!stats_.audio_source_frames)
            gap_frames_ = uint64_t(std::llround(stats_.audio_origin * frame.rate));
    } else {
        const int64_t desired = int64_t(std::llround(frame.pts * frame.rate));
        const int64_t delta = desired - int64_t(scheduled_);
        // Strict diagnostic comparison: quantization tolerance only. This may
        // edit codec-priming/packet artifacts and is not accepted audio fidelity.
        if (std::abs(delta) > frame.rate / 1000) {
            ++stats_.audio_timestamp_adjustments;
            if (delta > 0) {
                gap_frames_ = uint64_t(delta);
            } else {
                audio_offset_ = std::min(size_t(-delta), frame.samples());
                stats_.audio_overlap_frames += audio_offset_;
            }
        }
    }
    stats_.audio_source_frames += frame.samples();
    for (size_t i = 0; i < frame.samples(); ++i)
        for (unsigned c = 0; c < stats_.channels; ++c)
            stats_.source_pcm_hash[c] =
                (stats_.source_pcm_hash[c] ^ uint16_t(frame.pcm[i * stats_.channels + map_[c]])) *
                16777619u;
    audio_ = std::move(frame);
    return true;
}

bool TimedPlayer::submit_audio() {
    if (!audio_)
        return false;
    if (!gap_frames_ && audio_offset_ == audio_->samples()) {
        audio_.reset();
        return true;
    }
    int16_t buffer[2048 * 6]{};
    const auto channels = stats_.channels;
    const unsigned out = out_channels_ ? out_channels_ : channels;
    const uint32_t count = uint32_t(
        std::min<uint64_t>(2048, gap_frames_ ? gap_frames_ : audio_->samples() - audio_offset_));
    if (!gap_frames_ && !silence_) {
        const bool mono = stats_.presentation == 2;
        const auto clamp = [](float v) {
            return int16_t(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
        };
        for (uint32_t i = 0; i < count; ++i) {
            const int16_t *s = &audio_->pcm[(audio_offset_ + i) * channels];
            if (channels == 6 && out == 2) {
                // ITU-R BS.775 fold (LFE omitted), as the game mix is folded.
                const float k = 0.70710678f;
                float l = s[map_[0]] + k * s[map_[2]] + k * s[map_[4]];
                float r = s[map_[1]] + k * s[map_[2]] + k * s[map_[5]];
                if (mono)
                    l = r = 0.5f * (l + r);
                buffer[i * 2] = clamp(l);
                buffer[i * 2 + 1] = clamp(r);
            } else if (channels == 2 && mono) {
                buffer[i * 2] = buffer[i * 2 + 1] = clamp(0.5f * (float(s[map_[0]]) + s[map_[1]]));
            } else {
                const bool swap = stats_.presentation == 3;
                for (unsigned c = 0; c < channels; ++c)
                    buffer[i * channels + c] = s[map_[swap && (c == 2 || c == 3) ? 5 - c : c]];
            }
        }
    }
    const int written = os_audio_output_write(output_, buffer, count);
    if (written < 0)
        return reject("movie audio device rejected samples");
    if (!written)
        return false;
    scheduled_ += unsigned(written);
    if (gap_frames_) {
        gap_frames_ -= unsigned(written);
        stats_.audio_silence_frames += unsigned(written);
    } else {
        // Hash the semantic payload before optional probe-only muting. This
        // exposes omitted/duplicated PCM even when the device receives silence.
        for (int i = 0; i < written; ++i)
            for (unsigned c = 0; c < channels; ++c)
                stats_.submitted_pcm_hash[c] =
                    (stats_.submitted_pcm_hash[c] ^
                     uint16_t(audio_->pcm[(audio_offset_ + i) * channels + map_[c]])) *
                    16777619u;
        stats_.audio_submitted_frames += unsigned(written);
        audio_offset_ += unsigned(written);
    }
    return true;
}

void TimedPlayer::note_route(double media, uint64_t discontinuities) {
    Stats::RouteNote note;
    note.media = media;
    note.discontinuities = discontinuities;
    note.status = os_audio_output_route(output_, &note.route);
    const double device_rate =
        note.route.device_rate > 0 ? note.route.device_rate : note.route.nominal_rate;
    // A return leads presentation by about one device I/O period (Test50: learned
    // 8.6-11.8 ms for a 512-frame/44.1 kHz period, 11.6 ms), plus any pull-ahead the
    // output layer itself adds (its test tap only). 0 when the route is unknown.
    const double period = !note.status && note.route.io_frames && device_rate > 0
                              ? std::min(.1, note.route.io_frames / device_rate)
                              : 0;
    if (!stats_.route_notes) {
        lead_period_ = period; // the route the lead will be learned on
    } else if (!lead_known_) {
        // The timeline broke before any buffer return calibrated the lead.
        return_lead_ = (period > 0 ? period : 512.0 / stats_.rate) +
                       note.route.pipeline_frames / double(stats_.rate);
        stats_.lead_source = Stats::LeadDefault;
        lead_known_ = true;
        lead_period_ = period;
    } else if (period > 0 && lead_period_ > 0 && std::abs(period - lead_period_) > .0005) {
        // New timing epoch: the queue moved to a route with another I/O period
        // (e.g. MacBook -> HDMI). Keep what was learned beyond the period (the
        // caller's observation delay, which the post-break anchor shares) and
        // replace the device part. Device/transport latency is downstream of the
        // queue timeline and is not part of this lead.
        return_lead_ = std::max(0., return_lead_ + period - lead_period_);
        stats_.lead_source = Stats::LeadAdjusted;
        lead_period_ = period;
    }
    stats_.return_lead = return_lead_;
    note.lead = return_lead_;
    note.lead_source = stats_.lead_source;
    if (stats_.route_notes < stats_.routes.size())
        stats_.routes[stats_.route_notes++] = note;
}

bool TimedPlayer::update_clock() {
    const uint64_t now = os_monotonic_ns();
    if (!started_)
        return true; // Decode/prime first; loading time is not played media time.
    if (!has_audio_ || audio_tail_) {
        if (!host_anchor_)
            host_anchor_ = now;
        stats_.time = media_anchor_ + double(now - host_anchor_) * 1e-9;
        return true;
    }
    OsAudioClock clock{};
    os_audio_output_clock(output_, &clock);
    stats_.native_discontinuities = clock.discontinuities;
    if (stats_.rate)
        stats_.consumed_time = double(clock.returned_frames) / stats_.rate;
    if (clock.discontinuities > handled_discontinuities_) {
        // The output route changed under the queue: CoreAudio discards what was
        // queued for the old route and the sample time no longer counts our
        // submitted stream. Re-anchor (see the header) instead of abandoning
        // the movie. Repeated breaks still fail rather than drift unbounded.
        if (clock.discontinuities > max_clock_rebases_)
            return reject("movie audio presentation timeline discontinuity (" +
                          std::to_string(clock.discontinuities) + " route changes)");
        stats_.clock_rebases += clock.discontinuities - handled_discontinuities_;
        handled_discontinuities_ = clock.discontinuities;
        rebase_pending_ = true;
        clock_progress_sample_ = -1;
        clock_progress_host_ = now;
    }
    if (clock.valid && clock.sample_time > clock_progress_sample_) {
        clock_progress_sample_ = clock.sample_time;
        clock_progress_host_ = now;
    } else if (now - clock_progress_host_ > 5000000000ull) {
        return reject("movie audio presentation clock stopped for five seconds");
    }
    if (!clock.valid)
        return true; // Start is asynchronous; keep the first video frame until time is valid
    if (!stats_.route_notes)
        note_route(presentation_clock_.seconds(), clock.discontinuities);
    if (rebase_pending_) {
        rebase_media_ = presentation_clock_.seconds();
        rebase_sample_ = clock.sample_time;
        rebase_pending_ = false;
        rebased_ = true;
        note_route(rebase_media_, clock.discontinuities);
    }
    const double end = double(scheduled_) / stats_.rate;
    double presented = rebased_ ? rebase_media_ + (clock.sample_time - rebase_sample_) / stats_.rate
                                : clock.sample_time / stats_.rate;
    const double consumed = stats_.consumed_time;
    const bool returned_now = last_consumed_ >= 0 && consumed != last_consumed_;
    if (returned_now) {
        // The queue hands buffers back one or several at a time (measured:
        // pairs, 4096 frames every 93 ms at 44.1 kHz). Interpolation below may
        // cover one ordinary return step; larger jumps are discarded audio.
        const double step = consumed - last_consumed_;
        if (step > 0 && step <= kMaxReturnStepFrames / double(stats_.rate))
            return_step_ = std::max(step, 2048.0 / stats_.rate);
    }
    last_consumed_ = consumed;
    if (!rebased_) {
        // Learn how far a buffer return leads presentation at the moment it is
        // observed, during normal playback only.
        if (returned_now) {
            const double lead = consumed - presented;
            return_lead_ = lead_known_ ? return_lead_ + .1 * (lead - return_lead_) : lead;
            lead_known_ = true;
            stats_.lead_source = Stats::LeadLearned;
            stats_.return_lead = return_lead_;
            // The learned lead follows the route's I/O period (Test51: 512 -> 256
            // frames without a break re-learned 11 -> 4.7 ms). Track the period it
            // is being learned on, about once a second, so a later break compares
            // against the right epoch.
            if (now - period_checked_host_ > 1000000000ull) {
                period_checked_host_ = now;
                OsAudioRoute route{};
                if (!os_audio_output_route(output_, &route)) {
                    const double rate =
                        route.device_rate > 0 ? route.device_rate : route.nominal_rate;
                    if (route.io_frames && rate > 0)
                        lead_period_ = std::min(.1, route.io_frames / rate);
                }
            }
        }
    } else if (lead_known_) {
        if (consumed != anchor_consumed_) {
            anchor_consumed_ = consumed;
            anchor_sample_ = clock.sample_time;
        }
        const double step = return_step_ > 0 ? return_step_ : 2048.0 / stats_.rate;
        const double since =
            std::clamp((clock.sample_time - anchor_sample_) / stats_.rate, 0., step);
        double audible = consumed - return_lead_ + since;
        if (clock.returned_frames >= scheduled_ && media_.audio_finished() && !audio_ &&
            preroll_.empty())
            audible = std::max(audible, end); // everything played or was discarded
        const double previous = presentation_clock_.seconds();
        if (audible - previous > .040) {
            // Discarded audio was never heard: skip video forward to match.
            ++stats_.clock_resyncs;
            stats_.max_clock_resync = std::max(stats_.max_clock_resync, audible - previous);
        }
        presented = std::max(previous, audible); // hold rather than rewind
    }
    if (!presentation_clock_.observe(presented))
        return reject(
            "movie audio presentation time moved backwards: current=" + std::to_string(presented) +
            " previous=" + std::to_string(stats_.time) + " queued_end=" + std::to_string(end) +
            " running=" + std::to_string(clock.running));
    stats_.native_clock_corrections = presentation_clock_.corrections();
    stats_.max_native_clock_correction = presentation_clock_.max_correction();
    stats_.time = std::min(presentation_clock_.seconds(), end);
    if (media_.audio_finished() && preroll_.empty() && !audio_ && presented >= end) {
        // Video may outlast the audio stream (the USA PSA does). Waiting for
        // BOTH decoders/queues to empty would deadlock: the final video frames
        // cannot leave their queues until this tail clock starts. Every submitted
        // audio frame has reached the native timeline. Do not synchronously pause
        // the empty queue here: AudioQueuePause can block long enough to miss the
        // first tail frame. Close it at movie completion (or explicit stop).
        audio_tail_ = true;
        media_anchor_ = presented;
        host_anchor_ = now;
    }
    return true;
}

TimedPlayer::Status TimedPlayer::step(const std::function<void(VideoFrame &&)> &deliver) {
    if (status_ == Status::Closed || status_ == Status::Paused || status_ == Status::Error ||
        status_ == Status::Ended)
        return status_;
    if (!update_clock())
        return status_;
    // Bounded work per caller tick. Decoding stays off the audio callback;
    // both media queues are serviced even while one is backpressured.
    for (unsigned work = 0; work < 128; ++work) {
        bool progress = submit_audio();
        if (status_ == Status::Error)
            return status_;
        if (!audio_) {
            AudioFrame frame;
            bool available = false;
            if (audio_origin_known_ && !preroll_.empty()) {
                frame = std::move(preroll_.front());
                preroll_.pop_front();
                available = true;
            } else {
                available = media_.pop_audio(&frame);
            }
            if (available) {
                if (!prepare_audio(std::move(frame)))
                    return status_;
                progress = true;
            } else if (!audio_origin_known_ && !preroll_.empty() && media_.audio_finished()) {
                if (!finish_audio_preroll())
                    return status_;
                progress = true;
            }
        }
        if (videos_.size() < 8) {
            VideoFrame frame;
            if (media_.pop_video(&frame)) {
                if (frame.pts < previous_video_pts_)
                    return reject("movie video timestamps moved backwards"), status_;
                previous_video_pts_ = frame.pts;
                video_end_ = std::max(video_end_, frame.pts + frame.duration);
                videos_.push_back(std::move(frame));
                stats_.peak_video_queue = std::max(stats_.peak_video_queue, videos_.size());
                progress = true;
            }
        }
        const auto result = media_.pump();
        if (result == TimedMedia::Result::Error)
            return reject(media_.error()), status_;
        if (!progress && result != TimedMedia::Result::Progress)
            break;
    }
    if (has_audio_ && !scheduled_ && !audio_ && preroll_.empty() && media_.audio_finished())
        has_audio_ = false; // A declared but empty audio stream has no clock.
    if (!started_ &&
        ((!has_audio_ && (!videos_.empty() || media_.finished())) || scheduled_ >= 2048 ||
         (scheduled_ && !audio_ && preroll_.empty() && media_.audio_finished()))) {
        if (output_ && os_audio_output_start(output_))
            return reject("movie audio device could not start"), status_;
        started_ = true;
        host_anchor_ = os_monotonic_ns();
        clock_progress_host_ = host_anchor_;
        status_ = Status::Playing;
    }
    if (!update_clock())
        return status_;
    while (!videos_.empty() && videos_.front().pts <= stats_.time) {
        auto frame = std::move(videos_.front());
        videos_.pop_front();
        const double late = stats_.time - frame.pts;
        stats_.max_video_lateness = std::max(stats_.max_video_lateness, late);
        if (late > frame.duration)
            ++stats_.video_late;
        if (frame.decode_errors)
            ++stats_.video_decode_errors;
        ++stats_.video_delivered;
        deliver(std::move(frame));
    }
    if (media_.finished() && preroll_.empty() && !audio_) {
        status_ = Status::Draining;
        if (videos_.empty() && stats_.time >= video_end_ && (!has_audio_ || audio_tail_)) {
            os_audio_output_close(output_);
            output_ = nullptr;
            status_ = Status::Ended;
        }
    }
    return status_;
}
} // namespace mf
