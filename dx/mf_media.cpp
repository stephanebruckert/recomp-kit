// FFmpeg behind mf_media.h. The decoders are the ones cmake/Dependencies.cmake
// enables: Windows Media video and audio for the game's movies, MP3 for its
// music. A build without FFmpeg refuses to open anything, which the Media
// Foundation objects above report as an unsupported byte stream.
#include "mf_media.h"
#include <cstdio>
#include <cstring>

#include "video_frame.h"

#include <algorithm>
#include <cmath>
#include <memory>

#ifdef RECOMP_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace mf {

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

bool open_stream(AVFormatContext *input, int index, AVCodecContext **out) {
    const AVCodec *codec = avcodec_find_decoder(input->streams[index]->codecpar->codec_id);
    if (!codec)
        return false;
    *out = avcodec_alloc_context3(codec);
    if (!*out)
        return false;
    if (avcodec_parameters_to_context(*out, input->streams[index]->codecpar) < 0)
        return false;
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
            case AV_SAMPLE_FMT_U8: // the PCM of 1990s AVI and WAV soundtracks
                value = (f.extended_data[0][i * channels + ch] - 128) / 128.0;
                break;
            case AV_SAMPLE_FMT_U8P:
                value = (f.extended_data[ch][i] - 128) / 128.0;
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
            // YUV 4:2:0 (most codecs) and the RGB layouts the Video for
            // Windows codecs decode to: Cinepak gives RGB24, or PAL8 for an
            // 8-bit film; MS Video 1 and RLE give 16-bit RGB or a palette.
            const bool known = f.format == AV_PIX_FMT_YUV420P || f.format == AV_PIX_FMT_RGB24 ||
                               f.format == AV_PIX_FMT_BGR24 || f.format == AV_PIX_FMT_PAL8 ||
                               f.format == AV_PIX_FMT_RGB555LE || f.format == AV_PIX_FMT_RGB565LE;
            if (!known || f.width <= 0 || f.height <= 0) {
                av_frame_unref(s_->frame);
                continue; // an unexpected layout is skipped, not drawn wrong
            }
            out->width = f.width;
            out->height = f.height;
            out->argb.assign(size_t(f.width) * size_t(f.height), 0);
            for (int y = 0; y < f.height; ++y) {
                uint32_t *dst = out->argb.data() + size_t(y) * size_t(f.width);
                const uint8_t *row = f.data[0] + size_t(y) * f.linesize[0];
                if (f.format == AV_PIX_FMT_YUV420P) {
                    video_frame_convert_row(reinterpret_cast<uint8_t *>(dst), row,
                                            f.data[1] + size_t(y / 2) * f.linesize[1],
                                            f.data[2] + size_t(y / 2) * f.linesize[2],
                                            uint32_t(f.width), VIDEO_XRGB8888);
                    continue;
                }
                for (int x = 0; x < f.width; ++x) {
                    uint32_t r, g, b;
                    if (f.format == AV_PIX_FMT_RGB24 || f.format == AV_PIX_FMT_BGR24) {
                        const uint8_t *p = row + 3 * x;
                        r = f.format == AV_PIX_FMT_RGB24 ? p[0] : p[2];
                        g = p[1];
                        b = f.format == AV_PIX_FMT_RGB24 ? p[2] : p[0];
                    } else if (f.format == AV_PIX_FMT_PAL8) {
                        uint32_t c;
                        memcpy(&c, f.data[1] + 4 * row[x], 4); // ARGB in native order
                        r = (c >> 16) & 255;
                        g = (c >> 8) & 255;
                        b = c & 255;
                    } else {
                        const uint32_t v = row[2 * x] | uint32_t(row[2 * x + 1]) << 8;
                        if (f.format == AV_PIX_FMT_RGB565LE) {
                            r = (v >> 11 & 31) * 255 / 31;
                            g = (v >> 5 & 63) * 255 / 63;
                        } else {
                            r = (v >> 10 & 31) * 255 / 31;
                            g = (v >> 5 & 31) * 255 / 31;
                        }
                        b = (v & 31) * 255 / 31;
                    }
                    dst[x] = 0xff000000u | r << 16 | g << 8 | b;
                }
            }
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

#else // no FFmpeg in this build

// Without FFmpeg (the web build) the one format read is plain PCM WAV, 8 or
// 16-bit: CD music ripped to WAV still plays. Video and compressed audio are
// refused, which callers report as an unsupported file.
struct Media::State {
    FILE *file = nullptr;
    long data_at = 0;        // the PCM payload, read as it plays
    size_t data_bytes = 0, pos = 0;
    int32_t rate = 0, channels = 0, bits = 0;
    std::vector<int16_t> pcm;
    ~State() {
        if (file)
            fclose(file);
    }
};
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
    auto fail = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    auto st = std::make_unique<State>();
    st->file = fopen(path.c_str(), "rb");
    if (!st->file)
        return fail("cannot open the file");
    uint8_t h[12];
    if (fread(h, 1, 12, st->file) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return fail("this build has no video decoder and reads only PCM WAV");
    auto u32 = [](const uint8_t *b) {
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    };
    bool fmt = false;
    for (;;) { // the chunk headers, skipping each body but fmt's
        uint8_t c[8];
        if (fread(c, 1, 8, st->file) != 8)
            break;
        const uint32_t size = u32(c + 4);
        if (!memcmp(c, "fmt ", 4) && size >= 16) {
            uint8_t b[16];
            if (fread(b, 1, 16, st->file) != 16)
                break;
            if ((b[0] | b[1] << 8) != 1) // WAVE_FORMAT_PCM
                return fail("the WAV is not PCM");
            st->channels = b[2] | b[3] << 8;
            st->rate = int32_t(u32(b + 4));
            st->bits = b[14] | b[15] << 8;
            fmt = true;
            fseek(st->file, long(size - 16 + (size & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            st->data_at = ftell(st->file);
            st->data_bytes = size;
            break;
        } else {
            fseek(st->file, long(size + (size & 1)), SEEK_CUR);
        }
    }
    if (!fmt || !st->data_bytes || st->channels < 1 || st->channels > 2 || st->rate <= 0 ||
        (st->bits != 8 && st->bits != 16))
        return fail("the WAV is not 8 or 16-bit mono or stereo PCM");
    fseek(st->file, st->data_at, SEEK_SET);
    s_ = st.release();
    return true;
}
bool Media::has_video() const {
    return false;
}
bool Media::has_audio() const {
    return s_ != nullptr;
}
int32_t Media::width() const {
    return 0;
}
int32_t Media::height() const {
    return 0;
}
double Media::duration() const {
    return s_ ? double(s_->data_bytes) / (s_->rate * s_->channels * (s_->bits / 8)) : 0;
}
int32_t Media::audio_rate() const {
    return s_ ? s_->rate : 0;
}
int32_t Media::audio_channels() const {
    return s_ ? s_->channels : 0;
}
bool Media::next_video(VideoFrame *) {
    return false;
}
void Media::fill_audio(size_t samples) {
    if (!s_)
        return;
    const size_t bytes = size_t(s_->bits / 8);
    uint8_t buf[8192];
    while (s_->pcm.size() < samples && s_->pos < s_->data_bytes) {
        size_t want = std::min(sizeof buf, s_->data_bytes - s_->pos);
        want -= want % bytes;
        const size_t got = want ? fread(buf, 1, want, s_->file) : 0;
        if (got < bytes) {
            s_->pos = s_->data_bytes; // a short file ends here
            break;
        }
        for (size_t o = 0; o + bytes <= got; o += bytes)
            s_->pcm.push_back(bytes == 1 ? int16_t((int(buf[o]) - 128) * 256)
                                         : int16_t(uint16_t(buf[o] | buf[o + 1] << 8)));
        s_->pos += got - got % bytes;
    }
}
std::vector<int16_t> Media::take_audio() {
    std::vector<int16_t> out;
    if (s_)
        out.swap(s_->pcm);
    return out;
}
bool Media::finished() const {
    return !s_ || (s_->pos >= s_->data_bytes && s_->pcm.empty());
}

#endif

} // namespace mf
