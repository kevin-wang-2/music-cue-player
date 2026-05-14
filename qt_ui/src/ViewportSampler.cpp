#include "ViewportSampler.h"
#include "engine/AudioDecoder.h"

#include <QMetaObject>
#include <algorithm>
#include <cmath>
#include <pthread.h>

// ─── ViewportSampler ─────────────────────────────────────────────────────────

ViewportSampler::ViewportSampler()
    : m_worker(&ViewportSampler::workerLoop, this) {}

ViewportSampler::~ViewportSampler() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stopping.store(true, std::memory_order_relaxed);
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

bool ViewportSampler::needsSampleZoom(double audioSpan, int pixelWidth,
                                       int totalBuckets, double fileDur) {
    if (pixelWidth <= 0 || fileDur <= 0.0 || totalBuckets <= 0) return false;
    // bucketsPerPx < 1.0  ↔  audioSpan * totalBuckets < pixelWidth * fileDur
    return audioSpan * totalBuckets < static_cast<double>(pixelWidth) * fileDur;
}

void ViewportSampler::request(const std::string& path, double tL, double tR,
                               QObject* context, std::function<void()> callback) {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Deduplicate against the last submitted request, not m_current->tL/tR.
        // Frame tL/tR are frame-aligned and will never exactly equal the
        // continuous requested values, so comparing them causes an infinite
        // re-decode loop whenever a frame is ready and the viewport is stable.
        if (m_reqPath == path && m_reqTL == tL && m_reqTR == tR) return;
        m_reqPath = path;
        m_reqTL   = tL;
        m_reqTR   = tR;
        m_job    = {path, tL, tR, context, std::move(callback)};
        m_hasJob = true;
    }
    m_cv.notify_one();
}

std::shared_ptr<const SampleFrame> ViewportSampler::current() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_current;
}

void ViewportSampler::workerLoop() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_hasJob || m_stopping.load(std::memory_order_relaxed);
            });
            if (m_stopping.load() && !m_hasJob) break;
            job      = std::move(m_job);
            m_hasJob = false;
        }

        // Re-open only when the path changes or a previous open/seek failed.
        // Caching avoids avformat_find_stream_info on every request, which can
        // take 100ms–1s for compressed files and would stall join() on app exit.
        if (job.path != m_cachedPath || !m_decoder) {
            m_cachedPath = job.path;
            std::string openErr;
            m_decoder = mcp::AudioDecoder::open(job.path, openErr);
        }
        if (!m_decoder) continue;

        const int     fc  = m_decoder->nativeChannels();
        const int     sr  = m_decoder->nativeSampleRate();
        const int64_t nf  = m_decoder->nativeFrameCount();
        if (fc <= 0 || sr <= 0 || nf <= 0) continue;

        const double  fileDur = static_cast<double>(nf) / sr;
        const int64_t frameL  = static_cast<int64_t>(std::max(0.0, job.tL / fileDur * nf));
        const int64_t frameR  = std::min(nf, static_cast<int64_t>(job.tR / fileDur * nf) + 1);
        const int64_t count   = std::max(int64_t(1), frameR - frameL);

        if (!m_decoder->seekToFrame(frameL)) {
            m_decoder.reset();  // invalidate so next request re-opens
            continue;
        }

        const int dispCh = std::min(fc, 2);
        std::vector<float> interleaved(static_cast<size_t>(count * fc));
        const int64_t got = m_decoder->readFloat(interleaved.data(), count);
        if (got <= 0) continue;

        // Superseded while decoding — discard result and process fresher job.
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_hasJob) continue;
        }

        auto frame       = std::make_shared<SampleFrame>();
        frame->path      = job.path;
        frame->nSamples  = static_cast<int>(got);
        frame->nChannels = dispCh;
        frame->tL        = static_cast<double>(frameL) / nf * fileDur;
        frame->tR        = static_cast<double>(frameL + got) / nf * fileDur;
        for (int c = 0; c < dispCh; ++c)
            frame->ch[c].resize(static_cast<size_t>(got));
        for (int64_t i = 0; i < got; ++i)
            for (int c = 0; c < dispCh; ++c)
                frame->ch[c][i] = interleaved[i * fc + c];

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_hasJob) continue;  // check again under lock before publishing
            m_current = std::move(frame);
        }

        if (!m_stopping.load(std::memory_order_acquire))
            QMetaObject::invokeMethod(job.context, std::move(job.callback), Qt::QueuedConnection);
    }
}
