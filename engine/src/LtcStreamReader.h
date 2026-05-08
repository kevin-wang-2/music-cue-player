#pragma once

#include "engine/IAudioSource.h"
#include "engine/Timecode.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace mcp {

// Streaming LTC (Linear Timecode) audio generator.
//
// Generates BMC-encoded LTC audio for [startTC, endTC) into a lock-free SPSC
// ring buffer on a background thread.  The audio callback reads from the ring
// via read() without blocking — exactly the same model as StreamReader.
//
// The source has 1 logical audio channel.  Route it to a physical output
// channel by calling setRouting() before scheduling the voice.
class LtcStreamReader : public IAudioSource {
public:
    static constexpr int64_t kRingFrames = 65536;
    static constexpr int64_t kArmFrames  = 8192;

    LtcStreamReader(TcFps fps, TcPoint startTC, TcPoint endTC, int targetSR);
    ~LtcStreamReader() override;

    LtcStreamReader(const LtcStreamReader&) = delete;
    LtcStreamReader& operator=(const LtcStreamReader&) = delete;

    // IAudioSource
    int64_t read(float* out, int64_t frames, int outCh, float gain) override;
    bool    isArmed() const override;
    bool    isDone()  const override;
    void    requestStop() override;

    void    setRouting(std::vector<float> xpGains, std::vector<float> outLevGains,
                       int outCh) override;
    void    setOutLevelGain(int outCh, float linGain) override;
    float   getXpointGain(int srcCh, int outCh) const override;
    float   getOutLevelGain(int outCh) const override;
    int     xpOutCh() const override { return m_xpOutCh; }
    int64_t totalOutputFrames() const override { return m_totalFrames; }
    int64_t readPos() const override {
        return m_readPos.load(std::memory_order_acquire);
    }

private:
    void genThread();

    TcFps    m_fps;
    TcPoint  m_startTC;
    TcPoint  m_endTC;
    int      m_targetSR{48000};
    int64_t  m_totalFrames{0};

    // Mono SPSC ring buffer
    std::vector<float>   m_ring;
    std::atomic<int64_t> m_writePos{0};
    std::atomic<int64_t> m_readPos{0};
    std::atomic<bool>    m_genDone{false};
    std::atomic<bool>    m_stopThread{false};

    // Routing (1 source channel × m_xpOutCh output channels)
    std::vector<float> m_xpGains;     // [m_xpOutCh], NaN = no route
    std::vector<float> m_outLevGains; // [m_xpOutCh]
    int                m_xpOutCh{0};

    std::thread m_thread;
};

} // namespace mcp
