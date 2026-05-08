#pragma once

#include "AudioFile.h"
#include "IAudioSource.h"
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

class StreamReader : public IAudioSource {
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
    ~StreamReader() override;
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    // Number of frames this reader will output at targetSR.
    // Returns 0 if the file length is unknown (use isDone() to detect end).
    int64_t totalOutputFrames() const override { return m_totalOutputFrames; }

    // True once >= kArmFrames frames are buffered and ready for glitch-free start.
    bool isArmed() const override;

    // True when the file is fully consumed and the ring buffer is empty.
    bool isDone() const override;

    // True if opening / format validation failed.
    bool hasError() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }

    const AudioMetadata& metadata() const { return m_meta; }

    // Frames buffered and available to read right now.
    int64_t available() const;

    // Set the crosspoint routing matrix.  Call once before the voice starts.
    // xpGains[s * outCh + o] = linear gain from source channel s to output o.
    // NaN = no route.  outLevGains[o] = per-output-channel linear gain (1.0 = unity).
    void setRouting(std::vector<float> xpGains, std::vector<float> outLevGains, int outCh) override;

    // Safely update one output-channel level during playback (e.g. from a fade).
    // Plain float store — benign race; the audio callback reads it independently.
    void setOutLevelGain(int outCh, float linGain) override;

    // Safely update one crosspoint cell during playback.  NaN = disable route.
    void setXpointGain(int srcCh, int outCh, float linGain) override;

    // Read current live gains (NaN if not set / out of range).
    float getXpointGain(int srcCh, int outCh) const override;
    float getOutLevelGain(int outCh) const override;

    // Return output channel count set by setRouting(); 0 if routing not set.
    int xpOutCh() const override { return m_xpOutCh; }

    // Called ONLY from the audio callback (wait-free on the consumer side).
    // Mixes up to `frames` interleaved frames into out[0..frames*outCh-1] with
    // `gain`.  Returns frames actually consumed.
    int64_t read(float* out, int64_t frames, int outCh, float gain) override;

    // Signal the I/O thread to stop.  Idempotent, safe from any thread.
    void requestStop() override;

    // Devamp: after the current loop iteration of the current segment finishes,
    // either advance to the next segment (stopAfter=false) or stop completely
    // (stopAfter=true).  If preVamp=true, subsequent segments with loops != 1 are
    // automatically skipped after the devamp point.
    // Safe to call from any thread during playback.
    void devamp(bool stopAfter, bool preVamp = false);

    // True once the IO thread has processed a devamp signal (segment transition
    // occurred or stop was initiated).  Cleared by clearDevampFired().
    bool wasDevampFired() const { return m_devampFired.load(std::memory_order_acquire); }
    void clearDevampFired()     { m_devampFired.store(false, std::memory_order_release); }

    // Segment/loop start markers written by the IO thread.
    // Each entry records the ring writePos at the start of a loop iteration,
    // the segment index, and the file start position (seconds).
    // Consumers compare the current readPos against these to find the
    // file position currently being played.
    struct SegMarker {
        int64_t writePos{0};
        int     segIdx{0};
        double  fileStartSecs{0.0};
    };
    static constexpr int kMaxSegMarkers = 512;

    int       segMarkerCount()  const { return m_segMarkerCount.load(std::memory_order_acquire); }
    SegMarker segMarkerAt(int i) const { return m_segMarkers[static_cast<size_t>(i)]; }
    int       targetSampleRate() const { return m_targetSR; }

    // Frames consumed by the audio callback so far (monotonically increasing).
    // Directly reflects the ring-buffer read position — more accurate than the
    // engine's global playhead counter for determining file position.
    int64_t   readPos() const override { return m_readPos.load(std::memory_order_acquire); }

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
    std::atomic<int>     m_devampMode{0};    // 0=none, 1=nextSlice, 2=stop
    std::atomic<bool>    m_devampFired{false};
    std::atomic<bool>    m_preVamp{false};   // skip looping segments after devamp

    // Segment/loop start markers (written by IO thread only).
    std::array<SegMarker, kMaxSegMarkers> m_segMarkers{};
    std::atomic<int>                      m_segMarkerCount{0};

    // Routing state (set once before voice starts; outLevGains may be updated live).
    std::vector<float> m_xpGains;     // [m_fileCh * m_xpOutCh], NaN = no route
    std::vector<float> m_outLevGains; // [m_xpOutCh], 1.0f = unity
    int                m_xpOutCh{0};

    std::thread m_thread;
};

} // namespace mcp
