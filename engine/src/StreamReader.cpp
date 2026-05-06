#include "engine/StreamReader.h"
#include "engine/AudioDecoder.h"
#include <samplerate.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace mcp {

// ---------------------------------------------------------------------------
// Constructors

StreamReader::StreamReader(const std::string& path,
                            int targetSR, int /*targetCh*/,
                            std::vector<LoopSegment> segments)
    : m_path(path)
    , m_targetSR(targetSR)
    , m_segments(std::move(segments))
{
    std::string err;
    auto dec = AudioDecoder::open(path, err);
    if (!dec) { m_error = err; return; }

    m_fileSR = dec->nativeSampleRate();
    m_fileCh = dec->nativeChannels();
    const int64_t fileFrames = dec->nativeFrameCount();

    m_meta.sampleRate = m_fileSR;
    m_meta.channels   = m_fileCh;
    m_meta.frameCount = fileFrames;

    if (m_fileCh <= 0) { m_error = "invalid channel count"; return; }

    // Compute total output frames across all segments.
    if (fileFrames > 0 && m_fileSR > 0) {
        bool hasInfinite = false;
        int64_t total = 0;
        for (const auto& seg : m_segments) {
            if (seg.loops == 0) { hasInfinite = true; break; }
            const int64_t sf  = std::min(
                static_cast<int64_t>(seg.startSecs * m_fileSR), fileFrames);
            const int64_t ef  = (seg.endSecs > 0.0)
                ? std::min(static_cast<int64_t>(seg.endSecs * m_fileSR), fileFrames)
                : fileFrames;
            const int64_t len = std::max<int64_t>(0, ef - sf);
            const int64_t out = (targetSR == m_fileSR)
                ? len * seg.loops
                : static_cast<int64_t>(len * seg.loops * static_cast<double>(targetSR) / m_fileSR);
            total += out;
        }
        m_totalOutputFrames = hasInfinite ? 0 : total;
    }

    m_ring.resize(static_cast<size_t>(kRingFrames * m_fileCh), 0.0f);
    m_thread = std::thread(&StreamReader::ioThread, this);
}

// Convenience constructor: single segment, no looping.
StreamReader::StreamReader(const std::string& path,
                            int    targetSR, int targetCh,
                            double startTimeSecs, double durationSecs)
    : StreamReader(path, targetSR, targetCh, [&]{
        LoopSegment seg;
        seg.startSecs = startTimeSecs;
        seg.endSecs   = (durationSecs > 0.0) ? startTimeSecs + durationSecs : 0.0;
        seg.loops     = 1;
        return std::vector<LoopSegment>{ seg };
    }()) {}

StreamReader::~StreamReader() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

void StreamReader::requestStop() {
    m_stopThread.store(true, std::memory_order_release);
}

void StreamReader::devamp(bool stopAfter, bool preVamp) {
    m_preVamp.store(preVamp, std::memory_order_release);
    m_devampMode.store(stopAfter ? 2 : 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Routing

void StreamReader::setRouting(std::vector<float> xpGains, std::vector<float> outLevGains, int outCh) {
    m_xpGains     = std::move(xpGains);
    m_outLevGains = std::move(outLevGains);
    m_xpOutCh     = outCh;
}

void StreamReader::setOutLevelGain(int outCh, float linGain) {
    if (outCh >= 0 && outCh < (int)m_outLevGains.size())
        m_outLevGains[static_cast<size_t>(outCh)] = linGain;
}

void StreamReader::setXpointGain(int srcCh, int outCh, float linGain) {
    if (m_xpOutCh <= 0 || srcCh < 0 || outCh < 0 || outCh >= m_xpOutCh) return;
    const int idx = srcCh * m_xpOutCh + outCh;
    if (idx >= 0 && idx < (int)m_xpGains.size())
        m_xpGains[static_cast<size_t>(idx)] = linGain;
}

// ---------------------------------------------------------------------------
// Queries

bool StreamReader::isArmed() const { return available() >= kArmFrames; }

bool StreamReader::isDone() const {
    return m_fileDone.load(std::memory_order_acquire) && available() <= 0;
}

int64_t StreamReader::available() const {
    return m_writePos.load(std::memory_order_acquire)
         - m_readPos.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Read (audio callback)

int64_t StreamReader::read(float* out, int64_t frames, int outCh, float gain) {
    const int64_t avail  = m_writePos.load(std::memory_order_acquire)
                         - m_readPos.load(std::memory_order_relaxed);
    const int64_t toRead = std::min(frames, avail);
    if (toRead <= 0) return 0;

    const int   ch = m_fileCh;
    int64_t     rp = m_readPos.load(std::memory_order_relaxed) % kRingFrames;

    const bool useRouting = !m_xpGains.empty() && (m_xpOutCh == outCh);

    for (int64_t done = 0; done < toRead; ) {
        const int64_t chunk = std::min(toRead - done, kRingFrames - rp);
        const float*  src   = m_ring.data() + rp * ch;
        float*        dst   = out + done * outCh;

        if (useRouting) {
            for (int64_t f = 0; f < chunk; ++f) {
                const float* s = src + f * ch;
                float*       d = dst + f * outCh;
                for (int o = 0; o < outCh; ++o) {
                    float sum = 0.0f;
                    for (int sc = 0; sc < ch; ++sc) {
                        const float xp = m_xpGains[static_cast<size_t>(sc * outCh + o)];
                        if (!std::isnan(xp)) sum += s[sc] * xp;
                    }
                    const float outLvl = (o < (int)m_outLevGains.size())
                                         ? m_outLevGains[static_cast<size_t>(o)] : 1.0f;
                    d[o] += sum * outLvl * gain;
                }
            }
        } else {
            for (int64_t f = 0; f < chunk; ++f)
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

// ---------------------------------------------------------------------------
// I/O thread

void StreamReader::ioThread() {
    std::string err;
    auto dec = AudioDecoder::open(m_path, err);
    if (!dec) {
        m_error = err;
        m_fileDone.store(true, std::memory_order_release);
        return;
    }

    const int fileSR = dec->nativeSampleRate();
    const int fileCh = dec->nativeChannels();
    const int64_t fileFrames = dec->nativeFrameCount();

    constexpr int64_t kInputChunk = 4096;
    const bool   needSRC  = (fileSR != m_targetSR) && (m_targetSR > 0);
    const double srcRatio = needSRC
        ? static_cast<double>(m_targetSR) / fileSR : 1.0;

    const int64_t kOutputChunk = needSRC
        ? static_cast<int64_t>(kInputChunk * srcRatio * 1.05 + 128)
        : kInputChunk;

    std::vector<float> inBuf(static_cast<size_t>(kInputChunk * fileCh));
    std::vector<float> outBuf;

    SRC_STATE* srcState = nullptr;
    if (needSRC) {
        outBuf.resize(static_cast<size_t>(kOutputChunk * fileCh));
        int srcErr = 0;
        srcState = src_new(SRC_SINC_MEDIUM_QUALITY, fileCh, &srcErr);
    }

    // Iterate through segments in order.
    bool preVampActive = false;   // set when devamp fires with preVamp=true
    for (int segIdx = 0; segIdx < (int)m_segments.size(); ++segIdx) {
        const auto& seg = m_segments[static_cast<size_t>(segIdx)];
        if (m_stopThread.load(std::memory_order_relaxed)) break;
        // Pre-vamp: after devamp has fired, skip segments that loop more than once.
        if (preVampActive && seg.loops != 1) continue;

        const int64_t segStartFrame = std::min(
            static_cast<int64_t>(seg.startSecs * fileSR),
            fileFrames > 0 ? fileFrames : INT64_MAX);

        const int64_t segEndFrame = (seg.endSecs > 0.0)
            ? std::min(static_cast<int64_t>(seg.endSecs * fileSR),
                       fileFrames > 0 ? fileFrames : INT64_MAX)
            : (fileFrames > 0 ? fileFrames : INT64_MAX);

        const int64_t segNativeLen = std::max<int64_t>(0, segEndFrame - segStartFrame);
        if (segNativeLen == 0) continue;

        int loopsDone = 0;
        while ((seg.loops == 0 || loopsDone < seg.loops)
               && !m_stopThread.load(std::memory_order_relaxed)) {

            // Record segment/loop start marker BEFORE writing any audio for this
            // iteration.  writePos at this point is the ring position where the
            // first frame of this loop will be written, so that readPos can be
            // compared against it to determine the current file position.
            {
                const int mc = m_segMarkerCount.load(std::memory_order_relaxed);
                if (mc < kMaxSegMarkers) {
                    m_segMarkers[static_cast<size_t>(mc)] = {
                        m_writePos.load(std::memory_order_relaxed),
                        segIdx,
                        seg.startSecs
                    };
                    m_segMarkerCount.store(mc + 1, std::memory_order_release);
                }
            }

            dec->seekToFrame(segStartFrame);
            if (needSRC && srcState) src_reset(srcState);

            int64_t nativeLeft = segNativeLen;

            while (!m_stopThread.load(std::memory_order_relaxed) && nativeLeft > 0) {
                // Wait for ring space.
                while (!m_stopThread.load(std::memory_order_relaxed)) {
                    const int64_t space = kRingFrames
                        - (m_writePos.load(std::memory_order_relaxed)
                           - m_readPos.load(std::memory_order_acquire));
                    if (space >= kOutputChunk) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (m_stopThread.load(std::memory_order_relaxed)) break;

                const int64_t want = std::min(nativeLeft, kInputChunk);
                const int64_t got  = dec->readFloat(inBuf.data(), want);
                if (got <= 0) break;
                nativeLeft -= got;

                const float* writeData;
                int64_t      writeFrames;

                if (needSRC && srcState) {
                    const bool isLast = (nativeLeft == 0 || got < want);
                    SRC_DATA sd{};
                    sd.data_in       = inBuf.data();
                    sd.input_frames  = static_cast<long>(got);
                    sd.data_out      = outBuf.data();
                    sd.output_frames = static_cast<long>(outBuf.size() / static_cast<size_t>(fileCh));
                    sd.src_ratio     = srcRatio;
                    sd.end_of_input  = isLast ? 1 : 0;
                    src_process(srcState, &sd);
                    writeData   = outBuf.data();
                    writeFrames = sd.output_frames_gen;
                } else {
                    writeData   = inBuf.data();
                    writeFrames = got;
                }

                if (writeFrames <= 0) continue;

                int64_t wp   = m_writePos.load(std::memory_order_relaxed) % kRingFrames;
                int64_t writ = 0;
                while (writ < writeFrames) {
                    const int64_t chunk = std::min(writeFrames - writ, kRingFrames - wp);
                    std::memcpy(m_ring.data() + wp * fileCh,
                                writeData    + writ * fileCh,
                                static_cast<size_t>(chunk * fileCh) * sizeof(float));
                    writ += chunk;
                    wp = (wp + chunk) % kRingFrames;
                }
                m_writePos.fetch_add(writeFrames, std::memory_order_release);
            }

            ++loopsDone;

            // Devamp: if a signal arrived, exit the current-segment loop early.
            const int dm = m_devampMode.exchange(0, std::memory_order_acq_rel);
            if (dm != 0) {
                m_devampFired.store(true, std::memory_order_release);
                if (dm == 2) {
                    m_stopThread.store(true, std::memory_order_release);
                } else if (m_preVamp.load(std::memory_order_acquire)) {
                    preVampActive = true;
                }
                break;  // exit segment loop; outer for sees m_stopThread if dm==2
            }
        }

        // Fix: consume any devamp flag that arrived between the last per-loop
        // exchange(0) and the while-condition going false (natural segment end).
        // Without this, the flag would be picked up by the NEXT segment's first
        // loop iteration, firing devamp at the wrong boundary.
        {
            const int dm_post = m_devampMode.exchange(0, std::memory_order_acq_rel);
            if (dm_post != 0) {
                m_devampFired.store(true, std::memory_order_release);
                if (dm_post == 2) {
                    m_stopThread.store(true, std::memory_order_release);
                } else if (m_preVamp.load(std::memory_order_acquire)) {
                    preVampActive = true;
                }
                // dm_post==1: segment ended naturally at this boundary — devamp is
                // satisfied here.  preVampActive handles subsequent segment skipping.
            }
        }
    }

    if (srcState) src_delete(srcState);
    m_fileDone.store(true, std::memory_order_release);
}

} // namespace mcp
