#include "TrimProcessor.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace mcp::plugin {

static std::vector<ParameterInfo> makeTrimParams() {
    auto p = makeLinearParam("gain_db", "Gain", "dB", -96.0f, 12.0f, 0.0f);
    p.domain = mcp::AutoParam::Domain::DB;
    return {p};
}

TrimProcessor::TrimProcessor(int numChannels)
    : m_numChannels(numChannels)
    , m_params(makeTrimParams())
{
    m_gainDbIdx = m_params.indexFor("gain_db");
    m_gainLinear = 1.0f;  // 0 dB
}

float TrimProcessor::dbToLinear(float db) {
    return (db <= -96.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
}

void TrimProcessor::prepare(const ProcessContext& /*ctx*/) {
    m_gainLinear = dbToLinear(m_params.getValueAt(m_gainDbIdx));
    m_prepared   = true;
}

void TrimProcessor::process(const AudioBlock& block, const EventBlock& events) {
    if (!m_prepared) return;

    int nextEvt = 0;
    int pos     = 0;

    while (pos < block.numSamples) {
        // Apply all events whose sampleOffset <= pos
        while (nextEvt < events.numParameterEvents &&
               events.parameterEvents[nextEvt].sampleOffset <= pos) {
            const auto& ev = events.parameterEvents[nextEvt];
            m_params.applyEvent(ev.parameterId, ev.value);
            m_gainLinear = dbToLinear(m_params.getValueAt(m_gainDbIdx));
            ++nextEvt;
        }

        // End of this segment: either the next event or end of block
        int segEnd = block.numSamples;
        if (nextEvt < events.numParameterEvents)
            segEnd = events.parameterEvents[nextEvt].sampleOffset;

        // Apply constant gain over [pos, segEnd)
        const float g      = m_gainLinear;
        const int   numOut = block.numOutputChannels;
        const int   numIn  = block.numInputChannels;
        for (int ch = 0; ch < numOut; ++ch) {
            const float* in  = block.inputs [std::min(ch, numIn - 1)];
            float*       out = block.outputs[ch];
            for (int s = pos; s < segEnd; ++s)
                out[s] = in[s] * g;
        }

        pos = segEnd;
    }
}

const std::vector<ParameterInfo>& TrimProcessor::getParameters() const {
    return m_params.infos();
}

float TrimProcessor::getParameterValue(const std::string& id) const {
    return m_params.getValue(id);
}

void TrimProcessor::setParameterValue(const std::string& id, float value) {
    m_params.setValue(id, value);
    if (id == "gain_db")
        m_gainLinear = dbToLinear(m_params.getValueAt(m_gainDbIdx));
}

float TrimProcessor::getNormalizedParameterValue(const std::string& id) const {
    return m_params.getNormalized(id);
}

void TrimProcessor::setNormalizedParameterValue(const std::string& id, float n) {
    m_params.setNormalized(id, n);
    if (id == "gain_db")
        m_gainLinear = dbToLinear(m_params.getValueAt(m_gainDbIdx));
}

PluginState TrimProcessor::getState() const {
    nlohmann::json j;
    j["gain_db"] = m_params.getValueAt(m_gainDbIdx);

    PluginState s;
    s.pluginId = (m_numChannels == 1) ? "internal.trim.mono" : "internal.trim.stereo";
    s.backend  = PluginBackend::Internal;
    s.version  = 1;
    const std::string js = j.dump();
    s.stateData.assign(js.begin(), js.end());
    s.parameters["gain_db"] = m_params.getValueAt(m_gainDbIdx);
    return s;
}

void TrimProcessor::setState(const PluginState& state) {
    if (!state.stateData.empty()) {
        try {
            const std::string js(state.stateData.begin(), state.stateData.end());
            const auto j = nlohmann::json::parse(js);
            if (j.contains("gain_db") && j["gain_db"].is_number())
                setParameterValue("gain_db", j["gain_db"].get<float>());
        } catch (...) {}
    }
    for (const auto& [id, val] : state.parameters) {
        if (m_params.indexFor(id) >= 0)
            setParameterValue(id, val);
    }
}

} // namespace mcp::plugin
