#include "engine/StreamReader.h"
#include <sndfile.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace mcp {

StreamReader::StreamReader(const std::string& path,
                            int expectedSR, int expectedCh,
                            int64_t startFrame, int64_t durationFrames)
    : m_path(path)
    , m_startFrame(startFrame)
    , m_durationFrames(durationFrames)
{
    // Probe metadata synchronously so callers can validate before starting I/O.
    SF_INFO info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (!sf) {
        m_error = sf_strerror(nullptr);
        return;
    }
    m_meta.sampleRate = info.samplerate;
    m_meta.channels   = info.channels;
    m_meta.frameCount = info.frames;
    sf_close(sf);

    if (expectedSR > 0 && info.samplerate != expectedSR) {
        m_error = "sample rate mismatch (" + std::to_string(info.samplerate)
                  + " vs " + std::to_string(expectedSR) + ")";
        return;
    }
    if (expectedCh > 0 && info.channels != expectedCh) {
        m_error = "channel count mismatch (" + std::to_string(info.channels)
                  + " vs " + std::to_string(expectedCh) + ")";
        return;
    }

    m_ring.resize(static_cast<size_t>(kRingFrames * m_meta.channels), 0.0f);
    m_thread = std::thread(&StreamReader::ioThread, this);
}

StreamReader::~StreamReader() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

void StreamReader::requestStop() {
    m_stopThread.store(true, std::memory_order_release);
}

bool StreamReader::isArmed() const {
    return available() >= kArmFrames;
}

bool StreamReader::isDone() const {
    return m_fileDone.load(std::memory_order_acquire) && available() <= 0;
}

int64_t StreamReader::available() const {
    return m_writePos.load(std::memory_order_acquire)
         - m_readPos.load(std::memory_order_relaxed);
}

int64_t StreamReader::read(float* out, int64_t frames, int outCh, float gain) {
    const int64_t avail  = m_writePos.load(std::memory_order_acquire)
                         - m_readPos.load(std::memory_order_relaxed);
    const int64_t toRead = std::min(frames, avail);
    if (toRead <= 0) return 0;

    const int   ch   = m_meta.channels;
    int64_t     rp   = m_readPos.load(std::memory_order_relaxed) % kRingFrames;
    int64_t     done = 0;

    // At most two segments due to ring wrap-around.
    while (done < toRead) {
        const int64_t chunk = std::min(toRead - done, kRingFrames - rp);
        const float*  src   = m_ring.data() + rp * ch;
        float*        dst   = out + done * outCh;
        for (int64_t f = 0; f < chunk; ++f) {
            for (int c = 0; c < outCh; ++c) {
                const int sc = (c < ch) ? c : (ch - 1);
                dst[f * outCh + c] += src[f * ch + sc] * gain;
            }
        }
        done += chunk;
        rp = (rp + chunk) % kRingFrames;
    }

    m_readPos.fetch_add(toRead, std::memory_order_release);
    return toRead;
}

void StreamReader::ioThread() {
    SF_INFO info{};
    SNDFILE* sf = sf_open(m_path.c_str(), SFM_READ, &info);
    if (!sf) {
        m_error = sf_strerror(nullptr);
        m_fileDone.store(true, std::memory_order_release);
        return;
    }

    if (m_startFrame > 0)
        sf_seek(sf, m_startFrame, SEEK_SET);

    constexpr int64_t kChunkFrames = 4096;
    std::vector<float> tmp(static_cast<size_t>(kChunkFrames * m_meta.channels));
    int64_t framesLeft = (m_durationFrames > 0) ? m_durationFrames : INT64_MAX;

    while (!m_stopThread.load(std::memory_order_relaxed) && framesLeft > 0) {
        // Wait for space in the ring.
        while (!m_stopThread.load(std::memory_order_relaxed)) {
            const int64_t space = kRingFrames
                - (m_writePos.load(std::memory_order_relaxed)
                   - m_readPos.load(std::memory_order_acquire));
            if (space >= kChunkFrames) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (m_stopThread.load(std::memory_order_relaxed)) break;

        const sf_count_t want = static_cast<sf_count_t>(
            std::min(framesLeft, kChunkFrames));
        const sf_count_t got = sf_readf_float(sf, tmp.data(), want);
        if (got <= 0) break;

        // Write to ring (handle wrap-around).
        int64_t wp   = m_writePos.load(std::memory_order_relaxed) % kRingFrames;
        int64_t writ = 0;
        while (writ < got) {
            const int64_t chunk = std::min(got - writ, kRingFrames - wp);
            std::memcpy(m_ring.data() + wp * m_meta.channels,
                        tmp.data()   + writ * m_meta.channels,
                        static_cast<size_t>(chunk * m_meta.channels) * sizeof(float));
            writ += chunk;
            wp = (wp + chunk) % kRingFrames;
        }
        m_writePos.fetch_add(got, std::memory_order_release);
        framesLeft -= got;
    }

    sf_close(sf);
    m_fileDone.store(true, std::memory_order_release);
}

} // namespace mcp
