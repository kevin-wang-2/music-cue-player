#pragma once
#include "engine/ShowFile.h"
#include <vector>

namespace mcp {

// One physical channel-to-channel send edge (post-pan, post-level-law applied).
// gain == 0.0f when muted (edge retained for future topological delay compensation).
struct SendEdge {
    int   srcCh{-1};
    int   dstCh{-1};
    float gain{0.0f};        // linear: send_level * pan_gain (* summing_factor if needed)
    int   delaySamples{0};   // PDC compensation delay for this edge
};

// Compiled state consumed by the audio callback.
// Rebuilt whenever the send graph topology changes (add/remove send slots).
// Values inside SendEdge (gain) are updated in-place when only level/pan/mute changes,
// so the callback always reads current values without a recompile.
struct CompiledMixState {
    // Logical-channel processing order (topo sort; all N channels present).
    std::vector<int> channelOrder;

    // Per-channel master gains in channelOrder index — linear, 0 if muted.
    // Multiplied into send edge dispatch in the callback to achieve post-fader sends.
    // The fold matrix still carries masterGain for the chanBuf→output routing.
    std::vector<float> masterGains;

    // All send edges, stable storage. Grouped by source channel in channelOrder.
    // sendStart[i]..sendStart[i+1] are the edges for channelOrder[i].
    std::vector<SendEdge> edges;
    std::vector<int>      sendStart;  // size == channelOrder.size() + 1

    // PDC delays — indexed by CHANNEL NUMBER (not topo position).
    // Set by AppModel::rebuildPDC() after topology + plugin-latency calculation.
    std::vector<int> pdcVoiceDelay;   // pre-topo voice-fold delay per channel
    std::vector<int> pdcOutputDelay;  // post-topo output-alignment delay per channel
};

// Computes and maintains the CompiledMixState from a ShowFile::AudioSetup.
//
// Topology (channelOrder) is rebuilt on rebuildTopology().
// Per-edge gains are refreshed on rebuildGains() without touching channelOrder.
class SendRouter {
public:
    // True if adding a send from srcCh → dstCh would form a directed cycle.
    // Call before presenting the send destination picker.
    bool wouldCreateCycle(int srcCh, int dstCh,
                          const ShowFile::AudioSetup& as) const;

    // Full rebuild: topo sort + gain compilation.  Call when send topology changes.
    void rebuildTopology(const ShowFile::AudioSetup& as);

    // Light rebuild: only re-derive per-edge gains from current SendSlot values.
    // Call when only level/pan/mute changes (no structural change).
    void rebuildGains(const ShowFile::AudioSetup& as);

    const CompiledMixState& compiled() const { return m_state; }

private:
    CompiledMixState m_state;

    // Adjacency list for the current send graph (indexed by logical channel).
    // Rebuilt in rebuildTopology; used by wouldCreateCycle.
    std::vector<std::vector<int>> m_adj;

    void buildAdj(const ShowFile::AudioSetup& as);
    void topoSort(int nCh);
    void compileSendEdges(const ShowFile::AudioSetup& as);
};

} // namespace mcp
