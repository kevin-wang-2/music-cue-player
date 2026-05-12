#include "engine/SendRouter.h"
#include <algorithm>
#include <cmath>
#include <queue>

namespace mcp {

static constexpr float kPi2 = 1.5707963267948966f; // π/2

// Equal-power pan: input pan in [-1, +1], output L/R gains.
// pan=-1 → full left (L=1, R=0); pan=0 → center; pan=+1 → full right.
static void equalPowerPan(float pan, float& outL, float& outR)
{
    const float theta = (pan + 1.0f) * 0.5f * kPi2;  // [0, π/2]
    outL = std::cos(theta);
    outR = std::sin(theta);
}

static float dBToLinear(float db)
{
    return db <= -144.0f ? 0.0f : std::pow(10.0f, db * 0.05f);
}

// ---------------------------------------------------------------------------

void SendRouter::buildAdj(const ShowFile::AudioSetup& as)
{
    const int nCh = static_cast<int>(as.channels.size());
    m_adj.assign(static_cast<size_t>(nCh), {});

    for (int src = 0; src < nCh; ++src) {
        for (const auto& ss : as.channels[static_cast<size_t>(src)].sends) {
            if (!ss.isActive()) continue;
            const int dst = ss.dstChannel;
            if (dst < 0 || dst >= nCh || dst == src) continue;
            m_adj[static_cast<size_t>(src)].push_back(dst);
        }
    }
}

void SendRouter::topoSort(int nCh)
{
    // Kahn's algorithm (BFS).  Channels with no in-edges go first; ties preserve
    // original channel order (stable sort by original index).
    std::vector<int> inDeg(static_cast<size_t>(nCh), 0);
    for (int u = 0; u < nCh; ++u)
        for (int v : m_adj[static_cast<size_t>(u)])
            ++inDeg[static_cast<size_t>(v)];

    // Use a priority queue (min-heap on channel index) to preserve original
    // channel order when multiple nodes have in-degree 0.
    std::priority_queue<int, std::vector<int>, std::greater<int>> q;
    for (int i = 0; i < nCh; ++i)
        if (inDeg[static_cast<size_t>(i)] == 0) q.push(i);

    m_state.channelOrder.clear();
    m_state.channelOrder.reserve(static_cast<size_t>(nCh));

    while (!q.empty()) {
        const int u = q.top(); q.pop();
        m_state.channelOrder.push_back(u);
        for (int v : m_adj[static_cast<size_t>(u)]) {
            if (--inDeg[static_cast<size_t>(v)] == 0) q.push(v);
        }
    }

    // If graph has a cycle (shouldn't happen — cycle prevention at UI level),
    // append remaining channels in original order.
    if (static_cast<int>(m_state.channelOrder.size()) < nCh) {
        for (int i = 0; i < nCh; ++i) {
            if (std::find(m_state.channelOrder.begin(), m_state.channelOrder.end(), i)
                == m_state.channelOrder.end())
                m_state.channelOrder.push_back(i);
        }
    }
}

void SendRouter::compileSendEdges(const ShowFile::AudioSetup& as)
{
    const int nCh = static_cast<int>(as.channels.size());

    // Build a position map: channel index → position in channelOrder.
    std::vector<int> pos(static_cast<size_t>(nCh), 0);
    for (int i = 0; i < static_cast<int>(m_state.channelOrder.size()); ++i)
        pos[static_cast<size_t>(m_state.channelOrder[static_cast<size_t>(i)])] = i;

    m_state.edges.clear();
    const int n = static_cast<int>(m_state.channelOrder.size());
    m_state.sendStart.assign(static_cast<size_t>(n + 1), 0);

    // For each channel in topo order, collect its outgoing send edges.
    for (int i = 0; i < n; ++i) {
        m_state.sendStart[static_cast<size_t>(i)] = static_cast<int>(m_state.edges.size());
        const int srcCh = m_state.channelOrder[static_cast<size_t>(i)];
        if (srcCh >= nCh) continue;
        const auto& srcChan = as.channels[static_cast<size_t>(srcCh)];
        const bool srcStereo = srcChan.linkedStereo
                               && (srcCh + 1 < nCh);

        for (const auto& ss : srcChan.sends) {
            if (!ss.isActive()) continue;
            const int dstMaster = ss.dstChannel;
            if (dstMaster < 0 || dstMaster >= nCh) continue;
            const bool dstStereo = as.channels[static_cast<size_t>(dstMaster)].linkedStereo
                                   && (dstMaster + 1 < nCh);
            const float levelLin = dBToLinear(ss.levelDb);
            const float gain     = ss.muted ? 0.0f : levelLin;

            if (!srcStereo && !dstStereo) {
                // mono → mono
                m_state.edges.push_back({srcCh, dstMaster, gain});

            } else if (!srcStereo && dstStereo) {
                // mono → stereo  (equal-power pan on panL)
                float gl, gr;
                equalPowerPan(ss.panL, gl, gr);
                m_state.edges.push_back({srcCh, dstMaster,     gain * gl});
                m_state.edges.push_back({srcCh, dstMaster + 1, gain * gr});

            } else if (srcStereo && !dstStereo) {
                // stereo → mono  (-3 dB summing, no pan)
                static constexpr float k3dB = 0.7071067811865476f;
                m_state.edges.push_back({srcCh,     dstMaster, gain * k3dB});
                m_state.edges.push_back({srcCh + 1, dstMaster, gain * k3dB});

            } else {
                // stereo → stereo  (two independent panners)
                float glL, grL, glR, grR;
                equalPowerPan(ss.panL, glL, grL);
                equalPowerPan(ss.panR, glR, grR);
                // Left source panned in dest
                m_state.edges.push_back({srcCh,     dstMaster,     gain * glL});
                m_state.edges.push_back({srcCh,     dstMaster + 1, gain * grL});
                // Right source panned in dest
                m_state.edges.push_back({srcCh + 1, dstMaster,     gain * glR});
                m_state.edges.push_back({srcCh + 1, dstMaster + 1, gain * grR});
            }
        }
    }
    m_state.sendStart[static_cast<size_t>(n)] = static_cast<int>(m_state.edges.size());
    (void)pos;  // pos reserved for future reordering optimizations
}

// ---------------------------------------------------------------------------

bool SendRouter::wouldCreateCycle(int srcCh, int dstCh,
                                  const ShowFile::AudioSetup& as) const
{
    const int nCh = static_cast<int>(as.channels.size());
    if (srcCh < 0 || srcCh >= nCh || dstCh < 0 || dstCh >= nCh) return false;
    if (srcCh == dstCh) return true;

    // BFS from dstCh in the current adjacency; if we reach srcCh, adding
    // srcCh→dstCh would close a cycle.
    // We use m_adj which reflects the currently committed send graph.
    if (m_adj.empty()) return false;
    std::vector<bool> visited(static_cast<size_t>(nCh), false);
    std::queue<int> q;
    q.push(dstCh);
    visited[static_cast<size_t>(dstCh)] = true;
    while (!q.empty()) {
        const int u = q.front(); q.pop();
        if (u == srcCh) return true;
        if (u >= static_cast<int>(m_adj.size())) continue;
        for (int v : m_adj[static_cast<size_t>(u)]) {
            if (!visited[static_cast<size_t>(v)]) {
                visited[static_cast<size_t>(v)] = true;
                q.push(v);
            }
        }
    }
    return false;
}

void SendRouter::rebuildTopology(const ShowFile::AudioSetup& as)
{
    const int nCh = static_cast<int>(as.channels.size());

    // Master gains (per logical channel, in channelOrder).
    // Populated after topo sort.
    buildAdj(as);
    topoSort(nCh);
    compileSendEdges(as);

    // Build master gains in channelOrder.
    m_state.masterGains.resize(static_cast<size_t>(nCh));
    for (int i = 0; i < nCh; ++i) {
        const int ch = m_state.channelOrder[static_cast<size_t>(i)];
        const auto& c = as.channels[static_cast<size_t>(ch)];
        const float g = c.mute ? 0.0f
                      : (c.masterGainDb <= -144.0f ? 0.0f
                         : std::pow(10.0f, c.masterGainDb * 0.05f));
        m_state.masterGains[static_cast<size_t>(i)] = g;
    }
}

void SendRouter::rebuildGains(const ShowFile::AudioSetup& as)
{
    const int nCh = static_cast<int>(as.channels.size());

    // Recompile send edge gains in place (topology unchanged).
    compileSendEdges(as);

    // Refresh master gains.
    m_state.masterGains.resize(static_cast<size_t>(nCh));
    for (int i = 0; i < static_cast<int>(m_state.channelOrder.size()); ++i) {
        const int ch = m_state.channelOrder[static_cast<size_t>(i)];
        if (ch >= nCh) continue;
        const auto& c = as.channels[static_cast<size_t>(ch)];
        const float g = c.mute ? 0.0f
                      : (c.masterGainDb <= -144.0f ? 0.0f
                         : std::pow(10.0f, c.masterGainDb * 0.05f));
        m_state.masterGains[static_cast<size_t>(i)] = g;
    }
}

} // namespace mcp
