#include "engine/AudioEngine.h"
#include "engine/Scheduler.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

// All scheduler tests require audio hardware so the engine playhead advances.

TEST_CASE("Scheduler: fires callback after specified frames", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    std::atomic<bool> fired{false};
    const int64_t delay = static_cast<int64_t>(0.1 * engine.sampleRate()); // 100 ms
    scheduler.scheduleAfterFrames(delay, [&fired]() { fired.store(true); });

    // Should not fire immediately
    CHECK_FALSE(fired.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(fired.load());
}

TEST_CASE("Scheduler: does not fire before deadline", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    std::atomic<bool> fired{false};
    scheduler.scheduleAfterSeconds(0.3, [&fired]() { fired.store(true); });

    // Wait less than the deadline — should still be pending
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(fired.load());

    // Wait past the deadline
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(fired.load());
}

TEST_CASE("Scheduler: cancel prevents callback", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    std::atomic<bool> fired{false};
    const int id = scheduler.scheduleAfterSeconds(2.0, [&fired]() { fired.store(true); });
    CHECK(scheduler.pendingCount() == 1);

    scheduler.cancel(id);
    CHECK(scheduler.pendingCount() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(fired.load());
}

TEST_CASE("Scheduler: cancelAll stops all pending events", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    std::atomic<int> count{0};
    scheduler.scheduleAfterSeconds(2.0, [&count]() { ++count; });
    scheduler.scheduleAfterSeconds(3.0, [&count]() { ++count; });
    scheduler.scheduleAfterSeconds(4.0, [&count]() { ++count; });
    CHECK(scheduler.pendingCount() == 3);

    scheduler.cancelAll();
    CHECK(scheduler.pendingCount() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(count.load() == 0);
}

TEST_CASE("Scheduler: multiple events fire in deadline order", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    std::vector<int> order;
    std::mutex orderMutex;

    scheduler.scheduleAfterSeconds(0.2, [&]() { std::lock_guard lk(orderMutex); order.push_back(2); });
    scheduler.scheduleAfterSeconds(0.1, [&]() { std::lock_guard lk(orderMutex); order.push_back(1); });
    scheduler.scheduleAfterSeconds(0.3, [&]() { std::lock_guard lk(orderMutex); order.push_back(3); });

    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    std::lock_guard lk(orderMutex);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
}

TEST_CASE("Scheduler: timing accuracy within 2 buffer periods", "[scheduler][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler scheduler(engine);

    // Target: 200 ms = 9600 frames from now
    const double targetSeconds = 0.2;
    const int64_t targetFrames = static_cast<int64_t>(targetSeconds * engine.sampleRate());

    std::atomic<int64_t> firedAtFrame{-1};
    const int64_t originFrame = engine.enginePlayheadFrames();
    scheduler.scheduleAfterFrames(targetFrames,
        [&engine, &firedAtFrame]() {
            firedAtFrame.store(engine.enginePlayheadFrames());
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    const int64_t fired = firedAtFrame.load();
    REQUIRE(fired >= 0);

    const int64_t expectedFrame = originFrame + targetFrames;
    const int64_t error         = std::abs(fired - expectedFrame);

    // Scheduler fires within 2 ms (poll interval); callback records playhead
    // which advances in buffer steps (~256–1024 frames at 48 kHz).
    // 2048 frames ≈ 43 ms — well within acceptable for theatrical audio.
    CHECK(error < 2048);
}
