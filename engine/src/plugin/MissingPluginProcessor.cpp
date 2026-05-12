#include "engine/plugin/MissingPluginProcessor.h"
#include "engine/plugin/PluginState.h"
#include "engine/plugin/PluginDescriptor.h"
#include <cstring>

namespace mcp::plugin {

MissingPluginProcessor::MissingPluginProcessor(
        ExternalPluginReference ref,
        PluginRuntimeStatus     reason,
        std::string             message)
    : m_ref(std::move(ref))
    , m_status(reason)
    , m_message(std::move(message))
{}

void MissingPluginProcessor::prepare(const ProcessContext& /*ctx*/) {}

void MissingPluginProcessor::process(const AudioBlock& block, const EventBlock& /*events*/) {
    const size_t bytes = static_cast<size_t>(block.numSamples) * sizeof(float);
    // Passthrough: copy inputs → outputs to preserve signal flow through the chain.
    for (int ch = 0; ch < block.numOutputChannels && ch < block.numInputChannels; ++ch)
        std::memcpy(block.outputs[ch], block.inputs[ch], bytes);
    for (int ch = block.numInputChannels; ch < block.numOutputChannels; ++ch)
        std::memset(block.outputs[ch], 0, bytes);
}

const std::vector<ParameterInfo>& MissingPluginProcessor::getParameters() const {
    return m_emptyParams;
}

PluginState MissingPluginProcessor::getState() const {
    PluginState st;
    st.pluginId  = m_ref.pluginId;
    st.backend   = (m_ref.backend == "au")   ? PluginBackend::AU
                 : (m_ref.backend == "vst3") ? PluginBackend::VST3
                 :                             PluginBackend::Internal;
    st.version   = 1;
    st.stateData = m_ref.stateBlob;
    // Expose the normalized snapshot as the parameters map (denorm unavailable without a
    // live plugin, so we preserve the normalized values as-is for round-trip).
    for (const auto& [id, norm] : m_ref.paramSnapshot)
        st.parameters[id] = norm;
    return st;
}

PluginRuntimeStatus           MissingPluginProcessor::runtimeStatus() const { return m_status; }
const ExternalPluginReference& MissingPluginProcessor::reference()    const { return m_ref; }

} // namespace mcp::plugin
