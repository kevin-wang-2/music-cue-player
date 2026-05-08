#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mcp {

class AudioEngine;

// Engine-playhead-driven event scheduler.
//
// Events are registered with an absolute engine-frame deadline and fired on a
// dedicated thread the first time enginePlayheadFrames() >= targetFrame.
//
// Timing accuracy: within the poll interval (2 ms) plus one audio buffer
// period — far tighter than OS-timer approaches, and immune to wall-clock
// drift over long shows because all deadlines are expressed in audio frames.
//
// Thread model:
//   Any thread  — schedule / cancel / cancelAll (all lock-guarded)
//   Scheduler thread — fires callbacks (do not call schedule() from a callback)
class Scheduler {
public:
    using Callback = std::function<void()>;

    explicit Scheduler(AudioEngine& engine);
    ~Scheduler();

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
    // same origin so their deadlines stay aligned (e.g. co-temporal prewait
    // events that must fire in the same poll cycle).
    int scheduleFromFrame(int64_t originFrame, double seconds, Callback cb,
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
    };

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::vector<Event>      m_events;
    int                     m_nextId{0};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_debugLog{false};
    std::thread       m_thread;
};

} // namespace mcp
