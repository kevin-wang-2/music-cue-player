#pragma once

#include <sndfile.h>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

// Writes a 440Hz sine wave to a temp WAV file. Returns the file path.
// Caller is responsible for removing the file after use.
inline std::string makeSineWav(int sampleRate = 48000, int channels = 2, double durationSec = 1.0) {
    namespace fs = std::filesystem;
    auto path = (fs::temp_directory_path() / "mcp_test_sine.wav").string();

    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels   = channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_24;

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    REQUIRE(f != nullptr);

    const int64_t totalFrames = static_cast<int64_t>(sampleRate * durationSec);
    std::vector<float> buf(static_cast<size_t>(totalFrames * channels));
    constexpr float kAmplitude = 0.1f;  // -20 dBFS, not ear-destroying
    constexpr double kFreq     = 1000.0;
    for (int64_t i = 0; i < totalFrames; ++i) {
        float s = kAmplitude * static_cast<float>(std::sin(2.0 * M_PI * kFreq * i / sampleRate));
        for (int ch = 0; ch < channels; ++ch)
            buf[static_cast<size_t>(i * channels + ch)] = s;
    }

    sf_writef_float(f, buf.data(), totalFrames);
    sf_close(f);
    return path;
}
