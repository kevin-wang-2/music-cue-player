#pragma once
#include "PluginWrapper.h"
#include <array>
#include <memory>
#include <vector>

namespace mcp::plugin {

// RT-safe per-channel plugin chain.  Holds up to kMaxSlots PluginWrapper slots.
// process() deinterleaves one or two columns from an interleaved channel bus,
// runs the chain, and writes back — zero allocation in the audio thread.
//
// numChannels: 1 = mono, 2 = stereo pair (reads/writes logicalCh and logicalCh+1).
class ChannelPluginChain {
public:
    static constexpr int kMaxSlots = 16;

    explicit ChannelPluginChain(int numChannels = 1);

    // Control-thread: allocate scratch buffers and call prepare() on all loaded slots.
    void prepare(double sampleRate, int maxBlockSize);

    // RT-safe.  chanBuf layout: chanBuf[frame * totalCh + ch].
    // Processes columns [logicalCh … logicalCh + numChannels) in-place.
    void process(float* chanBuf, int totalCh, int numSamples, int logicalCh);

    // Control-thread slot management.
    void setSlot(int slot, std::shared_ptr<PluginWrapper> wrapper);
    void clearSlot(int slot);
    std::shared_ptr<PluginWrapper> getSlot(int slot) const;
    int numChannels() const { return m_numChannels; }

private:
    int    m_numChannels {1};
    double m_sampleRate  {48000.0};
    int    m_maxBlockSize{0};

    std::array<std::shared_ptr<PluginWrapper>, kMaxSlots> m_slots;

    // Ping-pong scratch: two halves, each numChannels × maxBlockSize floats.
    std::vector<float>         m_scratchData[2];
    std::vector<float*>        m_scratchWrite[2];  // mutable pointers for AudioBlock.outputs
    std::vector<const float*>  m_scratchRead [2];  // const pointers for AudioBlock.inputs
};

} // namespace mcp::plugin
