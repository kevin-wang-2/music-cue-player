#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcp {

struct AudioMetadata {
    int sampleRate{0};
    int channels{0};
    int64_t frameCount{0};

    double durationSeconds() const {
        return sampleRate > 0 ? static_cast<double>(frameCount) / sampleRate : 0.0;
    }
};

// Loads an audio file entirely into memory as interleaved float samples.
// Not thread-safe — load/unload from a single thread.
class AudioFile {
public:
    AudioFile() = default;
    ~AudioFile() = default;

    AudioFile(const AudioFile&) = delete;
    AudioFile& operator=(const AudioFile&) = delete;
    AudioFile(AudioFile&&) = default;
    AudioFile& operator=(AudioFile&&) = default;

    bool load(const std::string& path);
    void unload();

    bool isLoaded() const { return m_loaded; }
    const AudioMetadata& metadata() const { return m_metadata; }

    // Interleaved float samples, range [-1.0, 1.0], size = frameCount * channels
    const std::vector<float>& samples() const { return m_samples; }
    size_t sampleCount() const { return m_samples.size(); }

    const std::string& path() const { return m_path; }
    const std::string& errorMessage() const { return m_error; }

private:
    AudioMetadata m_metadata;
    std::vector<float> m_samples;
    std::string m_path;
    std::string m_error;
    bool m_loaded{false};
};

} // namespace mcp
