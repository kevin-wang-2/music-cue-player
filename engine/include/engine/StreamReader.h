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
// Sample-rate conversion (SRC) is applied transparently when the file's native
// sample rate differs from targetSR.  Channel mapping (mono↔stereo) is handled
// in read() — the ring always stores native-channel data at targetSR.
//
// Thread model
// ------------
//   I/O thread   — sole writer (ioThread): fills ring from disk
//   Audio thread — sole reader (read):     consumes ring frames, mixes into output
//   Any thread   — isArmed(), isDone(), available(), requestStop()
//
// Lifetime
// --------
//   ~StreamReader() calls requestStop() then joins the I/O thread.
//   For fast destruction call requestStop() before dropping the last reference.
class StreamReader {
public:
    static constexpr int64_t kRingFrames = 65536;  // ~1.36 s at 48 kHz
    static constexpr int64_t kArmFrames  = 8192;   // ~170 ms arm threshold

    // Open `path`, seek to startTimeSecs, start I/O thread.
    // targetSR / targetCh: engine's sample rate and channel count.
    // durationSecs == 0 means play to end of file.
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

    // True once ≥ kArmFrames frames are buffered and ready for glitch-free start.
    bool isArmed() const;

    // True when the file is fully consumed and the ring buffer is empty.
    bool isDone() const;

    // True if opening / format validation failed.
    bool hasError() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }

    const AudioMetadata& metadata() const { return m_meta; }

    // Frames buffered and available to read right now.
    int64_t available() const;

    // Called ONLY from the audio callback (wait-free on the consumer side).
    // Mixes up to `frames` interleaved frames into out[0..frames*outCh-1] with
    // `gain`.  Returns frames actually consumed.
    int64_t read(float* out, int64_t frames, int outCh, float gain);

    // Signal the I/O thread to stop.  Idempotent, safe from any thread.
    void requestStop();

private:
    void ioThread();

    std::string   m_path;
    std::string   m_error;
    AudioMetadata m_meta;        // file-native metadata (for display / duration calc)
    int           m_targetSR{0};
    int           m_fileSR{0};
    int           m_fileCh{0};
    double        m_startTimeSecs{0.0};
    double        m_durationSecs{0.0};
    int64_t       m_totalOutputFrames{0};  // at targetSR; 0 = unknown

    // SPSC ring: kRingFrames * fileCh interleaved floats, at targetSR.
    std::vector<float>   m_ring;
    std::atomic<int64_t> m_writePos{0};
    std::atomic<int64_t> m_readPos{0};
    std::atomic<bool>    m_fileDone{false};
    std::atomic<bool>    m_stopThread{false};

    std::thread m_thread;
};

} // namespace mcp
