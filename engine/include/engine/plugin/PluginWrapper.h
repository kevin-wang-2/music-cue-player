#pragma once
#include "AudioBlock.h"
#include "AudioProcessor.h"
#include "EventBlock.h"
#include "ProcessContext.h"
#include <memory>
#include <vector>

namespace mcp::plugin {

// Wraps an AudioProcessor with host-level conveniences: bypass, input/output
// gain staging.  process() is realtime-safe: no allocation, no locking.
//
// Bypass behaviour (v1): when bypassed, inputs are copied directly to outputs
// without calling the inner processor.  This may introduce a brief click on
// the bypass transition; a proper crossfade is deferred (TODO).
class PluginWrapper {
public:
    enum class Status { Idle, Prepared, Error };

    // Takes ownership of processor; stored internally as shared_ptr.
    explicit PluginWrapper(std::unique_ptr<AudioProcessor> processor);

    // Control-thread methods.
    void prepare(const ProcessContext& context);
    void reset();

    // Realtime-safe.
    void process(const AudioBlock& block, const EventBlock& events);

    // Bypass.
    bool isBypassed()        const { return m_bypassed; }
    void setBypassed(bool v)       { m_bypassed = v; }

    // Gain staging (dB).  Default 0 dB (unity).
    float getInputGainDb()  const;
    void  setInputGainDb (float db);
    float getOutputGainDb() const;
    void  setOutputGainDb(float db);

    int    getLatencySamples() const;
    Status getStatus()         const { return m_status; }

    // Direct access to the wrapped processor for parameter/state operations.
    AudioProcessor*                    getProcessor()       const { return m_processor.get(); }
    std::shared_ptr<AudioProcessor>    getProcessorShared() const { return m_processor; }

private:
    static float dbToLinear(float db);

    std::shared_ptr<AudioProcessor> m_processor;
    Status m_status          {Status::Idle};
    bool   m_bypassed        {false};
    float  m_inputGainLinear {1.0f};
    float  m_outputGainLinear{1.0f};
    int    m_numInputChannels {0};
    int    m_maxBlockSize     {0};

    // Pre-allocated scratch buffers for input gain scaling.
    // Resized in prepare(); not touched in process().
    std::vector<float>   m_inputScratchData;
    std::vector<float*>  m_inputScratchPtrs;  // write pointers into m_inputScratchData
    std::vector<const float*> m_inputConstPtrs;  // const view for AudioBlock.inputs
};

} // namespace mcp::plugin
