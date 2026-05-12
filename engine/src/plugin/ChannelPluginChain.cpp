#include "engine/plugin/ChannelPluginChain.h"
#include "engine/plugin/AudioBlock.h"
#include "engine/plugin/EventBlock.h"
#include "engine/plugin/ProcessContext.h"
#include <algorithm>
#include <cstring>

namespace mcp::plugin {

ChannelPluginChain::ChannelPluginChain(int numChannels)
    : m_numChannels(std::max(1, std::min(2, numChannels))) {}

void ChannelPluginChain::prepare(double sampleRate, int maxBlockSize) {
    m_sampleRate    = sampleRate;
    m_maxBlockSize  = maxBlockSize;

    const int total = m_numChannels * maxBlockSize;
    for (int i = 0; i < 2; ++i) {
        m_scratchData [i].assign(static_cast<size_t>(total), 0.0f);
        m_scratchWrite[i].resize(static_cast<size_t>(m_numChannels));
        m_scratchRead [i].resize(static_cast<size_t>(m_numChannels));
        for (int c = 0; c < m_numChannels; ++c) {
            float* p = m_scratchData[i].data() + c * maxBlockSize;
            m_scratchWrite[i][static_cast<size_t>(c)] = p;
            m_scratchRead [i][static_cast<size_t>(c)] = p;
        }
    }

    const ProcessContext ctx{sampleRate, maxBlockSize, m_numChannels, m_numChannels,
                              LatencyMode::Live};
    for (auto& slot : m_slots) {
        if (slot) slot->prepare(ctx);
    }
}

void ChannelPluginChain::process(float* chanBuf, int totalCh,
                                  int numSamples, int logicalCh) {
    if (m_maxBlockSize == 0 || numSamples <= 0) return;

    // Early-out when no slots are loaded.
    bool anyActive = false;
    for (const auto& s : m_slots)
        if (s) { anyActive = true; break; }
    if (!anyActive) return;

    const int ns = std::min(numSamples, m_maxBlockSize);

    // Deinterleave from chanBuf into scratch[0].
    for (int c = 0; c < m_numChannels; ++c) {
        float* dst = m_scratchWrite[0][static_cast<size_t>(c)];
        for (int f = 0; f < ns; ++f)
            dst[f] = chanBuf[f * totalCh + logicalCh + c];
    }

    // Ping-pong through each loaded slot.
    static const EventBlock kNoEvents{};
    int src = 0, dst = 1;
    for (auto& slot : m_slots) {
        if (!slot) continue;
        AudioBlock blk{
            m_scratchRead [static_cast<size_t>(src)].data(),
            m_scratchWrite[static_cast<size_t>(dst)].data(),
            m_numChannels, m_numChannels, ns
        };
        slot->process(blk, kNoEvents);
        std::swap(src, dst);
    }

    // Reinterleave from scratch[src] back into chanBuf.
    for (int c = 0; c < m_numChannels; ++c) {
        const float* sp = m_scratchRead[static_cast<size_t>(src)][static_cast<size_t>(c)];
        for (int f = 0; f < ns; ++f)
            chanBuf[f * totalCh + logicalCh + c] = sp[f];
    }
}

void ChannelPluginChain::setSlot(int slot, std::shared_ptr<PluginWrapper> wrapper) {
    if (slot < 0 || slot >= kMaxSlots) return;
    m_slots[static_cast<size_t>(slot)] = std::move(wrapper);
}

void ChannelPluginChain::clearSlot(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    m_slots[static_cast<size_t>(slot)].reset();
}

std::shared_ptr<PluginWrapper> ChannelPluginChain::getSlot(int slot) const {
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    return m_slots[static_cast<size_t>(slot)];
}

} // namespace mcp::plugin
