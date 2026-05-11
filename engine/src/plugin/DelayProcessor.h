#pragma once
#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/ParameterInfo.h"
#include <vector>

namespace mcp::plugin {

// Pure delay line (no feedback) with wet/dry mix.
// Registered as:  internal.delay.mono   (1 ch)
//                 internal.delay.stereo  (2 ch)
class DelayProcessor final : public AudioProcessor {
public:
    static constexpr float kMaxDelayMs = 2000.0f;

    explicit DelayProcessor(int numChannels);

    void prepare(const ProcessContext& ctx) override;
    void process(const AudioBlock& block, const EventBlock& events) override;
    void reset()   override;

    int getLatencySamples() const override { return 0; }

    const std::vector<ParameterInfo>& getParameters()              const override;
    float getParameterValue          (const std::string& id)       const override;
    void  setParameterValue          (const std::string& id, float value) override;
    float getNormalizedParameterValue(const std::string& id)       const override;
    void  setNormalizedParameterValue(const std::string& id, float n)     override;

    PluginState getState()                    const override;
    void        setState(const PluginState& s)      override;

private:
    void updateDelaySamples();

    int          m_numChannels;
    ParameterSet m_params;
    int          m_delayMsIdx{0};
    int          m_mixIdx    {1};

    double m_sampleRate   {44100.0};
    int    m_delaySamples {0};
    int    m_writePos     {0};

    // One ring buffer per channel, allocated in prepare().
    std::vector<std::vector<float>> m_buffers;
};

} // namespace mcp::plugin
