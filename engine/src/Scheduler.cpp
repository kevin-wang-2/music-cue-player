#include "engine/Scheduler.h"
#include "engine/AudioEngine.h"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace mcp {

static constexpr auto kPollInterval = std::chrono::milliseconds(2);

Scheduler::Scheduler(AudioEngine& engine) : m_engine(engine) {
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&Scheduler::threadLoop, this);
}

Scheduler::~Scheduler() {
    m_running.store(false, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

int Scheduler::schedule(int64_t targetFrame, Callback cb, std::string label) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const int id = m_nextId++;
    if (m_debugLog.load(std::memory_order_relaxed)) {
        // Called from main thread — I/O is safe here.
        const int64_t now = m_engine.enginePlayheadFrames();
        std::cerr << "[sched] schedule id=" << id
                  << " target=" << targetFrame
                  << " now=" << now
                  << " delta=" << (targetFrame - now) << " frames";
        if (!label.empty()) std::cerr << " [" << label << "]";
        std::cerr << "\n";
    }
    m_events.push_back({id, targetFrame, std::move(cb), std::move(label)});
    m_cv.notify_one();
    return id;
}

int Scheduler::scheduleAfterFrames(int64_t frames, Callback cb, std::string label) {
    return schedule(m_engine.enginePlayheadFrames() + frames, std::move(cb),
                    std::move(label));
}

int Scheduler::scheduleAfterSeconds(double seconds, Callback cb, std::string label) {
    const int sr = m_engine.sampleRate();
    const int64_t frames = (sr > 0) ? static_cast<int64_t>(seconds * sr) : 0;
    return scheduleAfterFrames(frames, std::move(cb), std::move(label));
}

int Scheduler::scheduleFromFrame(int64_t originFrame, double seconds, Callback cb,
                                  std::string label) {
    const int sr = m_engine.sampleRate();
    const int64_t frames = (sr > 0) ? static_cast<int64_t>(seconds * sr) : 0;
    return schedule(originFrame + frames, std::move(cb), std::move(label));
}

void Scheduler::cancel(int eventId) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_events.erase(
        std::remove_if(m_events.begin(), m_events.end(),
                       [eventId](const Event& e) { return e.id == eventId; }),
        m_events.end());
}

void Scheduler::cancelAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_events.clear();
}

int Scheduler::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_events.size());
}

bool Scheduler::isPending(int eventId) const {
    if (eventId < 0) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& e : m_events)
        if (e.id == eventId) return true;
    return false;
}

void Scheduler::threadLoop() {
    // This is the scheduler thread — I/O (stderr logging) is safe here.
    // The audio callback never enters this function.
    while (m_running.load(std::memory_order_relaxed)) {
        std::vector<Event> ready;

        {
            std::unique_lock<std::mutex> lk(m_mutex);

            if (m_events.empty()) {
                m_cv.wait(lk, [this] {
                    return !m_running.load(std::memory_order_relaxed) || !m_events.empty();
                });
            } else {
                m_cv.wait_for(lk, kPollInterval);
            }

            if (!m_running.load(std::memory_order_relaxed)) break;

            const int64_t now = m_engine.enginePlayheadFrames();
            for (auto it = m_events.begin(); it != m_events.end(); ) {
                if (it->targetFrame <= now) {
                    ready.push_back(std::move(*it));
                    it = m_events.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Log and fire outside the lock.
        const bool dbg = m_debugLog.load(std::memory_order_relaxed);
        if (dbg && !ready.empty()) {
            const int64_t now = m_engine.enginePlayheadFrames();
            for (const auto& e : ready) {
                std::cerr << "[sched] fire   id=" << e.id
                          << " target=" << e.targetFrame
                          << " actual=" << now
                          << " error=" << (now - e.targetFrame) << " frames";
                if (!e.label.empty()) std::cerr << " [" << e.label << "]";
                std::cerr << "\n";
            }
        }
        for (auto& e : ready) e.cb();
    }
}

} // namespace mcp
