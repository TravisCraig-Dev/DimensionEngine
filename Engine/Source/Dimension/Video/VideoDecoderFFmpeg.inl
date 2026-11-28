// VideoDecoder through FFmpeg's libraries (Linux; included by VideoDecoder.cpp when built with DIM_FFMPEG): MP4 / H.264,
// WebM / VP9, MKV, MOV, AVI... whatever the installed FFmpeg reads. Each stream has a reader of its own (its own
// demuxer and decoder), so pictures and sound are read apart as with Media Foundation.
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <mutex>

namespace Dimension
{
    // (named, not anonymous: VideoDecoder::Native, which every build sees, holds these)
    namespace VideoFFmpeg
    {
        // One demuxer and decoder for one stream of a file.
        struct StreamReader
        {
            AVFormatContext* Format = nullptr;
            AVCodecContext* Codec = nullptr;
            AVPacket* Packet = nullptr;
            AVFrame* Frame = nullptr;
            int Stream = -1;
            bool Draining = false; // the file has ended: the decoder gives what it still holds

            bool Open(const fs::path& file, AVMediaType type)
            {
                static std::once_flag quiet;
                std::call_once(quiet, [] { av_log_set_level(AV_LOG_ERROR); });
                if (avformat_open_input(&Format, file.string().c_str(), nullptr, nullptr) < 0) return false;
                if (avformat_find_stream_info(Format, nullptr) < 0) return false;
                const AVCodec* decoder = nullptr;
                Stream = av_find_best_stream(Format, type, -1, -1, &decoder, 0);
                if (Stream < 0 || !decoder) return false;
                // Only this stream's packets matter to this reader.
                for (unsigned i = 0; i < Format->nb_streams; ++i)
                    if ((int)i != Stream) Format->streams[i]->discard = AVDISCARD_ALL;
                Codec = avcodec_alloc_context3(decoder);
                if (!Codec || avcodec_parameters_to_context(Codec, Format->streams[Stream]->codecpar) < 0) return false;
                Codec->thread_count = 0; // (as many threads as suit it)
                if (avcodec_open2(Codec, decoder, nullptr) < 0) return false;
                Packet = av_packet_alloc();
                Frame = av_frame_alloc();
                return Packet && Frame;
            }

            ~StreamReader()
            {
                av_frame_free(&Frame);
                av_packet_free(&Packet);
                avcodec_free_context(&Codec);
                avformat_close_input(&Format);
            }

            double TimeOf(const AVFrame* f) const
            {
                int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
                return pts == AV_NOPTS_VALUE ? 0.0 : pts * av_q2d(Format->streams[Stream]->time_base);
            }

            // The next decoded frame into Frame; false at the end.
            bool Next()
            {
                for (;;)
                {
                    int r = avcodec_receive_frame(Codec, Frame);
                    if (r == 0) return true;
                    if (r != AVERROR(EAGAIN) || Draining) return false;
                    // Feed it the stream's next packet (or tell it the file has ended).
                    for (;;)
                    {
                        if (av_read_frame(Format, Packet) < 0)
                        {
                            avcodec_send_packet(Codec, nullptr);
                            Draining = true;
                            break;
                        }
                        bool ours = Packet->stream_index == Stream;
                        if (ours) avcodec_send_packet(Codec, Packet);
                        av_packet_unref(Packet);
                        if (ours) break;
                    }
                }
            }

            void Seek(double seconds)
            {
                AVRational tb = Format->streams[Stream]->time_base;
                int64_t ts = (int64_t)(std::max(seconds, 0.0) / av_q2d(tb));
                av_seek_frame(Format, Stream, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(Codec);
                Draining = false;
            }
        };
    }
    using VideoFFmpeg::StreamReader;

    struct VideoDecoder::Native
    {
        std::unique_ptr<StreamReader> Video, Audio;
        SwsContext* Scale = nullptr; // decoded pictures to RGBA (made for the first one's format)
        int ScaleFormat = -1, ScaleWidth = 0, ScaleHeight = 0;
        SwrContext* Resample = nullptr; // decoded sound to interleaved floats
        ~Native()
        {
            sws_freeContext(Scale);
            swr_free(&Resample);
        }
    };

    VideoDecoder::~VideoDecoder() = default;

    std::unique_ptr<VideoDecoder> VideoDecoder::Open(const fs::path& file)
    {
        std::unique_ptr<VideoDecoder> d(new VideoDecoder());
        d->m_Native = std::make_unique<Native>();
        Native& n = *d->m_Native;
        VideoInfo& info = d->m_Info;
        n.Video = std::make_unique<StreamReader>();
        if (!n.Video->Open(file, AVMEDIA_TYPE_VIDEO)) n.Video.reset();
        n.Audio = std::make_unique<StreamReader>();
        if (!n.Audio->Open(file, AVMEDIA_TYPE_AUDIO)) n.Audio.reset();
        if (!n.Video && !n.Audio) return nullptr;
        StreamReader& any = n.Video ? *n.Video : *n.Audio;
        if (any.Format->duration != AV_NOPTS_VALUE) info.Duration = (double)any.Format->duration / AV_TIME_BASE;
        if (n.Video)
        {
            info.Width = (uint32_t)n.Video->Codec->width;
            info.Height = (uint32_t)n.Video->Codec->height;
            AVRational rate = av_guess_frame_rate(n.Video->Format, n.Video->Format->streams[n.Video->Stream], nullptr);
            info.FrameRate = rate.den ? av_q2d(rate) : 0.0;
            info.HasVideo = info.Width > 0 && info.Height > 0;
        }
        if (n.Audio)
        {
            AVCodecContext* c = n.Audio->Codec;
            info.Channels = (uint32_t)c->ch_layout.nb_channels;
            info.SampleRate = (uint32_t)c->sample_rate;
            AVChannelLayout layout;
            av_channel_layout_copy(&layout, &c->ch_layout);
            if (layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&layout, c->ch_layout.nb_channels);
            if (swr_alloc_set_opts2(&n.Resample, &layout, AV_SAMPLE_FMT_FLT, c->sample_rate, &layout, c->sample_fmt, c->sample_rate, 0, nullptr) < 0 ||
                swr_init(n.Resample) < 0)
                swr_free(&n.Resample);
            av_channel_layout_uninit(&layout);
            info.HasAudio = info.Channels > 0 && info.SampleRate > 0 && n.Resample;
        }
        if (!info.HasVideo) n.Video.reset();
        if (!info.HasAudio) n.Audio.reset();
        if (!info.HasVideo && !info.HasAudio) return nullptr;
        return d;
    }

    bool VideoDecoder::Probe(const fs::path& file, VideoInfo& info)
    {
        auto d = Open(file);
        if (!d) return false;
        info = d->Info();
        return true;
    }

    bool VideoDecoder::NextFrame(double& time, std::vector<uint32_t>& rgba)
    {
        Native& n = *m_Native;
        if (!n.Video || !n.Video->Next()) return false;
        AVFrame* f = n.Video->Frame;
        if (!n.Scale || n.ScaleFormat != f->format || n.ScaleWidth != f->width || n.ScaleHeight != f->height)
        {
            sws_freeContext(n.Scale);
            n.Scale = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, (int)m_Info.Width, (int)m_Info.Height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
            n.ScaleFormat = f->format;
            n.ScaleWidth = f->width;
            n.ScaleHeight = f->height;
            if (!n.Scale) return false;
        }
        rgba.resize((size_t)m_Info.Width * m_Info.Height);
        uint8_t* out[4] = { (uint8_t*)rgba.data(), nullptr, nullptr, nullptr };
        int outStride[4] = { (int)m_Info.Width * 4, 0, 0, 0 };
        sws_scale(n.Scale, f->data, f->linesize, 0, f->height, out, outStride);
        time = n.Video->TimeOf(f);
        av_frame_unref(f);
        return true;
    }

    bool VideoDecoder::NextAudio(double& time, std::vector<float>& samples)
    {
        Native& n = *m_Native;
        if (!n.Audio || !n.Resample) return false;
        for (int attempts = 0; attempts < 64; ++attempts)
        {
            if (!n.Audio->Next()) return false;
            AVFrame* f = n.Audio->Frame;
            int capacity = swr_get_out_samples(n.Resample, f->nb_samples);
            samples.resize((size_t)std::max(capacity, 0) * m_Info.Channels);
            uint8_t* out = (uint8_t*)samples.data();
            int got = swr_convert(n.Resample, &out, capacity, (const uint8_t**)f->extended_data, f->nb_samples);
            time = n.Audio->TimeOf(f);
            av_frame_unref(f);
            if (got <= 0) continue;
            samples.resize((size_t)got * m_Info.Channels);
            return true;
        }
        return false;
    }

    void VideoDecoder::RewindVideo() { SeekVideo(0.0); }
    void VideoDecoder::RewindAudio() { SeekAudio(0.0); }
    void VideoDecoder::SeekVideo(double seconds)
    {
        if (m_Native->Video) m_Native->Video->Seek(seconds);
    }
    void VideoDecoder::SeekAudio(double seconds)
    {
        if (m_Native->Audio) m_Native->Audio->Seek(seconds);
        if (m_Native->Resample) swr_init(m_Native->Resample); // (drops what it held from before)
    }
}
