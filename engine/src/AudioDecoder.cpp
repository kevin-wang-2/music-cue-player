#include "engine/AudioDecoder.h"
#include <sndfile.h>
#include <algorithm>
#include <cstring>
#include <vector>

#ifdef MCP_HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace mcp {

// ─────────────────────────────────────────────────────────────────────────────
// libsndfile backend
// ─────────────────────────────────────────────────────────────────────────────
class SndfileDecoder final : public AudioDecoder {
    SNDFILE* m_sf{nullptr};
    SF_INFO  m_info{};

public:
    static std::unique_ptr<SndfileDecoder> tryOpen(const std::string& path) {
        SF_INFO info{};
        SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
        if (!sf) return nullptr;
        auto d    = std::make_unique<SndfileDecoder>();
        d->m_sf   = sf;
        d->m_info = info;
        return d;
    }

    ~SndfileDecoder() override { if (m_sf) sf_close(m_sf); }

    int     nativeSampleRate() const override { return m_info.samplerate; }
    int     nativeChannels()   const override { return m_info.channels; }
    int64_t nativeFrameCount() const override { return m_info.frames; }

    bool seekToFrame(int64_t frame) override {
        return sf_seek(m_sf, static_cast<sf_count_t>(frame), SEEK_SET) >= 0;
    }

    int64_t readFloat(float* buf, int64_t nFrames) override {
        return sf_readf_float(m_sf, buf, static_cast<sf_count_t>(nFrames));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FFmpeg backend
// ─────────────────────────────────────────────────────────────────────────────
#ifdef MCP_HAVE_FFMPEG
class FfmpegDecoder final : public AudioDecoder {
    AVFormatContext* m_fmt{nullptr};
    AVCodecContext*  m_codec{nullptr};
    AVPacket*        m_pkt{nullptr};
    AVFrame*         m_frame{nullptr};
    int              m_streamIdx{-1};
    int              m_sr{0};
    int              m_ch{0};
    bool             m_eof{false};

    // Decoded samples not yet returned to the caller.
    std::vector<float> m_pending;
    int64_t            m_pendingOff{0};

    void convertFrameToPending() {
        const int n  = m_frame->nb_samples;
        const int ch = m_ch;
        m_pending.resize(static_cast<size_t>(n * ch));
        const auto fmt = static_cast<AVSampleFormat>(m_frame->format);
        switch (fmt) {
        case AV_SAMPLE_FMT_FLT:
            std::memcpy(m_pending.data(), m_frame->data[0],
                        static_cast<size_t>(n * ch) * sizeof(float));
            break;
        case AV_SAMPLE_FMT_FLTP:
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < ch; ++c)
                    m_pending[f * ch + c] =
                        reinterpret_cast<float*>(m_frame->data[c])[f];
            break;
        case AV_SAMPLE_FMT_S16:
            for (int i = 0; i < n * ch; ++i)
                m_pending[i] =
                    reinterpret_cast<int16_t*>(m_frame->data[0])[i] * (1.0f / 32768.0f);
            break;
        case AV_SAMPLE_FMT_S16P:
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < ch; ++c)
                    m_pending[f * ch + c] =
                        reinterpret_cast<int16_t*>(m_frame->data[c])[f] * (1.0f / 32768.0f);
            break;
        case AV_SAMPLE_FMT_S32:
            for (int i = 0; i < n * ch; ++i)
                m_pending[i] =
                    reinterpret_cast<int32_t*>(m_frame->data[0])[i] * (1.0f / 2147483648.0f);
            break;
        case AV_SAMPLE_FMT_S32P:
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < ch; ++c)
                    m_pending[f * ch + c] =
                        reinterpret_cast<int32_t*>(m_frame->data[c])[f] * (1.0f / 2147483648.0f);
            break;
        case AV_SAMPLE_FMT_DBL:
            for (int i = 0; i < n * ch; ++i)
                m_pending[i] = static_cast<float>(
                    reinterpret_cast<double*>(m_frame->data[0])[i]);
            break;
        case AV_SAMPLE_FMT_DBLP:
            for (int f = 0; f < n; ++f)
                for (int c = 0; c < ch; ++c)
                    m_pending[f * ch + c] = static_cast<float>(
                        reinterpret_cast<double*>(m_frame->data[c])[f]);
            break;
        default:
            std::fill(m_pending.begin(), m_pending.end(), 0.0f);
            break;
        }
        m_pendingOff = 0;
    }

public:
    static std::unique_ptr<FfmpegDecoder> tryOpen(const std::string& path) {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0)
            return nullptr;
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt); return nullptr;
        }
        int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (si < 0) { avformat_close_input(&fmt); return nullptr; }

        const AVCodec* codec =
            avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id);
        if (!codec) { avformat_close_input(&fmt); return nullptr; }

        AVCodecContext* cctx = avcodec_alloc_context3(codec);
        if (!cctx) { avformat_close_input(&fmt); return nullptr; }
        avcodec_parameters_to_context(cctx, fmt->streams[si]->codecpar);
        if (avcodec_open2(cctx, codec, nullptr) < 0) {
            avcodec_free_context(&cctx); avformat_close_input(&fmt); return nullptr;
        }

        auto d          = std::make_unique<FfmpegDecoder>();
        d->m_fmt        = fmt;
        d->m_codec      = cctx;
        d->m_streamIdx  = si;
        d->m_pkt        = av_packet_alloc();
        d->m_frame      = av_frame_alloc();
        d->m_sr         = cctx->sample_rate;
        d->m_ch         = cctx->ch_layout.nb_channels;
        return d;
    }

    ~FfmpegDecoder() override {
        if (m_frame) av_frame_free(&m_frame);
        if (m_pkt)   av_packet_free(&m_pkt);
        if (m_codec) avcodec_free_context(&m_codec);
        if (m_fmt)   avformat_close_input(&m_fmt);
    }

    int nativeSampleRate() const override { return m_sr; }
    int nativeChannels()   const override { return m_ch; }

    int64_t nativeFrameCount() const override {
        if (m_fmt->duration == AV_NOPTS_VALUE || m_fmt->duration <= 0) return -1;
        return av_rescale_q(m_fmt->duration, AV_TIME_BASE_Q,
                            AVRational{1, m_sr});
    }

    bool seekToFrame(int64_t frame) override {
        const AVStream* st = m_fmt->streams[m_streamIdx];
        const int64_t ts   = av_rescale_q(frame, AVRational{1, m_sr}, st->time_base);
        if (av_seek_frame(m_fmt, m_streamIdx, ts, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(m_codec);
        m_pending.clear();
        m_pendingOff = 0;
        m_eof        = false;
        return true;
    }

    int64_t readFloat(float* buf, int64_t nFrames) override {
        int64_t written = 0;

        while (written < nFrames) {
            // 1. Drain buffered output
            const int64_t avail =
                (static_cast<int64_t>(m_pending.size()) - m_pendingOff) / m_ch;
            if (avail > 0) {
                const int64_t take = std::min(nFrames - written, avail);
                std::memcpy(buf + written * m_ch,
                            m_pending.data() + m_pendingOff,
                            static_cast<size_t>(take * m_ch) * sizeof(float));
                written      += take;
                m_pendingOff += take * m_ch;
                if (m_pendingOff >= static_cast<int64_t>(m_pending.size())) {
                    m_pending.clear(); m_pendingOff = 0;
                }
                continue;
            }

            // 2. Try to receive a decoded frame
            const int rcv = avcodec_receive_frame(m_codec, m_frame);
            if (rcv == 0) { convertFrameToPending(); continue; }
            if (rcv == AVERROR_EOF) break;

            // 3. EAGAIN — need to feed another packet
            if (m_eof) break;

            const int rd = av_read_frame(m_fmt, m_pkt);
            if (rd == AVERROR_EOF) {
                m_eof = true;
                avcodec_send_packet(m_codec, nullptr);  // flush decoder
            } else if (rd < 0) {
                break;
            } else if (m_pkt->stream_index == m_streamIdx) {
                avcodec_send_packet(m_codec, m_pkt);
                av_packet_unref(m_pkt);
            } else {
                av_packet_unref(m_pkt);  // skip non-audio streams
            }
        }
        return written;
    }
};
#endif // MCP_HAVE_FFMPEG

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<AudioDecoder> AudioDecoder::open(const std::string& path,
                                                  std::string& error) {
    if (auto d = SndfileDecoder::tryOpen(path)) return d;
#ifdef MCP_HAVE_FFMPEG
    if (auto d = FfmpegDecoder::tryOpen(path))  return d;
#endif
    error = "unsupported format or file not found: " + path;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Waveform peak building

bool buildWaveformPeaks(const std::string& path,
                         int numBuckets,
                         std::vector<float> minPeaks[2],
                         std::vector<float> maxPeaks[2],
                         double& fileDurationSecs,
                         int& fileChannels) {
    std::string err;
    auto dec = AudioDecoder::open(path, err);
    if (!dec) return false;

    const int     fileCh    = dec->nativeChannels();
    const int     fileSR    = dec->nativeSampleRate();
    const int64_t fileFrames = dec->nativeFrameCount();
    if (fileCh <= 0 || fileSR <= 0 || fileFrames <= 0) return false;

    fileDurationSecs = static_cast<double>(fileFrames) / fileSR;
    fileChannels     = fileCh;

    // We display 1 channel for mono/multi, 2 channels for stereo.
    const int dispCh = (fileCh == 2) ? 2 : 1;

    for (int c = 0; c < 2; ++c) {
        minPeaks[c].assign(static_cast<size_t>(numBuckets), 0.0f);
        maxPeaks[c].assign(static_cast<size_t>(numBuckets), 0.0f);
    }
    std::vector<bool> hasData(static_cast<size_t>(numBuckets), false);
    std::vector<bool> hasData1(static_cast<size_t>(numBuckets), false);

    constexpr int64_t kBuf = 4096;
    std::vector<float> buf(static_cast<size_t>(kBuf * fileCh));
    int64_t framePos = 0;

    while (true) {
        const int64_t got = dec->readFloat(buf.data(), kBuf);
        if (got <= 0) break;
        for (int64_t f = 0; f < got; ++f, ++framePos) {
            const int bi = static_cast<int>(framePos * numBuckets / fileFrames);
            if (bi >= numBuckets) break;
            const float s0 = buf[f * fileCh + 0];
            if (!hasData[static_cast<size_t>(bi)]) {
                minPeaks[0][static_cast<size_t>(bi)] = maxPeaks[0][static_cast<size_t>(bi)] = s0;
                hasData[static_cast<size_t>(bi)] = true;
            } else {
                if (s0 < minPeaks[0][static_cast<size_t>(bi)]) minPeaks[0][static_cast<size_t>(bi)] = s0;
                if (s0 > maxPeaks[0][static_cast<size_t>(bi)]) maxPeaks[0][static_cast<size_t>(bi)] = s0;
            }
            if (dispCh == 2) {
                const float s1 = buf[f * fileCh + 1];
                if (!hasData1[static_cast<size_t>(bi)]) {
                    minPeaks[1][static_cast<size_t>(bi)] = maxPeaks[1][static_cast<size_t>(bi)] = s1;
                    hasData1[static_cast<size_t>(bi)] = true;
                } else {
                    if (s1 < minPeaks[1][static_cast<size_t>(bi)]) minPeaks[1][static_cast<size_t>(bi)] = s1;
                    if (s1 > maxPeaks[1][static_cast<size_t>(bi)]) maxPeaks[1][static_cast<size_t>(bi)] = s1;
                }
            }
        }
    }
    return true;
}

} // namespace mcp
