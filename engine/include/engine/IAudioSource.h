#pragma once

#include <cstdint>
#include <vector>

namespace mcp {

// Abstract audio source consumed by the AudioEngine voice pool.
// StreamReader (file-backed) and LtcStreamReader (generated) both implement this.
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // Called ONLY from the audio callback.
    // Mixes up to `frames` interleaved frames into out[0..frames*outCh-1] with `gain`.
    // Returns frames actually consumed.
    virtual int64_t read(float* out, int64_t frames, int outCh, float gain) = 0;

    // True once enough samples are buffered for glitch-free playback start.
    virtual bool isArmed() const = 0;

    // True when the source is fully consumed and the output buffer is empty.
    virtual bool isDone() const = 0;

    // Signal the background thread to stop. Idempotent, safe from any thread.
    virtual void requestStop() = 0;

    // Routing — set the crosspoint matrix before the voice starts.
    virtual void setRouting(std::vector<float> xpGains, std::vector<float> outLevGains, int outCh) = 0;
    virtual void setOutLevelGain(int outCh, float linGain) = 0;
    // Update a single crosspoint cell during playback (e.g. from a fade).
    // Default no-op — only meaningful for sources with a mutable xpoint matrix.
    virtual void setXpointGain(int /*srcCh*/, int /*outCh*/, float /*linGain*/) {}
    virtual float getXpointGain(int srcCh, int outCh) const = 0;
    virtual float getOutLevelGain(int outCh) const = 0;
    virtual int   xpOutCh() const = 0;

    virtual int64_t totalOutputFrames() const = 0;
    virtual int64_t readPos() const = 0;
};

} // namespace mcp
