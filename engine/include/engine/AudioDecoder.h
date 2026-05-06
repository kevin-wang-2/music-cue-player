#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace mcp {

// Format-agnostic audio file reader.
// Open via AudioDecoder::open() — tries libsndfile first, then FFmpeg.
// All methods are NOT thread-safe; use from a single thread only.
class AudioDecoder {
public:
    // Open path with the best available backend.
    // Returns nullptr and fills `error` on failure.
    static std::unique_ptr<AudioDecoder> open(const std::string& path,
                                               std::string& error);

    virtual ~AudioDecoder() = default;

    virtual int     nativeSampleRate() const = 0;
    virtual int     nativeChannels()   const = 0;
    virtual int64_t nativeFrameCount() const = 0;  // -1 if unknown

    // Seek to frame position (in the file's native sample rate).
    virtual bool    seekToFrame(int64_t frame) = 0;

    // Read up to nFrames interleaved float frames into buf.
    // Returns actual frames read; 0 == EOF.
    virtual int64_t readFloat(float* buf, int64_t nFrames) = 0;
};

} // namespace mcp
