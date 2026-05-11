#pragma once

namespace mcp::plugin {

// Planar (non-interleaved) audio block passed to process().
// Samples are 32-bit float.  Buffers are owned by the caller; process() must
// not store pointers to them past the call boundary, and must not allocate.
struct AudioBlock {
    const float** inputs;        // inputs[ch][sample]  — read-only
    float**       outputs;       // outputs[ch][sample] — written by process()
    int           numInputChannels {0};
    int           numOutputChannels{0};
    int           numSamples      {0};
};

} // namespace mcp::plugin
