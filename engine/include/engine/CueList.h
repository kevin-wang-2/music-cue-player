#pragma once

#include "AudioEngine.h"
#include "Cue.h"
#include "FadeData.h"
#include "Scheduler.h"
#include "StreamReader.h"
#include <array>
#include <memory>
#include <mutex>
#include <optional>
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
    ~CueList();
    CueList(const CueList&) = delete;
    CueList& operator=(const CueList&) = delete;

    // --- Cue list construction ----------------------------------------------

    // Append an Audio cue. Fails if the file is missing or its format doesn't
    // match the engine (sample rate / channel count).
    bool addCue(const std::string& path, const std::string& name = "",
                double preWait = 0.0);

    // Append a broken/placeholder Audio cue (path may be empty or invalid).
    // Always succeeds — the cue will not play until a valid path is set.
    // Used so ShowFile indices stay in sync with CueList indices.
    bool addBrokenAudioCue(const std::string& path, const std::string& name = "",
                           double preWait = 0.0);

    // Append a Start cue that, when fired, plays the audio of cue[targetIndex].
    bool addStartCue(int targetIndex, const std::string& name = "",
                     double preWait = 0.0);

    // Append a Stop cue that, when fired, stops all voices of cue[targetIndex].
    bool addStopCue(int targetIndex, const std::string& name = "",
                    double preWait = 0.0);

    // Append a Fade cue.  Duration is set separately via setCueDuration.
    // Use setCueFadeMasterTarget / setCueFadeOutTarget to configure fade targets.
    bool addFadeCue(int resolvedTargetIdx, const std::string& targetCueNumber,
                    FadeData::Curve curve,
                    bool stopWhenDone = false,
                    const std::string& name = "", double preWait = 0.0);

    // Append an Arm cue that, when fired, pre-buffers the audio of cue[targetIndex]
    // so the next go()/start() on that cue fires with no I/O latency.
    // armStartTime: pre-load from this offset (seconds); 0 = from start of file.
    bool addArmCue(int targetIndex, const std::string& name = "", double preWait = 0.0);

    // Append a Devamp cue.  When fired, waits for cue[targetIndex] to finish its
    // current slice loop iteration and then:
    //   devampMode 0: advance to next slice (classic devamp / "exit vamp")
    //   devampMode 1: stop cue[targetIndex], then start cue[nextCueIdx]
    //   devampMode 2: advance cue[targetIndex] to next slice AND start cue[nextCueIdx]
    // nextCueIdx == -1 means the cue immediately after targetIndex in the list.
    bool addDevampCue(int targetIndex, const std::string& name = "", double preWait = 0.0,
                      int devampMode = 0);

    // Append a Group cue header.  The caller must immediately append all child
    // cues (and nested sub-group cues) after this call.  Once the entire flat
    // list has been built, call setCueChildCount / setCueParentIndex / setCueTimelineOffset
    // to wire up the parent-child relationships.
    bool addGroupCue(GroupData::Mode mode, bool random = false,
                     const std::string& name = "", double preWait = 0.0);

    // Append a standalone MusicContext cue (no audio).
    // isCuePlaying() returns m_cueFireFrame[idx] >= 0 for MC cues.
    bool addMCCue(const std::string& name = "", double preWait = 0.0);

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
    void setCueGoQuantize (int index, int mode);  // 0=none 1=bar 2=beat
    void setCueStartTime  (int index, double seconds);   // audio cues only
    void setCueDuration   (int index, double seconds);   // audio cues only; 0=to end
    void setCueLevel      (int index, double dB);        // audio cues only; 0=unity
    void setCueTrim       (int index, double dB);        // audio cues only; 0=no trim
    void setCueAutoContinue(int index, bool enable);
    void setCueAutoFollow  (int index, bool enable);
    void setCueName       (int index, const std::string& name);
    void setCueArmStartTime(int index, double seconds);  // Arm cues only
    void setCueCueNumber  (int index, const std::string& number);
    // Set the target cue index for Start/Stop/Fade/Arm/Devamp cues.
    // Also updates the stored targetCueNumber to match.
    void setCueTarget     (int index, int targetIndex);

    // Devamp cue setters (no-op if cue[index] is not a Devamp cue).
    void setCueDevampMode   (int index, int mode);      // 0/1/2 — see addDevampCue
    void setCueDevampPreVamp(int index, bool enabled);  // skip following looping slices

    // Group structure setters — must be called once the full flat list is built.
    void setCueParentIndex   (int index, int parentIndex); // flat index of parent group (-1 = top-level)
    void setCueChildCount    (int index, int childCount);  // total descendant count (not just direct children)
    void setCueTimelineOffset(int index, double seconds);    // offset within parent Timeline group
    void setCueTimelineArmSec(int index, double seconds);   // arm start position for Timeline groups
    void setCueGroupMode     (int index, GroupData::Mode mode);
    void setCueGroupRandom   (int index, bool random);

    // Audio cue routing setters.
    // outCh / srcCh: 0-based channel indices.
    void setCueOutLevel(int index, int outCh, float dB);   // per-output level; 0=unity
    void setCueXpoint  (int index, int srcCh, int outCh,
                        std::optional<float> dB);          // nullopt = disable cell
    // Initialize xpoint to default (diagonal) if it is empty.
    // srcCh = file channels, outCh = engine output channels.
    void initCueRouting(int index, int srcCh, int outCh);

    // Fade cue setters (no-op if cue[index] is not a Fade cue).
    void setCueFadeMasterTarget (int index, bool enabled, float targetDb);
    void setCueFadeOutTarget    (int index, int outCh, bool enabled, float targetDb);
    void setCueFadeOutTargetCount(int index, int count);   // resize outLevels vector
    void setCueFadeXpTarget     (int index, int srcCh, int outCh, bool enabled, float targetDb);
    void setCueFadeXpSize       (int index, int srcCh, int outCh);  // resize xpTargets matrix
    void setCueFadeCurve        (int index, FadeData::Curve curve);
    void setCueFadeStopWhenDone (int index, bool v);

    // Music Context — attach / detach / mutate.
    // Pass nullptr to remove any existing MC.
    void setCueMusicContext(int index, std::unique_ptr<MusicContext> mc);

    // Direct access to a mutable MC (returns nullptr if the cue has no MC).
    // The caller is responsible for calling markMCDirty() after bulk mutations.
    MusicContext* musicContextOf(int index);

    // Mark the MC as dirty so it recompiles on next query.
    // Call after any direct mutation via musicContextOf().
    void markMCDirty(int index);

    // Time-marker / slice-loop setters (audio cues only).
    // Markers must be kept sorted by time; sliceLoops size = markers.size()+1.
    void setCueMarkers   (int index, const std::vector<Cue::TimeMarker>& markers);
    void setCueSliceLoops(int index, const std::vector<int>& loops);
    void setCueMarkerTime(int index, int markerIdx, double time);
    void setCueMarkerName(int index, int markerIdx, const std::string& name);
    void addCueMarker    (int index, double time, const std::string& name = "");
    void removeCueMarker (int index, int markerIdx);

    // --- ARM ----------------------------------------------------------------

    // Pre-buffer audio for cue[index] so that the next go()/start() fires
    // with no I/O latency.  Idempotent — calling arm() twice just replaces
    // the previous buffer.  No-op for non-audio cues.
    // startOverride >= 0: load from that offset; otherwise uses cue.startTime.
    bool arm(int index, double startOverride = -1.0);

    // Release the pre-buffered data for cue[index] without playing it.
    void disarm(int index);

    // True when cue[index] has been armed and its buffer is full enough to
    // guarantee glitch-free playback.
    bool isArmed(int index) const;

    // --- Playback controls --------------------------------------------------

    // originFrame: the engine frame to use as the prewait origin.
    // Pass engine.enginePlayheadFrames() captured once before a batch of go/start
    // calls so that co-temporal cues share identical prewait deadlines.
    // Use -1 (default) to snapshot the playhead inside the call.
    bool go(int64_t originFrame = -1);
    bool start(int index, int64_t originFrame = -1);
    void stop(int index);    // stop all voices tagged with index, immediate
    void panic();            // cancel pending fires + stop all voices

    // Fade all active voices to silence over durationSeconds, then stop them.
    // Like panic(), also cancels all pending prewait events.
    void softPanic(double durationSeconds = 0.5);

    // Reclaim StreamReader resources from voices that have finished playing.
    // Call once per frame from the main thread (e.g., in the render loop).
    void update();

    // --- Queries (safe from any thread) -------------------------------------

    bool isAnyCuePlaying() const;
    bool isCuePlaying(int index) const;
    bool isCuePending(int index) const;   // prewait scheduled but not yet fired
    bool isFadeActive(int index) const;   // fade steps still running
    int  activeCueCount() const;

    // Most-recently activated voice slot for cue[index], or -1 if none.
    int cueVoiceSlot(int index) const;

    // Playhead of the most recently started voice for cue[index].
    // Returns 0 if that cue has no active voice.
    int64_t cuePlayheadFrames(int index) const;
    double  cuePlayheadSeconds(int index) const;

    // Elapsed cue-local time for any cue type:
    //   Audio  — same as cuePlayheadSeconds (engine elapsed since voice start)
    //   Group  — engine elapsed since fire() + arm-base offset
    // Returns 0 if the cue has not been fired or has no MC-relevant state.
    double  cueElapsedSeconds(int index) const;

    // File-position playhead for cue[index] in seconds from start of file.
    // Uses the voice's ring-buffer readPos and segment markers recorded by the
    // IO thread, so it stays accurate even after devamp cuts a loop short.
    // Returns 0 if the cue has no active voice.  Call from the main thread only.
    double  cuePlayheadFileSeconds(int index) const;

    // Total playable duration of cue[index] in seconds (respects startTime/duration).
    // Returns 0 for non-audio cues or unloaded files.
    double cueTotalSeconds(int index) const;

    // SyncGroup queries (safe from main thread).
    double syncGroupBaseDuration(int gi) const;  // max endpoint of direct children
    double syncGroupTotalSeconds(int gi) const;  // base × slice loops (inf if any ∞ slice)
    bool   isSyncGroupBroken   (int gi) const;   // any audio child has infinite loops

private:
    bool fire(int cueIndex);
    int64_t scheduleVoice(int cueIndex);

    // Extra delay (seconds) to align the cue's fire time to the next musical
    // boundary in the currently-playing global MC.  Returns 0 if no MC is active
    // or cue.goQuantize == 0.
    double quantizeDelay(int cueIdx, int64_t originFrame) const;

    // Returns the flat index that should become selectedIndex after cue[idx] is fired:
    //   Group (any mode): idx + childCount + 1  (jump over all descendants)
    //   Last child of a Group: logicalNext(parentIndex)  (exit the group recursively)
    //   Otherwise: idx + 1
    int logicalNext(int idx) const;

    // Fire all children of a Timeline or Playlist group cue at the correct times.
    // baseOffset is accumulated from ancestor Timeline groups.
    void fireGroup(int groupIdx, double baseOffset, int64_t originFrame);

    // Sync group helpers.
    void fireSyncGroup      (int gi, double armPos, int64_t originFrame);
    void devampSyncGroup    (int gi, int devampMode, bool preVamp);
    void stopSyncGroupChildren(int gi);

    // Pre-arm all audio descendants of a Timeline/Sync group so their StreamReaders
    // have buffered data before the first audio callback.
    // baseOffset: seconds into the group timeline we are starting from.
    void armGroupDescendants(int groupIdx, double baseOffset);

    // Build linear routing gains from cue.routing and set them on `reader`.
    // srcCh = reader's native file channel count; outCh = engine.channels().
    void applyRoutingToReader(const Cue& cue, StreamReader& reader,
                               int srcCh, int outCh);

    // Update a live voice's per-output-channel gain (called from fade callbacks).
    // Does NOT modify cue.routing — fade is a live-voice-only multiplier.
    void setCueOutLevelGain(int cueIdx, int outCh, float linGain);

    // Update a live voice's single crosspoint cell gain (called from fade callbacks).
    // Does NOT modify cue.routing.xpoint.
    void setCueXpointGain(int cueIdx, int srcCh, int outCh, float linGain);

    AudioEngine& m_engine;
    Scheduler&   m_scheduler;
    std::vector<Cue> m_cues;
    int m_selectedIndex{0};

    // Pending devamp follow-up actions: call go() when the trigger fires.
    // waitForStop=true → wait for voice to go inactive (mode 1);
    // waitForStop=false → wait for StreamReader devampFired signal (mode 2).
    // Accessed only from the main thread — no lock needed.
    struct FollowUp { int watchCueIdx; bool waitForStop; };
    std::vector<FollowUp> m_followUps;

    // Pending playlist sequencing: when watchCueIdx finishes, fire nextCueIdx.
    // activated is set to true once watchCueIdx starts playing, so that we don't
    // fire nextCueIdx before watchCueIdx has even started.
    struct PlaylistFollowUp { int watchCueIdx; int nextCueIdx; bool activated{false}; };
    std::vector<PlaylistFollowUp> m_playlistFollowUps;

    // Protected by m_slotMutex: written by scheduleVoice() / go() (possibly
    // from the scheduler thread), read from any thread.
    mutable std::mutex m_slotMutex;
    std::vector<int>   m_lastSlot;
    std::vector<int>   m_pendingEventId;  // scheduler event ID, -1 = none

    // Engine frame recorded when a Group cue is fired, plus the arm-base offset
    // that was active at fire time.  Used by cueElapsedSeconds() for group MCs.
    // Written on the main thread (fire() is always called from main).
    std::vector<int64_t> m_cueFireFrame;   // -1 = not yet fired
    std::vector<double>  m_cueFireArmBase; // seconds of timeline arm position at fire

    // One StreamReader per engine voice slot — keeps the reader alive until
    // the voice finishes.  Cleared by update() once isVoiceActive() is false.
    std::array<std::shared_ptr<StreamReader>, AudioEngine::kMaxVoices> m_slotStream;
};

} // namespace mcp
