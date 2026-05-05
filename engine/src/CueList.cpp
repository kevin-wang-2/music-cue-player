#include "engine/CueList.h"
#include <algorithm>

namespace mcp {

CueList::CueList(AudioEngine& engine, Scheduler& scheduler)
    : m_engine(engine), m_scheduler(scheduler) {}

// ---------------------------------------------------------------------------
// List construction

bool CueList::addCue(const std::string& path, const std::string& name,
                     double preWait) {
    Cue cue;
    cue.type           = CueType::Audio;
    cue.path           = path;
    cue.name           = name.empty() ? path : name;
    cue.preWaitSeconds = preWait;
    if (!cue.audioFile.load(path)) return false;

    if (m_engine.isInitialized()) {
        const auto& meta = cue.audioFile.metadata();
        if (meta.sampleRate != m_engine.sampleRate()) return false;
        if (meta.channels   != m_engine.channels())   return false;
    }

    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
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
    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
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
    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
    return true;
}

void CueList::clear() {
    panic();
    m_cues.clear();
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.clear();
    m_pendingEventId.clear();
    m_selectedIndex = 0;
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
void CueList::setCueStartTime  (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].startTime      = s; }
void CueList::setCueDuration   (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].duration       = s; }
void CueList::setCueAutoContinue(int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoContinue   = v; }
void CueList::setCueAutoFollow  (int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoFollow      = v; }
void CueList::setCueName        (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].name          = n; }

// ---------------------------------------------------------------------------
// Internal helpers

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

bool CueList::scheduleVoice(int cueIndex) {
    const auto& cue  = m_cues[cueIndex];
    const auto& meta = cue.audioFile.metadata();

    const int64_t startFrame = std::min(
        static_cast<int64_t>(cue.startTime * meta.sampleRate), meta.frameCount);
    const int64_t playFrames = voiceFrames(cue);
    if (playFrames <= 0) return false;

    const int slot = m_engine.scheduleVoice(
        cue.audioFile.samples().data() + startFrame * meta.channels,
        playFrames, meta.channels, cueIndex);
    if (slot < 0) return false;

    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot[cueIndex] = slot;
    return true;
}

bool CueList::fire(int idx) {
    const auto& cue = m_cues[idx];
    bool   result      = false;
    int64_t followFrames = 0;   // frames until autoFollow should trigger go()

    switch (cue.type) {
        case CueType::Audio:
            result       = scheduleVoice(idx);
            followFrames = result ? voiceFrames(cue) : 0;
            break;

        case CueType::Start: {
            const int ti = cue.targetIndex;
            if (ti >= 0 && ti < cueCount() && m_cues[ti].type == CueType::Audio) {
                result       = scheduleVoice(ti);
                followFrames = result ? voiceFrames(m_cues[ti]) : 0;
            }
            break;
        }

        case CueType::Stop:
            m_engine.clearVoicesByTag(cue.targetIndex);
            result = true;
            // followFrames = 0 → autoFollow fires in the next scheduler poll
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
// Playback controls

bool CueList::go(int64_t originFrame) {
    if (!m_engine.isInitialized()) return false;
    if (originFrame < 0) originFrame = m_engine.enginePlayheadFrames();

    bool anyFired = false;

    // Iterative autoContinue cascade: keep firing selected cues as long as
    // each one has autoContinue set.  All cues in the cascade share the same
    // originFrame so their prewait deadlines are computed from the same base.
    while (m_selectedIndex < cueCount()) {
        const int    idx = m_selectedIndex++;
        const auto&  cue = m_cues[idx];

        // A cue that is already playing or pending cannot be re-triggered.
        // Advance the selection past it but leave it running.
        if (isCuePlaying(idx) || isCuePending(idx)) break;

        const double pw = cue.preWaitSeconds;

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

    const auto&  cue = m_cues[index];
    const double pw  = cue.preWaitSeconds;

    if (pw > 0.0) {
        if (originFrame < 0) originFrame = m_engine.enginePlayheadFrames();
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

void CueList::stop(int index) {
    if (index < 0 || index >= cueCount()) return;
    // Cancel any pending prewait for this cue before clearing voices.
    int evtId = -1;
    { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[index]; m_pendingEventId[index] = -1; }
    if (evtId >= 0) m_scheduler.cancel(evtId);
    m_engine.clearVoicesByTag(index);
}

void CueList::panic() {
    m_scheduler.cancelAll();
    m_engine.clearAllVoices();
}

// ---------------------------------------------------------------------------
// Queries

bool CueList::isAnyCuePlaying() const { return m_engine.activeVoiceCount() > 0; }

bool CueList::isCuePlaying(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    return m_engine.anyVoiceActiveWithTag(index);
}

bool CueList::isCuePending(int index) const {
    if (index < 0 || index >= cueCount()) return false;
    int evtId;
    { std::lock_guard<std::mutex> lk(m_slotMutex); evtId = m_pendingEventId[index]; }
    return m_scheduler.isPending(evtId);
}

int CueList::activeCueCount() const {
    int n = 0;
    for (int i = 0; i < cueCount(); ++i)
        if (m_engine.anyVoiceActiveWithTag(i)) ++n;
    return n;
}

int CueList::cueVoiceSlot(int index) const {
    if (index < 0 || index >= cueCount()) return -1;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    return m_lastSlot[index];
}

int64_t CueList::cuePlayheadFrames(int index) const {
    if (index < 0 || index >= cueCount()) return 0;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    const int slot = m_lastSlot[index];
    if (slot < 0 || !m_engine.isVoiceActive(slot)) return 0;
    const int64_t pos = m_engine.enginePlayheadFrames() - m_engine.voiceStartEngineFrame(slot);
    return std::max<int64_t>(0, pos);
}

double CueList::cuePlayheadSeconds(int index) const {
    const int sr = m_engine.sampleRate();
    return sr > 0 ? static_cast<double>(cuePlayheadFrames(index)) / sr : 0.0;
}

double CueList::cueTotalSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;
    const auto& cue = m_cues[index];
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return 0.0;
    const int64_t frames = voiceFrames(cue);
    const int sr = cue.audioFile.metadata().sampleRate;
    return (sr > 0) ? static_cast<double>(frames) / sr : 0.0;
}

} // namespace mcp
