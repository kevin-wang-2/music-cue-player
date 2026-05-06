#include "engine/AudioFile.h"
#include <sndfile.h>

namespace mcp {

bool AudioFile::load(const std::string& path) {
    unload();
    m_path = path;

    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) {
        m_error = sf_strerror(nullptr);
        return false;
    }

    m_metadata.sampleRate = info.samplerate;
    m_metadata.channels   = info.channels;
    m_metadata.frameCount = info.frames;

    const size_t total = static_cast<size_t>(info.frames * info.channels);
    m_samples.resize(total);

    const sf_count_t read = sf_readf_float(file, m_samples.data(), info.frames);
    sf_close(file);

    if (read != info.frames) {
        m_error = "Short read: expected " + std::to_string(info.frames)
                + " frames, got " + std::to_string(read);
        m_samples.clear();
        return false;
    }

    m_loaded = true;
    m_error.clear();
    return true;
}

bool AudioFile::loadMetadata(const std::string& path) {
    unload();
    m_path = path;
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) { m_error = sf_strerror(nullptr); return false; }
    m_metadata.sampleRate = info.samplerate;
    m_metadata.channels   = info.channels;
    m_metadata.frameCount = info.frames;
    sf_close(file);
    m_loaded = true;
    m_error.clear();
    return true;
}

void AudioFile::unload() {
    m_samples.clear();
    m_samples.shrink_to_fit();
    m_metadata = {};
    m_path.clear();
    m_error.clear();
    m_loaded = false;
}

} // namespace mcp
