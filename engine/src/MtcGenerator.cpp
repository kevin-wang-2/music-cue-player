#include "MtcGenerator.h"
#include "engine/AudioEngine.h"

#include <RtMidi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace mcp {

MtcGenerator::MtcGenerator(std::string midiPort, TcFps fps,
                             TcPoint startTC, TcPoint endTC,
                             int sampleRate, const AudioEngine& engine)
    : m_midiPort(std::move(midiPort))
    , m_fps(fps), m_startTC(startTC), m_endTC(endTC)
    , m_sampleRate(sampleRate), m_engine(engine)
{}

MtcGenerator::~MtcGenerator() {
    stop();
    if (m_thread.joinable()) m_thread.join();
}

void MtcGenerator::start() {
    // Capture both anchors atomically-as-possible before thread launch.
    m_engineStartFrame.store(m_engine.enginePlayheadFrames(),
                             std::memory_order_release);
    m_wallStart = std::chrono::steady_clock::now();
    m_stop.store(false, std::memory_order_release);
    m_done.store(false, std::memory_order_release);
    m_thread = std::thread(&MtcGenerator::threadProc, this);
}

void MtcGenerator::stop() {
    m_stop.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
static std::vector<unsigned char> buildFullFrameSysEx(const TcPoint& tc, int fpsCode) {
    return {
        0xF0, 0x7F, 0x7F, 0x01, 0x01,
        static_cast<unsigned char>((static_cast<unsigned char>(fpsCode) << 5) | (tc.hh & 0x1F)),
        static_cast<unsigned char>(tc.mm & 0x3F),
        static_cast<unsigned char>(tc.ss & 0x3F),
        static_cast<unsigned char>(tc.ff & 0x1F),
        0xF7
    };
}

void MtcGenerator::threadProc() {
    RtMidiOut mout;
    const unsigned int count = mout.getPortCount();
    int portIdx = -1;
    for (unsigned int i = 0; i < count; ++i) {
        if (mout.getPortName(i) == m_midiPort) {
            portIdx = static_cast<int>(i);
            break;
        }
    }
    if (portIdx < 0) { m_done.store(true, std::memory_order_release); return; }

    try { mout.openPort(static_cast<unsigned int>(portIdx)); }
    catch (...) { m_done.store(true, std::memory_order_release); return; }

    const TcRate  rate        = tcRateFor(m_fps);
    const int     fpsCode     = mtcFpsCode(m_fps);
    const int64_t startFrame  = tcToFrames(m_startTC, m_fps);
    const int64_t endFrame    = tcToFrames(m_endTC,   m_fps);
    if (endFrame <= startFrame) {
        m_done.store(true, std::memory_order_release);
        return;
    }

    const int64_t engineAnchor = m_engineStartFrame.load(std::memory_order_acquire);
    // MTC: 4 quarter-frames per video frame (NOT 8).
    // Each group of 8 QF types spans 2 video frames → tcFrameNum strides by 2.
    // Total QFs = (endFrame - startFrame) * 4 keeps total duration identical.
    const int64_t totalQFs     = (endFrame - startFrame) * 4;
    const int64_t sr           = m_sampleRate;

    // Startup: Full Frame locate, then 50 ms pause before QF stream.
    {
        const auto ff = buildFullFrameSysEx(m_startTC, fpsCode);
        try { mout.sendMessage(&ff); } catch (...) {}
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (m_stop.load(std::memory_order_relaxed)) {
        try { mout.closePort(); } catch (...) {}
        m_done.store(true, std::memory_order_release);
        return;
    }
    // Re-anchor wall clock after the 50ms pause so QF timing is accurate.
    const auto qfWallAnchor = std::chrono::steady_clock::now();

    for (int64_t qfIdx = 0; qfIdx < totalQFs; ++qfIdx) {
        if (m_stop.load(std::memory_order_relaxed)) break;

        // QF interval = 1 / (nomFPS * 4 * rateRatio) seconds per QF.
        // Audio-engine sample target:
        const int64_t targetSample = engineAnchor
            + qfIdx * sr * rate.rateDen
              / ((int64_t)rate.nomFPS * 4 * rate.rateNum);

        // Wall-clock fallback (for when audio engine is idle).
        const auto targetWall = qfWallAnchor
            + std::chrono::nanoseconds(
                qfIdx * (int64_t)1'000'000'000 * rate.rateDen
                / ((int64_t)rate.nomFPS * 4 * rate.rateNum));

        while (!m_stop.load(std::memory_order_relaxed)) {
            const int64_t now       = m_engine.enginePlayheadFrames();
            const int64_t remaining = targetSample - now;
            if (remaining <= 0) break;
            if (std::chrono::steady_clock::now() >= targetWall) break;
            if (remaining > sr / 500)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else
                std::this_thread::yield();
        }

        if (m_stop.load(std::memory_order_relaxed)) break;

        // tcFrameNum strides by 2: each group of 8 QFs covers 2 video frames.
        const int64_t tcFrameNum = startFrame + (qfIdx / 8) * 2;
        const TcPoint tc         = framesToTc(tcFrameNum, m_fps);
        const int     qfType     = static_cast<int>(qfIdx % 8);

        unsigned char dataNibble = 0;
        switch (qfType) {
            case 0: dataNibble = static_cast<unsigned char>( tc.ff        & 0x0F); break;
            case 1: dataNibble = static_cast<unsigned char>((tc.ff >> 4)  & 0x01); break;
            case 2: dataNibble = static_cast<unsigned char>( tc.ss        & 0x0F); break;
            case 3: dataNibble = static_cast<unsigned char>((tc.ss >> 4)  & 0x03); break;
            case 4: dataNibble = static_cast<unsigned char>( tc.mm        & 0x0F); break;
            case 5: dataNibble = static_cast<unsigned char>((tc.mm >> 4)  & 0x03); break;
            case 6: dataNibble = static_cast<unsigned char>( tc.hh        & 0x0F); break;
            case 7: dataNibble = static_cast<unsigned char>(
                        (static_cast<unsigned char>(fpsCode) << 1)
                        | ((tc.hh >> 4) & 0x01)); break;
        }

        const std::vector<unsigned char> msg = {
            0xF1,
            static_cast<unsigned char>((qfType << 4) | dataNibble)
        };
        try { mout.sendMessage(&msg); } catch (...) {}
    }

    // Full-frame message at end to signal completion to the receiver.
    if (!m_stop.load(std::memory_order_relaxed)) {
        const TcPoint endTc = framesToTc(endFrame, m_fps);
        const auto ff = buildFullFrameSysEx(endTc, fpsCode);
        try { mout.sendMessage(&ff); } catch (...) {}
    }

    try { mout.closePort(); } catch (...) {}
    m_done.store(true, std::memory_order_release);
}

} // namespace mcp
