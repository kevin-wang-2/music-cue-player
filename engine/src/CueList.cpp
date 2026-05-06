#include "engine/CueList.h"
#include "engine/AudioMath.h"
#include "engine/StreamReader.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace mcp {


CueList::CueList(AudioEngine& engine, Scheduler& scheduler)
    : m_engine(engine), m_scheduler(scheduler) {}

CueList::~CueList() { panic(); }

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

bool CueList::addFadeCue(int resolvedTargetIdx, const std::string& targetCueNumber,
                          const std::string& parameter,
                          double targetValue,
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
    cue.fadeData = std::make_shared<FadeData>();
    cue.fadeData->targetCueNumber   = targetCueNumber;
    cue.fadeData->resolvedTargetIdx = resolvedTargetIdx;
    cue.fadeData->parameter         = parameter;
    cue.fadeData->targetValue       = targetValue;
    cue.fadeData->curve             = curve;
    cue.fadeData->stopWhenDone      = stopWhenDone;
    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
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
    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
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
    m_lastSlot.clear();
    m_pendingEventId.clear();
    m_selectedIndex = 0;
}

static int64_t voiceFrames(const Cue& cue);  // defined in "Internal helpers" below

// ---------------------------------------------------------------------------
// ARM

bool CueList::arm(int index) {
    if (index < 0 || index >= cueCount()) return false;
    const auto& cue = m_cues[index];
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return false;

    const int sr = m_engine.isInitialized() ? m_engine.sampleRate() : 48000;
    const int ch = m_engine.isInitialized() ? m_engine.channels()   : 2;
    auto reader = std::make_shared<StreamReader>(
        cue.path, sr, ch, cue.startTime, cue.duration);
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
    std::lock_guard<std::mutex> lk(m_slotMutex);
    const auto& s = m_cues[index].armedStream;
    return s && !s->hasError() && s->isArmed();
}

void CueList::softPanic(double fadeSecs) {
    m_scheduler.cancelAll();
    // Signal active I/O threads to stop filling (they're about to be faded out).
    for (auto& s : m_slotStream)
        if (s) s->requestStop();
    m_engine.softPanic(fadeSecs);
}

void CueList::update() {
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
void CueList::setCueStartTime  (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].startTime      = s; }
void CueList::setCueDuration   (int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].duration       = s; }
void CueList::setCueLevel       (int i, double dB)           { if (i>=0&&i<cueCount()) m_cues[i].level          = dB; }
void CueList::setCueTrim        (int i, double dB)           { if (i>=0&&i<cueCount()) m_cues[i].trim           = dB; }
void CueList::setCueAutoContinue(int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoContinue   = v; }
void CueList::setCueAutoFollow  (int i, bool v)             { if (i>=0&&i<cueCount()) m_cues[i].autoFollow      = v; }
void CueList::setCueName        (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].name          = n; }
void CueList::setCueCueNumber   (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].cueNumber     = n; }

void CueList::setCueFadeTargetValue(int i, double dB) {
    if (i >= 0 && i < cueCount() && m_cues[i].fadeData)
        m_cues[i].fadeData->targetValue = dB;
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
// Internal helpers

// dB → linear gain for stored level values.
// Treats kFaderFloor (−60 dB) and below as silence so the UI's "−inf" floor
// produces a true zero rather than the 0.1% residual of −60 dB.
static constexpr double kFaderFloor = -60.0;
static float levelGain(double levelDB, double trimDB) {
    const double dB = levelDB + trimDB;
    return (dB <= kFaderFloor) ? 0.0f : lut::dBToLinear(dB);
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

int64_t CueList::scheduleVoice(int cueIndex) {
    const auto& cue = m_cues[cueIndex];

    // Take the pre-armed stream if available; otherwise do a cold start.
    std::shared_ptr<StreamReader> reader;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        reader = std::move(m_cues[cueIndex].armedStream);
    }

    if (!reader || reader->hasError()) {
        reader = std::make_shared<StreamReader>(
            cue.path, m_engine.sampleRate(), m_engine.channels(),
            cue.startTime, cue.duration);
        if (reader->hasError()) return -1;
    }

    const int64_t totalFrames = reader->totalOutputFrames();
    // 0 means unknown length (stream); still valid — isDone() will stop the voice.
    if (totalFrames < 0) return -1;

    const float gain = levelGain(cue.level, cue.trim);
    // Pass engine.channels() as voiceChannels so the callback's channel check
    // always passes — StreamReader::read() handles the actual channel mapping.
    const int slot = m_engine.scheduleStreamingVoice(
        reader.get(), totalFrames > 0 ? totalFrames : INT64_MAX / 2,
        m_engine.channels(), cueIndex, gain);
    if (slot < 0) return -1;

    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot[cueIndex] = slot;
    m_slotStream[slot] = std::move(reader);
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
            const int ti = cue.targetIndex;
            if (ti >= 0 && ti < cueCount() && m_cues[ti].type == CueType::Audio) {
                const int64_t out = scheduleVoice(ti);
                result       = (out >= 0);
                followFrames = result ? out : 0;
            }
            break;
        }

        case CueType::Stop:
            m_engine.clearVoicesByTag(cue.targetIndex);
            result = true;
            // followFrames = 0 → autoFollow fires in the next scheduler poll
            break;

        case CueType::Arm: {
            const int ti = cue.targetIndex;
            if (ti >= 0 && ti < cueCount() && m_cues[ti].type == CueType::Audio) {
                arm(ti);
                result = true;
                // followFrames = 0 → autoFollow fires in the next scheduler poll
            }
            break;
        }

        case CueType::Fade: {
            const auto fd = cue.fadeData;
            if (!fd) break;
            const int tIdx = fd->resolvedTargetIdx;
            if (tIdx < 0 || tIdx >= cueCount()) break;

            // Duration lives on the Cue itself (shared with audio playback region).
            const double length  = std::max(0.01, cue.duration > 0.0 ? cue.duration : 3.0);
            const double startDB = m_cues[static_cast<std::size_t>(tIdx)].level;
            const int    sr      = m_engine.sampleRate();
            const int    steps   = std::max(2, static_cast<int>(length * 30.0));
            const double stepSec = length / static_cast<double>(steps - 1);

            // Join any leftover thread from a previous fire.
            if (fd->computeThread.joinable()) fd->computeThread.join();
            fd->rampReady.store(false, std::memory_order_relaxed);

            // Compute ramp in a separate thread.
            fd->computeThread = std::thread([fd, startDB, steps]() {
                fd->computeRamp(startDB, steps);
            });

            // Schedule one callback per ramp step.
            const int64_t baseFrame = m_engine.enginePlayheadFrames();
            for (int s = 0; s < steps; ++s) {
                const bool isLast = (s == steps - 1);
                m_scheduler.scheduleFromFrame(baseFrame, s * stepSec,
                    [this, fd, tIdx, s, isLast]() {
                        if (!fd->rampReady.load(std::memory_order_acquire)) return;
                        if (tIdx < 0 || tIdx >= cueCount()) return;
                        if (s >= static_cast<int>(fd->ramp.size()))  return;
                        const double rampDB = fd->ramp[static_cast<std::size_t>(s)];
                        // Apply gain to the live voice only — never mutate tc.level so
                        // that firing the cue again after the fade restores its original level.
                        const auto& tc  = m_cues[static_cast<std::size_t>(tIdx)];
                        const int   slot = cueVoiceSlot(tIdx);
                        if (slot >= 0 && m_engine.isVoiceActive(slot))
                            m_engine.setVoiceGain(slot, levelGain(rampDB, tc.trim));
                        if (isLast && fd->stopWhenDone)
                            m_engine.clearVoicesByTag(tIdx);
                    },
                    "fade[" + std::to_string(idx) + "][" + std::to_string(s) + "]");
            }

            followFrames = (sr > 0) ? static_cast<int64_t>(length * sr) : 0;
            result = true;
            break;
        }
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
