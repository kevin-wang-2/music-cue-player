#pragma once
#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/ExternalPluginReference.h"

namespace mcp::plugin {

// Passthrough AudioProcessor used when an external plugin cannot be loaded.
//
// Behaviour contract:
//   process()   — copies inputs → outputs; preserves signal flow
//   getState()  — returns a PluginState that contains the original stateBlob
//                 and paramSnapshot, so the data is never lost
//   setState()  — no-op; the preserved reference is not modified
//   getParameters() — returns an empty list; no automation
//
// The ExternalPluginReference is kept verbatim so the host can:
//   (a) display a "missing plugin" warning with the original name/backend
//   (b) retry loading after the user installs the missing plugin
//   (c) save/reload the showfile without data loss
class MissingPluginProcessor final : public AudioProcessor {
public:
    explicit MissingPluginProcessor(ExternalPluginReference ref,
                                    PluginRuntimeStatus     reason,
                                    std::string             message = {});

    void prepare(const ProcessContext& ctx) override;
    void process(const AudioBlock& block, const EventBlock& events) override;
    void reset() override {}

    const std::vector<ParameterInfo>& getParameters()                      const override;
    float getParameterValue          (const std::string& /*id*/)           const override { return 0.0f; }
    void  setParameterValue          (const std::string& /*id*/, float)          override {}
    float getNormalizedParameterValue(const std::string& /*id*/)           const override { return 0.0f; }
    void  setNormalizedParameterValue(const std::string& /*id*/, float)          override {}

    PluginState getState()                       const override;
    void        setState(const PluginState&)           override {} // preserved, never overwritten

    PluginRuntimeStatus           runtimeStatus()  const;
    const ExternalPluginReference& reference()     const;

private:
    ExternalPluginReference    m_ref;
    PluginRuntimeStatus        m_status;
    std::string                m_message;
    std::vector<ParameterInfo> m_emptyParams;
};

} // namespace mcp::plugin
