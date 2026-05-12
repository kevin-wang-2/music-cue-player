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

void Scheduler::shutdown() {
    m_running.store(false, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mutex);
    m_events.clear();
    m_ramps.clear();
}

Scheduler::~Scheduler() { shutdown(); }

int Scheduler::schedule(int64_t targetFrame, Callback cb, std::string label) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const int id = m_nextId++;
    if (m_debugLog.load(std::memory_order_relaxed)) {
        const int64_t now = m_engine.enginePlayheadFrames();
        std::cerr << "[sched] schedule id=" << id
                  << " target=" << targetFrame
                  << " now=" << now
                  << " delta=" << (targetFrame - now) << " frames";
        if (!label.empty()) std::cerr << " [" << label << "]";
        std::cerr << "\n";
    }
    m_events.push_back({id, targetFrame, std::move(cb), std::move(label), false});
    std::push_heap(m_events.begin(), m_events.end(), EventCmp{});
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

int Scheduler::scheduleRamp(int64_t originFrame, double stepSec, int numSteps,
                             std::shared_ptr<std::atomic<bool>> cancel,
                             std::function<void(int)> tick,
                             std::function<void()> done,
                             std::string /*label*/) {
    const int sr = m_engine.sampleRate();
    const int64_t stepFrames = (sr > 0)
        ? std::max(int64_t{1}, static_cast<int64_t>(stepSec * sr))
        : 1;
    std::lock_guard<std::mutex> lk(m_mutex);
    const int id = m_nextId++;
    m_ramps.push_back({id, originFrame, stepFrames, numSteps, 0,
                       std::move(cancel), std::move(tick), std::move(done)});
    m_cv.notify_one();
    return id;
}

void Scheduler::cancel(int eventId) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& e : m_events)
        if (e.id == eventId) { e.cancelled = true; return; }
    for (auto& r : m_ramps)
        if (r.id == eventId && r.cancel)
            { r.cancel->store(true, std::memory_order_relaxed); return; }
}

void Scheduler::cancelAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_events.clear();
    m_ramps.clear();
}

int Scheduler::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_events.size() + m_ramps.size());
}

bool Scheduler::isPending(int eventId) const {
    if (eventId < 0) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& e : m_events)
        if (e.id == eventId && !e.cancelled) return true;
    for (const auto& r : m_ramps)
        if (r.id == eventId) return true;
    return false;
}

int64_t Scheduler::pendingEventTargetFrame(int eventId) const {
    if (eventId < 0) return -1;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& e : m_events)
        if (e.id == eventId) return e.targetFrame;
    return -1;
}

void Scheduler::threadLoop() {
    while (m_running.load(std::memory_order_relaxed)) {
        std::vector<Event> ready;

        struct ReadyRamp {
            std::function<void(int)> tick;
            std::vector<int>         steps;
            std::function<void()>    done;  // non-null → ramp completed
        };
        std::vector<ReadyRamp> readyRamps;

        {
            std::unique_lock<std::mutex> lk(m_mutex);

            if (m_events.empty() && m_ramps.empty()) {
                m_cv.wait(lk, [this] {
                    return !m_running.load(std::memory_order_relaxed)
                        || !m_events.empty() || !m_ramps.empty();
                });
            } else {
                m_cv.wait_for(lk, kPollInterval);
            }

            if (!m_running.load(std::memory_order_relaxed)) break;

            const int64_t now = m_engine.enginePlayheadFrames();

            // Discrete events: pop all due from the min-heap.
            while (!m_events.empty() && m_events.front().targetFrame <= now) {
                std::pop_heap(m_events.begin(), m_events.end(), EventCmp{});
                Event e = std::move(m_events.back());
                m_events.pop_back();
                if (!e.cancelled)
                    ready.push_back(std::move(e));
            }

            // Ramps: advance all due steps, collect for firing outside the lock.
            for (auto it = m_ramps.begin(); it != m_ramps.end(); ) {
                Ramp& r = *it;
                if (r.cancel && r.cancel->load(std::memory_order_relaxed)) {
                    it = m_ramps.erase(it);
                    continue;
                }

                ReadyRamp rr;
                while (r.nextStep < r.numSteps) {
                    const int64_t fire = r.originFrame
                        + static_cast<int64_t>(r.nextStep) * r.stepFrames;
                    if (fire > now) break;
                    rr.steps.push_back(r.nextStep);
                    ++r.nextStep;
                }

                const bool isDone = (r.nextStep >= r.numSteps);
                if (!rr.steps.empty()) rr.tick = r.tick;  // copy once per ramp, not per step
                if (isDone)           rr.done = std::move(r.done);

                if (isDone) it = m_ramps.erase(it);
                else        ++it;

                if (!rr.steps.empty() || rr.done)
                    readyRamps.push_back(std::move(rr));
            }
        }

        // Fire everything outside the lock.
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
        for (auto& rr : readyRamps) {
            for (int s : rr.steps) rr.tick(s);
            if (rr.done) rr.done();
        }
    }
}

} // namespace mcp
