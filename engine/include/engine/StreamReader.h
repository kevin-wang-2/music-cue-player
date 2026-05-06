#pragma once

#include "AudioFile.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

// Background-streaming audio reader with a lock-free SPSC ring buffer.
//
// Opens a file (via AudioDecoder — libsndfile first, then FFmpeg), seeks to
// startTimeSecs, and starts an I/O thread that reads, optionally resamples
// (libsamplerate), and fills the ring in the background.  The audio callback
// reads from the ring without ever touching disk or blocking.
//
// Channel routing
// ---------------
//   setRouting() stores a crosspoint matrix (xpGains) and per-output-channel
//   level gains (outLevGains).  read() applies them during mixing:
//     out[o] += gain * outLevGain[o] * sum_s(ring[s] * xpGains[s*outCh+o])
//   NaN in xpGains[s*outCh+o] means no route from source s to output o.
//   setRouting() must be called before the voice starts playing.
//   setOutLevelGain() is safe to call from any thread during playback.
//
// Thread model
// ------------
//   I/O thread   — sole writer (ioThread): fills ring from disk
//   Audio thread — sole reader (read):     consumes ring frames, mixes into output
//   Any thread   — isArmed(), isDone(), available(), requestStop(), setOutLevelGain()
//
// Lifetime
// --------
//   ~StreamReader() calls requestStop() then joins the I/O thread.
//   For fast destruction call requestStop() before dropping the last reference.
// One contiguous region of a file, played `loops` times in sequence.
// endSecs == 0.0 means "to end of file from startSecs".
// loops  == 0   means infinite (play until requestStop()).
struct LoopSegment {
    double startSecs{0.0};
    double endSecs{0.0};
    int    loops{1};
};

class StreamReader {
public:
    static constexpr int64_t kRingFrames = 65536;  // ~1.36 s at 48 kHz
    static constexpr int64_t kArmFrames  = 8192;   // ~170 ms arm threshold

    // Multi-segment constructor.  Segments are played in order; each is
    // looped `segment.loops` times before advancing to the next one.
    StreamReader(const std::string& path,
                 int targetSR, int targetCh,
                 std::vector<LoopSegment> segments);

    // Convenience constructor for the common single-region, non-looping case.
    StreamReader(const std::string& path,
                 int    targetSR, int targetCh,
                 double startTimeSecs  = 0.0,
                 double durationSecs   = 0.0);
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    // Number of frames this reader will output at targetSR.
    // Returns 0 if the file length is unknown (use isDone() to detect end).
    int64_t totalOutputFrames() const { return m_totalOutputFrames; }

    // True once >= kArmFrames frames are buffered and ready for glitch-free start.
    bool isArmed() const;

    // True when the file is fully consumed and the ring buffer is empty.
    bool isDone() const;

    // True if opening / format validation failed.
    bool hasError() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }

    const AudioMetadata& metadata() const { return m_meta; }

    // Frames buffered and available to read right now.
    int64_t available() const;

    // Set the crosspoint routing matrix.  Call once before the voice starts.
    // xpGains[s * outCh + o] = linear gain from source channel s to output o.
    // NaN = no route.  outLevGains[o] = per-output-channel linear gain (1.0 = unity).
    void setRouting(std::vector<float> xpGains, std::vector<float> outLevGains, int outCh);

    // Safely update one output-channel level during playback (e.g. from a fade).
    // Plain float store — benign race; the audio callback reads it independently.
    void setOutLevelGain(int outCh, float linGain);

    // Safely update one crosspoint cell during playback.  NaN = disable route.
    void setXpointGain(int srcCh, int outCh, float linGain);

    // Return output channel count set by setRouting(); 0 if routing not set.
    int xpOutCh() const { return m_xpOutCh; }

    // Called ONLY from the audio callback (wait-free on the consumer side).
    // Mixes up to `frames` interleaved frames into out[0..frames*outCh-1] with
    // `gain`.  Returns frames actually consumed.
    int64_t read(float* out, int64_t frames, int outCh, float gain);

    // Signal the I/O thread to stop.  Idempotent, safe from any thread.
    void requestStop();

private:
    void ioThread();

    std::string              m_path;
    std::string              m_error;
    AudioMetadata            m_meta;
    int                      m_targetSR{0};
    int                      m_fileSR{0};
    int                      m_fileCh{0};
    std::vector<LoopSegment> m_segments;
    int64_t                  m_totalOutputFrames{0};

    // SPSC ring: kRingFrames * fileCh interleaved floats, at targetSR.
    std::vector<float>   m_ring;
    std::atomic<int64_t> m_writePos{0};
    std::atomic<int64_t> m_readPos{0};
    std::atomic<bool>    m_fileDone{false};
    std::atomic<bool>    m_stopThread{false};

    // Routing state (set once before voice starts; outLevGains may be updated live).
    std::vector<float> m_xpGains;     // [m_fileCh * m_xpOutCh], NaN = no route
    std::vector<float> m_outLevGains; // [m_xpOutCh], 1.0f = unity
    int                m_xpOutCh{0};

    std::thread m_thread;
};

} // namespace mcp
