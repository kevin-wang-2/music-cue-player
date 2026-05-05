#include "TestHelpers.h"
#include "engine/AudioEngine.h"
#include "engine/AudioFile.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

// Tests that require audio hardware are tagged [audio_hw].
// Skip them: engine_tests "~[audio_hw]"

TEST_CASE("AudioEngine: initialize and shutdown", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());
    CHECK(engine.isInitialized());
    CHECK(engine.sampleRate() == 48000);
    CHECK(engine.channels()   == 2);
    engine.shutdown();
    CHECK_FALSE(engine.isInitialized());
}

TEST_CASE("AudioEngine: globalPlayhead advances after initialize", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(engine.enginePlayheadFrames() > 0);
}

TEST_CASE("AudioEngine: no voices → activeVoiceCount == 0", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());
    CHECK(engine.activeVoiceCount() == 0);
}

TEST_CASE("AudioEngine: scheduleVoice returns valid slot", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 1.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    const int slot = engine.scheduleVoice(
        file.samples().data(), file.metadata().frameCount, file.metadata().channels, 0);
    CHECK(slot >= 0);
    CHECK(slot < mcp::AudioEngine::kMaxVoices);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(engine.isVoiceActive(slot));
    CHECK(engine.activeVoiceCount() == 1);

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: voice ends naturally", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 0.1);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    const int slot = engine.scheduleVoice(
        file.samples().data(), file.metadata().frameCount, file.metadata().channels);
    REQUIRE(slot >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(engine.isVoiceActive(slot));
    CHECK(engine.activeVoiceCount() == 0);

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: clearVoice stops playback", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 5.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    const int slot = engine.scheduleVoice(
        file.samples().data(), file.metadata().frameCount, file.metadata().channels);
    REQUIRE(slot >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(engine.isVoiceActive(slot));

    engine.clearVoice(slot);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(engine.isVoiceActive(slot));

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: multiple voices mix simultaneously", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 1.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    const int s0 = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 0);
    const int s1 = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 1);
    const int s2 = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 2);
    REQUIRE(s0 >= 0); REQUIRE(s1 >= 0); REQUIRE(s2 >= 0);
    CHECK(s0 != s1); CHECK(s1 != s2);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(engine.activeVoiceCount() == 3);

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: clearVoicesByTag stops correct voices", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 2.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    const int sa = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 10);
    const int sb = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 20);
    const int sc = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, 10);
    REQUIRE(sa >= 0); REQUIRE(sb >= 0); REQUIRE(sc >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(engine.activeVoiceCount() == 3);

    engine.clearVoicesByTag(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK_FALSE(engine.isVoiceActive(sa));
    CHECK(engine.isVoiceActive(sb));   // tag 20 untouched
    CHECK_FALSE(engine.isVoiceActive(sc));
    CHECK(engine.activeVoiceCount() == 1);

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: clearAllVoices stops everything", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 2.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    for (int i = 0; i < 5; ++i)
        engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2, i);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(engine.activeVoiceCount() == 5);

    engine.clearAllVoices();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(engine.activeVoiceCount() == 0);

    std::filesystem::remove(path);
}

TEST_CASE("AudioEngine: voiceStartFrame increases with each schedule", "[audio_engine][audio_hw]") {
    mcp::AudioEngine engine;
    REQUIRE(engine.initialize());

    auto path = makeSineWav(48000, 2, 2.0);
    mcp::AudioFile file;
    REQUIRE(file.load(path));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int s0 = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int s1 = engine.scheduleVoice(file.samples().data(), file.metadata().frameCount, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    CHECK(engine.voiceStartEngineFrame(s1) > engine.voiceStartEngineFrame(s0));

    std::filesystem::remove(path);
}
