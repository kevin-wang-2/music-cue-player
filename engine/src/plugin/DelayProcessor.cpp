#include "DelayProcessor.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace mcp::plugin {

static std::vector<ParameterInfo> makeDelayParams() {
    return {
        makeLinearParam("delay_ms", "Delay", "ms", 0.0f, DelayProcessor::kMaxDelayMs, 0.0f),
        makeLinearParam("mix",      "Mix",   "",   0.0f, 1.0f,  1.0f),
    };
}

DelayProcessor::DelayProcessor(int numChannels)
    : m_numChannels(numChannels)
    , m_params(makeDelayParams())
{
    m_delayMsIdx = m_params.indexFor("delay_ms");
    m_mixIdx     = m_params.indexFor("mix");
}

void DelayProcessor::prepare(const ProcessContext& ctx) {
    m_sampleRate = ctx.sampleRate;
    const int maxSamples =
        static_cast<int>(kMaxDelayMs / 1000.0f * static_cast<float>(m_sampleRate)) + 1;

    const int numCh = std::max(ctx.outputChannels, m_numChannels);
    m_buffers.assign(static_cast<size_t>(numCh),
                     std::vector<float>(static_cast<size_t>(maxSamples), 0.0f));
    m_writePos = 0;
    updateDelaySamples();
}

void DelayProcessor::reset() {
    for (auto& buf : m_buffers)
        std::fill(buf.begin(), buf.end(), 0.0f);
    m_writePos = 0;
}

void DelayProcessor::updateDelaySamples() {
    const float ms = m_params.getValueAt(m_delayMsIdx);
    m_delaySamples = static_cast<int>(ms / 1000.0f * static_cast<float>(m_sampleRate));
    if (!m_buffers.empty()) {
        const int maxSamples = static_cast<int>(m_buffers[0].size()) - 1;
        m_delaySamples = std::clamp(m_delaySamples, 0, maxSamples);
    }
}

void DelayProcessor::process(const AudioBlock& block, const EventBlock& events) {
    if (m_buffers.empty()) return;

    int nextEvt = 0;
    int pos     = 0;

    while (pos < block.numSamples) {
        // Apply events at current position
        while (nextEvt < events.numParameterEvents &&
               events.parameterEvents[nextEvt].sampleOffset <= pos) {
            const auto& ev = events.parameterEvents[nextEvt];
            m_params.applyEvent(ev.parameterId, ev.value);
            updateDelaySamples();
            ++nextEvt;
        }

        int segEnd = block.numSamples;
        if (nextEvt < events.numParameterEvents)
            segEnd = events.parameterEvents[nextEvt].sampleOffset;

        const float mix     = m_params.getValueAt(m_mixIdx);
        const int   numOut  = block.numOutputChannels;
        const int   numIn   = block.numInputChannels;
        const int   bufSize = static_cast<int>(m_buffers[0].size());

        for (int s = pos; s < segEnd; ++s) {
            for (int ch = 0; ch < numOut; ++ch) {
                const int   inCh = std::min(ch, numIn - 1);
                const float dry  = (numIn > 0) ? block.inputs[inCh][s] : 0.0f;

                float wet = 0.0f;
                if (m_delaySamples > 0 && ch < static_cast<int>(m_buffers.size())) {
                    auto& buf    = m_buffers[static_cast<size_t>(ch)];
                    const int rp = (m_writePos - m_delaySamples + bufSize) % bufSize;
                    wet          = buf[static_cast<size_t>(rp)];
                    buf[static_cast<size_t>(m_writePos)] = dry;
                } else if (ch < static_cast<int>(m_buffers.size())) {
                    // delay == 0: still write into buffer to keep state consistent
                    m_buffers[static_cast<size_t>(ch)][static_cast<size_t>(m_writePos)] = dry;
                    wet = dry;
                }

                block.outputs[ch][s] = dry * (1.0f - mix) + wet * mix;
            }
            m_writePos = (m_writePos + 1) % bufSize;
        }

        pos = segEnd;
    }
}

const std::vector<ParameterInfo>& DelayProcessor::getParameters() const {
    return m_params.infos();
}

float DelayProcessor::getParameterValue(const std::string& id) const {
    return m_params.getValue(id);
}

void DelayProcessor::setParameterValue(const std::string& id, float value) {
    m_params.setValue(id, value);
    if (id == "delay_ms") updateDelaySamples();
}

float DelayProcessor::getNormalizedParameterValue(const std::string& id) const {
    return m_params.getNormalized(id);
}

void DelayProcessor::setNormalizedParameterValue(const std::string& id, float n) {
    m_params.setNormalized(id, n);
    if (id == "delay_ms") updateDelaySamples();
}

PluginState DelayProcessor::getState() const {
    nlohmann::json j;
    j["delay_ms"] = m_params.getValueAt(m_delayMsIdx);
    j["mix"]      = m_params.getValueAt(m_mixIdx);

    PluginState s;
    s.pluginId = (m_numChannels == 1) ? "internal.delay.mono" : "internal.delay.stereo";
    s.backend  = PluginBackend::Internal;
    s.version  = 1;
    const std::string js = j.dump();
    s.stateData.assign(js.begin(), js.end());
    s.parameters["delay_ms"] = m_params.getValueAt(m_delayMsIdx);
    s.parameters["mix"]      = m_params.getValueAt(m_mixIdx);
    return s;
}

void DelayProcessor::setState(const PluginState& state) {
    if (!state.stateData.empty()) {
        try {
            const std::string js(state.stateData.begin(), state.stateData.end());
            const auto j = nlohmann::json::parse(js);
            if (j.contains("delay_ms") && j["delay_ms"].is_number())
                setParameterValue("delay_ms", j["delay_ms"].get<float>());
            if (j.contains("mix") && j["mix"].is_number())
                setParameterValue("mix", j["mix"].get<float>());
        } catch (...) {}
    }
    for (const auto& [id, val] : state.parameters) {
        if (m_params.indexFor(id) >= 0)
            setParameterValue(id, val);
    }
}

} // namespace mcp::plugin
