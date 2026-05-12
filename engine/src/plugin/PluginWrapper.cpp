#include "engine/plugin/PluginWrapper.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mcp::plugin {

// Crossfade length for host-bypass and reactivation transitions.
// 512 samples ≈ 10 ms at 48 kHz — enough to mask a click, short enough to
// feel immediate.
static constexpr int kXfadeLen = 512;

PluginWrapper::PluginWrapper(std::unique_ptr<AudioProcessor> processor)
    : m_processor(std::move(processor))
{}

// ── computeBypassInto ────────────────────────────────────────────────────────
// Routes dry audio through the PDC ring buffer and writes it to block.outputs.
// When ring is empty (zero-latency plugin), falls back to direct copy.
// Called from process() for bypass and deactivated paths; updates ring head.

void PluginWrapper::computeBypassInto(const AudioBlock& block) {
    const int nSamples = block.numSamples;
    const int nInCh    = block.numInputChannels;
    const int nOutCh   = block.numOutputChannels;

    if (m_bypassRingLen >= nSamples && !m_bypassRings.empty()) {
        const int numR = static_cast<int>(m_bypassRings.size());
        for (int ch = 0; ch < nOutCh; ++ch) {
            auto& ring = m_bypassRings[static_cast<size_t>(std::min(ch, numR - 1))];
            for (int s = 0; s < nSamples; ++s) {
                const int   pos = (m_bypassRingHead + s) % m_bypassRingLen;
                const float del = ring[pos];
                ring[pos] = (nInCh > 0)
                            ? block.inputs[std::min(ch, nInCh - 1)][s]
                            : 0.0f;
                block.outputs[ch][s] = del;
            }
        }
        m_bypassRingHead = (m_bypassRingHead + nSamples) % m_bypassRingLen;
    } else {
        for (int ch = 0; ch < nOutCh; ++ch) {
            if (nInCh > 0) {
                std::memcpy(block.outputs[ch],
                            block.inputs[std::min(ch, nInCh - 1)],
                            static_cast<size_t>(nSamples) * sizeof(float));
            } else {
                std::memset(block.outputs[ch], 0,
                            static_cast<size_t>(nSamples) * sizeof(float));
            }
        }
    }
}

// ── ensureRingAndScratch ──────────────────────────────────────────────────────
// Allocates ring and output scratch if not yet present or undersized.
// Does NOT clear existing ring content so PDC is preserved across calls.

void PluginWrapper::ensureRingAndScratch(int latencyHint, int numOutCh) {
    if (latencyHint > 0 && (m_bypassRings.empty() ||
                             static_cast<int>(m_bypassRings[0].size()) < latencyHint))
    {
        m_bypassRingLen  = latencyHint;
        m_bypassRingHead = 0;
        m_bypassRings.assign(static_cast<size_t>(numOutCh),
                             std::vector<float>(static_cast<size_t>(latencyHint), 0.0f));
    }
    if (m_outputScratchPtrs.empty() && numOutCh > 0 && m_maxBlockSize > 0) {
        const int total = numOutCh * m_maxBlockSize;
        m_outputScratchData.assign(static_cast<size_t>(total), 0.0f);
        m_outputScratchPtrs.resize(static_cast<size_t>(numOutCh));
        for (int ch = 0; ch < numOutCh; ++ch)
            m_outputScratchPtrs[static_cast<size_t>(ch)] =
                m_outputScratchData.data() + ch * m_maxBlockSize;
    }
}

// ── prepare ───────────────────────────────────────────────────────────────────

void PluginWrapper::prepare(const ProcessContext& context) {
    if (!m_processor) { m_status = Status::Error; return; }

    m_lastContext      = context;
    m_numInputChannels = context.inputChannels;
    m_numOutputChannels= context.outputChannels;
    m_maxBlockSize     = context.maxBlockSize;

    // Input gain scratch.
    const int totalIn = context.inputChannels * context.maxBlockSize;
    m_inputScratchData.assign(static_cast<size_t>(totalIn), 0.0f);
    m_inputScratchPtrs.resize(static_cast<size_t>(context.inputChannels));
    m_inputConstPtrs  .resize(static_cast<size_t>(context.inputChannels));
    for (int ch = 0; ch < context.inputChannels; ++ch) {
        float* p = m_inputScratchData.data() + ch * context.maxBlockSize;
        m_inputScratchPtrs[static_cast<size_t>(ch)] = p;
        m_inputConstPtrs  [static_cast<size_t>(ch)] = p;
    }

    m_processor->prepare(context);

    m_hasNativeBypass = m_processor->hasNativeBypass();

    if (!m_hasNativeBypass) {
        // PDC ring: sized to plugin latency.
        const int lat = m_processor->getLatencySamples();
        m_bypassRingLen  = lat;
        m_bypassRingHead = 0;
        if (lat > 0) {
            m_bypassRings.assign(static_cast<size_t>(context.outputChannels),
                                 std::vector<float>(static_cast<size_t>(lat), 0.0f));
        } else {
            m_bypassRings.clear();
        }

        // Output scratch for crossfade blending.
        const int totalOut = context.outputChannels * context.maxBlockSize;
        m_outputScratchData.assign(static_cast<size_t>(totalOut), 0.0f);
        m_outputScratchPtrs.resize(static_cast<size_t>(context.outputChannels));
        for (int ch = 0; ch < context.outputChannels; ++ch)
            m_outputScratchPtrs[static_cast<size_t>(ch)] =
                m_outputScratchData.data() + ch * context.maxBlockSize;
    } else {
        m_bypassRings.clear();
        m_bypassRingLen  = 0;
        m_bypassRingHead = 0;
        m_outputScratchData.clear();
        m_outputScratchPtrs.clear();
    }

    m_xfadePos  = -1;
    m_status    = Status::Prepared;
}

// ── reset ─────────────────────────────────────────────────────────────────────

void PluginWrapper::reset() {
    if (m_processor) m_processor->reset();
    m_bypassRingHead = 0;
    for (auto& ring : m_bypassRings)
        std::fill(ring.begin(), ring.end(), 0.0f);
    m_xfadePos = -1;
}

// ── deactivate / reactivate ───────────────────────────────────────────────────

void PluginWrapper::deactivate() {
    if (m_deactivated || m_status != Status::Prepared) return;

    // Save latency from the live processor before destroying it.
    const int lat = m_processor ? m_processor->getLatencySamples() : m_bypassRingLen;

    // Native bypass plugins never had a ring — allocate one now so dry audio
    // continues to flow at the correct delay while the instance is gone.
    ensureRingAndScratch(lat, m_numOutputChannels);
    m_hasNativeBypass = false;  // plugin is gone; use ring path unconditionally

    if (m_processor) {
        m_savedState = m_processor->getState();
        m_processor->suspend();
        m_processor.reset();
    }

    m_deactivated = true;
    // m_bypassed and ring content are intentionally left unchanged so the
    // signal path is transparent to everything above PluginWrapper.
}

void PluginWrapper::reactivate(std::unique_ptr<AudioProcessor> processor) {
    if (!processor || m_lastContext.maxBlockSize <= 0) return;

    m_processor   = std::move(processor);
    m_deactivated = false;
    m_bypassed    = false;

    // Prepare the new plugin without calling PluginWrapper::prepare() so the
    // ring content is preserved for the crossfade back to the plugin signal.
    m_hasNativeBypass = m_processor->hasNativeBypass();
    m_processor->prepare(m_lastContext);
    if (!m_savedState.pluginId.empty())
        m_processor->setState(m_savedState);

    // Ensure scratch exists for crossfade (native bypass plugins cleared it
    // during the original prepare() but we may need it now).
    ensureRingAndScratch(m_bypassRingLen, m_numOutputChannels);

    if (!m_hasNativeBypass && !m_bypassRings.empty()) {
        // Crossfade from ring (deactivated path) back to plugin output.
        m_xfadeToBypass = false;
        m_xfadePos      = 0;
    } else {
        m_xfadePos = -1;
    }
}

// ── setBypassed ───────────────────────────────────────────────────────────────

void PluginWrapper::setBypassed(bool v) {
    if (m_bypassed == v) return;
    m_bypassed = v;

    if (m_hasNativeBypass) {
        if (m_processor) m_processor->setNativeBypass(v);
        return;
    }

    m_xfadeToBypass = v;
    m_xfadePos      = 0;
    if (v) {
        m_bypassRingHead = 0;
        for (auto& ring : m_bypassRings)
            std::fill(ring.begin(), ring.end(), 0.0f);
    }
}

// ── process ───────────────────────────────────────────────────────────────────

void PluginWrapper::process(const AudioBlock& block, const EventBlock& events) {
    if (m_status != Status::Prepared) return;

    // Deactivated: plugin instance is gone; preserve PDC via ring.
    if (m_deactivated) {
        computeBypassInto(block);
        return;
    }

    if (!m_processor) return;

    const int nSamples = block.numSamples;
    const int nInCh    = block.numInputChannels;
    const int nOutCh   = block.numOutputChannels;

    // ── Apply input gain to scratch if needed ─────────────────────────────────
    const float** effInputs = block.inputs;
    if (m_inputGainLinear != 1.0f && nInCh > 0) {
        const float g = m_inputGainLinear;
        for (int ch = 0; ch < nInCh; ++ch) {
            float*       dst = m_inputScratchPtrs[static_cast<size_t>(ch)];
            const float* src = block.inputs[ch];
            for (int s = 0; s < nSamples; ++s) dst[s] = src[s] * g;
        }
        effInputs = const_cast<const float**>(m_inputConstPtrs.data());
    }

    auto applyOutGain = [&](float** bufs, int nCh) {
        if (m_outputGainLinear == 1.0f) return;
        const float g = m_outputGainLinear;
        for (int ch = 0; ch < nCh; ++ch) {
            float* out = bufs[ch];
            for (int s = 0; s < nSamples; ++s) out[s] *= g;
        }
    };

    // ── Native bypass or steady active: plugin always runs ────────────────────
    if (m_hasNativeBypass || (!m_bypassed && m_xfadePos < 0)) {
        AudioBlock cb{effInputs, block.outputs, nInCh, nOutCh, nSamples};
        m_processor->process(cb, events);
        applyOutGain(block.outputs, nOutCh);
        return;
    }

    // ── Crossfade (bypass ↔ active, or reactivation) ──────────────────────────
    if (m_xfadePos >= 0) {
        // Plugin → output scratch.
        const int nScratch = std::min(nOutCh,
                                      static_cast<int>(m_outputScratchPtrs.size()));
        {
            AudioBlock sb{effInputs, m_outputScratchPtrs.data(),
                          nInCh, nScratch, nSamples};
            m_processor->process(sb, events);
        }

        // Bypass / ring → block.outputs.
        computeBypassInto(block);

        // Blend: t=0 → pure plugin, t=1 → pure bypass.
        const float invLen = 1.0f / static_cast<float>(kXfadeLen);
        for (int ch = 0; ch < nOutCh; ++ch) {
            const float* plug = (ch < nScratch)
                                ? m_outputScratchPtrs[static_cast<size_t>(ch)]
                                : nullptr;
            float* out = block.outputs[ch];
            for (int s = 0; s < nSamples; ++s) {
                const float t    = std::min(1.0f, (m_xfadePos + s) * invLen);
                const float toBP = m_xfadeToBypass ? t : (1.0f - t);
                const float plugV = plug ? plug[s] * m_outputGainLinear : 0.0f;
                out[s] = (1.0f - toBP) * plugV + toBP * out[s];
            }
        }

        m_xfadePos += nSamples;
        if (m_xfadePos >= kXfadeLen)
            m_xfadePos = -1;
        return;
    }

    // ── Steady host bypass ────────────────────────────────────────────────────
    computeBypassInto(block);
}

// ── Gain helpers ──────────────────────────────────────────────────────────────

float PluginWrapper::dbToLinear(float db) {
    return (db <= -96.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
}

float PluginWrapper::getInputGainDb() const {
    return (m_inputGainLinear <= 0.0f) ? -96.0f : 20.0f * std::log10(m_inputGainLinear);
}
void PluginWrapper::setInputGainDb(float db) { m_inputGainLinear = dbToLinear(db); }

float PluginWrapper::getOutputGainDb() const {
    return (m_outputGainLinear <= 0.0f) ? -96.0f : 20.0f * std::log10(m_outputGainLinear);
}
void PluginWrapper::setOutputGainDb(float db) { m_outputGainLinear = dbToLinear(db); }

int PluginWrapper::getLatencySamples() const {
    return m_processor ? m_processor->getLatencySamples() : m_bypassRingLen;
}

} // namespace mcp::plugin
