#pragma once

#include "AudioEngine.h"
#include "Cue.h"
#include "Scheduler.h"
#include <mutex>
#include <string>
#include <vector>

namespace mcp {

// High-level cue sequencer backed by a shared AudioEngine and Scheduler.
//
// Cue types
// ---------
//   Audio  — plays a pre-loaded audio file as a new voice
//   Start  — triggers the audio of another cue (by index) in this list
//   Stop   — stops all voices currently tagged with a target cue index
//
// Playback controls
// -----------------
//   go()         — fire selected cue (respecting its preWait), advance selectedIndex
//   start(idx)   — fire cue[idx] directly, selectedIndex unchanged
//   stop(idx)    — stop all voices tagged with idx, no prewait
//   panic()      — cancel pending scheduled fires + stop every active voice
//
// Multiple voices may play simultaneously; the engine mixes them.
//
// Thread model
// ------------
//   Main thread      — all mutating calls
//   Scheduler thread — fires delayed fire() calls (via prewait lambdas)
//   Any thread       — const queries
class CueList {
public:
    CueList(AudioEngine& engine, Scheduler& scheduler);
    ~CueList() = default;
    CueList(const CueList&) = delete;
    CueList& operator=(const CueList&) = delete;

    // --- Cue list construction ----------------------------------------------

    // Append an Audio cue. Fails if the file is missing or its format doesn't
    // match the engine (sample rate / channel count).
    bool addCue(const std::string& path, const std::string& name = "",
                double preWait = 0.0);

    // Append a Start cue that, when fired, plays the audio of cue[targetIndex].
    bool addStartCue(int targetIndex, const std::string& name = "",
                     double preWait = 0.0);

    // Append a Stop cue that, when fired, stops all voices of cue[targetIndex].
    bool addStopCue(int targetIndex, const std::string& name = "",
                    double preWait = 0.0);

    void clear();
    int  cueCount() const;

    // The cue that will fire on the next go(). May equal cueCount() (past-end).
    int  selectedIndex() const;
    void setSelectedIndex(int index);
    void selectNext();
    void selectPrev();

    const Cue* cueAt(int index) const;
    const Cue* selectedCue() const;  // nullptr when past-end

    // Per-cue parameter setters (safe to call before or after addCue).
    void setCuePreWait    (int index, double seconds);
    void setCueStartTime  (int index, double seconds);   // audio cues only
    void setCueDuration   (int index, double seconds);   // audio cues only; 0=to end
    void setCueAutoContinue(int index, bool enable);
    void setCueAutoFollow  (int index, bool enable);
    void setCueName       (int index, const std::string& name);
    void setCueCueNumber  (int index, const std::string& number);

    // --- Playback controls --------------------------------------------------

    // originFrame: the engine frame to use as the prewait origin.
    // Pass engine.enginePlayheadFrames() captured once before a batch of go/start
    // calls so that co-temporal cues share identical prewait deadlines.
    // Use -1 (default) to snapshot the playhead inside the call.
    bool go(int64_t originFrame = -1);
    bool start(int index, int64_t originFrame = -1);
    void stop(int index);    // stop all voices tagged with index, immediate
    void panic();            // cancel pending fires + stop all voices

    // --- Queries (safe from any thread) -------------------------------------

    bool isAnyCuePlaying() const;
    bool isCuePlaying(int index) const;
    bool isCuePending(int index) const;   // prewait scheduled but not yet fired
    int  activeCueCount() const;

    // Most-recently activated voice slot for cue[index], or -1 if none.
    int cueVoiceSlot(int index) const;

    // Playhead of the most recently started voice for cue[index].
    // Returns 0 if that cue has no active voice.
    int64_t cuePlayheadFrames(int index) const;
    double  cuePlayheadSeconds(int index) const;

    // Total playable duration of cue[index] in seconds (respects startTime/duration).
    // Returns 0 for non-audio cues or unloaded files.
    double cueTotalSeconds(int index) const;

private:
    // Execute the cue's action immediately (called after prewait, or directly).
    // For Audio:  schedules the voice.
    // For Start:  schedules the audio of the target cue.
    // For Stop:   clears voices tagged with targetIndex.
    bool fire(int cueIndex);

    // Shared by fire() (Audio path) — may be called from the scheduler thread.
    bool scheduleVoice(int cueIndex);

    AudioEngine& m_engine;
    Scheduler&   m_scheduler;
    std::vector<Cue> m_cues;
    int m_selectedIndex{0};

    // Protected by m_slotMutex: written by scheduleVoice() / go() (possibly
    // from the scheduler thread), read from any thread.
    mutable std::mutex m_slotMutex;
    std::vector<int>   m_lastSlot;
    std::vector<int>   m_pendingEventId;  // scheduler event ID, -1 = none
};

} // namespace mcp
