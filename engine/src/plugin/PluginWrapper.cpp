#include "engine/plugin/PluginWrapper.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mcp::plugin {

PluginWrapper::PluginWrapper(std::unique_ptr<AudioProcessor> processor)
    : m_processor(std::move(processor))
{}

void PluginWrapper::prepare(const ProcessContext& context) {
    if (!m_processor) { m_status = Status::Error; return; }

    m_numInputChannels = context.inputChannels;
    m_maxBlockSize     = context.maxBlockSize;

    // Allocate input-gain scratch buffers (one contiguous block per channel).
    const int totalSamples = context.inputChannels * context.maxBlockSize;
    m_inputScratchData.assign(static_cast<size_t>(totalSamples), 0.0f);
    m_inputScratchPtrs .resize(static_cast<size_t>(context.inputChannels));
    m_inputConstPtrs   .resize(static_cast<size_t>(context.inputChannels));
    for (int ch = 0; ch < context.inputChannels; ++ch) {
        float* p = m_inputScratchData.data() + ch * context.maxBlockSize;
        m_inputScratchPtrs[static_cast<size_t>(ch)] = p;
        m_inputConstPtrs  [static_cast<size_t>(ch)] = p;
    }

    m_processor->prepare(context);
    m_status = Status::Prepared;
}

void PluginWrapper::reset() {
    if (m_processor) m_processor->reset();
}

void PluginWrapper::process(const AudioBlock& block, const EventBlock& events) {
    if (m_status != Status::Prepared || !m_processor) return;

    if (m_bypassed) {
        // TODO: add linear crossfade on bypass state change to avoid click
        for (int ch = 0; ch < block.numOutputChannels; ++ch) {
            if (block.numInputChannels > 0) {
                const int inCh = std::min(ch, block.numInputChannels - 1);
                std::memcpy(block.outputs[ch], block.inputs[inCh],
                            static_cast<size_t>(block.numSamples) * sizeof(float));
            } else {
                std::memset(block.outputs[ch], 0,
                            static_cast<size_t>(block.numSamples) * sizeof(float));
            }
        }
        return;
    }

    // Apply input gain via scratch buffers if needed.
    if (m_inputGainLinear != 1.0f && block.numInputChannels > 0) {
        const float g = m_inputGainLinear;
        for (int ch = 0; ch < block.numInputChannels; ++ch) {
            float*       dst = m_inputScratchPtrs[static_cast<size_t>(ch)];
            const float* src = block.inputs[ch];
            for (int s = 0; s < block.numSamples; ++s)
                dst[s] = src[s] * g;
        }
        const AudioBlock inner{
            m_inputConstPtrs.data(),
            block.outputs,
            block.numInputChannels,
            block.numOutputChannels,
            block.numSamples
        };
        m_processor->process(inner, events);
    } else {
        m_processor->process(block, events);
    }

    // Apply output gain in-place.
    if (m_outputGainLinear != 1.0f) {
        const float g = m_outputGainLinear;
        for (int ch = 0; ch < block.numOutputChannels; ++ch) {
            float* out = block.outputs[ch];
            for (int s = 0; s < block.numSamples; ++s)
                out[s] *= g;
        }
    }
}

float PluginWrapper::dbToLinear(float db) {
    return (db <= -96.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
}

float PluginWrapper::getInputGainDb() const {
    return (m_inputGainLinear <= 0.0f) ? -96.0f : 20.0f * std::log10(m_inputGainLinear);
}

void PluginWrapper::setInputGainDb(float db) {
    m_inputGainLinear = dbToLinear(db);
}

float PluginWrapper::getOutputGainDb() const {
    return (m_outputGainLinear <= 0.0f) ? -96.0f : 20.0f * std::log10(m_outputGainLinear);
}

void PluginWrapper::setOutputGainDb(float db) {
    m_outputGainLinear = dbToLinear(db);
}

int PluginWrapper::getLatencySamples() const {
    return m_processor ? m_processor->getLatencySamples() : 0;
}

} // namespace mcp::plugin
