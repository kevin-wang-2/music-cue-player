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
// Opens a file on construction, seeks to startFrame, and starts an I/O
// thread that fills the ring in the background.  The audio callback reads
// from the ring without ever touching disk or blocking.
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
//   For fast destruction (e.g., on stop), call requestStop() before letting
//   the shared_ptr refcount drop to zero.
class StreamReader {
public:
    static constexpr int64_t kRingFrames = 65536;  // ~1.36 s at 48 kHz stereo
    static constexpr int64_t kArmFrames  = 8192;   // ~170 ms — "armed and ready" threshold

    // Open `path`, seek to `startFrame`, start I/O thread.
    // `durationFrames == 0` means play to end of file.
    // If the file doesn't match expectedSR / expectedCh (when > 0), hasError() is set.
    StreamReader(const std::string& path,
                 int      expectedSR, int expectedCh,
                 int64_t  startFrame,
                 int64_t  durationFrames = 0);
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

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
    // Mixes up to `frames` interleaved frames into out[0..frames*outCh-1] with `gain`.
    // Returns frames actually consumed; shortfall stays as silence in the caller's
    // zeroed buffer.
    int64_t read(float* out, int64_t frames, int outCh, float gain);

    // Signal the I/O thread to stop filling the ring.  After this call
    // read() still drains whatever is already buffered but no new data arrives.
    // Safe to call from any thread; idempotent.
    void requestStop();

private:
    void ioThread();

    std::string   m_path;
    std::string   m_error;
    AudioMetadata m_meta;
    int64_t       m_startFrame{0};
    int64_t       m_durationFrames{0};

    // SPSC ring buffer: kRingFrames * channels interleaved floats.
    std::vector<float>   m_ring;
    std::atomic<int64_t> m_writePos{0};   // absolute frames written by I/O thread
    std::atomic<int64_t> m_readPos{0};    // absolute frames consumed by audio thread
    std::atomic<bool>    m_fileDone{false};
    std::atomic<bool>    m_stopThread{false};

    std::thread m_thread;
};

} // namespace mcp
