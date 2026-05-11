#pragma once
#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/ParameterInfo.h"

namespace mcp::plugin {

// Applies a fixed gain (in dB) to all channels.
// Registered as:  internal.trim.mono  (1 ch)
//                 internal.trim.stereo (2 ch)
class TrimProcessor final : public AudioProcessor {
public:
    explicit TrimProcessor(int numChannels);

    void prepare(const ProcessContext& ctx) override;
    void process(const AudioBlock& block, const EventBlock& events) override;
    void reset()   override {}

    int getLatencySamples() const override { return 0; }

    const std::vector<ParameterInfo>& getParameters()              const override;
    float getParameterValue          (const std::string& id)       const override;
    void  setParameterValue          (const std::string& id, float value) override;
    float getNormalizedParameterValue(const std::string& id)       const override;
    void  setNormalizedParameterValue(const std::string& id, float n)     override;

    PluginState getState()                   const override;
    void        setState(const PluginState& s)     override;

private:
    static float dbToLinear(float db);

    int          m_numChannels;
    ParameterSet m_params;
    int          m_gainDbIdx{0};  // cached index for gain_db
    float        m_gainLinear{1.0f};  // computed at prepare() / event time
    bool         m_prepared  {false};
};

} // namespace mcp::plugin
