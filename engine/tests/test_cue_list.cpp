#include "TestHelpers.h"
#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <sndfile.h>
#include <thread>
#include <vector>

static std::vector<std::string> makeWavs(int count, int sampleRate = 48000,
                                          int channels = 2, double dur = 1.0) {
    std::vector<std::string> paths;
    for (int i = 0; i < count; ++i) {
        auto p = (std::filesystem::temp_directory_path()
                  / ("mcp_cue_" + std::to_string(i) + ".wav")).string();
        SF_INFO info{};
        info.samplerate = sampleRate; info.channels = channels;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
        SNDFILE* f = sf_open(p.c_str(), SFM_WRITE, &info); REQUIRE(f != nullptr);
        const int64_t frames = static_cast<int64_t>(sampleRate * dur);
        std::vector<float> buf(static_cast<size_t>(frames * channels));
        constexpr float kAmp = 0.05f;
        for (int64_t j = 0; j < frames; ++j) {
            float s = kAmp * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * j / sampleRate));
            for (int ch = 0; ch < channels; ++ch)
                buf[static_cast<size_t>(j * channels + ch)] = s;
        }
        sf_writef_float(f, buf.data(), frames); sf_close(f);
        paths.push_back(p);
    }
    return paths;
}

static void removeAll(const std::vector<std::string>& paths) {
    for (const auto& p : paths) std::filesystem::remove(p);
}

// ---- Structural (no audio hardware) ----------------------------------------

TEST_CASE("CueList: addCue and navigation", "[cue_list]") {
    mcp::AudioEngine engine;
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3);

    REQUIRE(cues.addCue(paths[0], "A"));
    REQUIRE(cues.addCue(paths[1], "B"));
    REQUIRE(cues.addCue(paths[2], "C"));
    CHECK(cues.cueCount()     == 3);
    CHECK(cues.selectedIndex() == 0);
    CHECK(cues.selectedCue()->name == "A");

    cues.selectNext(); CHECK(cues.selectedIndex() == 1);
    cues.selectNext(); CHECK(cues.selectedIndex() == 2);
    cues.selectNext(); CHECK(cues.selectedIndex() == 3);   // past-end
    CHECK(cues.selectedCue() == nullptr);
    cues.selectPrev(); CHECK(cues.selectedIndex() == 2);
    cues.setSelectedIndex(0); CHECK(cues.selectedIndex() == 0);

    removeAll(paths);
}

TEST_CASE("CueList: addCue fails on missing file", "[cue_list]") {
    mcp::AudioEngine engine;
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    CHECK_FALSE(cues.addCue("/nonexistent/nope.wav"));
    CHECK(cues.cueCount() == 0);
}

TEST_CASE("CueList: clear resets everything", "[cue_list]") {
    mcp::AudioEngine engine;
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(2);
    REQUIRE(cues.addCue(paths[0])); REQUIRE(cues.addCue(paths[1]));
    cues.clear();
    CHECK(cues.cueCount()     == 0);
    CHECK(cues.selectedIndex() == 0);
    removeAll(paths);
}

// ---- Playback (require audio hardware) -------------------------------------

TEST_CASE("CueList: go plays and advances selection", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 1.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(cues.selectedIndex()  == 1);
    CHECK(cues.isCuePlaying(0));

    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(cues.selectedIndex() == 2);
    CHECK(cues.isCuePlaying(1));

    // Both cue 0 and cue 1 playing simultaneously
    CHECK(cues.isAnyCuePlaying());
    CHECK(engine.activeVoiceCount() >= 2);

    removeAll(paths);
}

TEST_CASE("CueList: start plays without advancing selection", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 1.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    REQUIRE(cues.start(2));  // play cue 2
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK(cues.selectedIndex() == 0);    // unchanged
    CHECK(cues.isCuePlaying(2));
    CHECK_FALSE(cues.isCuePlaying(0));

    removeAll(paths);
}

TEST_CASE("CueList: stop targets only the specified cue", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 2.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    REQUIRE(cues.start(0));
    REQUIRE(cues.start(1));
    REQUIRE(cues.start(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(engine.activeVoiceCount() == 3);

    cues.stop(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(cues.isCuePlaying(0));
    CHECK_FALSE(cues.isCuePlaying(1));
    CHECK(cues.isCuePlaying(2));
    CHECK(engine.activeVoiceCount() == 2);

    removeAll(paths);
}

TEST_CASE("CueList: panic stops all voices", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 2.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    REQUIRE(cues.start(0)); REQUIRE(cues.start(1)); REQUIRE(cues.start(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(engine.activeVoiceCount() == 3);

    const int sel = cues.selectedIndex();
    cues.panic();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK(engine.activeVoiceCount() == 0);
    CHECK(cues.selectedIndex() == sel);   // selection unchanged

    removeAll(paths);
}

TEST_CASE("CueList: same cue can play multiple times simultaneously", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));

    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK(engine.activeVoiceCount() == 2);

    removeAll(paths);
}

TEST_CASE("CueList: cuePlayheadSeconds advances during playback", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));

    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const double t0 = cues.cuePlayheadSeconds(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double t1 = cues.cuePlayheadSeconds(0);

    CHECK(t1 > t0);

    removeAll(paths);
}

TEST_CASE("CueList: go past end returns false", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 0.5);
    REQUIRE(cues.addCue(paths[0]));

    REQUIRE(cues.go());          // plays cue 0, selection → 1 (past-end)
    CHECK_FALSE(cues.go());      // past-end, nothing to play

    removeAll(paths);
}

// ---- Pre-wait / Scheduler --------------------------------------------------

TEST_CASE("CueList: pre-wait delays voice start", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));

    cues.setCuePreWait(0, 0.2);   // 200 ms = 9600 frames

    // Capture engine frame at the moment scheduleAfterSeconds() will read it.
    // go() calls scheduleAfterSeconds() immediately, so beforeGo ≈ target origin.
    const int64_t beforeGo = engine.enginePlayheadFrames();
    REQUIRE(cues.go());

    // Should NOT be playing yet (pre-wait hasn't elapsed)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(cues.isCuePlaying(0));

    // Should be playing after the pre-wait has elapsed
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(cues.isCuePlaying(0));

    // Verify timing accuracy: voiceStartEngineFrame should be within 2 buffer
    // periods of the target frame (~10 ms at 48 kHz / 512 frames).
    const int slot = cues.cueVoiceSlot(0);
    REQUIRE(slot >= 0);
    const int64_t startFrame    = engine.voiceStartEngineFrame(slot);
    const int64_t expectedFrame = beforeGo + static_cast<int64_t>(0.2 * 48000);
    const int64_t error         = std::abs(startFrame - expectedFrame);

    // Tolerance: 2 × 1024 frames ≈ 43 ms — much tighter than an OS-timer approach.
    CHECK(error < 2048);

    removeAll(paths);
}

TEST_CASE("CueList: panic cancels pending pre-wait", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));

    cues.setCuePreWait(0, 1.0);   // 1 s — long enough to cancel before it fires
    REQUIRE(cues.go());
    CHECK(scheduler.pendingCount() == 1);

    cues.panic();
    CHECK(scheduler.pendingCount() == 0);

    // Voice should never start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(cues.isCuePlaying(0));

    removeAll(paths);
}

// ---- Start / Stop cue types ------------------------------------------------

TEST_CASE("CueList: StartCue triggers target audio", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));          // [0] audio
    cues.addStartCue(0);                     // [1] start(0)

    // Fire the StartCue — should activate cue[0]'s audio
    cues.setSelectedIndex(1);
    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK(cues.isCuePlaying(0));             // audio tagged with target index 0
    CHECK(engine.activeVoiceCount() == 1);

    removeAll(paths);
}

TEST_CASE("CueList: StopCue silences target", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));          // [0] audio
    cues.addStopCue(0);                      // [1] stop(0)

    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(cues.isCuePlaying(0));

    // Fire StopCue
    cues.setSelectedIndex(1);
    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK_FALSE(cues.isCuePlaying(0));
    CHECK(engine.activeVoiceCount() == 0);

    removeAll(paths);
}

TEST_CASE("CueList: StartCue with pre-wait delays target audio", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));          // [0] audio
    cues.addStartCue(0, "", 0.2);            // [1] start(0) with 200 ms pre-wait

    cues.setSelectedIndex(1);
    REQUIRE(cues.go());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(cues.isCuePlaying(0));       // pre-wait not elapsed

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(cues.isCuePlaying(0));             // should be playing now

    removeAll(paths);
}

TEST_CASE("CueList: addCue with preWait parameter", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0], "", 0.2));  // 200 ms pre-wait via addCue

    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(cues.isCuePlaying(0));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(cues.isCuePlaying(0));

    removeAll(paths);
}

// ---- startTime / duration --------------------------------------------------

TEST_CASE("CueList: startTime offsets into file", "[cue_list][audio_hw]") {
    // 2-second file; start at 1.5 s → only 0.5 s plays
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));
    cues.setCueStartTime(0, 1.5);

    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(cues.isCuePlaying(0));

    // After ~600 ms the voice should have finished (only 0.5 s of audio)
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_FALSE(cues.isCuePlaying(0));

    removeAll(paths);
}

TEST_CASE("CueList: duration limits playback length", "[cue_list][audio_hw]") {
    // 2-second file; play only 0.4 s
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(1, 48000, 2, 2.0);
    REQUIRE(cues.addCue(paths[0]));
    cues.setCueDuration(0, 0.4);

    REQUIRE(cues.start(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(cues.isCuePlaying(0));

    // Should finish well before the full 2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_FALSE(cues.isCuePlaying(0));

    removeAll(paths);
}

// ---- autoContinue ----------------------------------------------------------

TEST_CASE("CueList: autoContinue cascades consecutive cues", "[cue_list][audio_hw]") {
    // Cues 0, 1 have autoContinue; cue 2 does not.
    // A single go() should fire all three simultaneously.
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 2.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    cues.setCueAutoContinue(0, true);
    cues.setCueAutoContinue(1, true);
    // cue 2: autoContinue = false (default)

    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // All three should be playing
    CHECK(cues.isCuePlaying(0));
    CHECK(cues.isCuePlaying(1));
    CHECK(cues.isCuePlaying(2));
    CHECK(engine.activeVoiceCount() == 3);

    // Selection advances past the whole cascade (to index 3)
    CHECK(cues.selectedIndex() == 3);

    removeAll(paths);
}

TEST_CASE("CueList: autoContinue stops at first non-AC cue", "[cue_list][audio_hw]") {
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    auto paths = makeWavs(3, 48000, 2, 2.0);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    cues.setCueAutoContinue(0, true);
    // cue 1: autoContinue = false — cascade stops here
    // cue 2 should NOT fire

    REQUIRE(cues.go());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(cues.isCuePlaying(0));
    CHECK(cues.isCuePlaying(1));
    CHECK_FALSE(cues.isCuePlaying(2));
    CHECK(engine.activeVoiceCount() == 2);
    CHECK(cues.selectedIndex() == 2);

    removeAll(paths);
}

// ---- autoFollow ------------------------------------------------------------

TEST_CASE("CueList: autoFollow triggers go() when voice ends", "[cue_list][audio_hw]") {
    // Cue 0 (0.3 s) with autoFollow → go() fires automatically → plays cue 1
    mcp::AudioEngine engine; REQUIRE(engine.initialize());
    mcp::Scheduler   scheduler(engine);
    mcp::CueList     cues(engine, scheduler);
    // Short cue so the test completes quickly
    auto paths = makeWavs(2, 48000, 2, 0.3);
    for (const auto& p : paths) REQUIRE(cues.addCue(p));

    cues.setCueAutoFollow(0, true);

    REQUIRE(cues.go());    // fires cue 0, schedules autoFollow go()
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(cues.isCuePlaying(0));
    CHECK_FALSE(cues.isCuePlaying(1));

    // Wait for cue 0 to finish + scheduler to fire the autoFollow go()
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(cues.isCuePlaying(1));
    // Selection should have advanced to 2 (past both cues)
    CHECK(cues.selectedIndex() == 2);

    removeAll(paths);
}
