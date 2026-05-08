#pragma once

#include "engine/Timecode.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace mcp {

class AudioEngine;  // forward declaration — full header only needed in .cpp

// Sends MIDI Timecode (MTC) quarter-frame messages to a named MIDI output port.
//
// Timing is locked to the AudioEngine sample counter when it is running.
// If the audio engine is idle (enginePlayheadFrames() not advancing), the
// thread falls back to the wall-clock anchor captured at start() so MTC
// still runs at the correct rate.  Each QF target is computed as:
//   engineStartFrame + qfIdx * SR * rateDen / (nomFPS * 8 * rateNum)
// or equivalently (wall-clock fallback):
//   wallStart + qfIdx * rateDen * 1e9 / (nomFPS * 8 * rateNum)  nanoseconds
class MtcGenerator {
public:
    MtcGenerator(std::string midiPort, TcFps fps,
                 TcPoint startTC, TcPoint endTC,
                 int sampleRate, const AudioEngine& engine);
    ~MtcGenerator();

    MtcGenerator(const MtcGenerator&) = delete;
    MtcGenerator& operator=(const MtcGenerator&) = delete;

    // Capture engine anchor + wall-clock anchor, then launch the thread.
    void start();
    void stop();

    bool isDone() const { return m_done.load(std::memory_order_acquire); }

private:
    void threadProc();

    std::string         m_midiPort;
    TcFps               m_fps;
    TcPoint             m_startTC;
    TcPoint             m_endTC;
    int                 m_sampleRate;
    const AudioEngine&  m_engine;

    std::atomic<int64_t> m_engineStartFrame{0};
    std::chrono::steady_clock::time_point m_wallStart;
    std::atomic<bool>    m_stop{false};
    std::atomic<bool>    m_done{false};

    std::thread m_thread;
};

} // namespace mcp
