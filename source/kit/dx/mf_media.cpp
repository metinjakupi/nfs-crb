// FFmpeg behind mf_media.h. The decoders are the ones cmake/Dependencies.cmake
// enables: Windows Media video and audio for the game's movies, MP3 for its
// music. A build without FFmpeg refuses to open anything, which the Media
// Foundation objects above report as an unsupported byte stream.
#include "mf_media.h"

#include "video_frame.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>

#ifdef RECOMP_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace mf {

const char *audio_channel_name(AudioChannel channel) {
    switch (channel) {
    case AudioChannel::Left:
        return "L";
    case AudioChannel::Right:
        return "R";
    case AudioChannel::Center:
        return "C";
    case AudioChannel::Lfe:
        return "LFE";
    case AudioChannel::BackLeft:
        return "Lb";
    case AudioChannel::BackRight:
        return "Rb";
    case AudioChannel::SideLeft:
        return "Ls";
    case AudioChannel::SideRight:
        return "Rs";
    }
    return "unknown";
}

#ifdef RECOMP_HAVE_FFMPEG

struct Media::State {
    AVFormatContext *input = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVFrame *frame = nullptr, *audio_frame = nullptr;
    AVPacket *packet = nullptr;
    int video_index = -1, audio_index = -1;
    std::vector<int16_t> pcm;
    bool eof = false, drained_audio = false;

    ~State() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&audio_frame);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        avformat_close_input(&input);
    }
};

namespace {

bool fail(std::string *why, const char *what) {
    if (why)
        *why = what;
    return false;
}

bool open_stream(AVFormatContext *input, int index, AVCodecContext **out, bool timed = false) {
    const AVCodec *codec = avcodec_find_decoder(input->streams[index]->codecpar->codec_id);
    if (!codec)
        return false;
    *out = avcodec_alloc_context3(codec);
    if (!*out)
        return false;
    if (avcodec_parameters_to_context(*out, input->streams[index]->codecpar) < 0)
        return false;
    if (timed)
        (*out)->pkt_timebase = input->streams[index]->time_base;
    return avcodec_open2(*out, codec, nullptr) >= 0;
}

// Float or planar float, the formats the Windows Media and MP3 decoders emit,
// clamped before the conversion so a peak cannot wrap.
void append_samples(std::vector<int16_t> &out, const AVFrame &f) {
    const int channels = f.ch_layout.nb_channels;
    for (int i = 0; i < f.nb_samples; ++i)
        for (int ch = 0; ch < channels; ++ch) {
            double value = 0;
            switch (f.format) {
            case AV_SAMPLE_FMT_FLTP:
                value = reinterpret_cast<const float *>(f.extended_data[ch])[i];
                break;
            case AV_SAMPLE_FMT_FLT:
                value = reinterpret_cast<const float *>(f.extended_data[0])[i * channels + ch];
                break;
            case AV_SAMPLE_FMT_S16P:
                value = reinterpret_cast<const int16_t *>(f.extended_data[ch])[i] / 32768.0;
                break;
            case AV_SAMPLE_FMT_S16:
                value = reinterpret_cast<const int16_t *>(f.extended_data[0])[i * channels + ch] /
                        32768.0;
                break;
            default:
                return; // an unexpected format is silence rather than noise
            }
            if (!std::isfinite(value))
                value = 0;
            const long sample = std::lround(std::clamp(value, -1.0, 1.0) * 32767.0);
            out.push_back(int16_t(std::clamp(sample, -32768L, 32767L)));
        }
}

} // namespace

Media::~Media() {
    close();
}

void Media::close() {
    delete s_;
    s_ = nullptr;
}

bool Media::is_open() const {
    return s_ != nullptr;
}

bool Media::open(const std::string &path, std::string *why) {
    close();
    auto state = new State();
    if (avformat_open_input(&state->input, path.c_str(), nullptr, nullptr) < 0) {
        delete state;
        return fail(why, "the file could not be opened");
    }
    if (avformat_find_stream_info(state->input, nullptr) < 0) {
        delete state;
        return fail(why, "the file has no readable stream information");
    }
    for (unsigned i = 0; i < state->input->nb_streams; ++i) {
        const AVCodecParameters *p = state->input->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && state->video_index < 0)
            state->video_index = int(i);
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO && state->audio_index < 0)
            state->audio_index = int(i);
    }
    if (state->video_index >= 0 && !open_stream(state->input, state->video_index, &state->video))
        state->video_index = -1;
    if (state->audio_index >= 0 && !open_stream(state->input, state->audio_index, &state->audio))
        state->audio_index = -1;
    if (state->video_index < 0 && state->audio_index < 0) {
        delete state;
        return fail(why, "no stream in the file has a decoder");
    }
    state->frame = av_frame_alloc();
    state->audio_frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (!state->frame || !state->audio_frame || !state->packet) {
        delete state;
        return fail(why, "the decoder ran out of memory");
    }
    s_ = state;
    return true;
}

bool Media::has_video() const {
    return s_ && s_->video_index >= 0;
}
bool Media::has_audio() const {
    return s_ && s_->audio_index >= 0;
}
int32_t Media::width() const {
    return has_video() ? s_->video->width : 0;
}
int32_t Media::height() const {
    return has_video() ? s_->video->height : 0;
}
double Media::duration() const {
    if (!s_ || s_->input->duration == AV_NOPTS_VALUE)
        return 0;
    return double(s_->input->duration) / AV_TIME_BASE;
}
int32_t Media::audio_rate() const {
    return has_audio() ? s_->audio->sample_rate : 0;
}
int32_t Media::audio_channels() const {
    return has_audio() ? s_->audio->ch_layout.nb_channels : 0;
}
bool Media::finished() const {
    return !s_ || (s_->eof && s_->pcm.empty());
}

std::vector<int16_t> Media::take_audio() {
    std::vector<int16_t> out;
    if (s_)
        out.swap(s_->pcm);
    return out;
}

namespace {
// Everything the decoder has ready, appended to the queue.
void drain_audio(Media::State &s) {
    while (avcodec_receive_frame(s.audio, s.audio_frame) >= 0) {
        append_samples(s.pcm, *s.audio_frame);
        av_frame_unref(s.audio_frame);
    }
}
} // namespace

// One packet: video packets go to the video decoder, audio straight into the
// queue. At the end both decoders are flushed once.
bool Media::next_video(VideoFrame *out) {
    if (!s_ || !out || s_->video_index < 0)
        return false;
    for (;;) {
        if (avcodec_receive_frame(s_->video, s_->frame) >= 0) {
            const AVFrame &f = *s_->frame;
            if (f.format != AV_PIX_FMT_YUV420P || f.width <= 0 || f.height <= 0) {
                av_frame_unref(s_->frame);
                continue; // an unexpected layout is skipped, not drawn wrong
            }
            out->width = f.width;
            out->height = f.height;
            out->argb.assign(size_t(f.width) * size_t(f.height), 0);
            for (int y = 0; y < f.height; ++y)
                video_frame_convert_row(
                    reinterpret_cast<uint8_t *>(out->argb.data() + size_t(y) * size_t(f.width)),
                    f.data[0] + size_t(y) * f.linesize[0],
                    f.data[1] + size_t(y / 2) * f.linesize[1],
                    f.data[2] + size_t(y / 2) * f.linesize[2], uint32_t(f.width), VIDEO_XRGB8888);
            const AVRational tb = s_->input->streams[s_->video_index]->time_base;
            const int64_t pts =
                f.best_effort_timestamp != AV_NOPTS_VALUE ? f.best_effort_timestamp : f.pts;
            out->pts = pts == AV_NOPTS_VALUE ? 0 : double(pts) * tb.num / tb.den;
            av_frame_unref(s_->frame);
            return true;
        }
        if (s_->eof)
            return false;
        if (av_read_frame(s_->input, s_->packet) < 0) {
            s_->eof = true;
            avcodec_send_packet(s_->video, nullptr);
            if (s_->audio && !s_->drained_audio) {
                s_->drained_audio = true;
                avcodec_send_packet(s_->audio, nullptr);
                drain_audio(*s_);
            }
            continue;
        }
        if (s_->packet->stream_index == s_->video_index)
            avcodec_send_packet(s_->video, s_->packet);
        else if (s_->audio && s_->packet->stream_index == s_->audio_index &&
                 avcodec_send_packet(s_->audio, s_->packet) >= 0)
            drain_audio(*s_);
        av_packet_unref(s_->packet);
    }
}

void Media::fill_audio(size_t samples) {
    if (!s_ || s_->audio_index < 0)
        return;
    while (s_->pcm.size() < samples && !s_->eof) {
        if (av_read_frame(s_->input, s_->packet) < 0) {
            s_->eof = true;
            if (!s_->drained_audio) {
                s_->drained_audio = true;
                avcodec_send_packet(s_->audio, nullptr);
                drain_audio(*s_);
            }
            break;
        }
        if (s_->packet->stream_index == s_->audio_index &&
            avcodec_send_packet(s_->audio, s_->packet) >= 0)
            drain_audio(*s_);
        av_packet_unref(s_->packet);
    }
}

// The new timed path is isolated from Media's legacy audio-only consumers.
// Independent compressed queues let demux get past a video's interleave lead
// to the first audio packet without allocating dozens of decoded full-size
// frames. Count and byte bounds apply; no packet is discarded under pressure.
struct TimedMedia::State {
    AVFormatContext *input = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    int video_index = -1, audio_index = -1;
    Limits limits;
    VideoColorSelector color_selector = nullptr;
    std::deque<VideoFrame> videos;
    std::deque<AudioFrame> audios;
    std::deque<AVPacket *> video_packets, audio_packets;
    size_t packet_bytes = 0;
    static constexpr size_t max_packets = 128, max_packet_bytes = 32 * 1024 * 1024;
    bool paused = false, pending = false, eof = false;
    bool video_flush = false, audio_flush = false;
    bool video_drained = true, audio_drained = true;
    double audio_packet_pts = NAN, audio_anchor_pts = NAN;
    uint64_t audio_anchor_samples = 0;
    int audio_anchor_rate = 0;
    std::string error;
    void clear_packets() {
        for (auto *p : video_packets)
            av_packet_free(&p);
        for (auto *p : audio_packets)
            av_packet_free(&p);
        video_packets.clear();
        audio_packets.clear();
        packet_bytes = 0;
    }
    ~State() {
        clear_packets();
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        avformat_close_input(&input);
    }
    bool reject(const char *message) {
        error = message;
        return false;
    }
};

namespace {
VideoColorDescription timed_color(const AVFrame &frame) {
    using C = VideoColorDescription;
    C out;
    switch (frame.colorspace) {
    case AVCOL_SPC_UNSPECIFIED:
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        out.matrix = C::Matrix::Bt601;
        break;
    case AVCOL_SPC_BT709:
        out.matrix = C::Matrix::Bt709;
        break;
    default:
        out.matrix = C::Matrix::Other;
        break;
    }
    switch (frame.color_range) {
    case AVCOL_RANGE_UNSPECIFIED:
        break;
    case AVCOL_RANGE_MPEG:
        out.range = C::Range::Limited;
        break;
    case AVCOL_RANGE_JPEG:
        out.range = C::Range::Full;
        break;
    default:
        out.range = C::Range::Other;
        break;
    }
    return out;
}
bool timed_pts(TimedMedia::State &s, const AVFrame &frame, int index, double &pts) {
    const int64_t ticks =
        frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    const AVRational tb = s.input->streams[index]->time_base;
    if (ticks == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0)
        return s.reject(index == s.video_index ? "decoded video has no source timestamp"
                                               : "decoded audio has no source timestamp");
    pts = double(ticks) * av_q2d(tb);
    return std::isfinite(pts) || s.reject("decoded timestamp is not finite");
}

bool timed_layout(const AVChannelLayout &source, std::vector<AudioChannel> &out) {
    if (source.nb_channels < 1 || source.nb_channels > 8 || !av_channel_layout_check(&source))
        return false;
    // WAVEFORMATEX/WMA2 uses implicit mono/stereo without a speaker mask.
    // Only those standardized small layouts may omit it. Never infer 5.1
    // (or any other multichannel placement) from a channel count alone.
    if (source.order == AV_CHANNEL_ORDER_UNSPEC && source.nb_channels <= 2) {
        out = source.nb_channels == 1
                  ? std::vector<AudioChannel>{AudioChannel::Center}
                  : std::vector<AudioChannel>{AudioChannel::Left, AudioChannel::Right};
        return true;
    }
    for (int i = 0; i < source.nb_channels; ++i) {
        switch (av_channel_layout_channel_from_index(&source, unsigned(i))) {
        case AV_CHAN_FRONT_LEFT:
            out.push_back(AudioChannel::Left);
            break;
        case AV_CHAN_FRONT_RIGHT:
            out.push_back(AudioChannel::Right);
            break;
        case AV_CHAN_FRONT_CENTER:
            out.push_back(AudioChannel::Center);
            break;
        case AV_CHAN_LOW_FREQUENCY:
            out.push_back(AudioChannel::Lfe);
            break;
        case AV_CHAN_BACK_LEFT:
            out.push_back(AudioChannel::BackLeft);
            break;
        case AV_CHAN_BACK_RIGHT:
            out.push_back(AudioChannel::BackRight);
            break;
        case AV_CHAN_SIDE_LEFT:
            out.push_back(AudioChannel::SideLeft);
            break;
        case AV_CHAN_SIDE_RIGHT:
            out.push_back(AudioChannel::SideRight);
            break;
        default:
            return false; // unspecified channel order must not silently become 5.1
        }
    }
    return true;
}

// WMA Pro packets can produce many frames, most without AVFrame timestamps.
// Use an explicit frame PTS first, otherwise the accepted packet's first-frame
// anchor, then an integer sample offset. A new source anchor is never smoothed
// away: source discontinuities must survive. This is media time, not the audio
// device's presentation clock, and the origin is exposed for sync diagnostics.
bool timed_audio_pts(TimedMedia::State &s, const AVFrame &f, AudioFrame &out) {
    const int64_t ticks =
        f.best_effort_timestamp != AV_NOPTS_VALUE ? f.best_effort_timestamp : f.pts;
    double anchor = s.audio_packet_pts;
    out.timestamp_origin = AudioFrame::TimestampOrigin::Packet;
    if (ticks != AV_NOPTS_VALUE) {
        anchor = double(ticks) * av_q2d(s.input->streams[s.audio_index]->time_base);
        out.timestamp_origin = AudioFrame::TimestampOrigin::Frame;
    }
    s.audio_packet_pts = NAN;
    if (std::isfinite(anchor)) {
        s.audio_anchor_pts = anchor;
        s.audio_anchor_samples = 0;
        s.audio_anchor_rate = f.sample_rate;
    } else {
        out.timestamp_origin = AudioFrame::TimestampOrigin::SampleContinuation;
        if (!std::isfinite(s.audio_anchor_pts) || s.audio_anchor_rate != f.sample_rate)
            return s.reject("audio samples have no valid timestamp anchor");
    }
    out.pts = s.audio_anchor_pts + double(s.audio_anchor_samples) / s.audio_anchor_rate;
    s.audio_anchor_samples += unsigned(f.nb_samples);
    return true;
}

// Receive at most one output from each codec per pump. EAGAIN asks the single
// demux owner for more input; EOF means the decoder, not just the file, drained.
bool timed_receive(TimedMedia::State &s, bool video) {
    auto *decoder = video ? s.video : s.audio;
    bool &drained = video ? s.video_drained : s.audio_drained;
    if (!decoder || drained ||
        (video ? s.videos.size() >= s.limits.video_frames
               : s.audios.size() >= s.limits.audio_frames))
        return false;
    const int result = avcodec_receive_frame(decoder, s.frame);
    if (result == AVERROR(EAGAIN))
        return false;
    if (result == AVERROR_EOF) {
        drained = true;
        return true;
    }
    if (result < 0)
        return s.reject("movie decoder failed to receive a frame");
    const AVFrame &f = *s.frame;
    bool ok = true;
    if (video) {
        VideoFrame out;
        if (f.format != AV_PIX_FMT_YUV420P || f.width <= 0 || f.height <= 0 ||
            uint64_t(f.width) * uint64_t(f.height) > 4096u * 4096u) {
            ok = s.reject("unsupported or oversized video frame");
        } else if ((ok = timed_pts(s, f, s.video_index, out.pts))) {
            out.width = f.width;
            out.height = f.height;
            out.decode_errors = uint32_t(f.decode_error_flags);
            const AVStream *stream = s.input->streams[s.video_index];
            out.duration = double(f.duration) * av_q2d(stream->time_base);
            // ASF may omit per-frame duration and average_rate while retaining
            // a declared base cadence. That supplies duration only; never use
            // it to rewrite the original PTS or close a source gap.
            const AVRational cadence =
                av_guess_frame_rate(s.input, s.input->streams[s.video_index], s.frame);
            if (!(out.duration > 0) && cadence.num > 0 && cadence.den > 0)
                out.duration = av_q2d(av_inv_q(cadence));
            if (!(out.duration > 0) || !std::isfinite(out.duration)) {
                ok = s.reject("video frame has no usable source duration");
            } else {
                out.source_color = timed_color(f);
                VideoColorConversion conversion;
                if (s.color_selector &&
                    (!s.color_selector(out.width, out.height, out.source_color, &conversion) ||
                     !video_color_conversion_valid(conversion))) {
                    ok = s.reject("unsupported movie color conversion");
                } else {
                    out.argb.resize(size_t(f.width) * size_t(f.height));
                    for (int y = 0; y < f.height; ++y) {
                        auto *dest = reinterpret_cast<uint8_t *>(out.argb.data() +
                                                                 size_t(y) * size_t(f.width));
                        const auto *luma = f.data[0] + ptrdiff_t(y) * f.linesize[0];
                        const auto *u = f.data[1] + ptrdiff_t(y / 2) * f.linesize[1];
                        const auto *v = f.data[2] + ptrdiff_t(y / 2) * f.linesize[2];
                        if (s.color_selector)
                            video_frame_convert_color_row(dest, luma, u, v, uint32_t(f.width),
                                                          conversion);
                        else
                            video_frame_convert_row(dest, luma, u, v, uint32_t(f.width),
                                                    VIDEO_XRGB8888);
                    }
                    s.videos.push_back(std::move(out));
                }
            }
        }
    } else {
        AudioFrame out;
        if (f.sample_rate <= 0 || f.nb_samples <= 0 || f.nb_samples > 65536 ||
            !timed_layout(f.ch_layout, out.layout) ||
            (f.format != AV_SAMPLE_FMT_FLTP && f.format != AV_SAMPLE_FMT_FLT &&
             f.format != AV_SAMPLE_FMT_S16P && f.format != AV_SAMPLE_FMT_S16)) {
            ok = s.reject("unsupported audio format or ambiguous channel layout");
        } else if ((ok = timed_audio_pts(s, f, out))) {
            out.rate = f.sample_rate;
            out.pcm.reserve(size_t(f.nb_samples) * out.layout.size());
            append_samples(out.pcm, f);
            s.audios.push_back(std::move(out));
        }
    }
    av_frame_unref(s.frame);
    return ok;
}
} // namespace

TimedMedia::~TimedMedia() {
    close();
}
bool TimedMedia::has_audio() const {
    return s_ && s_->audio;
}
void TimedMedia::close() {
    delete s_;
    s_ = nullptr;
}

bool TimedMedia::open(const std::string &path, Limits limits, std::string *why,
                      VideoColorSelector color_selector) {
    close();
    if (!limits.video_frames || limits.video_frames > 8 || !limits.audio_frames ||
        limits.audio_frames > 128)
        return fail(why, "invalid movie queue bounds");
    auto state = std::make_unique<State>();
    state->limits = limits;
    state->color_selector = color_selector;
    if (avformat_open_input(&state->input, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(state->input, nullptr) < 0)
        return fail(why, "movie could not be opened or indexed");
    for (unsigned i = 0; i < state->input->nb_streams; ++i) {
        const auto type = state->input->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && state->video_index < 0)
            state->video_index = int(i);
        if (type == AVMEDIA_TYPE_AUDIO && state->audio_index < 0)
            state->audio_index = int(i);
    }
    if (state->video_index < 0 && state->audio_index < 0)
        return fail(why, "movie has no video or audio stream");
    if ((state->video_index >= 0 &&
         !open_stream(state->input, state->video_index, &state->video, true)) ||
        (state->audio_index >= 0 &&
         !open_stream(state->input, state->audio_index, &state->audio, true)))
        return fail(why, "a movie stream has no working decoder");
    state->video_drained = !state->video;
    state->audio_drained = !state->audio;
    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (!state->frame || !state->packet)
        return fail(why, "movie decoder ran out of memory");
    s_ = state.release();
    return true;
}

bool TimedMedia::reset(std::string *why) {
    if (!s_)
        return fail(why, "movie is closed");
    const int64_t start = s_->input->start_time == AV_NOPTS_VALUE ? 0 : s_->input->start_time;
    if (av_seek_frame(s_->input, -1, start, AVSEEK_FLAG_BACKWARD) < 0) {
        s_->reject("movie cannot seek to its start");
        return fail(why, s_->error.c_str());
    }
    if (s_->video)
        avcodec_flush_buffers(s_->video);
    if (s_->audio)
        avcodec_flush_buffers(s_->audio);
    av_packet_unref(s_->packet);
    av_frame_unref(s_->frame);
    s_->videos.clear();
    s_->audios.clear();
    s_->clear_packets();
    s_->pending = s_->eof = s_->video_flush = s_->audio_flush = false;
    s_->video_drained = !s_->video;
    s_->audio_drained = !s_->audio;
    s_->audio_packet_pts = s_->audio_anchor_pts = NAN;
    s_->audio_anchor_samples = 0;
    s_->audio_anchor_rate = 0;
    s_->error.clear();
    return true;
}
void TimedMedia::pause(bool paused) {
    if (s_)
        s_->paused = paused;
}

TimedMedia::Result TimedMedia::pump() {
    if (!s_)
        return Result::Ended;
    auto &s = *s_;
    if (!s.error.empty())
        return Result::Error;
    if (s.paused)
        return Result::Paused;
    const bool video_progress = timed_receive(s, true);
    const bool audio_progress = s.error.empty() && timed_receive(s, false);
    if (!s.error.empty())
        return Result::Error;
    if (video_progress || audio_progress)
        return Result::Progress;
    if (s.video_drained && s.audio_drained)
        return finished() ? Result::Ended : Result::Blocked;
    for (bool video : {true, false}) {
        auto *decoder = video ? s.video : s.audio;
        auto &packets = video ? s.video_packets : s.audio_packets;
        const bool full = video ? s.videos.size() >= s.limits.video_frames
                                : s.audios.size() >= s.limits.audio_frames;
        if (!decoder || full || packets.empty())
            continue;
        auto *packet = packets.front();
        const int result = avcodec_send_packet(decoder, packet);
        if (result == AVERROR(EAGAIN))
            continue;
        if (result < 0) {
            s.reject("movie decoder rejected a queued packet");
            return Result::Error;
        }
        if (!video) {
            s.audio_packet_pts =
                packet->pts == AV_NOPTS_VALUE
                    ? NAN
                    : double(packet->pts) * av_q2d(s.input->streams[s.audio_index]->time_base);
        }
        s.packet_bytes -= size_t(packet->size);
        av_packet_free(&packet);
        packets.pop_front();
        return Result::Progress;
    }
    if (s.pending) {
        if (s.packet->size < 0 || size_t(s.packet->size) > State::max_packet_bytes) {
            s.reject("movie compressed packet exceeds the bounded read-ahead limit");
            return Result::Error;
        }
        auto &packets = s.packet->stream_index == s.video_index ? s.video_packets : s.audio_packets;
        if (packets.size() >= State::max_packets ||
            s.packet_bytes + size_t(s.packet->size) > State::max_packet_bytes)
            return Result::Blocked;
        AVPacket *copy = av_packet_alloc();
        if (!copy) {
            s.reject("movie packet queue ran out of memory");
            return Result::Error;
        }
        s.packet_bytes += size_t(s.packet->size);
        av_packet_move_ref(copy, s.packet);
        packets.push_back(copy);
        s.pending = false;
        return Result::Progress;
    }
    if (!s.eof) {
        const int result = av_read_frame(s.input, s.packet);
        if (result == AVERROR_EOF) {
            s.eof = true;
        } else if (result < 0) {
            s.reject("movie demux failed before EOF");
            return Result::Error;
        } else if (s.packet->stream_index == s.video_index ||
                   s.packet->stream_index == s.audio_index) {
            s.pending = true;
        } else {
            av_packet_unref(s.packet);
        }
        return Result::Progress;
    }
    for (bool video : {true, false}) {
        auto *decoder = video ? s.video : s.audio;
        bool &flushed = video ? s.video_flush : s.audio_flush;
        const auto &packets = video ? s.video_packets : s.audio_packets;
        const bool full = video ? s.videos.size() >= s.limits.video_frames
                                : s.audios.size() >= s.limits.audio_frames;
        if (decoder && !flushed && packets.empty() && !full) {
            const int result = avcodec_send_packet(decoder, nullptr);
            if (result == AVERROR(EAGAIN))
                continue;
            if (result < 0 && result != AVERROR_EOF) {
                s.reject("movie decoder could not start EOF drain");
                return Result::Error;
            }
            flushed = true;
            return Result::Progress;
        }
    }
    return Result::Blocked; // a consumer must free an output slot before EOF drain
}

bool TimedMedia::pop_video(VideoFrame *out) {
    if (!s_ || !out || s_->videos.empty())
        return false;
    *out = std::move(s_->videos.front());
    s_->videos.pop_front();
    return true;
}
bool TimedMedia::pop_audio(AudioFrame *out) {
    if (!s_ || !out || s_->audios.empty())
        return false;
    *out = std::move(s_->audios.front());
    s_->audios.pop_front();
    return true;
}
size_t TimedMedia::queued_video() const {
    return s_ ? s_->videos.size() : 0;
}
size_t TimedMedia::queued_audio() const {
    return s_ ? s_->audios.size() : 0;
}
bool TimedMedia::finished() const {
    return !s_ || (s_->error.empty() && s_->video_drained && s_->audio_drained &&
                   s_->videos.empty() && s_->audios.empty());
}
bool TimedMedia::audio_finished() const {
    return !s_ || (s_->error.empty() && s_->audio_drained && s_->audios.empty());
}
const std::string &TimedMedia::error() const {
    static const std::string closed = "movie is closed";
    return s_ ? s_->error : closed;
}

#else // no FFmpeg in this build

struct Media::State {};
struct TimedMedia::State {};
TimedMedia::~TimedMedia() = default;
bool TimedMedia::has_audio() const {
    return false;
}
bool TimedMedia::open(const std::string &, Limits, std::string *why, VideoColorSelector) {
    if (why)
        *why = "this build has no video decoder";
    return false;
}
void TimedMedia::close() {}
bool TimedMedia::reset(std::string *why) {
    if (why)
        *why = "this build has no video decoder";
    return false;
}
void TimedMedia::pause(bool) {}
TimedMedia::Result TimedMedia::pump() {
    return Result::Ended;
}
bool TimedMedia::pop_video(VideoFrame *) {
    return false;
}
bool TimedMedia::pop_audio(AudioFrame *) {
    return false;
}
size_t TimedMedia::queued_video() const {
    return 0;
}
size_t TimedMedia::queued_audio() const {
    return 0;
}
bool TimedMedia::finished() const {
    return true;
}
bool TimedMedia::audio_finished() const {
    return true;
}
const std::string &TimedMedia::error() const {
    static const std::string why = "this build has no video decoder";
    return why;
}
Media::~Media() = default;
void Media::close() {}
bool Media::is_open() const {
    return false;
}
bool Media::open(const std::string &, std::string *why) {
    if (why)
        *why = "this build has no video decoder";
    return false;
}
bool Media::has_video() const {
    return false;
}
bool Media::has_audio() const {
    return false;
}
int32_t Media::width() const {
    return 0;
}
int32_t Media::height() const {
    return 0;
}
double Media::duration() const {
    return 0;
}
int32_t Media::audio_rate() const {
    return 0;
}
int32_t Media::audio_channels() const {
    return 0;
}
bool Media::next_video(VideoFrame *) {
    return false;
}
void Media::fill_audio(size_t) {}
std::vector<int16_t> Media::take_audio() {
    return {};
}
bool Media::finished() const {
    return true;
}

#endif

} // namespace mf
