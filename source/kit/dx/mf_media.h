// A media file the guest asked Media Foundation to play, decoded on the host
// with FFmpeg. Video comes out as opaque ARGB frames with the time each is
// due; audio as interleaved signed 16-bit samples. Nothing here touches guest
// memory: the Media Foundation objects above it own that side.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "video_frame.h"

namespace mf {

struct VideoColorDescription {
    enum class Matrix { Unspecified, Bt601, Bt709, Other };
    enum class Range { Unspecified, Limited, Full, Other };
    Matrix matrix = Matrix::Unspecified;
    Range range = Range::Unspecified;
};
// Called on the single demux owner with each decoded frame. Return false to
// reject a source the caller cannot interpret. Null preserves legacy conversion.
using VideoColorSelector = bool (*)(uint32_t width, uint32_t height, VideoColorDescription source,
                                    VideoColorConversion *conversion);

struct VideoFrame {
    int32_t width = 0, height = 0;
    double pts = 0;             // seconds from the start of the stream
    double duration = 0;        // source duration; does not collapse gaps between PTS
    uint32_t decode_errors = 0; // decoder-reported damage/concealment, not hidden by the player
    VideoColorDescription source_color;
    std::vector<uint32_t> argb; // width * height, top row first
};

// Decoded order is explicit. Back and side speakers remain distinct until an
// output policy deliberately maps them; a channel count alone is not a layout.
enum class AudioChannel { Left, Right, Center, Lfe, BackLeft, BackRight, SideLeft, SideRight };
const char *audio_channel_name(AudioChannel channel);

struct AudioFrame {
    enum class TimestampOrigin { Frame, Packet, SampleContinuation };
    double pts = 0;
    TimestampOrigin timestamp_origin = TimestampOrigin::Frame;
    int32_t rate = 0;
    std::vector<AudioChannel> layout;
    std::vector<int16_t> pcm; // interleaved in layout order
    size_t samples() const {
        return layout.empty() ? 0 : pcm.size() / layout.size();
    }
    double duration() const {
        return rate > 0 ? double(samples()) / rate : 0;
    }
};

// Timed movie decoder: one owner calls pump/pop/reset on one thread. Consumers
// must service BOTH queues. Compressed read-ahead (128 packets per stream,
// 32 MiB total plus one pending packet <=32 MiB) decouples interleave leads
// before applying backpressure. There are no device or guest-memory calls.
// The player owns its presentation clock; decoded/queued samples are not played
// samples. In particular, pts=0 followed by pts=.834 must remain a source gap.
class TimedMedia {
  public:
    enum class Result { Progress, Blocked, Paused, Ended, Error };
    struct Limits {
        size_t video_frames = 4, audio_frames = 32;
    };
    TimedMedia() = default;
    ~TimedMedia();
    TimedMedia(const TimedMedia &) = delete;
    TimedMedia &operator=(const TimedMedia &) = delete;
    bool open(const std::string &path, Limits limits, std::string *why = nullptr,
              VideoColorSelector color_selector = nullptr);
    void close();
    bool reset(std::string *why = nullptr); // seek to source start and flush both codecs/queues
    void pause(bool paused);
    Result pump();
    bool pop_video(VideoFrame *out);
    bool pop_audio(AudioFrame *out);
    size_t queued_video() const;
    size_t queued_audio() const;
    bool finished() const; // both codecs drained AND both queues consumed
    bool
    audio_finished() const; // audio decoder drained and audio queue consumed, independent of video
    bool has_audio() const;
    const std::string &error() const;
    struct State;

  private:
    State *s_ = nullptr;
};

class Media {
  public:
    Media() = default;
    ~Media();
    Media(const Media &) = delete;
    Media &operator=(const Media &) = delete;

    // Opens `path` (a host path) and reads its stream layout. False leaves the
    // object closed and, when `why` is given, says what the decoder refused.
    bool open(const std::string &path, std::string *why = nullptr);
    void close();
    bool is_open() const;

    bool has_video() const;
    bool has_audio() const;
    int32_t width() const;
    int32_t height() const;
    double duration() const; // seconds; 0 when the container does not say
    int32_t audio_rate() const;
    int32_t audio_channels() const;

    // Decodes until the next video frame is ready, appending any audio decoded
    // on the way. False means the file ended (or there is no video at all).
    bool next_video(VideoFrame *out);
    // Decodes audio until at least `samples` interleaved samples are queued or
    // the file ends. Audio-only files need this; with video, next_video feeds
    // the same queue.
    void fill_audio(size_t samples);
    // Hands over everything decoded so far, leaving the queue empty.
    std::vector<int16_t> take_audio();
    bool finished() const;

    // Opaque to callers; the decoder file defines it.
    struct State;

  private:
    State *s_ = nullptr;
};

} // namespace mf
