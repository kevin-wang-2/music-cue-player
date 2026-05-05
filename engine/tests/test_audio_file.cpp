#include "TestHelpers.h"
#include "engine/AudioFile.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <filesystem>

TEST_CASE("AudioFile: correct metadata after load", "[audio_file]") {
    auto path = makeSineWav(48000, 2, 1.0);

    mcp::AudioFile file;
    REQUIRE(file.load(path));
    REQUIRE(file.isLoaded());

    CHECK(file.metadata().sampleRate == 48000);
    CHECK(file.metadata().channels   == 2);
    CHECK(file.metadata().frameCount == 48000);
    CHECK_THAT(file.metadata().durationSeconds(),
               Catch::Matchers::WithinAbs(1.0, 0.001));

    std::filesystem::remove(path);
}

TEST_CASE("AudioFile: sample count matches metadata", "[audio_file]") {
    auto path = makeSineWav(48000, 2, 1.0);

    mcp::AudioFile file;
    REQUIRE(file.load(path));
    CHECK(file.sampleCount() ==
          static_cast<size_t>(file.metadata().frameCount * file.metadata().channels));

    std::filesystem::remove(path);
}

TEST_CASE("AudioFile: samples stay in [-1, 1]", "[audio_file]") {
    auto path = makeSineWav(48000, 1, 0.1);

    mcp::AudioFile file;
    REQUIRE(file.load(path));

    for (float s : file.samples()) {
        CHECK(s >= -1.0f);
        CHECK(s <=  1.0f);
    }

    std::filesystem::remove(path);
}

TEST_CASE("AudioFile: fails gracefully on missing file", "[audio_file]") {
    mcp::AudioFile file;
    CHECK_FALSE(file.load("/nonexistent/mcp_no_such_file.wav"));
    CHECK_FALSE(file.isLoaded());
    CHECK(file.sampleCount() == 0);
    CHECK_FALSE(file.errorMessage().empty());
}

TEST_CASE("AudioFile: unload then reload", "[audio_file]") {
    auto path = makeSineWav(48000, 1, 0.5);

    mcp::AudioFile file;
    REQUIRE(file.load(path));
    file.unload();

    CHECK_FALSE(file.isLoaded());
    CHECK(file.sampleCount() == 0);

    REQUIRE(file.load(path));
    CHECK(file.isLoaded());
    CHECK(file.metadata().sampleRate == 48000);

    std::filesystem::remove(path);
}

TEST_CASE("AudioFile: various sample rates", "[audio_file]") {
    for (int sr : {22050, 44100, 48000, 96000}) {
        auto path = makeSineWav(sr, 1, 0.05);
        mcp::AudioFile file;
        REQUIRE(file.load(path));
        CHECK(file.metadata().sampleRate == sr);
        std::filesystem::remove(path);
    }
}
