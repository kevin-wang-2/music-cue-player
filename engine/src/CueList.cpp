#include "engine/CueList.h"
#include "engine/AudioMath.h"
#include "engine/IAudioSource.h"
#include "engine/plugin/AudioProcessor.h"
#include "engine/StreamReader.h"
#include "LtcStreamReader.h"
#include "MtcGenerator.h"
#include "MidiSender.h"
#include "NetworkSender.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <cmath>
#include <limits>
#include <thread>

// ---------------------------------------------------------------------------
// Built-in AutoParam implementations
// Access mcp::CueList internals via the friend declarations in CueList.h.
// Defined outside namespace mcp so the friend specifiers resolve to ::mcp_detail.

namespace mcp_detail {

// Send slot params queue updates only; the AppModel drain path applies them to
// sf.audioSetup and calls rebuildSendGains() on the main thread.
class SendParamBase : public mcp::AutoParam {
protected:
    mcp::CueList* m_list;
    int           m_ch, m_slot;
    std::string   m_path;
public:
    SendParamBase(mcp::CueList* list, int ch, int slot, std::string path)
        : m_list(list), m_ch(ch), m_slot(slot), m_path(std::move(path)) {}

    void setValue(double value) override {
        std::lock_guard<std::mutex> lk(m_list->m_scriptletMutex);
        m_list->m_pendingAutomationUpdates.emplace_back(m_path, value);
    }
};

class SendLevelParam final : public SendParamBase {
public:
    using SendParamBase::SendParamBase;
    Domain domain() const override { return Domain::FaderTaper; }
};
class SendMuteParam final : public SendParamBase {
public:
    using SendParamBase::SendParamBase;
    Domain domain() const override { return Domain::Linear; }
};
class SendPanParam final : public SendParamBase {
public:
    using SendParamBase::SendParamBase;
    Domain domain() const override { return Domain::Linear; }
};

class ChannelFaderParam final : public mcp::AutoParam {
    mcp::CueList* m_list;
    int           m_ch, m_slave;
    std::string   m_path;
public:
    ChannelFaderParam(mcp::CueList* list, int ch, int slave, std::string path)
        : m_list(list), m_ch(ch), m_slave(slave), m_path(std::move(path)) {}

    Domain domain() const override { return Domain::FaderTaper; }

    void setValue(double value) override {
        auto& cm          = m_list->m_channelMap;
        const int numCh   = cm.numCh;
        const int numPhys = cm.numPhys;
        if (m_ch >= numCh || cm.xpLinear.empty()) return;
        const float fVal  = static_cast<float>(value);
        auto applyFold = [&](int tCh) {
            const size_t tSz = static_cast<size_t>(tCh);
            cm.liveMasterGainDb[tSz] = fVal;
            const float lin = cm.liveMute[tSz] ? 0.0f
                            : ((fVal <= -60.0f) ? 0.0f : mcp::lut::dBToLinear(fVal));
            for (int p = 0; p < numPhys; ++p) {
                const size_t idx = static_cast<size_t>(tCh * numPhys + p);
                cm.fold[idx] = cm.xpLinear[idx] * lin;
            }
        };
        applyFold(m_ch);
        if (m_slave >= 0 && m_slave < numCh) applyFold(m_slave);
        m_list->pushFoldToEngine();
        { std::lock_guard<std::mutex> lk(m_list->m_scriptletMutex);
          m_list->m_pendingAutomationUpdates.emplace_back(m_path, value); }
    }
};

class ChannelMuteParam final : public mcp::AutoParam {
    mcp::CueList* m_list;
    int           m_ch, m_slave;
    std::string   m_path;
public:
    ChannelMuteParam(mcp::CueList* list, int ch, int slave, std::string path)
        : m_list(list), m_ch(ch), m_slave(slave), m_path(std::move(path)) {}

    Domain domain() const override { return Domain::Linear; }

    void setValue(double value) override {
        auto& cm          = m_list->m_channelMap;
        const int numCh   = cm.numCh;
        const int numPhys = cm.numPhys;
        if (m_ch >= numCh || cm.xpLinear.empty()) return;
        const bool muted  = (value >= 0.5);
        auto applyFold = [&](int tCh) {
            const size_t tSz = static_cast<size_t>(tCh);
            cm.liveMute[tSz] = muted;
            const float dB  = cm.liveMasterGainDb[tSz];
            const float lin = muted ? 0.0f
                            : ((dB <= -60.0f) ? 0.0f : mcp::lut::dBToLinear(dB));
            for (int p = 0; p < numPhys; ++p) {
                const size_t idx = static_cast<size_t>(tCh * numPhys + p);
                cm.fold[idx] = cm.xpLinear[idx] * lin;
            }
        };
        applyFold(m_ch);
        if (m_slave >= 0 && m_slave < numCh) applyFold(m_slave);
        m_list->pushFoldToEngine();
        { std::lock_guard<std::mutex> lk(m_list->m_scriptletMutex);
          m_list->m_pendingAutomationUpdates.emplace_back(m_path, value); }
    }
};

class CrosspointParam final : public mcp::AutoParam {
    mcp::CueList* m_list;
    int           m_ch, m_out;
    std::string   m_path;
public:
    CrosspointParam(mcp::CueList* list, int ch, int out, std::string path)
        : m_list(list), m_ch(ch), m_out(out), m_path(std::move(path)) {}

    Domain domain() const override { return Domain::DB; }

    void setValue(double value) override {
        auto& cm          = m_list->m_channelMap;
        const int numCh   = cm.numCh;
        const int numPhys = cm.numPhys;
        if (m_ch >= numCh || m_out >= numPhys || cm.xpLinear.empty()) return;
        const size_t chSz = static_cast<size_t>(m_ch);
        const float xpLin = (value <= -144.0) ? 0.0f
                          : mcp::lut::dBToLinear(static_cast<float>(value));
        const size_t idx  = static_cast<size_t>(m_ch * numPhys + m_out);
        cm.xpLinear[idx]  = xpLin;
        const float dB    = cm.liveMasterGainDb[chSz];
        const float lin   = cm.liveMute[chSz] ? 0.0f
                          : ((dB <= -60.0f) ? 0.0f : mcp::lut::dBToLinear(dB));
        cm.fold[idx]      = xpLin * lin;
        m_list->pushFoldToEngine();
        { std::lock_guard<std::mutex> lk(m_list->m_scriptletMutex);
          m_list->m_pendingAutomationUpdates.emplace_back(m_path, value); }
    }
};

class PluginParamAutoParam final : public mcp::AutoParam {
    mcp::CueList*  m_list;
    int            m_ch;
    int            m_slot;
    std::string    m_paramId;
    std::string    m_path;
    // Cached weak_ptr to avoid map lookup on every setValue().
    mutable std::weak_ptr<mcp::plugin::AudioProcessor> m_weakProc;
public:
    PluginParamAutoParam(mcp::CueList* list, int ch, int slot,
                         std::string paramId, std::string path)
        : m_list(list), m_ch(ch), m_slot(slot),
          m_paramId(std::move(paramId)), m_path(std::move(path))
    {
        if (m_list->m_pluginAccessor)
            m_weakProc = m_list->m_pluginAccessor(ch, slot);
    }

    Domain domain() const override {
        auto proc = m_weakProc.lock();
        if (!proc && m_list->m_pluginAccessor)
            m_weakProc = proc = m_list->m_pluginAccessor(m_ch, m_slot);
        if (proc) {
            for (const auto& info : proc->getParameters())
                if (info.id == m_paramId) return info.domain;
        }
        return Domain::Linear;
    }

    void setValue(double value) override {
        auto proc = m_weakProc.lock();
        if (!proc && m_list->m_pluginAccessor)
            m_weakProc = proc = m_list->m_pluginAccessor(m_ch, m_slot);
        if (!proc) return;
        proc->setParameterValue(m_paramId, static_cast<float>(value));
        { std::lock_guard<std::mutex> lk(m_list->m_scriptletMutex);
          m_list->m_pendingAutomationUpdates.emplace_back(m_path, value); }
    }
};

// ---------------------------------------------------------------------------
// PCHIP (Monotone Cubic Hermite) interpolation helpers
// Used to pre-compute automation value tables at fire() time.

static void pchipDerivs(const double* t, const double* y, double* d, int n) {
    // n = number of points (n >= 2).  Computes n tangent derivatives in d[].
    // Fritsch-Carlson algorithm: monotone within each interval.
    if (n < 2) return;
    if (n == 2) {
        const double slope = (t[1] > t[0]) ? (y[1] - y[0]) / (t[1] - t[0]) : 0.0;
        d[0] = d[1] = slope;
        return;
    }

    // Step sizes and chord slopes
    std::vector<double> h(static_cast<size_t>(n - 1));
    std::vector<double> delta(static_cast<size_t>(n - 1));
    for (int k = 0; k < n - 1; ++k) {
        h[static_cast<size_t>(k)]     = t[k + 1] - t[k];
        delta[static_cast<size_t>(k)] = (h[static_cast<size_t>(k)] > 0.0)
            ? (y[k + 1] - y[k]) / h[static_cast<size_t>(k)] : 0.0;
    }

    // One-sided endpoint derivatives, clamped for monotonicity
    auto endDeriv = [](double hk, double hk1, double dk, double dk1) -> double {
        const double d0 = ((2.0 * hk + hk1) * dk - hk * dk1) / (hk + hk1);
        if (dk == 0.0 || dk1 == 0.0) return 0.0;
        if ((dk > 0.0) != (d0 > 0.0)) return 0.0;
        if (std::abs(d0) > 3.0 * std::abs(dk)) return 3.0 * dk;
        return d0;
    };
    d[0]     = endDeriv(h[0], h[1], delta[0], delta[1]);
    d[n - 1] = endDeriv(h[static_cast<size_t>(n - 2)], h[static_cast<size_t>(n - 3)],
                        delta[static_cast<size_t>(n - 2)], delta[static_cast<size_t>(n - 3)]);

    // Interior derivatives: weighted harmonic mean
    for (int k = 1; k < n - 1; ++k) {
        const double dk0 = delta[static_cast<size_t>(k - 1)];
        const double dk1 = delta[static_cast<size_t>(k)];
        if (dk0 * dk1 <= 0.0) {
            d[k] = 0.0; // local extremum or flat
        } else {
            const double w1 = 2.0 * h[static_cast<size_t>(k)] + h[static_cast<size_t>(k - 1)];
            const double w2 = h[static_cast<size_t>(k)] + 2.0 * h[static_cast<size_t>(k - 1)];
            d[k] = (w1 + w2) / (w1 / dk0 + w2 / dk1);
        }
    }
}

static double pchipEval(const double* t, const double* y, const double* d, int n, double x) {
    if (n <= 0) return 0.0;
    if (x <= t[0]) return y[0];
    if (x >= t[n - 1]) return y[n - 1];
    // Binary search for the interval
    int lo = 0, hi = n - 2;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (t[mid] <= x) lo = mid; else hi = mid - 1;
    }
    const double hk = t[lo + 1] - t[lo];
    if (hk <= 0.0) return y[lo];
    const double s  = (x - t[lo]) / hk;
    const double s2 = s * s, s3 = s2 * s;
    // Cubic Hermite basis
    const double h00 = 2*s3 - 3*s2 + 1;
    const double h10 = s3 - 2*s2 + s;
    const double h01 = -2*s3 + 3*s2;
    const double h11 = s3 - s2;
    return h00 * y[lo] + h10 * hk * d[lo] + h01 * y[lo + 1] + h11 * hk * d[lo + 1];
}

} // namespace mcp_detail

namespace mcp {

// Each CueList instance gets a unique block of tag space so that voice tags from
// different CueLists sharing the same AudioEngine never collide.
static constexpr int kTagStride = 1'000'000;  // supports up to 1M cues per list
static std::atomic<int> s_nextTagBase{0};

CueList::CueList(AudioEngine& engine, Scheduler& scheduler)
    : m_engine(engine)
    , m_scheduler(scheduler)
    , m_tagBase(s_nextTagBase.fetch_add(kTagStride, std::memory_order_relaxed))
{}

CueList::~CueList() { panic(); }

void CueList::pushSlots() {
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_cueRuntime.push_back({});
    m_pendingEventId.push_back(-1);
    m_cueFireFrame.push_back(-1);
    m_cueFireArmBase.push_back(0.0);
    m_lastFilePosSec.push_back(-1.0);
}

// ---------------------------------------------------------------------------
// List construction

bool CueList::addCue(const std::string& path, const std::string& name,
                     double preWait) {
    Cue cue;
    cue.type           = CueType::Audio;
    cue.path           = path;
    cue.name           = name.empty() ? path : name;
    cue.preWaitSeconds = preWait;
    // Metadata-only load: validates format without reading all PCM data.
    if (!cue.audioFile.loadMetadata(path)) return false;

    // SR / channel-count mismatches are handled by SRC and channel mapping in
    // StreamReader — any format accepted here as long as it can be opened.

    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addStartCue(int targetIndex, const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Start;
    cue.targetIndex    = targetIndex;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty()
                             ? ("start(" + std::to_string(targetIndex) + ")")
                             : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addStopCue(int targetIndex, const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Stop;
    cue.targetIndex    = targetIndex;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty()
                             ? ("stop(" + std::to_string(targetIndex) + ")")
                             : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addFadeCue(int resolvedTargetIdx, const std::string& targetCueNumber,
                          FadeData::Curve curve,
                          bool stopWhenDone,
                          const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Fade;
    cue.preWaitSeconds = preWait;
    cue.duration       = 3.0;  // default; overridden by setCueDuration after construction
    cue.name           = name.empty()
                             ? ("fade(Q" + targetCueNumber + ")")
                             : name;
    cue.targetIndex = resolvedTargetIdx;  // mirrors fadeData so targetLabel works
    cue.fadeData = std::make_shared<FadeData>();
    cue.fadeData->targetCueNumber   = targetCueNumber;
    cue.fadeData->resolvedTargetIdx = resolvedTargetIdx;
    cue.fadeData->curve             = curve;
    cue.fadeData->stopWhenDone      = stopWhenDone;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addBrokenAudioCue(const std::string& path, const std::string& name,
                                 double preWait) {
    Cue cue;
    cue.type           = CueType::Audio;
    cue.path           = path;
    cue.name           = name.empty() ? (path.empty() ? "Audio Cue" : path) : name;
    cue.preWaitSeconds = preWait;
    // Intentionally skip loadMetadata — this is a placeholder/broken cue.
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addArmCue(int targetIndex, const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Arm;
    cue.targetIndex    = targetIndex;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty()
                             ? ("arm(Q" + std::to_string(targetIndex) + ")")
                             : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addDevampCue(int targetIndex, const std::string& name, double preWait,
                            int devampMode) {
    Cue cue;
    cue.type           = CueType::Devamp;
    cue.targetIndex    = targetIndex;
    cue.devampMode     = devampMode;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty()
                             ? ("devamp(Q" + std::to_string(targetIndex) + ")")
                             : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addGroupCue(GroupData::Mode mode, bool random,
                           const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Group;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Group" : name;
    cue.groupData      = std::make_unique<GroupData>();
    cue.groupData->mode   = mode;
    cue.groupData->random = random;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addMCCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::MusicContext;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "MC" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addMarkerCue(int targetIndex, int markerIndex,
                            const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Marker;
    cue.targetIndex    = targetIndex;
    cue.markerIndex    = markerIndex;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty()
        ? ("mk(" + std::to_string(targetIndex) + ":" + std::to_string(markerIndex) + ")")
        : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addGotoCue(int targetIndex, const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Goto;
    cue.targetIndex    = targetIndex;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Goto" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addMemoCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Memo;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Memo" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addScriptletCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Scriptlet;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Scriptlet" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setCueScriptletCode(int index, const std::string& code) {
    if (index >= 0 && index < (int)m_cues.size() &&
        m_cues[static_cast<size_t>(index)].type == CueType::Scriptlet)
        m_cues[static_cast<size_t>(index)].scriptletCode = code;
}

std::vector<std::pair<int, std::string>> CueList::drainScriptlets() {
    std::lock_guard<std::mutex> lk(m_scriptletMutex);
    std::vector<std::pair<int, std::string>> out;
    out.swap(m_pendingScriptlets);
    return out;
}

std::vector<int> CueList::drainFiredCues() {
    std::lock_guard<std::mutex> lk(m_scriptletMutex);
    std::vector<int> out;
    out.swap(m_pendingFiredCues);
    return out;
}

bool CueList::addSnapshotCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Snapshot;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Snapshot" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setCueSnapshotId(int index, int snapshotId) {
    if (index >= 0 && index < static_cast<int>(m_cues.size()))
        m_cues[static_cast<size_t>(index)].snapshotId = snapshotId;
}

std::vector<std::pair<int, int>> CueList::drainPendingSnapshots() {
    std::lock_guard<std::mutex> lk(m_scriptletMutex);
    std::vector<std::pair<int, int>> out;
    out.swap(m_pendingSnapshots);
    return out;
}

bool CueList::addAutomationCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Automation;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Automation" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setCueAutomationPath(int index, const std::string& path) {
    if (index >= 0 && index < (int)m_cues.size())
        m_cues[static_cast<size_t>(index)].automationPath = path;
}

void CueList::setCueAutomationCurve(int index, const std::vector<Cue::AutomationPoint>& pts) {
    if (index >= 0 && index < (int)m_cues.size())
        m_cues[static_cast<size_t>(index)].automationCurve = pts;
}

void CueList::setCueAutomationDuration(int index, double seconds) {
    if (index >= 0 && index < (int)m_cues.size())
        m_cues[static_cast<size_t>(index)].automationDuration = seconds;
}

std::vector<std::pair<std::string, double>> CueList::drainAutomationUpdates() {
    std::vector<std::pair<std::string, double>> out;
    std::lock_guard<std::mutex> lk(m_scriptletMutex);
    out.swap(m_pendingAutomationUpdates);
    return out;
}

bool CueList::addDeactivateCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Deactivate;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Deactivate Plugin" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

bool CueList::addReactivateCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Reactivate;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Reactivate Plugin" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setCuePluginSlot(int index, int channel, int slot) {
    if (index < 0 || index >= (int)m_cues.size()) return;
    m_cues[static_cast<size_t>(index)].pluginChannel = channel;
    m_cues[static_cast<size_t>(index)].pluginSlot    = slot;
}

bool CueList::addNetworkCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Network;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Network Cue" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setNetworkPatches(std::vector<ShowFile::NetworkSetup::Patch> patches) {
    m_networkPatches = std::move(patches);
}

void CueList::setCueNetworkPatch(int index, int patchIdx) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[static_cast<size_t>(index)].type == CueType::Network)
        m_cues[static_cast<size_t>(index)].networkPatchIdx = patchIdx;
}

void CueList::setCueNetworkCommand(int index, const std::string& command) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[static_cast<size_t>(index)].type == CueType::Network)
        m_cues[static_cast<size_t>(index)].networkCommand = command;
}

std::string CueList::networkPatchName(int patchIdx) const {
    if (patchIdx >= 0 && patchIdx < (int)m_networkPatches.size())
        return m_networkPatches[static_cast<size_t>(patchIdx)].name;
    return {};
}

int CueList::networkPatchCount() const {
    return static_cast<int>(m_networkPatches.size());
}

bool CueList::addMidiCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Midi;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "MIDI Cue" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setMidiPatches(std::vector<ShowFile::MidiSetup::Patch> patches) {
    m_midiPatches = std::move(patches);
}

void CueList::setCueMidiPatch(int index, int patchIdx) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[static_cast<size_t>(index)].type == CueType::Midi)
        m_cues[static_cast<size_t>(index)].midiPatchIdx = patchIdx;
}

void CueList::setCueMidiMessage(int index, const std::string& type,
                                int channel, int data1, int data2) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[static_cast<size_t>(index)].type == CueType::Midi) {
        auto& c = m_cues[static_cast<size_t>(index)];
        c.midiMessageType = type;
        c.midiChannel     = channel;
        c.midiData1       = data1;
        c.midiData2       = data2;
    }
}

std::string CueList::midiPatchName(int patchIdx) const {
    if (patchIdx >= 0 && patchIdx < (int)m_midiPatches.size())
        return m_midiPatches[static_cast<size_t>(patchIdx)].name;
    return {};
}

int CueList::midiPatchCount() const {
    return static_cast<int>(m_midiPatches.size());
}

bool CueList::addTimecodeCue(const std::string& name, double preWait) {
    Cue cue;
    cue.type           = CueType::Timecode;
    cue.preWaitSeconds = preWait;
    cue.name           = name.empty() ? "Timecode Cue" : name;
    cue.stableId = m_nextStableId++;
    m_cues.push_back(std::move(cue));
    pushSlots();
    return true;
}

void CueList::setCueTcType(int index, const std::string& type) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcType = type;
}

void CueList::setCueTcFps(int index, TcFps fps) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcFps = fps;
}

void CueList::setCueTcStart(int index, const TcPoint& tc) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcStartTC = tc;
}

void CueList::setCueTcEnd(int index, const TcPoint& tc) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcEndTC = tc;
}

void CueList::setCueTcLtcChannel(int index, int physOutCh) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcLtcChannel = physOutCh;
}

void CueList::setCueTcMidiPatch(int index, int patchIdx) {
    if (index >= 0 && index < (int)m_cues.size() && m_cues[index].type == CueType::Timecode)
        m_cues[index].tcMidiPatchIdx = patchIdx;
}

// ---------------------------------------------------------------------------
// Structural mutations — engine is the source of truth

// Helper: shift all flat-index refs >= k upward by 1.
void CueList::fixupRefsForInsert(int k) {
    auto shiftUp = [k](int& idx) { if (idx >= k) ++idx; };
    for (auto& c : m_cues) {
        shiftUp(c.targetIndex);
        shiftUp(c.parentIndex);
        shiftUp(c.mcSourceIdx);
        for (auto& mk : c.markers) shiftUp(mk.anchorMarkerCueIdx);
        // Ancestors: any Group that contains position k gets childCount+1.
        // NOTE: we iterate here but the group check below handles childCount.
    }
    // Increment childCount for every group that contains position k.
    for (int g = 0; g < (int)m_cues.size(); ++g)
        if (m_cues[g].groupData && g < k && k <= g + m_cues[g].childCount)
            ++m_cues[g].childCount;
    if (m_selectedIndex >= k) ++m_selectedIndex;
    for (auto& fu : m_followUps)    if (fu.watchCueIdx >= k) ++fu.watchCueIdx;
    for (auto& fu : m_playlistFollowUps) {
        if (fu.watchCueIdx >= k) ++fu.watchCueIdx;
        if (fu.nextCueIdx  >= k) ++fu.nextCueIdx;
    }
    { std::lock_guard<std::mutex> lk(m_scriptletMutex);
      for (auto& i : m_pendingFiredCues)  if (i >= k) ++i;
      for (auto& p : m_pendingScriptlets) if (p.first >= k) ++p.first;
      for (auto& p : m_pendingSnapshots)  if (p.first >= k) ++p.first; }
}

// Helper: shift refs after removal of [k, k+count).
void CueList::fixupRefsForRemove(int k, int count) {
    const int kEnd = k + count;
    auto shiftDown = [k, kEnd, count](int& idx) {
        if (idx >= k && idx < kEnd) idx = -1;
        else if (idx >= kEnd)       idx -= count;
    };
    for (auto& c : m_cues) {
        shiftDown(c.targetIndex);
        shiftDown(c.parentIndex);
        shiftDown(c.mcSourceIdx);
        for (auto& mk : c.markers) shiftDown(mk.anchorMarkerCueIdx);
    }
    // Decrement childCount for groups that contained the removed range.
    for (int g = 0; g < (int)m_cues.size(); ++g) {
        if (!m_cues[g].groupData) continue;
        // How many of the removed cues were inside group g?
        const int gEnd = g + m_cues[g].childCount;
        const int overlapStart = std::max(g + 1, k);
        const int overlapEnd   = std::min(gEnd + 1, kEnd);
        if (overlapStart < overlapEnd)
            m_cues[g].childCount -= (overlapEnd - overlapStart);
    }
    if (m_selectedIndex >= k && m_selectedIndex < kEnd)
        m_selectedIndex = k;
    else if (m_selectedIndex >= kEnd)
        m_selectedIndex -= count;
    m_selectedIndex = std::max(0, m_selectedIndex);
    // followUps: remove entries watching deleted cues, shift the rest.
    m_followUps.erase(std::remove_if(m_followUps.begin(), m_followUps.end(),
        [k, kEnd](const FollowUp& fu) { return fu.watchCueIdx >= k && fu.watchCueIdx < kEnd; }),
        m_followUps.end());
    for (auto& fu : m_followUps) if (fu.watchCueIdx >= kEnd) fu.watchCueIdx -= count;
    m_playlistFollowUps.erase(std::remove_if(m_playlistFollowUps.begin(), m_playlistFollowUps.end(),
        [k, kEnd](const PlaylistFollowUp& fu) {
            return (fu.watchCueIdx >= k && fu.watchCueIdx < kEnd) ||
                   (fu.nextCueIdx  >= k && fu.nextCueIdx  < kEnd);
        }), m_playlistFollowUps.end());
    for (auto& fu : m_playlistFollowUps) {
        if (fu.watchCueIdx >= kEnd) fu.watchCueIdx -= count;
        if (fu.nextCueIdx  >= kEnd) fu.nextCueIdx  -= count;
    }
    // Pending queues: remove stale, shift remainder (under m_scriptletMutex).
    { std::lock_guard<std::mutex> lk(m_scriptletMutex);
      auto drop = [k, kEnd, count](int& i) -> bool {
          if (i >= k && i < kEnd) return true;
          if (i >= kEnd) i -= count;
          return false;
      };
      m_pendingFiredCues.erase(std::remove_if(m_pendingFiredCues.begin(),
          m_pendingFiredCues.end(), [&](int& i){ return drop(i); }), m_pendingFiredCues.end());
      m_pendingScriptlets.erase(std::remove_if(m_pendingScriptlets.begin(),
          m_pendingScriptlets.end(), [&](auto& p){ return drop(p.first); }), m_pendingScriptlets.end());
      m_pendingSnapshots.erase(std::remove_if(m_pendingSnapshots.begin(),
          m_pendingSnapshots.end(), [&](auto& p){ return drop(p.first); }), m_pendingSnapshots.end());
    }
}

// Helper: fix refs after rotating [from, from+count) to destination `to` (original indexing).
void CueList::fixupRefsForMove(int from, int count, int to) {
    const int n = static_cast<int>(m_cues.size());
    // Build oldIdx → newIdx table.
    std::vector<int> remap(static_cast<size_t>(n));
    std::iota(remap.begin(), remap.end(), 0);
    if (to > from + count) {
        // Moving forward: [from..from+count) goes to [to-count..to)
        for (int i = from; i < from + count; ++i)       remap[i] = i + (to - from - count);
        for (int i = from + count; i < to; ++i)         remap[i] = i - count;
    } else if (to < from) {
        // Moving backward: [from..from+count) goes to [to..to+count)
        for (int i = to; i < from; ++i)                 remap[i] = i + count;
        for (int i = from; i < from + count; ++i)       remap[i] = i - from + to;
    }
    auto applyRemap = [&](int& idx) {
        if (idx >= 0 && idx < n) idx = remap[idx];
    };
    for (auto& c : m_cues) {
        applyRemap(c.targetIndex);
        applyRemap(c.parentIndex);
        applyRemap(c.mcSourceIdx);
        for (auto& mk : c.markers) applyRemap(mk.anchorMarkerCueIdx);
    }
    applyRemap(m_selectedIndex);
    for (auto& fu : m_followUps) applyRemap(fu.watchCueIdx);
    for (auto& fu : m_playlistFollowUps) {
        applyRemap(fu.watchCueIdx);
        applyRemap(fu.nextCueIdx);
    }
    { std::lock_guard<std::mutex> lk(m_scriptletMutex);
      for (auto& i : m_pendingFiredCues)  applyRemap(i);
      for (auto& p : m_pendingScriptlets) applyRemap(p.first);
      for (auto& p : m_pendingSnapshots)  applyRemap(p.first); }
}

int CueList::insertCueAt(int idx, Cue cue) {
    const int n = static_cast<int>(m_cues.size());
    if (idx < 0 || idx > n) return -1;
    cue.stableId = m_nextStableId++;
    const int sid = cue.stableId;
    fixupRefsForInsert(idx);
    m_cues.insert(m_cues.begin() + idx, std::move(cue));
    { std::lock_guard<std::mutex> lk(m_slotMutex);
      m_cueRuntime.insert(m_cueRuntime.begin() + idx, CueRuntime{});
      m_pendingEventId.insert(m_pendingEventId.begin() + idx, -1); }
    m_cueFireFrame.insert(m_cueFireFrame.begin() + idx, -1LL);
    m_cueFireArmBase.insert(m_cueFireArmBase.begin() + idx, 0.0);
    m_lastFilePosSec.insert(m_lastFilePosSec.begin() + idx, -1.0);
    return sid;
}

void CueList::removeCueAt(int idx) {
    const int n = static_cast<int>(m_cues.size());
    if (idx < 0 || idx >= n) return;
    const int count = 1 + m_cues[idx].childCount;
    const int kEnd  = idx + count;
    // Stop all voices and running automations for the removed range.
    for (int i = idx; i < kEnd; ++i) {
        const int sid = m_cues[i].stableId;
        // Cancel pending prewait
        int evtId = -1;
        { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[i]; m_pendingEventId[i] = -1; }
        if (evtId >= 0) m_scheduler.cancel(evtId);
        m_engine.clearVoicesByTag(m_tagBase + sid);
        { std::lock_guard<std::mutex> lk(m_scriptletMutex);
          auto it = m_automationCancel.find(sid);
          if (it != m_automationCancel.end()) {
              it->second->store(true, std::memory_order_relaxed);
              m_automationCancel.erase(it);
          } }
    }
    fixupRefsForRemove(idx, count);
    m_cues.erase(m_cues.begin() + idx, m_cues.begin() + kEnd);
    { std::lock_guard<std::mutex> lk(m_slotMutex);
      m_cueRuntime.erase(m_cueRuntime.begin() + idx, m_cueRuntime.begin() + kEnd);
      m_pendingEventId.erase(m_pendingEventId.begin() + idx, m_pendingEventId.begin() + kEnd); }
    m_cueFireFrame.erase(m_cueFireFrame.begin() + idx, m_cueFireFrame.begin() + kEnd);
    m_cueFireArmBase.erase(m_cueFireArmBase.begin() + idx, m_cueFireArmBase.begin() + kEnd);
    m_lastFilePosSec.erase(m_lastFilePosSec.begin() + idx, m_lastFilePosSec.begin() + kEnd);
}

void CueList::moveCueTo(int from, int to) {
    const int n = static_cast<int>(m_cues.size());
    if (from < 0 || from >= n || to < 0 || to > n) return;
    if (to == from || to == from + 1) return;   // no-op
    const int count = 1 + m_cues[from].childCount;
    if (to > from && to < from + count) return; // can't move inside own subtree
    // Rotate m_cues and all parallel arrays so voices keep playing.
    auto rot = [&](auto& v) {
        if (to > from + count)
            std::rotate(v.begin() + from, v.begin() + from + count, v.begin() + to);
        else
            std::rotate(v.begin() + to, v.begin() + from, v.begin() + from + count);
    };
    rot(m_cues);
    rot(m_cueFireFrame);
    rot(m_cueFireArmBase);
    rot(m_lastFilePosSec);
    { std::lock_guard<std::mutex> lk(m_slotMutex);
      rot(m_cueRuntime);
      rot(m_pendingEventId); }
    fixupRefsForMove(from, count, to);
}

bool CueList::reloadCueAudio(int idx, const std::string& newPath, const std::string& /*baseDir*/) {
    if (idx < 0 || idx >= (int)m_cues.size()) return false;
    auto& cue = m_cues[idx];
    if (cue.type != CueType::Audio) return false;
    stop(idx);
    disarm(idx);
    cue.path = newPath;
    cue.audioFile = AudioFile{};
    if (!newPath.empty() && !cue.audioFile.loadMetadata(newPath)) return false;
    return true;
}

void CueList::clear() {
    panic();
    // Signal all active I/O threads to stop early.
    for (auto& s : m_slotStream)
        if (s) s->requestStop();
    // Give the audio callback one tick to process pending clears before
    // releasing the raw StreamReader pointers it might still be touching.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    for (auto& s : m_slotStream) s.reset();
    // Disarm all cues (these are not in the engine, safe to destroy now).
    for (auto& cue : m_cues) {
        if (cue.armedStream) { cue.armedStream->requestStop(); cue.armedStream.reset(); }
    }
    m_cues.clear();
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_cueRuntime.clear();
    m_pendingEventId.clear();
    m_cueFireFrame.clear();
    m_cueFireArmBase.clear();
    m_lastFilePosSec.clear();
    m_selectedIndex = 0;
}

static int64_t voiceFrames(const Cue& cue);  // defined in "Internal helpers" below

// ---------------------------------------------------------------------------
// ARM

bool CueList::arm(int index, double startOverride) {
    if (index < 0 || index >= cueCount()) return false;
    const auto& cue = m_cues[index];
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return false;

    const int    sr    = m_engine.isInitialized() ? m_engine.sampleRate() : 48000;
    const int    ch    = (m_channelMap.numCh > 0) ? m_channelMap.numCh
                       : (m_engine.isInitialized() ? m_engine.channels() : 2);
    const double start = (startOverride >= 0.0) ? startOverride : cue.startTime;
    auto reader = std::make_shared<StreamReader>(
        cue.path, sr, ch, start, cue.duration);
    if (reader->hasError()) return false;

    std::lock_guard<std::mutex> lk(m_slotMutex);
    if (m_cues[index].armedStream)
        m_cues[index].armedStream->requestStop();
    m_cues[index].armedStream = std::move(reader);
    return true;
}

void CueList::disarm(int index) {
    if (index < 0 || index >= cueCount()) return;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    if (m_cues[index].armedStream) {
        m_cues[index].armedStream->requestStop();
        m_cues[index].armedStream.reset();
    }
}

bool CueList::isArmed(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    const auto& cue = m_cues[index];

    // Audio cue with pre-buffered stream
    if (cue.type == CueType::Audio) {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        const auto& s = cue.armedStream;
        if (s && !s->hasError() && s->isArmed()) return true;
    }

    // Timeline or Sync group with arm position set
    if (cue.type == CueType::Group && cue.groupData &&
        (cue.groupData->mode == GroupData::Mode::Timeline ||
         cue.groupData->mode == GroupData::Mode::Sync) &&
        cue.timelineArmSec > 0.0)
        return true;

    // Child of an armed Timeline/Sync group that would not be skipped
    const int pi = cue.parentIndex;
    if (pi >= 0 && pi < cueCount()) {
        const auto& parent = m_cues[pi];
        if (parent.type == CueType::Group && parent.groupData &&
            (parent.groupData->mode == GroupData::Mode::Timeline ||
             parent.groupData->mode == GroupData::Mode::Sync) &&
            parent.timelineArmSec > 0.0) {
            const double armInto  = parent.timelineArmSec - cue.timelineOffset;
            const double childDur = (cue.duration > 0.0) ? cue.duration
                : (cue.type == CueType::Audio ? cueTotalSeconds(index) : 2.0);
            return armInto < childDur;  // would fire (not entirely past arm point)
        }
    }

    return false;
}

void CueList::softPanic(double fadeSecs) {
    m_scheduler.cancelAll();
    m_followUps.clear();
    m_playlistFollowUps.clear();
    for (auto& s : m_slotStream)
        if (s) s->requestStop();
    m_engine.softPanic(fadeSecs);
    std::lock_guard<std::mutex> lk(m_slotMutex);
    for (auto& cue : m_cues) {
        if (cue.armedStream) {
            cue.armedStream->requestStop();
            cue.armedStream.reset();
        }
    }
}

void CueList::update() {
    // Process playlist follow-ups: fire the next child once the watched child finishes.
    for (auto it = m_playlistFollowUps.begin(); it != m_playlistFollowUps.end(); ) {
        if (!it->activated) {
            // Wait until the watched cue actually starts playing before monitoring.
            if (isCuePlaying(it->watchCueIdx) || isCuePending(it->watchCueIdx))
                it->activated = true;
            ++it;
        } else if (!isCuePlaying(it->watchCueIdx) && !isCuePending(it->watchCueIdx)) {
            const int next = it->nextCueIdx;
            it = m_playlistFollowUps.erase(it);
            if (next >= 0 && next < cueCount())
                fire(next);
        } else {
            ++it;
        }
    }

    // Process devamp follow-ups: fire the next cue when the trigger condition fires.
    for (auto it = m_followUps.begin(); it != m_followUps.end(); ) {
        bool triggered = false;
        if (it->waitForStop) {
            // Mode 1: wait for the target voice to go inactive
            triggered = !isCuePlaying(it->watchCueIdx);
        } else {
            // Mode 2: wait for StreamReader to signal segment transition
            StreamReader* raw = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_slotMutex);
                const int slot = m_cueRuntime[static_cast<size_t>(it->watchCueIdx)].canonicalSlot();
                if (slot >= 0) raw = slotStreamReader(static_cast<size_t>(slot));
            }
            if (raw && raw->wasDevampFired()) {
                raw->clearDevampFired();
                triggered = true;
            }
        }
        if (triggered) {
            it = m_followUps.erase(it);
            go();
        } else {
            ++it;
        }
    }

    // Release StreamReader resources for voice slots that have finished playing.
    // Guard: skip slots with pendingReady==true — the callback holds a raw pointer
    // to the StreamReader stored in m_slotStream and will dereference it once it
    // activates the voice.  Destroying the shared_ptr here before that happens
    // produces a use-after-free.
    for (int s = 0; s < AudioEngine::kMaxVoices; ++s) {
        if (m_slotStream[s]
            && !m_engine.isVoiceActive(s)
            && !m_engine.isVoicePending(s)) {
            m_slotStream[s]->requestStop();
            m_slotStream[s].reset();
            // Clear fire frame for Timecode LTC cues whose voice just ended.
            for (int ci = 0; ci < cueCount(); ++ci) {
                if (m_cues[ci].type == CueType::Timecode && [&](){
                    for (const auto& kv : m_cueRuntime[static_cast<size_t>(ci)].slotByDevice)
                        if (kv.second == s) return true;
                    return false; }())
                    m_cueFireFrame[static_cast<size_t>(ci)] = -1;
            }
        }
    }

    // Anchor-marker crossing: when an audio cue's playhead passes a TimeMarker that
    // has anchorMarkerCueIdx set, auto-advance the selected index to the cue after
    // the anchor Marker cue.
    for (int i = 0; i < cueCount(); ++i) {
        const auto& cue = m_cues[static_cast<size_t>(i)];
        if (cue.markers.empty()) continue;

        // Quick check: does this cue have any anchored markers?
        bool hasAnchor = false;
        for (const auto& mk : cue.markers)
            if (mk.anchorMarkerCueIdx >= 0) { hasAnchor = true; break; }
        if (!hasAnchor) continue;

        if (cue.type == CueType::Audio) {
            int slot;
            { std::lock_guard<std::mutex> lk(m_slotMutex); slot = m_cueRuntime[static_cast<size_t>(i)].canonicalSlot(); }
            const double curr = (slot >= 0 && m_engine.isVoiceActive(slot))
                                ? cuePlayheadFileSeconds(i) : 0.0;

            if (curr <= 0.0) {
                m_lastFilePosSec[static_cast<size_t>(i)] = -1.0;
                continue;
            }
            double& prev = m_lastFilePosSec[static_cast<size_t>(i)];
            if (prev < 0.0) {
                prev = curr;
                continue;
            }

            for (const auto& mk : cue.markers) {
                if (mk.anchorMarkerCueIdx < 0) continue;
                const double t = mk.time;
                bool crossed = (curr >= prev)
                    ? (t > prev && t <= curr)
                    : (t > prev || t <= curr);
                if (crossed) {
                    const int anchor = mk.anchorMarkerCueIdx;
                    if (anchor >= 0 && anchor < cueCount())
                        m_selectedIndex = logicalNext(anchor);
                }
            }
            prev = curr;

        } else if (cue.type == CueType::Group && cue.groupData &&
                   cue.groupData->mode == GroupData::Mode::Sync) {
            if (!isCuePlaying(i)) {
                m_lastFilePosSec[static_cast<size_t>(i)] = -1.0;
                continue;
            }
            const int currSlice = cue.groupData->syncPlaySlice;
            double& prev = m_lastFilePosSec[static_cast<size_t>(i)];
            if (prev < 0.0) {
                // First tick after the group started — initialise without triggering.
                prev = static_cast<double>(currSlice);
                continue;
            }
            const int prevSlice = static_cast<int>(prev);
            // Only fire on a single natural step forward (not devamp jumps).
            if (currSlice == prevSlice + 1 && prevSlice < (int)cue.markers.size()) {
                const int anchor = cue.markers[static_cast<size_t>(prevSlice)].anchorMarkerCueIdx;
                if (anchor >= 0 && anchor < cueCount())
                    m_selectedIndex = logicalNext(anchor);
            }
            prev = static_cast<double>(currSlice);
        }
    }

}

int  CueList::cueCount()      const { return static_cast<int>(m_cues.size()); }
int  CueList::selectedIndex() const { return m_selectedIndex; }

void CueList::setSelectedIndex(int index) {
    m_selectedIndex = std::clamp(index, 0, cueCount());
}
void CueList::selectNext() { if (m_selectedIndex < cueCount()) ++m_selectedIndex; }
void CueList::selectPrev() { if (m_selectedIndex > 0) --m_selectedIndex; }

const Cue* CueList::cueAt(int index) const {
    return (index >= 0 && index < cueCount()) ? &m_cues[index] : nullptr;
}
const Cue* CueList::selectedCue() const { return cueAt(m_selectedIndex); }

void CueList::setCuePreWait    (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].preWaitSeconds = s; }
void CueList::setCueGoQuantize (int i, int v)               { if (i>=0&&i<cueCount()) m_cues[i].goQuantize     = v; }
void CueList::setCueStartTime  (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].startTime      = s; }
void CueList::setCueDuration   (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].duration       = s; }
void CueList::setCueLevel       (int i, double dB)           { if (i>=0&&i<cueCount()) m_cues[i].level          = dB; }
void CueList::setCueTrim        (int i, double dB)           { if (i>=0&&i<cueCount()) m_cues[i].trim           = dB; }
void CueList::setCueAutoContinue(int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoContinue   = v; }
void CueList::setCueAutoFollow  (int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoFollow      = v; }
void CueList::setCueName        (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].name          = n; }
void CueList::setCueArmStartTime(int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].armStartTime   = s; }
void CueList::setCueCueNumber   (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].cueNumber     = n; }

void CueList::setCueTarget(int i, int targetIdx) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[i].targetIndex = targetIdx;
    if (m_cues[i].fadeData) m_cues[i].fadeData->resolvedTargetIdx = targetIdx;
}

void CueList::setCueCrossListTarget(int i, int numericId, int flatIdx) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[i].crossListNumericId = numericId;
    m_cues[i].crossListFlatIdx   = flatIdx;
}

void CueList::setCrossListStartCallback(CrossListCallback cb) { m_crossListStart = std::move(cb); }
void CueList::setCrossListStopCallback (CrossListCallback cb) { m_crossListStop  = std::move(cb); }

void CueList::setCueDevampMode   (int i, int mode) { if (i>=0&&i<cueCount()&&m_cues[i].type==CueType::Devamp) m_cues[i].devampMode    = mode; }
void CueList::setCueDevampPreVamp(int i, bool v)   { if (i>=0&&i<cueCount()&&m_cues[i].type==CueType::Devamp) m_cues[i].devampPreVamp = v; }

void CueList::setCueParentIndex   (int i, int p)   { if (i>=0&&i<cueCount()) m_cues[i].parentIndex    = p; }
void CueList::setCueChildCount    (int i, int c)   { if (i>=0&&i<cueCount()) m_cues[i].childCount     = c; }
void CueList::setCueTimelineOffset(int i, double s){ if (i>=0&&i<cueCount()) m_cues[i].timelineOffset  = s; }
void CueList::setCueTimelineArmSec(int i, double s){ if (i>=0&&i<cueCount()) m_cues[i].timelineArmSec = s; }
void CueList::setCueGroupMode(int i, GroupData::Mode m) {
    if (i>=0&&i<cueCount()&&m_cues[i].groupData) m_cues[i].groupData->mode = m;
}
void CueList::setCueGroupRandom(int i, bool r) {
    if (i>=0&&i<cueCount()&&m_cues[i].groupData) m_cues[i].groupData->random = r;
}

// ---------------------------------------------------------------------------
// Routing setters

void CueList::setCueOutLevel(int i, int outCh, float dB) {
    if (i < 0 || i >= cueCount() || outCh < 0) return;
    auto& r = m_cues[i].routing;
    if (outCh >= (int)r.outLevelDb.size())
        r.outLevelDb.resize(static_cast<size_t>(outCh + 1), 0.0f);
    r.outLevelDb[static_cast<size_t>(outCh)] = dB;
}

void CueList::setCueXpoint(int i, int srcCh, int outCh, std::optional<float> dB) {
    if (i < 0 || i >= cueCount() || srcCh < 0 || outCh < 0) return;
    auto& r = m_cues[i].routing;

    if (srcCh >= (int)r.xpoint.size())
        r.xpoint.resize(static_cast<size_t>(srcCh + 1));

    auto& row = r.xpoint[static_cast<size_t>(srcCh)];
    if (outCh >= (int)row.size())
        row.resize(static_cast<size_t>(outCh + 1), std::nullopt);
    row[static_cast<size_t>(outCh)] = dB;
}

void CueList::initCueRouting(int i, int srcCh, int outCh) {
    if (i < 0 || i >= cueCount() || srcCh <= 0 || outCh <= 0) return;
    auto& r = m_cues[i].routing;

    // Ensure outLevelDb is sized; leave existing values unchanged.
    if ((int)r.outLevelDb.size() < outCh)
        r.outLevelDb.resize(static_cast<size_t>(outCh), 0.0f);

    // If xpoint is empty or wrong size, fill with default diagonal.
    if ((int)r.xpoint.size() != srcCh) {
        r.xpoint.assign(static_cast<size_t>(srcCh),
                        std::vector<std::optional<float>>(static_cast<size_t>(outCh), std::nullopt));
        for (int s = 0; s < srcCh; ++s)
            if (s < outCh)
                r.xpoint[static_cast<size_t>(s)][static_cast<size_t>(s)] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Fade setters

void CueList::setCueFadeMasterTarget(int i, bool enabled, float targetDb) {
    if (i >= 0 && i < cueCount() && m_cues[i].fadeData) {
        m_cues[i].fadeData->masterLevel.enabled  = enabled;
        m_cues[i].fadeData->masterLevel.targetDb = targetDb;
    }
}

void CueList::setCueFadeOutTarget(int i, int outCh, bool enabled, float targetDb) {
    if (i < 0 || i >= cueCount() || !m_cues[i].fadeData || outCh < 0) return;
    auto& fd = *m_cues[i].fadeData;
    if (outCh >= (int)fd.outLevels.size())
        fd.outLevels.resize(static_cast<size_t>(outCh + 1));
    fd.outLevels[static_cast<size_t>(outCh)].enabled  = enabled;
    fd.outLevels[static_cast<size_t>(outCh)].targetDb = targetDb;
}

void CueList::setCueFadeOutTargetCount(int i, int count) {
    if (i >= 0 && i < cueCount() && m_cues[i].fadeData)
        m_cues[i].fadeData->outLevels.resize(static_cast<size_t>(count));
}

void CueList::setCueFadeXpTarget(int i, int srcCh, int outCh, bool enabled, float targetDb) {
    if (i < 0 || i >= cueCount() || !m_cues[i].fadeData || srcCh < 0 || outCh < 0) return;
    auto& fd = *m_cues[i].fadeData;
    if (srcCh >= (int)fd.xpTargets.size())
        fd.xpTargets.resize(static_cast<size_t>(srcCh + 1));
    auto& row = fd.xpTargets[static_cast<size_t>(srcCh)];
    if (outCh >= (int)row.size())
        row.resize(static_cast<size_t>(outCh + 1));
    row[static_cast<size_t>(outCh)].enabled  = enabled;
    row[static_cast<size_t>(outCh)].targetDb = targetDb;
}

void CueList::setCueFadeXpSize(int i, int srcCh, int outCh) {
    if (i < 0 || i >= cueCount() || !m_cues[i].fadeData || srcCh <= 0 || outCh <= 0) return;
    auto& fd = *m_cues[i].fadeData;
    fd.xpTargets.resize(static_cast<size_t>(srcCh));
    for (auto& row : fd.xpTargets)
        if ((int)row.size() < outCh)
            row.resize(static_cast<size_t>(outCh));
}

void CueList::setCueFadeCurve(int i, FadeData::Curve curve) {
    if (i >= 0 && i < cueCount() && m_cues[i].fadeData)
        m_cues[i].fadeData->curve = curve;
}
void CueList::setCueFadeStopWhenDone(int i, bool v) {
    if (i >= 0 && i < cueCount() && m_cues[i].fadeData)
        m_cues[i].fadeData->stopWhenDone = v;
}

// ---------------------------------------------------------------------------
// Marker / slice-loop setters

static void normaliseSliceLoops(Cue& cue) {
    const int want = (int)cue.markers.size() + 1;
    while ((int)cue.sliceLoops.size() < want) cue.sliceLoops.push_back(1);
    cue.sliceLoops.resize(static_cast<size_t>(want));
}

void CueList::setCueMusicContext(int i, std::unique_ptr<MusicContext> mc) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[i].musicContext = std::move(mc);
    m_cues[i].mcSourceIdx  = -1;  // own MC takes precedence; clear any inherited link
}

void CueList::setCueMCSource(int index, int sourceIdx) {
    if (index < 0 || index >= cueCount()) return;
    if (sourceIdx < -1 || sourceIdx >= cueCount()) return;
    if (sourceIdx == index) return;  // trivial self-loop
    // Guard against indirect cycles: walk the source chain.
    int cur = sourceIdx;
    while (cur >= 0 && cur < cueCount()) {
        if (cur == index) return;  // would create cycle
        cur = m_cues[static_cast<size_t>(cur)].mcSourceIdx;
    }
    m_cues[static_cast<size_t>(index)].mcSourceIdx = sourceIdx;
}

bool CueList::hasMusicContext(int index) const {
    return musicContextOf(index) != nullptr;
}

MusicContext* CueList::musicContextOf(int i) {
    if (i < 0 || i >= cueCount()) return nullptr;
    const int src = m_cues[static_cast<size_t>(i)].mcSourceIdx;
    if (src >= 0) return musicContextOf(src);
    return m_cues[static_cast<size_t>(i)].musicContext.get();
}

const MusicContext* CueList::musicContextOf(int i) const {
    if (i < 0 || i >= cueCount()) return nullptr;
    const int src = m_cues[static_cast<size_t>(i)].mcSourceIdx;
    if (src >= 0) return musicContextOf(src);
    return m_cues[static_cast<size_t>(i)].musicContext.get();
}

int CueList::cueMCSourceIdx(int i) const {
    if (i < 0 || i >= cueCount()) return -1;
    return m_cues[static_cast<size_t>(i)].mcSourceIdx;
}

void CueList::markMCDirty(int i) {
    if (i < 0 || i >= cueCount()) return;
    MusicContext* mc = musicContextOf(i);
    if (mc) mc->markDirty();
}

void CueList::setCueMarkers(int i, const std::vector<Cue::TimeMarker>& m) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[i].markers = m;
    normaliseSliceLoops(m_cues[i]);
}
void CueList::setCueSliceLoops(int i, const std::vector<int>& l) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[i].sliceLoops = l;
    normaliseSliceLoops(m_cues[i]);
}
void CueList::setCueMarkerTime(int i, int mi, double t) {
    if (i < 0 || i >= cueCount() || mi < 0 || mi >= (int)m_cues[i].markers.size()) return;
    m_cues[i].markers[static_cast<size_t>(mi)].time = t;
    std::stable_sort(m_cues[i].markers.begin(), m_cues[i].markers.end(),
                     [](const Cue::TimeMarker& a, const Cue::TimeMarker& b){ return a.time < b.time; });
}
void CueList::setCueMarkerName(int i, int mi, const std::string& n) {
    if (i < 0 || i >= cueCount() || mi < 0 || mi >= (int)m_cues[i].markers.size()) return;
    m_cues[i].markers[static_cast<size_t>(mi)].name = n;
}
void CueList::setCueMarkerIndex(int i, int mi) {
    if (i < 0 || i >= cueCount()) return;
    m_cues[static_cast<size_t>(i)].markerIndex = mi;
}

void CueList::setMarkerAnchor(int cueIdx, int markerIdx, int anchorCueIdx) {
    if (cueIdx < 0 || cueIdx >= cueCount()) return;
    auto& markers = m_cues[static_cast<size_t>(cueIdx)].markers;
    if (markerIdx < 0 || markerIdx >= (int)markers.size()) return;
    markers[static_cast<size_t>(markerIdx)].anchorMarkerCueIdx = anchorCueIdx;
}

void CueList::addCueMarker(int i, double time, const std::string& name) {
    if (i < 0 || i >= cueCount()) return;
    Cue::TimeMarker tm; tm.time = time; tm.name = name;
    m_cues[i].markers.push_back(tm);
    std::stable_sort(m_cues[i].markers.begin(), m_cues[i].markers.end(),
                     [](const Cue::TimeMarker& a, const Cue::TimeMarker& b){ return a.time < b.time; });
    normaliseSliceLoops(m_cues[i]);
}
void CueList::removeCueMarker(int i, int mi) {
    if (i < 0 || i >= cueCount() || mi < 0 || mi >= (int)m_cues[i].markers.size()) return;
    m_cues[i].markers.erase(m_cues[i].markers.begin() + mi);
    normaliseSliceLoops(m_cues[i]);
}

// ---------------------------------------------------------------------------
// Live routing update (called from fade callbacks in the scheduler thread)
// These do NOT modify the Cue struct — fades are live-voice-only multipliers.
// ch parameter is a channel index; mapped to primaryPhys when a ChannelMap is set.

void CueList::setChannelMap(ChannelMap map) {
    m_channelMap = std::move(map);
    pushFoldToEngine();
    reapplyRoutingToActive();
}

void CueList::pushFoldToEngine() {
    const auto& cm = m_channelMap;
    if (cm.numCh <= 0 || cm.numPhys <= 0 || cm.fold.empty()) return;

    if (cm.physDevice.empty()) {
        // Single-device: fold maps numCh → numPhys (= outChannels for device 0).
        m_engine.setDeviceChannelFold(0, cm.fold.data(), cm.numCh, cm.numPhys);
    } else {
        // Multi-device: slice the global fold into per-device matrices.
        const int numDev = static_cast<int>(cm.devicePhysCount.size());
        for (int d = 0; d < numDev; ++d) {
            const int devPhysCh = cm.devicePhysCount[static_cast<size_t>(d)];
            if (devPhysCh <= 0) continue;

            std::vector<float> devFold(static_cast<size_t>(cm.numCh * devPhysCh), 0.0f);
            for (int ch = 0; ch < cm.numCh; ++ch) {
                for (int p = 0; p < cm.numPhys; ++p) {
                    if (p >= (int)cm.physDevice.size()) break;
                    if (cm.physDevice[static_cast<size_t>(p)] != d) continue;
                    const int lp = cm.physLocalCh.empty()
                        ? -1 : cm.physLocalCh[static_cast<size_t>(p)];
                    if (lp < 0 || lp >= devPhysCh) continue;
                    devFold[static_cast<size_t>(ch * devPhysCh + lp)] =
                        cm.fold[static_cast<size_t>(ch * cm.numPhys + p)];
                }
            }
            m_engine.setDeviceChannelFold(d, devFold.data(), cm.numCh, devPhysCh);
        }
    }
}

void CueList::reapplyRoutingToActive() {
    const int n = cueCount();
    const int numCh = (m_channelMap.numCh > 0) ? m_channelMap.numCh : m_engine.channels();
    if (numCh <= 0) return;
    const bool multiDevice = !m_channelMap.physDevice.empty();

    for (int i = 0; i < n; ++i) {
        const auto& cue = m_cues[static_cast<size_t>(i)];
        if (cue.type != CueType::Audio) continue;
        const int srcCh = static_cast<int>(cue.audioFile.metadata().channels);
        if (srcCh <= 0) continue;

        if (multiDevice) {
            const int numDev = static_cast<int>(m_channelMap.devicePhysCount.size());
            for (int d = 0; d < numDev; ++d) {
                int slot;
                { std::lock_guard<std::mutex> lk(m_slotMutex);
                  slot = m_cueRuntime[static_cast<size_t>(i)].slotForDevice(d); }
                if (slot < 0 || !m_slotStream[static_cast<size_t>(slot)]) continue;
                applyRoutingToSource(cue, *m_slotStream[static_cast<size_t>(slot)], srcCh, numCh);
            }
        } else {
            int slot;
            { std::lock_guard<std::mutex> lk(m_slotMutex);
              slot = m_cueRuntime[static_cast<size_t>(i)].canonicalSlot(); }
            if (slot < 0 || !m_slotStream[static_cast<size_t>(slot)]) continue;
            applyRoutingToSource(cue, *m_slotStream[static_cast<size_t>(slot)], srcCh, numCh);
        }
    }
}

void CueList::setCueOutLevelGain(int cueIdx, int ch, float linGain) {
    if (cueIdx < 0 || cueIdx >= cueCount() || ch < 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(m_slotMutex); slot = m_cueRuntime[static_cast<size_t>(cueIdx)].canonicalSlot(); }
    if (slot < 0 || !m_slotStream[static_cast<size_t>(slot)]) return;
    // ch is a channel-space index — matches the StreamReader's outLev directly.
    m_slotStream[static_cast<size_t>(slot)]->setOutLevelGain(ch, linGain);
}

void CueList::setCueXpointGain(int cueIdx, int srcCh, int ch, float linGain) {
    if (cueIdx < 0 || cueIdx >= cueCount() || srcCh < 0 || ch < 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(m_slotMutex); slot = m_cueRuntime[static_cast<size_t>(cueIdx)].canonicalSlot(); }
    if (slot < 0 || !m_slotStream[static_cast<size_t>(slot)]) return;
    // ch is a channel-space index — matches the StreamReader's xpGains column directly.
    m_slotStream[static_cast<size_t>(slot)]->setXpointGain(srcCh, ch, linGain);
}

std::unique_ptr<AutoParam> CueList::makeAutoParam(const std::string& path) {
    int ch = -1;
    if (path.size() > 7 && path.compare(0, 7, "/mixer/") == 0) {
        const size_t sl2 = path.find('/', 7);
        try {
            ch = std::stoi(path.substr(7, sl2 == std::string::npos
                                          ? std::string::npos : sl2 - 7));
        } catch (...) {}
    }
    if (ch < 0 || ch >= m_channelMap.numCh) return nullptr;

    const size_t chSz = static_cast<size_t>(ch);
    const int slave   = (chSz < m_channelMap.stereoSlave.size())
                      ? m_channelMap.stereoSlave[chSz] : -1;

    // Send slot parameters: /mixer/{ch}/send/{slot}/{param}
    // Must be checked before the bare /fader and /mute checks to avoid false matches.
    const std::string sendMid = "/send/";
    const auto sendIt = path.find(sendMid, 7);
    if (sendIt != std::string::npos) {
        const std::string rest = path.substr(sendIt + sendMid.size());
        const auto slashPos = rest.find('/');
        if (slashPos != std::string::npos) {
            int slot = -1;
            try { slot = std::stoi(rest.substr(0, slashPos)); } catch (...) {}
            const std::string paramId = rest.substr(slashPos + 1);
            if (slot >= 0) {
                if (paramId == "level")
                    return std::make_unique<mcp_detail::SendLevelParam>(this, ch, slot, path);
                if (paramId == "mute")
                    return std::make_unique<mcp_detail::SendMuteParam>(this, ch, slot, path);
                if (paramId == "panL" || paramId == "panR")
                    return std::make_unique<mcp_detail::SendPanParam>(this, ch, slot, path);
            }
        }
    }

    if (path.find("/fader") != std::string::npos)
        return std::make_unique<mcp_detail::ChannelFaderParam>(this, ch, slave, path);

    if (path.find("/mute") != std::string::npos)
        return std::make_unique<mcp_detail::ChannelMuteParam>(this, ch, slave, path);

    const std::string xpMid = "/crosspoint/";
    const auto it2 = path.find(xpMid);
    if (it2 != std::string::npos) {
        int out = -1;
        try { out = std::stoi(path.substr(it2 + xpMid.size())); } catch (...) {}
        if (out >= 0 && out < m_channelMap.numPhys)
            return std::make_unique<mcp_detail::CrosspointParam>(this, ch, out, path);
    }

    // Plugin parameter: /mixer/{ch}/plugin/{slot}/{paramId}
    const std::string pluginMid = "/plugin/";
    const auto pit = path.find(pluginMid, 7 + std::to_string(ch).size());
    if (pit != std::string::npos && m_pluginAccessor) {
        const std::string rest = path.substr(pit + pluginMid.size());
        const auto sl2 = rest.find('/');
        if (sl2 != std::string::npos) {
            int slot = -1;
            try { slot = std::stoi(rest.substr(0, sl2)); } catch (...) {}
            const std::string paramId = rest.substr(sl2 + 1);
            if (slot >= 0 && !paramId.empty())
                return std::make_unique<mcp_detail::PluginParamAutoParam>(
                    this, ch, slot, paramId, path);
        }
    }

    return nullptr;  // polarity and other params: no live audio effect
}

// ---------------------------------------------------------------------------
// Internal helpers

static constexpr double kFaderFloor = -60.0;
static float levelGain(double levelDB, double trimDB) {
    const double dB = levelDB + trimDB;
    return (dB <= kFaderFloor) ? 0.0f : lut::dBToLinear(dB);
}

void CueList::applyRoutingToSource(const Cue& cue, IAudioSource& reader,
                                    int srcCh, int numCh) {
    // Build srcCh × numCh routing in channel space.
    // xpGains[s * numCh + ch] = cue crosspoint gain × per-channel output level.
    // The channel fold (fader / mute / channel-level xpoint) lives in AudioEngine
    // and is applied there independently — no fold is baked in here.
    std::vector<float> xpGains(static_cast<size_t>(srcCh * numCh),
                                 std::numeric_limits<float>::quiet_NaN());
    std::vector<float> chanOutLev(static_cast<size_t>(numCh), 1.0f);

    for (int ch = 0; ch < numCh && ch < (int)cue.routing.outLevelDb.size(); ++ch)
        chanOutLev[static_cast<size_t>(ch)] =
            lut::dBToLinear(cue.routing.outLevelDb[static_cast<size_t>(ch)]);

    for (int s = 0; s < srcCh; ++s) {
        for (int ch = 0; ch < numCh; ++ch) {
            float xpLin = 0.0f;
            if (!cue.routing.xpoint.empty()) {
                if (s < (int)cue.routing.xpoint.size() &&
                    ch < (int)cue.routing.xpoint[static_cast<size_t>(s)].size() &&
                    cue.routing.xpoint[static_cast<size_t>(s)][static_cast<size_t>(ch)].has_value())
                    xpLin = lut::dBToLinear(
                        *cue.routing.xpoint[static_cast<size_t>(s)][static_cast<size_t>(ch)]);
                // else: xpoint configured but this cell cleared → silence
            } else {
                xpLin = (s == ch) ? 1.0f : 0.0f;  // fresh cue: identity default
            }
            if (xpLin > 1e-10f)
                xpGains[static_cast<size_t>(s * numCh + ch)] = xpLin;
        }
    }

    reader.setRouting(std::move(xpGains), std::move(chanOutLev), numCh);
}

void CueList::applyRoutingToSourceForDevice(const Cue& cue, IAudioSource& reader,
                                             int srcCh, int /*deviceIdx*/) {
    // With the channel-bus architecture, all per-device readers use the same
    // channel-space routing (srcCh → numCh).  The per-device fold matrix in
    // AudioEngine handles the numCh → devPhysCh mapping independently.
    const int numCh = (m_channelMap.numCh > 0) ? m_channelMap.numCh : m_engine.channels();
    applyRoutingToSource(cue, reader, srcCh, numCh);
}

// Build a segment list from a cue's markers and sliceLoops.
// Each marker divides the [startTime, endTime] range into a slice;
// each slice is played `sliceLoops[i]` times.
static std::vector<LoopSegment> buildSegments(const Cue& cue) {
    std::vector<LoopSegment> segs;

    // Boundary times: use -1.0 as "end of file" sentinel.
    const double endSec = (cue.duration > 0.0) ? cue.startTime + cue.duration : -1.0;

    std::vector<double> bounds;
    bounds.push_back(cue.startTime);
    for (const auto& m : cue.markers) {
        if (m.time > cue.startTime && (endSec < 0.0 || m.time < endSec))
            bounds.push_back(m.time);
    }
    bounds.push_back(endSec);  // -1 = play to file end

    for (int bi = 0; bi < (int)bounds.size() - 1; ++bi) {
        LoopSegment s;
        s.startSecs = bounds[static_cast<size_t>(bi)];
        // Convert -1 sentinel to 0.0 (StreamReader: 0 = to end of file).
        const double e = bounds[static_cast<size_t>(bi + 1)];
        s.endSecs   = (e < 0.0) ? 0.0 : e;
        const int lc  = (bi < (int)cue.sliceLoops.size()) ? cue.sliceLoops[static_cast<size_t>(bi)] : 1;
        s.loops     = (lc <= 0) ? 0 : lc;
        segs.push_back(s);
    }
    return segs;
}

// Returns the number of frames that will actually be played for a given cue.
// For non-audio cues returns 0.
static int64_t voiceFrames(const Cue& cue) {
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return 0;
    const auto& meta   = cue.audioFile.metadata();
    const int64_t sf   = std::min(static_cast<int64_t>(cue.startTime * meta.sampleRate),
                                  meta.frameCount);
    const int64_t avail = meta.frameCount - sf;
    return (cue.duration > 0.0)
        ? std::min(avail, static_cast<int64_t>(cue.duration * meta.sampleRate))
        : avail;
}

int CueList::logicalNext(int idx) const {
    if (idx < 0 || idx >= cueCount()) return cueCount();
    const auto& cue = m_cues[idx];

    // A Group cue: skip over all its descendants in one step.
    if (cue.type == CueType::Group) {
        return idx + cue.childCount + 1;
    }

    // A child cue: check if it is the last descendant of its parent group.
    if (cue.parentIndex >= 0 && cue.parentIndex < cueCount()) {
        const int pi       = cue.parentIndex;
        const int lastDesc = pi + m_cues[pi].childCount;
        if (idx == lastDesc) {
            // Exit the group — recurse to handle nested groups.
            return logicalNext(pi);
        }
    }

    return idx + 1;
}

// ---------------------------------------------------------------------------
// Group execution helpers

void CueList::armGroupDescendants(int groupIdx, double baseOffset) {
    if (groupIdx < 0 || groupIdx >= cueCount()) return;
    const auto& group = m_cues[groupIdx];
    if (!group.groupData || group.childCount == 0) return;

    for (int i = groupIdx + 1; i <= groupIdx + group.childCount; ) {
        if (m_cues[i].parentIndex != groupIdx) { ++i; continue; }
        const int ci = i;
        const double armInto = baseOffset - m_cues[ci].timelineOffset;
        const double childDur = (m_cues[ci].duration > 0.0) ? m_cues[ci].duration
            : (m_cues[ci].type == CueType::Audio ? cueTotalSeconds(ci) : 99999.0);

        if (armInto < childDur) {
            if (m_cues[ci].type == CueType::Audio && m_cues[ci].audioFile.isLoaded()) {
                const double filePos = m_cues[ci].startTime + std::max(0.0, armInto);
                arm(ci, filePos);
            } else if (m_cues[ci].type == CueType::Group && m_cues[ci].groupData) {
                armGroupDescendants(ci, std::max(0.0, armInto));
            }
        }
        const int cc = (m_cues[ci].type == CueType::Group) ? m_cues[ci].childCount : 0;
        i += cc + 1;
    }
}

void CueList::fireGroup(int groupIdx, double baseOffset, int64_t originFrame) {
    if (groupIdx < 0 || groupIdx >= cueCount()) return;
    const auto& group = m_cues[groupIdx];
    if (!group.groupData) return;
    if (group.childCount == 0) return;

    const bool isPlaylist = (group.groupData->mode == GroupData::Mode::Playlist);

    // Collect direct children (those whose parentIndex == groupIdx).
    std::vector<int> directChildren;
    for (int i = groupIdx + 1; i <= groupIdx + group.childCount; ) {
        if (m_cues[i].parentIndex == groupIdx) {
            directChildren.push_back(i);
            // Jump over this child's descendants (if it is itself a group).
            const int cc = (m_cues[i].type == CueType::Group) ? m_cues[i].childCount : 0;
            i += cc + 1;
        } else {
            ++i;
        }
    }
    if (directChildren.empty()) return;

    if (isPlaylist) {
        // Optionally randomise child order.
        if (group.groupData->random) {
            for (int i = (int)directChildren.size() - 1; i > 0; --i) {
                const int j = std::rand() % (i + 1);
                std::swap(directChildren[i], directChildren[j]);
            }
        }
        // Register follow-ups: after child[i] finishes, fire child[i+1].
        for (int i = 0; i + 1 < (int)directChildren.size(); ++i)
            m_playlistFollowUps.push_back({directChildren[i], directChildren[i + 1], false});
        // Fire the first child immediately (baseOffset not used for playlist).
        fire(directChildren[0]);
    } else {
        // Timeline mode.
        // Pre-arm all audio descendants so StreamReaders have buffered data.
        armGroupDescendants(groupIdx, baseOffset);

        // baseOffset = timelineArmSec: how many seconds into the timeline we are starting.
        // For each child:
        //   armInto  = baseOffset - child.timelineOffset
        //            > 0  → child has already started; seek audio by armInto, fire now
        //            = 0  → child starts exactly now, fire now
        //            < 0  → child starts in the future, schedule after -armInto secs
        //   if armInto >= childDuration → child is fully past, skip
        for (int ci : directChildren) {
            const double childTL  = m_cues[ci].timelineOffset;
            const double armInto  = baseOffset - childTL;          // how far into child we are

            // Effective duration: stored, or derived for audio, or a safe fallback.
            const double childDur = (m_cues[ci].duration > 0.0) ? m_cues[ci].duration
                : (m_cues[ci].type == CueType::Audio ? cueTotalSeconds(ci) : 2.0);

            if (armInto >= childDur) continue;   // entirely in the past — skip

            const double fireDelay = std::max(0.0, -armInto);   // seconds until child starts

            if (m_cues[ci].type == CueType::Group && m_cues[ci].groupData) {
                // Nested group: recurse with the arm position relative to that group's origin.
                const double nestedArm = std::max(0.0, armInto);
                if (fireDelay > 0.0)
                    m_scheduler.scheduleFromFrame(originFrame, fireDelay,
                        [this, ci, nestedArm, originFrame]() {
                            fireGroup(ci, nestedArm, originFrame);
                        }, "tl-group[" + std::to_string(ci) + "]");
                else
                    fireGroup(ci, nestedArm, originFrame);
            } else if (fireDelay > 0.0) {
                // Child hasn't started yet — schedule at the right moment.
                m_scheduler.scheduleFromFrame(originFrame, fireDelay,
                    [this, ci]() { fire(ci); },
                    "tl-child[" + std::to_string(ci) + "]");
            } else if (armInto > 0.0 && m_cues[ci].type == CueType::Audio) {
                // Child is mid-play — seek into the audio file by armInto seconds.
                const double origStart = m_cues[ci].startTime;
                m_cues[ci].startTime = origStart + armInto;
                fire(ci);
                m_cues[ci].startTime = origStart;
            } else {
                fire(ci);
            }
        }
    }
}

int64_t CueList::scheduleVoice(int cueIndex) {
    const auto& cue = m_cues[cueIndex];
    const float gain  = levelGain(cue.level, cue.trim);
    const int   srcCh = static_cast<int>(cue.audioFile.metadata().channels);

    // All readers output to channel space (numCh), not physical space.
    // The channel fold in AudioEngine handles numCh → physCh per device.
    const int numCh = (m_channelMap.numCh > 0) ? m_channelMap.numCh : m_engine.channels();

    // Multi-device path: create one independent StreamReader per device so each
    // device's callback advances its own read position independently.
    if (!m_channelMap.physDevice.empty()) {
        const int numDevices = static_cast<int>(m_channelMap.devicePhysCount.size());
        int64_t totalFrames = -1;

        for (int d = 0; d < numDevices; ++d) {
            auto segs = buildSegments(cue);
            auto devReader = std::make_shared<StreamReader>(
                cue.path, m_engine.sampleRate(), numCh, std::move(segs));
            if (devReader->hasError()) continue;

            if (totalFrames < 0) totalFrames = devReader->totalOutputFrames();
            if (totalFrames < 0) continue;

            if (srcCh > 0 && numCh > 0)
                applyRoutingToSource(cue, *devReader, srcCh, numCh);

            const int slot = m_engine.scheduleStreamingVoice(
                devReader.get(),
                totalFrames > 0 ? totalFrames : INT64_MAX / 2,
                numCh, m_tagBase + m_cues[static_cast<size_t>(cueIndex)].stableId, gain, d);
            if (slot < 0) continue;

            std::lock_guard<std::mutex> lk(m_slotMutex);
            m_cueRuntime[static_cast<size_t>(cueIndex)].slotByDevice[d] = slot;
            m_slotStream[static_cast<size_t>(slot)] = std::move(devReader);
        }

        if (totalFrames < 0) return -1;
        return totalFrames > 0 ? totalFrames : voiceFrames(cue);
    }

    // Single-device path: take the pre-armed stream if available and error-free.
    // arm() uses the same numCh formula so the channel count is always compatible.
    std::shared_ptr<StreamReader> reader;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        auto armed = std::move(m_cues[cueIndex].armedStream);
        if (armed && !armed->hasError())
            reader = std::move(armed);
    }

    if (!reader) {
        auto segs = buildSegments(cue);
        reader = std::make_shared<StreamReader>(
            cue.path, m_engine.sampleRate(), numCh, std::move(segs));
        if (reader->hasError()) return -1;
    }

    const int64_t totalFrames = reader->totalOutputFrames();
    if (totalFrames < 0) return -1;

    if (srcCh > 0 && numCh > 0)
        applyRoutingToSource(cue, *reader, srcCh, numCh);

    const int slot = m_engine.scheduleStreamingVoice(
        reader.get(), totalFrames > 0 ? totalFrames : INT64_MAX / 2,
        numCh, m_tagBase + m_cues[static_cast<size_t>(cueIndex)].stableId, gain);
    if (slot < 0) return -1;

    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_cueRuntime[static_cast<size_t>(cueIndex)].slotByDevice[0] = slot;
    m_slotStream[static_cast<size_t>(slot)] = std::move(reader);
    return totalFrames > 0 ? totalFrames : voiceFrames(cue);
}

bool CueList::fire(int idx) {
    const auto& cue = m_cues[idx];
    bool   result      = false;
    int64_t followFrames = 0;   // frames until autoFollow should trigger go()

    switch (cue.type) {
        case CueType::Audio: {
            const int64_t out = scheduleVoice(idx);
            result       = (out >= 0);
            followFrames = result ? out : 0;
            break;
        }

        case CueType::Start: {
            if (cue.crossListNumericId != -1) {
                if (m_crossListStart) m_crossListStart(cue.crossListNumericId, cue.crossListFlatIdx);
                result = true;
            } else {
                const int ti = cue.targetIndex;
                if (ti >= 0 && ti < cueCount() && m_cues[ti].type == CueType::Audio) {
                    const int64_t out = scheduleVoice(ti);
                    result       = (out >= 0);
                    followFrames = result ? out : 0;
                }
            }
            break;
        }

        case CueType::Stop: {
            if (cue.crossListNumericId != -1) {
                if (m_crossListStop) m_crossListStop(cue.crossListNumericId, cue.crossListFlatIdx);
                result = true;
                break;
            }
            const int ti = cue.targetIndex;
            // Block stop if the target is a direct child of a SyncGroup
            bool blocked = false;
            if (ti >= 0 && ti < cueCount()) {
                const int pi = m_cues[ti].parentIndex;
                blocked = (pi >= 0 && pi < cueCount() &&
                           m_cues[pi].type == CueType::Group &&
                           m_cues[pi].groupData &&
                           m_cues[pi].groupData->mode == GroupData::Mode::Sync);
            }
            if (!blocked) {
                m_engine.clearVoicesByTag(m_tagBase + m_cues[static_cast<size_t>(ti)].stableId);
                result = true;
            }
            break;
        }

        case CueType::Arm: {
            const int ti = cue.targetIndex;
            if (ti >= 0 && ti < cueCount() && m_cues[ti].type == CueType::Audio) {
                arm(ti, cue.armStartTime > 0.0 ? cue.armStartTime : -1.0);
                result = true;
                // followFrames = 0 → autoFollow fires in the next scheduler poll
            }
            break;
        }

        case CueType::Devamp: {
            const int ti = cue.targetIndex;
            if (ti < 0 || ti >= cueCount()) break;

            // SyncGroup devamp: advance slice without using StreamReader
            if (m_cues[ti].type == CueType::Group && m_cues[ti].groupData &&
                m_cues[ti].groupData->mode == GroupData::Mode::Sync) {
                devampSyncGroup(ti, cue.devampMode, cue.devampPreVamp);
                result = true;
                break;
            }

            // Collect StreamReaders for all active device slots.
            // canonical is the device-0 slot used for wasDevampFired() polling.
            // In single-device mode allReaders has exactly one entry.
            StreamReader* canonical = nullptr;
            std::vector<StreamReader*> allReaders;
            int slot = -1;
            {
                std::lock_guard<std::mutex> lk(m_slotMutex);
                const auto& rt = m_cueRuntime[static_cast<size_t>(ti)];
                slot = rt.canonicalSlot();
                if (slot >= 0) canonical = slotStreamReader(static_cast<size_t>(slot));
                for (const auto& kv : rt.slotByDevice) {
                    auto* r = slotStreamReader(static_cast<size_t>(kv.second));
                    if (r && m_engine.isVoiceActive(kv.second))
                        allReaders.push_back(r);
                }
            }
            if (!canonical || !m_engine.isVoiceActive(slot)) break;

            if (cue.devampMode == 0) {
                // Mode 0: advance to next slice on every device (classic devamp)
                for (auto* r : allReaders) r->devamp(false, cue.devampPreVamp);
            } else {
                // Modes 1/2: call go() at the end of the current slice.
                // Pre-arm the currently-selected cue for a seamless start.
                if (m_selectedIndex >= 0 && m_selectedIndex < cueCount())
                    arm(m_selectedIndex);

                canonical->clearDevampFired();  // only canonical drives the fired signal
                const bool stopCurrent = (cue.devampMode == 1);
                for (auto* r : allReaders) r->devamp(stopCurrent, cue.devampPreVamp);

                // Register follow-up: call go() when trigger fires
                m_followUps.push_back({ti, stopCurrent});
            }
            result = true;
            break;
        }

        case CueType::Fade: {
            const auto fd = cue.fadeData;
            if (!fd) break;
            const int tIdx = fd->resolvedTargetIdx;
            if (tIdx < 0 || tIdx >= cueCount()) break;

            const double length  = std::max(0.01, cue.duration > 0.0 ? cue.duration : 3.0);
            const int    sr      = m_engine.sampleRate();
            const int    steps   = std::max(2, static_cast<int>(length * 30.0));
            const double stepSec = length / static_cast<double>(steps - 1);

            // Capture start values at fire() time.
            // Prefer LIVE voice gains (from StreamReader) over stored routing so
            // that a second fade continues smoothly from wherever the first left off.
            const auto& tc = m_cues[static_cast<std::size_t>(tIdx)];
            {
                int tSlot;
                { std::lock_guard<std::mutex> lk(m_slotMutex); tSlot = m_cueRuntime[static_cast<size_t>(tIdx)].canonicalSlot(); }
                const IAudioSource* tsr = (tSlot >= 0 && m_engine.isVoiceActive(tSlot)
                                           && m_slotStream[static_cast<size_t>(tSlot)])
                    ? m_slotStream[static_cast<size_t>(tSlot)].get() : nullptr;

                fd->masterLevelStartDb = static_cast<float>(tc.level);

                fd->outLevelStartDb.resize(fd->outLevels.size());
                for (int o = 0; o < (int)fd->outLevels.size(); ++o) {
                    float startDb = 0.0f;
                    if (tsr) {
                        const float lin = tsr->getOutLevelGain(o);
                        startDb = (lin > 0.0f)
                            ? static_cast<float>(lut::linearToDB(static_cast<double>(lin)))
                            : -144.0f;
                    } else if (o < (int)tc.routing.outLevelDb.size()) {
                        startDb = tc.routing.outLevelDb[static_cast<size_t>(o)];
                    }
                    fd->outLevelStartDb[static_cast<size_t>(o)] = startDb;
                }

                const int xs = (int)fd->xpTargets.size();
                fd->xpStartDb.assign(static_cast<size_t>(xs), {});
                for (int s = 0; s < xs; ++s) {
                    const int xo = (int)fd->xpTargets[static_cast<size_t>(s)].size();
                    fd->xpStartDb[static_cast<size_t>(s)].assign(static_cast<size_t>(xo), 0.0f);
                    for (int o = 0; o < xo; ++o) {
                        float startDb = -144.0f;
                        if (tsr) {
                            const float lin = tsr->getXpointGain(s, o);
                            if (std::isnan(lin)) {
                                startDb = -144.0f;  // no route = silence
                            } else {
                                startDb = (lin > 0.0f)
                                    ? static_cast<float>(lut::linearToDB(static_cast<double>(lin)))
                                    : -144.0f;
                            }
                        } else if (s < (int)tc.routing.xpoint.size() &&
                                   o < (int)tc.routing.xpoint[static_cast<size_t>(s)].size()) {
                            auto xv = tc.routing.xpoint[static_cast<size_t>(s)][static_cast<size_t>(o)];
                            if (xv.has_value()) startDb = *xv;
                        }
                        fd->xpStartDb[static_cast<size_t>(s)][static_cast<size_t>(o)] = startDb;
                    }
                }
            }

            // Compute progress ramp asynchronously (finishes near-instantly).
            if (fd->computeThread.joinable()) fd->computeThread.join();
            fd->rampReady.store(false, std::memory_order_relaxed);
            fd->activeSteps.store(steps, std::memory_order_relaxed);
            fd->computeThread = std::thread([fd, steps]() { fd->computeRamp(steps); });

            const int64_t baseFrame = m_engine.enginePlayheadFrames();
            for (int s = 0; s < steps; ++s) {
                const bool isLast = (s == steps - 1);
                m_scheduler.scheduleFromFrame(baseFrame, s * stepSec,
                    [this, fd, tIdx, s, isLast]() {
                        if (!fd->rampReady.load(std::memory_order_acquire)) return;
                        if (tIdx < 0 || tIdx >= cueCount()) return;
                        if (s >= (int)fd->ramp.size()) return;

                        const float t    = fd->ramp[static_cast<size_t>(s)];
                        const bool  isEP = (fd->curve == FadeData::Curve::EqualPower);

                        // Interpolate amplitude start→target using the chosen curve.
                        auto interpAmp = [&](float startDb, float endDb) -> float {
                            const float sl = lut::dBToLinear(startDb);
                            const float el = lut::dBToLinear(endDb);
                            return isEP ? (sl * lut::cos_hp(t) + el * lut::sin_hp(t))
                                        : (sl + t * (el - sl));
                        };

                        // Master level fade — apply to live voice gain only
                        if (fd->masterLevel.enabled) {
                            const float amp = interpAmp(fd->masterLevelStartDb,
                                                         fd->masterLevel.targetDb);
                            const auto& tc2 = m_cues[static_cast<size_t>(tIdx)];
                            if (tc2.type == CueType::Group) {
                                // Fade all Audio descendants of the group
                                for (int di = tIdx + 1;
                                     di <= tIdx + tc2.childCount && di < cueCount(); ++di) {
                                    if (m_cues[di].type != CueType::Audio) continue;
                                    const int ds = cueVoiceSlot(di);
                                    if (ds >= 0 && m_engine.isVoiceActive(ds))
                                        m_engine.setVoiceGain(ds,
                                            levelGain(lut::linearToDB(amp), m_cues[di].trim));
                                }
                            } else {
                                const int slot = cueVoiceSlot(tIdx);
                                if (slot >= 0 && m_engine.isVoiceActive(slot))
                                    m_engine.setVoiceGain(slot, levelGain(lut::linearToDB(amp),
                                                                           tc2.trim));
                            }
                        }

                        // Per-output-channel level fades
                        for (int o = 0; o < (int)fd->outLevels.size(); ++o) {
                            if (!fd->outLevels[static_cast<size_t>(o)].enabled) continue;
                            if (o >= (int)fd->outLevelStartDb.size()) continue;
                            const float amp = interpAmp(
                                fd->outLevelStartDb[static_cast<size_t>(o)],
                                fd->outLevels[static_cast<size_t>(o)].targetDb);
                            setCueOutLevelGain(tIdx, o, amp);
                        }

                        // Crosspoint cell fades
                        for (int s = 0; s < (int)fd->xpTargets.size(); ++s) {
                            for (int o = 0; o < (int)fd->xpTargets[static_cast<size_t>(s)].size(); ++o) {
                                if (!fd->xpTargets[static_cast<size_t>(s)][static_cast<size_t>(o)].enabled) continue;
                                if (s >= (int)fd->xpStartDb.size()) continue;
                                if (o >= (int)fd->xpStartDb[static_cast<size_t>(s)].size()) continue;
                                const float amp = interpAmp(
                                    fd->xpStartDb[static_cast<size_t>(s)][static_cast<size_t>(o)],
                                    fd->xpTargets[static_cast<size_t>(s)][static_cast<size_t>(o)].targetDb);
                                setCueXpointGain(tIdx, s, o, amp);
                            }
                        }

                        if (isLast && fd->stopWhenDone) {
                            // Don't stop if the target is a child of a SyncGroup
                            const int pi2 = (tIdx >= 0 && tIdx < cueCount())
                                            ? m_cues[tIdx].parentIndex : -1;
                            const bool blocked =
                                pi2 >= 0 && pi2 < cueCount() &&
                                m_cues[pi2].type == CueType::Group &&
                                m_cues[pi2].groupData &&
                                m_cues[pi2].groupData->mode == GroupData::Mode::Sync;
                            if (!blocked) {
                                const auto& tc2 = m_cues[static_cast<size_t>(tIdx)];
                                if (tc2.type == CueType::Group) {
                                    for (int di = tIdx + 1;
                                         di <= tIdx + tc2.childCount && di < cueCount(); ++di)
                                        m_engine.clearVoicesByTag(m_tagBase + m_cues[static_cast<size_t>(di)].stableId);
                                } else {
                                    m_engine.clearVoicesByTag(m_tagBase + m_cues[static_cast<size_t>(tIdx)].stableId);
                                }
                            }
                        }
                        fd->activeSteps.fetch_sub(1, std::memory_order_relaxed);
                    },
                    "fade[" + std::to_string(idx) + "][" + std::to_string(s) + "]");
            }

            followFrames = (sr > 0) ? static_cast<int64_t>(length * sr) : 0;
            result = true;
            break;
        }

        case CueType::Group: {
            if (!cue.groupData) break;
            if (cue.groupData->mode == GroupData::Mode::StartFirst) break;
            const double armBase = m_cues[idx].timelineArmSec;
            m_cues[idx].timelineArmSec = 0.0;
            const int64_t now = m_engine.enginePlayheadFrames();
            if (cue.groupData->mode == GroupData::Mode::Sync) {
                if (!isSyncGroupBroken(idx)) {
                    m_cueFireFrame[idx]   = now;
                    m_cueFireArmBase[idx] = armBase;
                    fireSyncGroup(idx, armBase, now);
                    const double total = syncGroupTotalSeconds(idx);
                    if (std::isfinite(total) && total > 0.0 && m_engine.sampleRate() > 0)
                        followFrames = static_cast<int64_t>(total * m_engine.sampleRate());
                    result = true;
                }
            } else {
                m_cueFireFrame[idx]   = now;
                m_cueFireArmBase[idx] = armBase;
                fireGroup(idx, armBase, now);
                result = true;
            }
            break;
        }

        case CueType::MusicContext:
            m_cueFireFrame[static_cast<size_t>(idx)] = m_engine.enginePlayheadFrames();
            result = true;
            break;

        case CueType::Marker: {
            const int ti = cue.targetIndex;
            const int mi = cue.markerIndex;
            if (ti < 0 || ti >= cueCount()) break;
            auto& tgt = m_cues[static_cast<size_t>(ti)];

            // Resolve the marker time (0.0 = start of target if no marker selected).
            double markerTime = 0.0;
            if (mi >= 0 && mi < (int)tgt.markers.size())
                markerTime = tgt.markers[static_cast<size_t>(mi)].time;

            if (tgt.type == CueType::Audio) {
                if (!tgt.audioFile.isLoaded()) break;
                disarm(ti);
                const double origStart = tgt.startTime;
                tgt.startTime = markerTime;
                const int64_t out = scheduleVoice(ti);
                tgt.startTime = origStart;
                result       = (out >= 0);
                followFrames = result ? out : 0;
            } else if (tgt.type == CueType::Group && tgt.groupData &&
                       tgt.groupData->mode == GroupData::Mode::Sync) {
                if (isSyncGroupBroken(ti)) break;
                const int64_t now = m_engine.enginePlayheadFrames();
                m_cueFireFrame[static_cast<size_t>(ti)]   = now;
                m_cueFireArmBase[static_cast<size_t>(ti)] = markerTime;
                fireSyncGroup(ti, markerTime, now);
                const double total = syncGroupTotalSeconds(ti);
                if (std::isfinite(total) && total > 0.0 && m_engine.sampleRate() > 0)
                    followFrames = static_cast<int64_t>(total * m_engine.sampleRate());
                result = true;
            }
            break;
        }

        case CueType::Network: {
            if (cue.networkPatchIdx >= 0 && cue.networkPatchIdx < (int)m_networkPatches.size()) {
                const auto& patch = m_networkPatches[static_cast<size_t>(cue.networkPatchIdx)];
                std::string netErr;
                sendNetworkMessage(patch, cue.networkCommand, netErr);
                result = true;
            }
            followFrames = 0;
            break;
        }

        case CueType::Midi: {
            if (cue.midiPatchIdx >= 0 && cue.midiPatchIdx < (int)m_midiPatches.size()) {
                const auto& patch = m_midiPatches[static_cast<size_t>(cue.midiPatchIdx)];
                std::string midiErr;
                sendMidiMessage(patch, cue.midiMessageType,
                                cue.midiChannel, cue.midiData1, cue.midiData2, midiErr);
                result = true;
            }
            followFrames = 0;
            break;
        }

        case CueType::Timecode: {
            if (!(cue.tcStartTC < cue.tcEndTC)) break;

            if (cue.tcType == "ltc") {
                auto reader = std::make_shared<LtcStreamReader>(
                    cue.tcFps, cue.tcStartTC, cue.tcEndTC, m_engine.sampleRate());

                const int outCh = m_engine.channels();
                // Route mono LTC to the specified physical output channel.
                std::vector<float> xpGains(static_cast<size_t>(outCh),
                                            std::numeric_limits<float>::quiet_NaN());
                if (cue.tcLtcChannel >= 0 && cue.tcLtcChannel < outCh)
                    xpGains[static_cast<size_t>(cue.tcLtcChannel)] = 1.0f;
                std::vector<float> outLev(static_cast<size_t>(outCh), 1.0f);
                reader->setRouting(std::move(xpGains), std::move(outLev), outCh);

                const int64_t totalFrames = reader->totalOutputFrames();
                const int slot = m_engine.scheduleStreamingVoice(
                    reader.get(), totalFrames > 0 ? totalFrames : INT64_MAX / 2,
                    outCh, m_tagBase + m_cues[static_cast<size_t>(idx)].stableId, 1.0f, /*deviceIndex=*/0,
                    /*bypassChannelFold=*/true);
                if (slot >= 0) {
                    std::lock_guard<std::mutex> lk(m_slotMutex);
                    m_cueRuntime[static_cast<size_t>(idx)].slotByDevice[0] = slot;
                    m_slotStream[static_cast<size_t>(slot)] = std::move(reader);
                    result = true;
                    followFrames = totalFrames;
                    m_cueFireFrame[static_cast<size_t>(idx)] = m_engine.enginePlayheadFrames();
                }
            } else if (cue.tcType == "mtc") {
                // MTC: find the MIDI patch by index and start the generator.
                if (cue.tcMidiPatchIdx >= 0 &&
                    cue.tcMidiPatchIdx < (int)m_midiPatches.size()) {
                    const auto& patch = m_midiPatches[static_cast<size_t>(cue.tcMidiPatchIdx)];

                    // Stop any generator already running for this cue (re-fire case).
                    {
                        std::lock_guard<std::mutex> lk(m_slotMutex);
                        auto it = m_activeMtcGens.find(idx);
                        if (it != m_activeMtcGens.end()) {
                            it->second->stop();
                            m_activeMtcGens.erase(it);
                        }
                    }

                    auto gen = std::make_shared<MtcGenerator>(
                        patch.destination, cue.tcFps,
                        cue.tcStartTC, cue.tcEndTC, m_engine.sampleRate(), m_engine);
                    gen->start();

                    {
                        std::lock_guard<std::mutex> lk(m_slotMutex);
                        m_activeMtcGens[idx] = gen;
                    }

                    result = true;
                    m_cueFireFrame[static_cast<size_t>(idx)] = m_engine.enginePlayheadFrames();
                    followFrames = tcRangeSamples(cue.tcStartTC, cue.tcEndTC,
                                                   cue.tcFps, m_engine.sampleRate());
                    m_scheduler.scheduleAfterFrames(followFrames,
                        [this, idx, gen]() {
                            std::lock_guard<std::mutex> lk(m_slotMutex);
                            m_activeMtcGens.erase(idx);
                            m_cueFireFrame[static_cast<size_t>(idx)] = -1;
                        },
                        "mtc-cleanup[" + std::to_string(idx) + "]");
                }
            }
            break;
        }

        case CueType::Goto: {
            const int ti = cue.targetIndex;
            if (ti >= 0 && ti < cueCount()) {
                m_selectedIndex = ti;
                result = true;
            }
            followFrames = 0;
            break;
        }

        case CueType::Memo: {
            result = true;
            followFrames = 0;
            break;
        }

        case CueType::Scriptlet: {
            {
                std::lock_guard<std::mutex> lk(m_scriptletMutex);
                m_pendingScriptlets.emplace_back(idx, cue.scriptletCode);
            }
            result = true;
            followFrames = 0;
            break;
        }

        case CueType::Snapshot: {
            {
                std::lock_guard<std::mutex> lk(m_scriptletMutex);
                m_pendingSnapshots.emplace_back(idx, cue.snapshotId);
            }
            result = true;
            followFrames = 0;
            break;
        }

        case CueType::Automation: {
            if (cue.automationPath.empty() || cue.automationCurve.empty()) break;

            const double duration   = std::max(0.01, cue.automationDuration);
            const int    steps      = std::max(2, static_cast<int>(duration * 200.0));
            const double stepSec    = duration / static_cast<double>(steps - 1);
            const int64_t baseFrame = m_engine.enginePlayheadFrames();
            m_cueFireFrame[static_cast<size_t>(idx)] = baseFrame;

            std::shared_ptr<AutoParam> sharedParam = makeAutoParam(cue.automationPath);
            auto cancel = std::make_shared<std::atomic<bool>>(false);

            // Capture stableId so the done() lambda can find the cue after any
            // insert/remove/move that might shift flat indices.
            const int automationSid = m_cues[static_cast<size_t>(idx)].stableId;

            // Register cancel BEFORE scheduling so panic() always finds it,
            // even if it fires while the scheduling loop is still running.
            {
                std::lock_guard<std::mutex> lk(m_scriptletMutex);
                m_automationCancel[automationSid] = cancel;
            }

            const bool isStepMode = (Cue::automationParamMode(cue.automationPath)
                                     == Cue::AutomationParamMode::Step);

            // Pre-compute the full value table at fire() time using PCHIP.
            // Scheduler callbacks do zero math: they just index the table.
            const auto& pts = cue.automationCurve;

            // Build arrays of (time, value) for PCHIP — all points, including handles.
            const int np = static_cast<int>(pts.size());
            std::vector<double> pt_t(static_cast<size_t>(np));
            std::vector<double> pt_y(static_cast<size_t>(np));
            std::vector<double> pt_d(static_cast<size_t>(np));
            for (int k = 0; k < np; ++k) {
                pt_t[static_cast<size_t>(k)] = pts[static_cast<size_t>(k)].time;
                pt_y[static_cast<size_t>(k)] = pts[static_cast<size_t>(k)].value;
            }
            mcp_detail::pchipDerivs(pt_t.data(), pt_y.data(), pt_d.data(), np);

            auto sharedValues = std::make_shared<std::vector<double>>(
                                    static_cast<size_t>(steps));
            for (int s = 0; s < steps; ++s) {
                const double elapsed = s * stepSec;
                double value = mcp_detail::pchipEval(
                    pt_t.data(), pt_y.data(), pt_d.data(), np, elapsed);
                if (isStepMode) value = (value >= 0.5) ? 1.0 : 0.0;
                (*sharedValues)[static_cast<size_t>(s)] = value;
            }

            // Single O(1) ramp registration replaces the burst of `steps` discrete events.
            // The Ramp checks the cancel flag each poll; tick() receives the pre-computed
            // step index directly.  done() fires only if not cancelled.
            m_scheduler.scheduleRamp(baseFrame, stepSec, steps, cancel,
                [sharedParam, sharedValues](int s) {
                    if (sharedParam)
                        sharedParam->setValue((*sharedValues)[static_cast<size_t>(s)]);
                },
                [this, automationSid, cancel]() {
                    // Find current flat index by stableId (stable across structural mutations).
                    for (int i = 0; i < (int)m_cues.size(); ++i)
                        if (m_cues[i].stableId == automationSid) {
                            m_cueFireFrame[static_cast<size_t>(i)] = -1;
                            break;
                        }
                    std::lock_guard<std::mutex> lk(m_scriptletMutex);
                    auto it = m_automationCancel.find(automationSid);
                    if (it != m_automationCancel.end() &&
                        it->second.get() == cancel.get())
                        m_automationCancel.erase(it);
                });

            result = true;
            if (cue.autoFollow && duration > 0.0) {
                const int sr = m_engine.sampleRate();
                if (sr > 0) followFrames = static_cast<int64_t>(duration * sr);
            } else {
                followFrames = 0;
            }
            break;
        }

        case CueType::Deactivate:
            if (cue.pluginChannel >= 0 && cue.pluginSlot >= 0 && m_pluginDeactivateCb)
                m_pluginDeactivateCb(cue.pluginChannel, cue.pluginSlot);
            result = true;
            break;

        case CueType::Reactivate:
            if (cue.pluginChannel >= 0 && cue.pluginSlot >= 0 && m_pluginReactivateCb)
                m_pluginReactivateCb(cue.pluginChannel, cue.pluginSlot);
            result = true;
            break;
    }

    if (result && cue.autoFollow) {
        // Schedule go() to fire when the voice is expected to end.
        // The estimate is current enginePlayhead + followFrames.
        // It may be off by up to one buffer period (voice activation latency)
        // which is imperceptible for auto-follow purposes.
        m_scheduler.scheduleAfterFrames(followFrames,
            [this]() { go(); },
            "auto-follow [" + std::to_string(idx) + "] " + cue.name);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Quantize helper

double CueList::quantizeDelay(int cueIdx, int64_t originFrame) const {
    if (cueIdx < 0 || cueIdx >= cueCount()) return 0.0;
    const int q = m_cues[cueIdx].goQuantize;
    if (q == 0) return 0.0;

    // Find the outermost playing cue that has an MC attached.
    int mcCueIdx = -1;
    for (int i = 0; i < cueCount(); i++) {
        const auto& c = m_cues[i];
        if (!hasMusicContext(i) || !isCuePlaying(i)) continue;
        if (c.parentIndex < 0 || !hasMusicContext(c.parentIndex)) {
            mcCueIdx = i;
            break;
        }
    }
    if (mcCueIdx < 0) return 0.0;

    const auto* mc = musicContextOf(mcCueIdx);
    // Use cueElapsedSeconds for audio; for groups use stored fire frame.
    // For the quantize computation we need the elapsed time at originFrame,
    // not at enginePlayheadFrames(), so adjust for the difference.
    const int sr = m_engine.sampleRate();
    if (sr <= 0) return 0.0;
    const double nowElapsed  = cueElapsedSeconds(mcCueIdx);
    const double frameDelta  = static_cast<double>(
        originFrame - m_engine.enginePlayheadFrames()) / sr;
    const double elapsedSec  = nowElapsed + frameDelta;
    if (elapsedSec < 0.0) return 0.0;

    const auto cur = mc->secondsToMusical(elapsedSec);

    int nextBar  = cur.bar;
    int nextBeat = cur.beat;
    if (q == 1) {
        // Next bar boundary
        if (cur.fraction > 1e-9 || cur.beat > 1) ++nextBar;
        nextBeat = 1;
    } else {
        // Next beat boundary (q == 2)
        if (cur.fraction > 1e-9) ++nextBeat;
        const auto ts = mc->timeSigAt(nextBar, nextBeat);
        if (nextBeat > ts.num) { ++nextBar; nextBeat = 1; }
    }

    const double nextSec = mc->musicalToSeconds(nextBar, nextBeat);
    return std::max(0.0, nextSec - elapsedSec);
}

// ---------------------------------------------------------------------------
// Playback controls

bool CueList::go(int64_t originFrame) {
    if (!m_engine.isInitialized()) return false;
    if (originFrame < 0) originFrame = m_engine.enginePlayheadFrames();

    bool anyFired = false;

    // Iterative autoContinue cascade: keep firing selected cues as long as
    // each one has autoContinue set.  All cues in the cascade share the same
    // originFrame so their prewait deadlines are computed from the same base.
    while (m_selectedIndex < cueCount()) {
        const int   idx = m_selectedIndex;
        const auto& cue = m_cues[idx];

        // StartFirst Group: enter transparently — advance into first child and
        // re-enter the loop so that child fires on this same go() call.
        if (cue.type == CueType::Group && cue.groupData &&
            cue.groupData->mode == GroupData::Mode::StartFirst) {
            m_selectedIndex = (cue.childCount > 0) ? idx + 1 : logicalNext(idx);
            continue;
        }

        // Advance selection logically past this cue (skips group descendants).
        m_selectedIndex = logicalNext(idx);

        // A cue that is already playing or pending cannot be re-triggered.
        // Selection has already been advanced; leave the cue running.
        if (isCuePlaying(idx) || isCuePending(idx)) break;

        { std::lock_guard<std::mutex> lk(m_scriptletMutex); m_pendingFiredCues.push_back(idx); }

        const double pw = cue.preWaitSeconds + quantizeDelay(idx, originFrame);

        if (pw > 0.0) {
            const std::string lbl = "cue[" + std::to_string(idx) + "] " + cue.name;
            const int evtId = m_scheduler.scheduleFromFrame(originFrame, pw,
                [this, idx]() {
                    { std::lock_guard<std::mutex> lk(m_slotMutex); m_pendingEventId[idx] = -1; }
                    fire(idx);
                }, lbl);
            { std::lock_guard<std::mutex> lk(m_slotMutex); m_pendingEventId[idx] = evtId; }
            anyFired = true;
        } else {
            anyFired |= fire(idx);
        }

        if (!cue.autoContinue) break;
        // autoContinue: loop with same originFrame (next cue starts from the
        // same engine-time origin, not from when this loop iteration runs)
    }

    return anyFired;
}

bool CueList::start(int index, int64_t originFrame) {
    if (!m_engine.isInitialized())        return false;
    if (index < 0 || index >= cueCount()) return false;

    const auto& cue = m_cues[index];
    if (originFrame < 0) originFrame = m_engine.enginePlayheadFrames();

    { std::lock_guard<std::mutex> lk(m_scriptletMutex); m_pendingFiredCues.push_back(index); }

    const double pw = cue.preWaitSeconds + quantizeDelay(index, originFrame);

    if (pw > 0.0) {
        const std::string lbl = "cue[" + std::to_string(index) + "] " + cue.name;
        const int evtId = m_scheduler.scheduleFromFrame(originFrame, pw,
            [this, index]() {
                { std::lock_guard<std::mutex> lk(m_slotMutex); m_pendingEventId[index] = -1; }
                fire(index);
            }, lbl);
        { std::lock_guard<std::mutex> lk(m_slotMutex); m_pendingEventId[index] = evtId; }
        return true;
    }
    return fire(index);
}

bool CueList::prev() {
    const int n = cueCount();
    if (n == 0) return false;
    // Move cursor backwards, skipping group children.
    int cur = m_selectedIndex;
    if (cur <= 0) return false;
    cur -= 1;
    // Skip over group children (they shouldn't be the direct selection target
    // when navigating backwards — stop at the first non-child cue).
    while (cur > 0 && m_cues[cur].parentIndex >= 0)
        cur -= 1;
    m_selectedIndex = cur;
    return true;
}

void CueList::toggleArm(int index) {
    if (index < 0 || index >= cueCount()) return;
    if (isArmed(index)) disarm(index);
    else                arm(index);
}

int CueList::findByCueNumber(const std::string& num) const {
    for (int i = 0; i < cueCount(); ++i)
        if (m_cues[i].cueNumber == num) return i;
    return -1;
}

void CueList::stop(int index) {
    if (index < 0 || index >= cueCount()) return;
    // Cancel any pending prewait for this cue before clearing voices.
    int evtId = -1;
    { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[index]; m_pendingEventId[index] = -1; }
    if (evtId >= 0) m_scheduler.cancel(evtId);
    m_engine.clearVoicesByTag(m_tagBase + m_cues[static_cast<size_t>(index)].stableId);
    // Clear timeline arm position on group stop so next GO starts from 0.
    if (m_cues[index].type == CueType::Group)
        m_cues[index].timelineArmSec = 0.0;
    // Stop any running MTC generator for this cue.
    if (m_cues[index].type == CueType::Timecode) {
        std::shared_ptr<MtcGenerator> gen;
        {
            std::lock_guard<std::mutex> lk(m_slotMutex);
            auto it = m_activeMtcGens.find(index);
            if (it != m_activeMtcGens.end()) {
                gen = std::move(it->second);
                m_activeMtcGens.erase(it);
            }
        }
        if (gen) gen->stop();
    }
    // Clear fire frame for cue types that use it for playing-state tracking.
    if (index >= 0 && index < cueCount() &&
        (m_cues[index].type == CueType::MusicContext ||
         m_cues[index].type == CueType::Timecode     ||
         m_cues[index].type == CueType::Automation))
        m_cueFireFrame[static_cast<size_t>(index)] = -1;
    // Cancel any running automation for this cue (keyed by stableId).
    {
        std::lock_guard<std::mutex> lk(m_scriptletMutex);
        auto it = m_automationCancel.find(m_cues[static_cast<size_t>(index)].stableId);
        if (it != m_automationCancel.end()) {
            it->second->store(true, std::memory_order_relaxed);
            m_automationCancel.erase(it);
        }
    }
}

void CueList::panic() {
    m_scheduler.cancelAll();
    m_engine.clearAllVoices();
    m_followUps.clear();
    m_playlistFollowUps.clear();
    std::lock_guard<std::mutex> lk(m_slotMutex);
    for (auto& cue : m_cues) {
        if (cue.armedStream) {
            cue.armedStream->requestStop();
            cue.armedStream.reset();
        }
        if (cue.type == CueType::Group) cue.timelineArmSec = 0.0;
    }
    // Stop all active MTC generators.
    for (auto& kv : m_activeMtcGens)
        kv.second->stop();
    m_activeMtcGens.clear();
    // Reset fire frames for all cue types that use them for playing-state tracking.
    for (int i = 0; i < cueCount(); ++i)
        if (m_cues[i].type == CueType::MusicContext ||
            m_cues[i].type == CueType::Timecode     ||
            m_cues[i].type == CueType::Automation)
            m_cueFireFrame[static_cast<size_t>(i)] = -1;
    // Cancel all running automations.
    {
        std::lock_guard<std::mutex> lk2(m_scriptletMutex);
        for (auto& [ci, cancel] : m_automationCancel)
            cancel->store(true, std::memory_order_relaxed);
        m_automationCancel.clear();
    }
}

// ---------------------------------------------------------------------------
// Queries

bool CueList::isAnyCuePlaying() const {
    const int n = cueCount();
    for (int i = 0; i < n; ++i)
        if (isCuePlaying(i)) return true;
    return false;
}

bool CueList::isCuePlaying(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    // Cue types that have no audio voice: playing = fire frame is set.
    if (m_cues[index].type == CueType::MusicContext ||
        m_cues[index].type == CueType::Timecode     ||
        m_cues[index].type == CueType::Automation)
        return m_cueFireFrame[static_cast<size_t>(index)] >= 0;
    if (m_engine.anyVoiceActiveWithTag(m_tagBase + m_cues[static_cast<size_t>(index)].stableId)) return true;
    const auto* cue = cueAt(index);
    if (cue && cue->type == CueType::Group && cue->childCount > 0) {
        for (int i = index + 1; i <= index + cue->childCount; ++i)
            if (m_engine.anyVoiceActiveWithTag(m_tagBase + m_cues[static_cast<size_t>(i)].stableId)) return true;
    }
    return false;
}

bool CueList::isCuePending(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    int evtId;
    { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[index]; }
    return m_scheduler.isPending(evtId);
}

double CueList::cuePendingFraction(int index) const {
    if (index < 0 || index >= cueCount()) return -1.0;
    int evtId;
    { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[index]; }
    const int64_t fireFrame = m_scheduler.pendingEventTargetFrame(evtId);
    if (fireFrame < 0) return -1.0;
    const double preWait = m_cues[index].preWaitSeconds;
    const int sr = m_engine.isInitialized() ? m_engine.sampleRate() : 48000;
    if (sr <= 0 || preWait <= 0.0) return 1.0;
    const int64_t preWaitFrames = static_cast<int64_t>(preWait * sr);
    const int64_t goFrame = fireFrame - preWaitFrames;
    const int64_t now = m_engine.enginePlayheadFrames();
    return std::max(0.0, std::min(1.0,
        static_cast<double>(now - goFrame) / static_cast<double>(preWaitFrames)));
}

double CueList::cueSliceProgress(int index, bool& isLooping) const {
    isLooping = false;
    if (index < 0 || index >= cueCount()) return -1.0;
    const auto& cue = m_cues[index];

    // Automation cue: linear progress over its duration.
    if (cue.type == CueType::Automation) {
        const int64_t fireFrame = m_cueFireFrame[static_cast<size_t>(index)];
        if (fireFrame < 0) return -1.0;
        const int sr = m_engine.sampleRate();
        if (sr <= 0) return -1.0;
        const double elapsed = static_cast<double>(
            m_engine.enginePlayheadFrames() - fireFrame) / sr;
        const double dur = std::max(0.01, cue.automationDuration);
        return std::clamp(elapsed / dur, 0.0, 1.0);
    }

    // SyncGroup: use syncPlaySlice + cueElapsedSeconds (monotone timeline position)
    if (cue.type == CueType::Group && cue.groupData &&
        cue.groupData->mode == GroupData::Mode::Sync) {
        if (!isCuePlaying(index)) return -1.0;
        const int slice   = cue.groupData->syncPlaySlice;
        const double elap = cueElapsedSeconds(index);
        const auto& marks = cue.markers;
        const double sliceStart = (slice == 0) ? m_cueFireArmBase[static_cast<size_t>(index)]
                                               : marks[slice - 1].time;
        const double sliceEnd   = (slice < (int)marks.size()) ? marks[slice].time
                                                               : syncGroupBaseDuration(index);
        const double sliceDur   = sliceEnd - sliceStart;
        if (sliceDur <= 0.0) return -1.0;
        if (slice < (int)cue.sliceLoops.size())
            isLooping = (cue.sliceLoops[slice] != 1);
        const double pos = std::fmod(std::max(0.0, elap - sliceStart), sliceDur);
        return pos / sliceDur;
    }

    // Audio cue
    if (cue.type != CueType::Audio || !isCuePlaying(index)) return -1.0;
    const double filePos = cuePlayheadFileSeconds(index);

    const auto& marks = cue.markers;
    double sliceStart = cue.startTime;
    double sliceEnd;
    int sliceIdx = 0;

    const double fileDur = (cue.duration > 0.0) ? cue.duration
                                                 : cue.audioFile.metadata().durationSeconds();
    if (marks.empty()) {
        sliceEnd = cue.startTime + fileDur;
    } else {
        sliceEnd = marks[0].time;
        for (int i = 0; i < (int)marks.size(); ++i) {
            if (filePos < marks[i].time) {
                sliceStart = (i == 0) ? cue.startTime : marks[i - 1].time;
                sliceEnd   = marks[i].time;
                sliceIdx   = i;
                break;
            }
            if (i == (int)marks.size() - 1) {
                sliceStart = marks[i].time;
                sliceEnd   = cue.startTime + fileDur;
                sliceIdx   = i + 1;
            }
        }
    }

    const double sliceDur = sliceEnd - sliceStart;
    if (sliceDur <= 0.0) return -1.0;
    if (sliceIdx < (int)cue.sliceLoops.size())
        isLooping = (cue.sliceLoops[sliceIdx] != 1);
    // Audio file position naturally loops back to sliceStart on each iteration.
    return std::max(0.0, std::min(1.0, (filePos - sliceStart) / sliceDur));
}

bool CueList::isFadeActive(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    const auto& fd = m_cues[index].fadeData;
    if (!fd) return false;
    return fd->activeSteps.load(std::memory_order_relaxed) > 0;
}

int CueList::activeCueCount() const {
    int n = 0;
    for (int i = 0; i < cueCount(); ++i)
        if (m_engine.anyVoiceActiveWithTag(m_tagBase + m_cues[static_cast<size_t>(i)].stableId)) ++n;
    return n;
}

int CueList::cueVoiceSlot(int index) const {
    if (index < 0 || index >= cueCount()) return -1;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    return m_cueRuntime[static_cast<size_t>(index)].canonicalSlot();
}

int64_t CueList::cuePlayheadFrames(int index) const {
    if (index < 0 || index >= cueCount()) return 0;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    const int slot = m_cueRuntime[static_cast<size_t>(index)].canonicalSlot();
    if (slot < 0 || !m_engine.isVoiceActive(slot)) return 0;
    const int64_t pos = m_engine.enginePlayheadFrames() - m_engine.voiceStartEngineFrame(slot);
    return std::max<int64_t>(0, pos);
}

double CueList::cuePlayheadSeconds(int index) const {
    const int sr = m_engine.sampleRate();
    return sr > 0 ? static_cast<double>(cuePlayheadFrames(index)) / sr : 0.0;
}

double CueList::cueElapsedSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;
    const auto& cue = m_cues[index];
    if (cue.type == CueType::Group || cue.type == CueType::MusicContext) {
        const int64_t fireFrame = m_cueFireFrame[static_cast<size_t>(index)];
        if (fireFrame < 0) return 0.0;
        const int sr = m_engine.sampleRate();
        if (sr <= 0) return 0.0;
        const int64_t elapsed = m_engine.enginePlayheadFrames() - fireFrame;
        return m_cueFireArmBase[static_cast<size_t>(index)]
               + static_cast<double>(std::max<int64_t>(0, elapsed)) / sr;
    }
    // For audio cues use the file-position playhead so that loop wrap-arounds
    // are reflected in the MC position.  cuePlayheadFileSeconds returns 0 only
    // during the brief startup race before the IO thread writes the first
    // SegMarker — fall back to the monotonic engine elapsed in that window.
    if (cue.type == CueType::Audio) {
        const double filePos    = cuePlayheadFileSeconds(index);
        const double engineElap = cuePlayheadSeconds(index);
        if (filePos > 0.0 || engineElap < 0.05) {
            return std::max(0.0, filePos - cue.startTime);
        }
        return engineElap;
    }
    return cuePlayheadSeconds(index);
}

double CueList::cuePlayheadFileSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;

    StreamReader* raw = nullptr;
    int targetSR = 0;
    int64_t rp = 0;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        const int slot = m_cueRuntime[static_cast<size_t>(index)].canonicalSlot();
        if (slot < 0 || !m_engine.isVoiceActive(slot)) return 0.0;
        raw      = slotStreamReader(static_cast<size_t>(slot));
        if (!raw) return 0.0;
        targetSR = raw->targetSampleRate();
        rp       = raw->readPos();
    }
    if (!raw || targetSR <= 0) return 0.0;

    const int mc = raw->segMarkerCount();
    if (mc == 0) return 0.0;

    // Binary-search for the last marker with writePos <= rp.
    // Markers are appended in writePos order (IO thread writes monotonically).
    int bestIdx = 0;
    for (int i = 1; i < mc; ++i) {
        if (raw->segMarkerAt(i).writePos <= rp) bestIdx = i;
        else break;
    }

    const StreamReader::SegMarker sm = raw->segMarkerAt(bestIdx);
    const double elapsed = static_cast<double>(rp - sm.writePos) / targetSR;
    return sm.fileStartSecs + elapsed;
}

// ---------------------------------------------------------------------------
// Sync group helpers

double CueList::syncGroupBaseDuration(int gi) const {
    if (gi < 0 || gi >= cueCount()) return 0.0;
    const auto& g = m_cues[gi];
    double maxEnd = 0.0;
    for (int i = gi + 1; i <= gi + g.childCount; ) {
        if (m_cues[i].parentIndex == gi) {
            const int ci = i;
            double childDur = m_cues[ci].duration;
            if (childDur <= 0.0 && m_cues[ci].type == CueType::Audio)
                childDur = cueTotalSeconds(ci);  // 0 if infinite (broken guard handles that)
            if (childDur <= 0.0) childDur = 2.0;
            maxEnd = std::max(maxEnd, m_cues[ci].timelineOffset + childDur);
            const int cc = (m_cues[ci].type == CueType::Group) ? m_cues[ci].childCount : 0;
            i += cc + 1;
        } else { ++i; }
    }
    return maxEnd;
}

double CueList::syncGroupTotalSeconds(int gi) const {
    if (gi < 0 || gi >= cueCount()) return 0.0;
    const auto& g = m_cues[gi];
    if (!g.groupData || g.groupData->mode != GroupData::Mode::Sync) return 0.0;
    const double base = syncGroupBaseDuration(gi);
    if (base <= 0.0) return 0.0;
    if (g.markers.empty()) {
        const int lc = g.sliceLoops.empty() ? 1 : g.sliceLoops[0];
        if (lc == 0) return std::numeric_limits<double>::infinity();
        return base * lc;
    }
    std::vector<double> bounds = {0.0};
    for (const auto& m : g.markers) bounds.push_back(m.time);
    if (bounds.back() < base) bounds.push_back(base);
    double total = 0.0;
    for (int i = 0; i + 1 < (int)bounds.size(); ++i) {
        const int lc = (i < (int)g.sliceLoops.size()) ? g.sliceLoops[i] : 1;
        if (lc == 0) return std::numeric_limits<double>::infinity();
        total += (bounds[i + 1] - bounds[i]) * lc;
    }
    return total;
}

bool CueList::isSyncGroupBroken(int gi) const {
    if (gi < 0 || gi >= cueCount()) return false;
    const auto& g = m_cues[gi];
    for (int i = gi + 1; i <= gi + g.childCount && i < cueCount(); ++i) {
        if (m_cues[i].type == CueType::Audio) {
            for (int lc : m_cues[i].sliceLoops)
                if (lc == 0) return true;
        }
    }
    return false;
}

void CueList::stopSyncGroupChildren(int gi) {
    if (gi < 0 || gi >= cueCount()) return;
    const auto& g = m_cues[gi];
    for (int i = gi + 1; i <= gi + g.childCount && i < cueCount(); ++i)
        if (m_cues[i].type == CueType::Audio)
            m_engine.clearVoicesByTag(m_tagBase + m_cues[static_cast<size_t>(i)].stableId);
}

void CueList::fireSyncGroup(int gi, double armPos, int64_t originFrame) {
    if (gi < 0 || gi >= cueCount()) return;
    auto& g = m_cues[gi];
    if (!g.groupData || g.childCount == 0) return;
    if (isSyncGroupBroken(gi)) return;

    const double baseDur = syncGroupBaseDuration(gi);
    if (baseDur <= 0.0) return;

    // Build slice boundaries from group's own markers
    std::vector<double> bounds = {0.0};
    for (const auto& m : g.markers) bounds.push_back(m.time);
    if (bounds.back() < baseDur) bounds.push_back(baseDur);
    const int numSlices = (int)bounds.size() - 1;

    // Find which slice armPos falls in
    int sliceIdx = numSlices - 1;
    for (int i = 0; i < numSlices; ++i)
        if (armPos < bounds[i + 1]) { sliceIdx = i; break; }

    // Bump generation → stale callbacks become no-ops; clear devamp flags
    ++g.groupData->syncGeneration;
    g.groupData->syncDevampMode    = -1;
    g.groupData->syncDevampPreVamp = false;
    const int gen = g.groupData->syncGeneration;

    g.groupData->syncPlaySlice = sliceIdx;
    g.groupData->syncLoopsLeft = (sliceIdx < (int)g.sliceLoops.size())
                                  ? g.sliceLoops[sliceIdx] : 1;

    const double sliceEnd = bounds[sliceIdx + 1];

    // Collect direct children
    std::vector<int> directChildren;
    for (int i = gi + 1; i <= gi + g.childCount; ) {
        if (m_cues[i].parentIndex == gi) {
            directChildren.push_back(i);
            const int cc = (m_cues[i].type == CueType::Group) ? m_cues[i].childCount : 0;
            i += cc + 1;
        } else { ++i; }
    }

    // Pre-arm all audio descendants so StreamReaders have buffered data.
    armGroupDescendants(gi, armPos);

    // Fire children (same timeline-arm logic as fireGroup)
    for (int ci : directChildren) {
        const double armInto  = armPos - m_cues[ci].timelineOffset;
        const double childDur = (m_cues[ci].duration > 0.0) ? m_cues[ci].duration
            : (m_cues[ci].type == CueType::Audio ? cueTotalSeconds(ci) : 2.0);
        if (childDur > 0.0 && armInto >= childDur) continue;  // past

        const double fireDelay = std::max(0.0, -armInto);

        if (m_cues[ci].type == CueType::Group && m_cues[ci].groupData) {
            const double nestedArm = std::max(0.0, armInto);
            if (fireDelay > 0.0)
                m_scheduler.scheduleFromFrame(originFrame, fireDelay,
                    [this, ci, nestedArm, originFrame, gi, gen]() {
                        if (m_cues[gi].groupData &&
                            m_cues[gi].groupData->syncGeneration == gen)
                            fireGroup(ci, nestedArm, originFrame);
                    }, "sg-nest[" + std::to_string(ci) + "]");
            else
                fireGroup(ci, nestedArm, originFrame);
        } else if (fireDelay > 0.0) {
            m_scheduler.scheduleFromFrame(originFrame, fireDelay,
                [this, ci, gi, gen]() {
                    if (m_cues[gi].groupData &&
                        m_cues[gi].groupData->syncGeneration == gen)
                        fire(ci);
                }, "sg-child[" + std::to_string(ci) + "]");
        } else if (armInto > 0.0 && m_cues[ci].type == CueType::Audio) {
            const double origStart = m_cues[ci].startTime;
            m_cues[ci].startTime = origStart + armInto;
            fire(ci);
            m_cues[ci].startTime = origStart;
        } else {
            fire(ci);
        }
    }

    // Schedule end-of-slice callback
    const double timeToEnd = sliceEnd - armPos;
    if (timeToEnd > 0.001) {
        m_scheduler.scheduleFromFrame(originFrame, timeToEnd,
            [this, gi, gen, sliceIdx, numSlices, bounds]() mutable {
                if (gi >= cueCount()) return;
                auto* gd = m_cues[gi].groupData.get();
                if (!gd || gd->syncGeneration != gen) return;

                const int64_t now = m_engine.enginePlayheadFrames();
                const int loopsLeft = gd->syncLoopsLeft;

                if (loopsLeft == 0 || loopsLeft > 1) {
                    if (loopsLeft > 1) --gd->syncLoopsLeft;
                    stopSyncGroupChildren(gi);
                    // Reset fire-time tracking so cueElapsedSeconds wraps back.
                    m_cueFireFrame[static_cast<size_t>(gi)]   = now;
                    m_cueFireArmBase[static_cast<size_t>(gi)] = bounds[sliceIdx];
                    fireSyncGroup(gi, bounds[sliceIdx], now);
                } else {
                    // Last (or only) iteration: execute devamp or natural advance.
                    const int  devMode = gd->syncDevampMode;
                    const bool prv     = gd->syncDevampPreVamp;
                    gd->syncLoopsLeft      = -1;
                    gd->syncDevampMode     = -1;
                    gd->syncDevampPreVamp  = false;
                    stopSyncGroupChildren(gi);

                    if (devMode < 0) {
                        // Natural end — normal slice advance
                        if (sliceIdx + 1 < numSlices)
                            fireSyncGroup(gi, bounds[sliceIdx + 1], now);
                        else { ++gd->syncGeneration; if (m_cues[gi].autoFollow) go(); }
                    } else {
                        // Devamp: choose target slice, honouring prevamp skip
                        int target = sliceIdx + 1;
                        if (prv && target < numSlices) {
                            // Skip target if it is configured to loop
                            const int cfg = (target < (int)m_cues[gi].sliceLoops.size())
                                            ? m_cues[gi].sliceLoops[target] : 1;
                            if (cfg != 1) ++target;
                        }
                        const bool hasTarget = (target < numSlices);
                        if (devMode == 0) {
                            // Next slice (no go())
                            if (hasTarget) fireSyncGroup(gi, bounds[target], now);
                            else ++gd->syncGeneration;
                        } else if (devMode == 1) {
                            // Stop group + go()
                            ++gd->syncGeneration;
                            go();
                        } else {
                            // Keep group (fire target if any) + go()
                            if (hasTarget) fireSyncGroup(gi, bounds[target], now);
                            else ++gd->syncGeneration;
                            go();
                        }
                    }
                }
            },
            "sg-end[" + std::to_string(gi) + "]");
    }
}

void CueList::devampSyncGroup(int gi, int devampMode, bool preVamp) {
    if (gi < 0 || gi >= cueCount()) return;
    auto* gd = m_cues[gi].groupData.get();
    if (!gd) return;

    // Always wait for the current slice to finish its iteration.
    // If the slice is currently looping (infinite or multi), cut to 1 remaining
    // so the end-of-slice callback fires after this playthrough.
    const bool isLooping = (gd->syncLoopsLeft == 0 || gd->syncLoopsLeft > 1);
    if (isLooping) gd->syncLoopsLeft = 1;

    // Store the operation.  Prevamp skip only applies when the current slice
    // itself is not looping — when in a looping slice, behaviour is identical
    // to normal devamp (user spec: "在B里devamp发现自己是loop则behavior和普通devamp相同").
    gd->syncDevampMode    = devampMode;
    gd->syncDevampPreVamp = preVamp && !isLooping;
}

// ---------------------------------------------------------------------------
// cueTotalSeconds

double CueList::cueTotalSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;
    const auto& cue = m_cues[index];
    // Delegate SyncGroup to its own calculator
    if (cue.type == CueType::Group && cue.groupData &&
        cue.groupData->mode == GroupData::Mode::Sync) {
        const double t = syncGroupTotalSeconds(index);
        return std::isfinite(t) ? t : 0.0;
    }
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return 0.0;
    const auto& meta = cue.audioFile.metadata();
    const int sr = meta.sampleRate;
    if (sr <= 0) return 0.0;

    if (cue.markers.empty()) {
        const int lc  = cue.sliceLoops.empty() ? 1 : cue.sliceLoops[0];
        if (lc == 0) return 0.0;  // infinite → unknown duration
        const int64_t frames = voiceFrames(cue);
        return static_cast<double>(frames) / sr * lc;
    }

    // Multi-slice: sum up each slice duration * its loop count.
    const double fileDur = static_cast<double>(meta.frameCount) / sr;
    const double endSec  = (cue.duration > 0.0) ? cue.startTime + cue.duration : fileDur;
    std::vector<double> bounds;
    bounds.push_back(cue.startTime);
    for (const auto& m : cue.markers) bounds.push_back(m.time);
    bounds.push_back(endSec);

    double total = 0.0;
    for (int i = 0; i < (int)bounds.size() - 1; ++i) {
        const int lc = (i < (int)cue.sliceLoops.size()) ? cue.sliceLoops[static_cast<size_t>(i)] : 1;
        if (lc == 0) return 0.0;  // infinite
        total += (bounds[static_cast<size_t>(i+1)] - bounds[static_cast<size_t>(i)]) * lc;
    }
    return total;
}

} // namespace mcp
