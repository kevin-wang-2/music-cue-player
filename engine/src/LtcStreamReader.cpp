#include "LtcStreamReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace mcp {

LtcStreamReader::LtcStreamReader(TcFps fps, TcPoint startTC, TcPoint endTC, int targetSR)
    : m_fps(fps), m_startTC(startTC), m_endTC(endTC), m_targetSR(targetSR)
{
    m_ring.resize(static_cast<size_t>(kRingFrames), 0.0f);
    m_totalFrames = tcRangeSamples(startTC, endTC, fps, targetSR);
    m_thread = std::thread(&LtcStreamReader::genThread, this);
}

LtcStreamReader::~LtcStreamReader() {
    requestStop();
    if (m_thread.joinable()) m_thread.join();
}

void LtcStreamReader::requestStop() {
    m_stopThread.store(true, std::memory_order_release);
}

bool LtcStreamReader::isArmed() const {
    return (m_writePos.load(std::memory_order_acquire)
            - m_readPos.load(std::memory_order_relaxed)) >= kArmFrames
           || m_genDone.load(std::memory_order_acquire);
}

bool LtcStreamReader::isDone() const {
    return m_genDone.load(std::memory_order_acquire)
        && (m_writePos.load(std::memory_order_acquire)
            == m_readPos.load(std::memory_order_relaxed));
}

int64_t LtcStreamReader::read(float* out, int64_t frames, int outCh, float gain) {
    const int64_t wp    = m_writePos.load(std::memory_order_acquire);
    const int64_t rp    = m_readPos.load(std::memory_order_relaxed);
    const int64_t avail = wp - rp;
    const int64_t toRead = std::min(frames, avail);
    if (toRead <= 0) return 0;

    if (m_xpOutCh == outCh && outCh > 0) {
        for (int64_t f = 0; f < toRead; ++f) {
            const float sample = m_ring[static_cast<size_t>((rp + f) % kRingFrames)];
            for (int o = 0; o < outCh; ++o) {
                const float xp = (o < (int)m_xpGains.size())
                    ? m_xpGains[static_cast<size_t>(o)]
                    : std::numeric_limits<float>::quiet_NaN();
                if (!std::isnan(xp)) {
                    const float ol = (o < (int)m_outLevGains.size())
                        ? m_outLevGains[static_cast<size_t>(o)] : 1.0f;
                    out[f * outCh + o] += sample * xp * ol * gain;
                }
            }
        }
    }

    m_readPos.fetch_add(toRead, std::memory_order_release);
    return toRead;
}

void LtcStreamReader::setRouting(std::vector<float> xpGains,
                                  std::vector<float> outLevGains, int outCh) {
    m_xpGains     = std::move(xpGains);
    m_outLevGains = std::move(outLevGains);
    m_xpOutCh     = outCh;
}

void LtcStreamReader::setOutLevelGain(int outCh, float linGain) {
    if (outCh >= 0 && outCh < (int)m_outLevGains.size())
        m_outLevGains[static_cast<size_t>(outCh)] = linGain;
}

float LtcStreamReader::getXpointGain(int srcCh, int outCh) const {
    if (srcCh != 0 || outCh < 0 || outCh >= (int)m_xpGains.size())
        return std::numeric_limits<float>::quiet_NaN();
    return m_xpGains[static_cast<size_t>(outCh)];
}

float LtcStreamReader::getOutLevelGain(int outCh) const {
    if (outCh < 0 || outCh >= (int)m_outLevGains.size()) return 1.0f;
    return m_outLevGains[static_cast<size_t>(outCh)];
}

// ---------------------------------------------------------------------------
// Background generation thread — fills the ring with BMC-encoded LTC audio.
// ---------------------------------------------------------------------------
void LtcStreamReader::genThread() {
    const TcRate rate = tcRateFor(m_fps);
    const bool   df   = rate.dropFrame;

    const int64_t startFrameNum = tcToFrames(m_startTC, m_fps);
    const int64_t endFrameNum   = tcToFrames(m_endTC,   m_fps);
    if (endFrameNum <= startFrameNum) { m_genDone.store(true, std::memory_order_release); return; }

    const int64_t totalBits = (endFrameNum - startFrameNum) * 80;

    bool frameBits[80];
    int64_t curTcFrame = startFrameNum;
    buildLtcFrameBits(framesToTc(curTcFrame, m_fps), df, frameBits);

    bool polarity = false;

    for (int64_t bitIdx = 0; bitIdx < totalBits; ++bitIdx) {
        if (m_stopThread.load(std::memory_order_relaxed)) break;

        const int64_t samStart = bitStartSample(bitIdx,     m_fps, m_targetSR);
        const int64_t samEnd   = bitStartSample(bitIdx + 1, m_fps, m_targetSR);
        const int64_t bitDur   = samEnd - samStart;
        const int64_t midOff   = bitMidSample(bitIdx, m_fps, m_targetSR) - samStart;
        const bool    bitVal   = frameBits[static_cast<int>(bitIdx % 80)];

        // Wait for ring space
        while (!m_stopThread.load(std::memory_order_relaxed)) {
            const int64_t wp = m_writePos.load(std::memory_order_relaxed);
            const int64_t rp = m_readPos.load(std::memory_order_acquire);
            if (kRingFrames - (wp - rp) >= bitDur) break;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (m_stopThread.load(std::memory_order_relaxed)) break;

        const int64_t wp = m_writePos.load(std::memory_order_relaxed);

        // BMC encoding: transition at start of every bit.
        polarity = !polarity;

        for (int64_t s = 0; s < bitDur; ++s) {
            if (bitVal && s == midOff) polarity = !polarity;
            m_ring[static_cast<size_t>((wp + s) % kRingFrames)] =
                polarity ? 1.0f : -1.0f;
        }

        m_writePos.fetch_add(bitDur, std::memory_order_release);

        // Advance to next LTC frame when we finish bit 79
        if (bitIdx % 80 == 79) {
            ++curTcFrame;
            if (curTcFrame < endFrameNum)
                buildLtcFrameBits(framesToTc(curTcFrame, m_fps), df, frameBits);
        }
    }

    m_genDone.store(true, std::memory_order_release);
}

} // namespace mcp
