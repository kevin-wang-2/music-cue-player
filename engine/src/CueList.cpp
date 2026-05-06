#include "engine/CueList.h"
#include "engine/AudioMath.h"
#include "engine/StreamReader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
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
    cue.fadeData->curve             = curve;
    cue.fadeData->stopWhenDone      = stopWhenDone;
    m_cues.push_back(std::move(cue));
    std::lock_guard<std::mutex> lk(m_slotMutex);
    m_lastSlot.push_back(-1);
    m_pendingEventId.push_back(-1);
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

bool CueList::arm(int index, double startOverride) {
    if (index < 0 || index >= cueCount()) return false;
    const auto& cue = m_cues[index];
    if (cue.type != CueType::Audio || !cue.audioFile.isLoaded()) return false;

    const int    sr    = m_engine.isInitialized() ? m_engine.sampleRate() : 48000;
    const int    ch    = m_engine.isInitialized() ? m_engine.channels()   : 2;
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
    std::lock_guard<std::mutex> lk(m_slotMutex);
    const auto& s = m_cues[index].armedStream;
    return s && !s->hasError() && s->isArmed();
}

void CueList::softPanic(double fadeSecs) {
    m_scheduler.cancelAll();
    m_followUps.clear();
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
                const int slot = m_lastSlot[it->watchCueIdx];
                if (slot >= 0 && m_slotStream[slot])
                    raw = m_slotStream[slot].get();
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
void CueList::setCueArmStartTime(int i, double s)            { if (i>=0&&i<cueCount()) m_cues[i].armStartTime   = s; }
void CueList::setCueCueNumber   (int i, const std::string& n){ if (i>=0&&i<cueCount()) m_cues[i].cueNumber     = n; }

void CueList::setCueDevampMode   (int i, int mode) { if (i>=0&&i<cueCount()&&m_cues[i].type==CueType::Devamp) m_cues[i].devampMode    = mode; }
void CueList::setCueDevampPreVamp(int i, bool v)   { if (i>=0&&i<cueCount()&&m_cues[i].type==CueType::Devamp) m_cues[i].devampPreVamp = v; }

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

void CueList::setCueOutLevelGain(int cueIdx, int outCh, float linGain) {
    if (cueIdx < 0 || cueIdx >= cueCount() || outCh < 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(m_slotMutex); slot = m_lastSlot[static_cast<size_t>(cueIdx)]; }
    if (slot >= 0 && m_slotStream[static_cast<size_t>(slot)])
        m_slotStream[static_cast<size_t>(slot)]->setOutLevelGain(outCh, linGain);
}

void CueList::setCueXpointGain(int cueIdx, int srcCh, int outCh, float linGain) {
    if (cueIdx < 0 || cueIdx >= cueCount() || srcCh < 0 || outCh < 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(m_slotMutex); slot = m_lastSlot[static_cast<size_t>(cueIdx)]; }
    if (slot >= 0 && m_slotStream[static_cast<size_t>(slot)])
        m_slotStream[static_cast<size_t>(slot)]->setXpointGain(srcCh, outCh, linGain);
}

// ---------------------------------------------------------------------------
// Internal helpers

static constexpr double kFaderFloor = -60.0;
static float levelGain(double levelDB, double trimDB) {
    const double dB = levelDB + trimDB;
    return (dB <= kFaderFloor) ? 0.0f : lut::dBToLinear(dB);
}

void CueList::applyRoutingToReader(const Cue& cue, StreamReader& reader,
                                    int srcCh, int outCh) {
    std::vector<float> xpGains(static_cast<size_t>(srcCh * outCh),
                                std::numeric_limits<float>::quiet_NaN());
    std::vector<float> outLevGains(static_cast<size_t>(outCh), 1.0f);

    // Per-output-channel levels
    for (int o = 0; o < outCh; ++o)
        if (o < (int)cue.routing.outLevelDb.size())
            outLevGains[static_cast<size_t>(o)] = lut::dBToLinear(cue.routing.outLevelDb[static_cast<size_t>(o)]);

    // Crosspoint matrix
    for (int s = 0; s < srcCh; ++s) {
        for (int o = 0; o < outCh; ++o) {
            std::optional<float> xpDb;
            if (!cue.routing.xpoint.empty() &&
                s < (int)cue.routing.xpoint.size() &&
                o < (int)cue.routing.xpoint[static_cast<size_t>(s)].size()) {
                xpDb = cue.routing.xpoint[static_cast<size_t>(s)][static_cast<size_t>(o)];
            } else {
                // Default diagonal routing
                xpDb = (s == o && s < outCh) ? std::optional<float>(0.0f) : std::nullopt;
            }
            if (xpDb.has_value())
                xpGains[static_cast<size_t>(s * outCh + o)] = lut::dBToLinear(*xpDb);
        }
    }

    reader.setRouting(std::move(xpGains), std::move(outLevGains), outCh);
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

int64_t CueList::scheduleVoice(int cueIndex) {
    const auto& cue = m_cues[cueIndex];

    // Take the pre-armed stream if available; otherwise do a cold start.
    std::shared_ptr<StreamReader> reader;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        reader = std::move(m_cues[cueIndex].armedStream);
    }

    if (!reader || reader->hasError()) {
        auto segs = buildSegments(cue);
        reader = std::make_shared<StreamReader>(
            cue.path, m_engine.sampleRate(), m_engine.channels(), std::move(segs));
        if (reader->hasError()) return -1;
    }

    const int64_t totalFrames = reader->totalOutputFrames();
    // 0 means unknown length (stream); still valid — isDone() will stop the voice.
    if (totalFrames < 0) return -1;

    const float gain  = levelGain(cue.level, cue.trim);
    const int   outCh = m_engine.channels();
    const int   srcCh = static_cast<int>(cue.audioFile.metadata().channels);
    if (srcCh > 0 && outCh > 0)
        applyRoutingToReader(cue, *reader, srcCh, outCh);

    const int slot = m_engine.scheduleStreamingVoice(
        reader.get(), totalFrames > 0 ? totalFrames : INT64_MAX / 2,
        outCh, cueIndex, gain);
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
                arm(ti, cue.armStartTime > 0.0 ? cue.armStartTime : -1.0);
                result = true;
                // followFrames = 0 → autoFollow fires in the next scheduler poll
            }
            break;
        }

        case CueType::Devamp: {
            const int ti = cue.targetIndex;
            if (ti < 0 || ti >= cueCount()) break;

            // Get the raw StreamReader pointer for the target cue's active voice.
            // Obtained under the mutex; valid as long as the voice stays active.
            StreamReader* raw = nullptr;
            int slot = -1;
            {
                std::lock_guard<std::mutex> lk(m_slotMutex);
                slot = m_lastSlot[ti];
                if (slot >= 0 && m_slotStream[slot])
                    raw = m_slotStream[slot].get();
            }
            if (!raw || !m_engine.isVoiceActive(slot)) break;

            if (cue.devampMode == 0) {
                // Mode 0: advance to next slice (classic devamp)
                raw->devamp(false, cue.devampPreVamp);
            } else {
                // Modes 1/2: call go() at the end of the current slice.
                // Pre-arm the currently-selected cue for a seamless start.
                if (m_selectedIndex >= 0 && m_selectedIndex < cueCount())
                    arm(m_selectedIndex);

                raw->clearDevampFired();
                const bool stopCurrent = (cue.devampMode == 1);
                raw->devamp(stopCurrent, cue.devampPreVamp);

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

            // Capture start values at fire() time
            const auto& tc = m_cues[static_cast<std::size_t>(tIdx)];
            fd->masterLevelStartDb = static_cast<float>(tc.level);
            fd->outLevelStartDb.resize(fd->outLevels.size());
            for (int o = 0; o < (int)fd->outLevels.size(); ++o) {
                float startDb = 0.0f;
                if (o < (int)tc.routing.outLevelDb.size())
                    startDb = tc.routing.outLevelDb[static_cast<size_t>(o)];
                fd->outLevelStartDb[static_cast<size_t>(o)] = startDb;
            }
            // Capture xpoint start values
            {
                const int xs = (int)fd->xpTargets.size();
                fd->xpStartDb.assign(static_cast<size_t>(xs), {});
                for (int s = 0; s < xs; ++s) {
                    const int xo = (int)fd->xpTargets[static_cast<size_t>(s)].size();
                    fd->xpStartDb[static_cast<size_t>(s)].assign(static_cast<size_t>(xo), 0.0f);
                    for (int o = 0; o < xo; ++o) {
                        float startDb = 0.0f;
                        if (s < (int)tc.routing.xpoint.size() &&
                            o < (int)tc.routing.xpoint[static_cast<size_t>(s)].size()) {
                            auto xv = tc.routing.xpoint[static_cast<size_t>(s)][static_cast<size_t>(o)];
                            if (xv.has_value()) startDb = *xv;
                            else startDb = -144.0f;
                        }
                        fd->xpStartDb[static_cast<size_t>(s)][static_cast<size_t>(o)] = startDb;
                    }
                }
            }

            // Compute progress ramp asynchronously (finishes near-instantly).
            if (fd->computeThread.joinable()) fd->computeThread.join();
            fd->rampReady.store(false, std::memory_order_relaxed);
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
                            const float amp  = interpAmp(fd->masterLevelStartDb,
                                                          fd->masterLevel.targetDb);
                            const auto& tc2  = m_cues[static_cast<size_t>(tIdx)];
                            const int   slot = cueVoiceSlot(tIdx);
                            if (slot >= 0 && m_engine.isVoiceActive(slot))
                                m_engine.setVoiceGain(slot, levelGain(lut::linearToDB(amp),
                                                                        tc2.trim));
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
    m_followUps.clear();
    std::lock_guard<std::mutex> lk(m_slotMutex);
    for (auto& cue : m_cues) {
        if (cue.armedStream) {
            cue.armedStream->requestStop();
            cue.armedStream.reset();
        }
    }
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

double CueList::cuePlayheadFileSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;

    StreamReader* raw = nullptr;
    int targetSR = 0;
    int64_t rp = 0;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        const int slot = m_lastSlot[static_cast<size_t>(index)];
        if (slot < 0 || !m_slotStream[static_cast<size_t>(slot)]
                     || !m_engine.isVoiceActive(slot))
            return 0.0;
        raw      = m_slotStream[static_cast<size_t>(slot)].get();
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

double CueList::cueTotalSeconds(int index) const {
    if (index < 0 || index >= cueCount()) return 0.0;
    const auto& cue = m_cues[index];
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
