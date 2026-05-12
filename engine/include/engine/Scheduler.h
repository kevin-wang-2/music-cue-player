#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mcp {

class AudioEngine;

// Engine-playhead-driven event scheduler.
//
// Discrete events: registered with an absolute engine-frame deadline and fired
// on a dedicated thread the first time enginePlayheadFrames() >= targetFrame.
// Stored in a min-heap — O(log N) insert, O(1) peek, O(log N) pop.
//
// Continuous ramps: a single O(1) registration replaces the burst of N discrete
// events used by Fade/Automation cues.  The scheduler thread advances each ramp
// step-by-step every poll cycle; the tick(stepIndex) callback receives pre-
// computed values directly from the caller's table.  Ramps are cancelled via a
// shared_ptr<atomic<bool>> flag — the same pattern as discrete event cancel.
//
// Thread model:
//   Any thread  — schedule / scheduleRamp / cancel / cancelAll (all lock-guarded)
//   Scheduler thread — fires callbacks (do not call schedule() from a callback)
class Scheduler {
public:
    using Callback = std::function<void()>;

    explicit Scheduler(AudioEngine& engine);
    ~Scheduler();

    // Stop the scheduler thread and clear all pending events.
    // Idempotent — safe to call multiple times or before the destructor.
    // Must be called while all objects referenced by scheduled callbacks
    // are still alive (i.e. before any CueList referencing this scheduler
    // is destroyed).
    void shutdown();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Fire cb when enginePlayhead >= targetFrame.  Returns an event ID that
    // can be passed to cancel().
    int schedule(int64_t targetFrame, Callback cb, std::string label = "");

    // Convenience: schedule relative to the current enginePlayhead.
    int scheduleAfterFrames(int64_t frames, Callback cb, std::string label = "");
    int scheduleAfterSeconds(double seconds, Callback cb, std::string label = "");

    // Schedule relative to a caller-supplied origin frame instead of the
    // current playhead.  Use this when multiple events need to share the
    // same origin so their deadlines stay aligned.
    int scheduleFromFrame(int64_t originFrame, double seconds, Callback cb,
                          std::string label = "");

    // Register a continuous ramp.  tick(s) fires when
    //   enginePlayhead >= originFrame + s * stepFrames,  s = 0..numSteps-1.
    // done() fires after the last tick (not called when cancelled).
    // cancel is the shared atomic flag; setting it to true before the next
    // poll silently removes the ramp without calling done().
    // Returns an ID (same namespace as discrete event IDs).
    int scheduleRamp(int64_t originFrame, double stepSec, int numSteps,
                     std::shared_ptr<std::atomic<bool>> cancel,
                     std::function<void(int)> tick,
                     std::function<void()> done,
                     std::string label = "");

    // Silently ignored if the event has already fired or the ID is unknown.
    void cancel(int eventId);
    void cancelAll();

    int     pendingCount() const;
    bool    isPending(int eventId) const;
    int64_t pendingEventTargetFrame(int eventId) const;

    // When enabled, prints scheduling and firing info to stderr.
    void setDebugLog(bool enable) { m_debugLog.store(enable); }

private:
    void threadLoop();

    AudioEngine& m_engine;

    struct Event {
        int         id;
        int64_t     targetFrame;
        Callback    cb;
        std::string label;
        bool        cancelled{false};
    };

    // Min-heap comparator: smallest targetFrame at top.
    struct EventCmp {
        bool operator()(const Event& a, const Event& b) const {
            return a.targetFrame > b.targetFrame;
        }
    };

    struct Ramp {
        int     id;
        int64_t originFrame;
        int64_t stepFrames;
        int     numSteps;
        int     nextStep{0};
        std::shared_ptr<std::atomic<bool>> cancel;
        std::function<void(int)> tick;
        std::function<void()>    done;
    };

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::vector<Event>      m_events;   // maintained as a min-heap
    std::vector<Ramp>       m_ramps;
    int                     m_nextId{0};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_debugLog{false};
    std::thread       m_thread;
};

} // namespace mcp
