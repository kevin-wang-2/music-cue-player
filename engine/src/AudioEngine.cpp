#include "engine/AudioEngine.h"
#include "engine/IAudioSource.h"
#include <portaudio.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace mcp {

// ---------------------------------------------------------------------------
// Voice slot
// ---------------------------------------------------------------------------
struct VoiceSlot {
    struct Pending {
        const float*   samples{nullptr};       // in-memory mode
        IAudioSource*  streamReader{nullptr};  // streaming mode
        bool          streaming{false};
        int64_t       totalFrames{0};
        int           channels{0};
        int           tag{-1};
        float         gain{1.0f};
    } pending;

    std::atomic<bool>  pendingReady{false};
    std::atomic<bool>  pendingClear{false};

    // gain is written by the main thread (setVoiceGain) and read by the audio
    // thread every callback; atomic<float> provides the necessary ordering.
    std::atomic<float> gain{1.0f};

    // Written by audio thread at activation; readable by the main thread.
    const float*   samples{nullptr};
    IAudioSource*  streamReader{nullptr};
    bool          streaming{false};
    int64_t       totalFrames{0};
    int           channels{0};
    int64_t       startEngineFrame{0};  // engine-time coordinate
    std::atomic<int>  activeTag{-1};
    std::atomic<bool> active{false};
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct AudioEngineImpl {
    PaStream* stream{nullptr};
    bool initialized{false};
    int sampleRate{48000};
    int outChannels{2};
    std::string lastError;

    std::array<VoiceSlot, AudioEngine::kMaxVoices> voices;

    std::atomic<int64_t> bufferPlayhead{0};
    std::atomic<int64_t> enginePlayhead{0};

    // Soft-panic fade state — written by main thread, read by audio callback.
    std::atomic<bool>    softPanicActive{false};
    std::atomic<int64_t> softPanicStartFrame{0};
    std::atomic<int64_t> softPanicEndFrame{0};
};

// ---------------------------------------------------------------------------
// Audio callback
// ---------------------------------------------------------------------------
static int paCallback(const void* /*input*/, void* output,
                      unsigned long frameCount,
                      const PaStreamCallbackTimeInfo* /*timeInfo*/,
                      PaStreamCallbackFlags /*statusFlags*/,
                      void* userData) {
    auto* impl = static_cast<AudioEngineImpl*>(userData);
    auto* out  = static_cast<float*>(output);

    // Snapshot both playheads at the start of this buffer.
    const int64_t bufStart = impl->bufferPlayhead.load(std::memory_order_relaxed);
    const int64_t engStart = bufStart;  // single master device: engine time == buffer time

    const size_t outSamples = frameCount * static_cast<size_t>(impl->outChannels);
    std::memset(out, 0, outSamples * sizeof(float));

    for (auto& v : impl->voices) {
        // Apply pending clear
        if (v.pendingClear.load(std::memory_order_acquire)) {
            v.active.store(false, std::memory_order_relaxed);
            v.activeTag.store(-1, std::memory_order_relaxed);
            v.pendingClear.store(false, std::memory_order_release);
        }

        // Apply pending voice — voice starts at the first sample of this buffer.
        if (v.pendingReady.load(std::memory_order_acquire)) {
            v.samples          = v.pending.samples;
            v.streamReader     = v.pending.streamReader;
            v.streaming        = v.pending.streaming;
            v.totalFrames      = v.pending.totalFrames;
            v.channels         = v.pending.channels;
            v.startEngineFrame = engStart;
            v.gain.store(v.pending.gain, std::memory_order_relaxed);
            v.activeTag.store(v.pending.tag, std::memory_order_relaxed);
            v.active.store(true, std::memory_order_relaxed);
            v.pendingReady.store(false, std::memory_order_release);
        }

        if (!v.active.load(std::memory_order_relaxed)) continue;

        // For in-memory voices, channel counts must match (direct array indexing).
        // Streaming voices are always scheduled with engine.channels() so this
        // check is satisfied; StreamReader::read() handles internal remapping.
        if (!v.streaming && v.channels != impl->outChannels) {
            v.active.store(false, std::memory_order_release);
            continue;
        }

        const int64_t localStart = engStart - v.startEngineFrame;
        if (localStart < 0) { v.active.store(false, std::memory_order_release); continue; }

        const int64_t remaining = v.totalFrames - localStart;
        const int64_t toRead    = std::min(static_cast<int64_t>(frameCount), remaining);
        const float   gain      = v.gain.load(std::memory_order_relaxed);

        if (v.streaming) {
            // Ring-buffer path: read() adds to out directly and tracks position internally.
            if (v.streamReader && toRead > 0) {
                const int64_t got = v.streamReader->read(out, toRead, impl->outChannels, gain);
                if (got < toRead && v.streamReader->isDone())
                    v.active.store(false, std::memory_order_release);
            }
            if (toRead < static_cast<int64_t>(frameCount))
                v.active.store(false, std::memory_order_release);
        } else {
            // In-memory path (unchanged for backward compatibility).
            const float* src = v.samples + localStart * v.channels;
            for (int64_t f = 0; f < toRead; ++f)
                for (int ch = 0; ch < impl->outChannels; ++ch)
                    out[f * impl->outChannels + ch] += src[f * v.channels + ch] * gain;

            if (toRead < static_cast<int64_t>(frameCount))
                v.active.store(false, std::memory_order_release);
        }
    }

    // Soft-panic fade: linearly ramp output to zero, then clear all voices.
    if (impl->softPanicActive.load(std::memory_order_acquire)) {
        const int64_t pStart = impl->softPanicStartFrame.load(std::memory_order_relaxed);
        const int64_t pEnd   = impl->softPanicEndFrame.load(std::memory_order_relaxed);
        const float   pLen   = static_cast<float>(pEnd - pStart);
        bool allDone = false;
        for (unsigned long f = 0; f < frameCount; ++f) {
            const int64_t eng = engStart + static_cast<int64_t>(f);
            float t = 0.0f;
            if (pLen > 0.0f && eng < pEnd)
                t = std::max(0.0f, 1.0f - static_cast<float>(eng - pStart) / pLen);
            if (eng >= pEnd) allDone = true;
            for (int ch = 0; ch < impl->outChannels; ++ch)
                out[f * static_cast<unsigned long>(impl->outChannels) + ch] *= t;
        }
        if (allDone) {
            for (auto& v : impl->voices) {
                v.active.store(false, std::memory_order_relaxed);
                v.pendingReady.store(false, std::memory_order_relaxed);
            }
            impl->softPanicActive.store(false, std::memory_order_release);
        }
    }

    // Advance device counter and push master clock forward (master-only update).
    impl->bufferPlayhead.fetch_add(static_cast<int64_t>(frameCount), std::memory_order_relaxed);
    impl->enginePlayhead.store(bufStart + static_cast<int64_t>(frameCount),
                               std::memory_order_release);
    return paContinue;
}

// ---------------------------------------------------------------------------
AudioEngine::AudioEngine() : m_impl(std::make_unique<AudioEngineImpl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

std::vector<DeviceInfo> AudioEngine::listOutputDevices() {
    std::vector<DeviceInfo> result;
    PaError err = Pa_Initialize();
    if (err != paNoError) return result;

    const int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        DeviceInfo d;
        d.index             = i;
        d.name              = info->name ? info->name : "";
        d.maxOutputChannels = info->maxOutputChannels;
        result.push_back(std::move(d));
    }

    Pa_Terminate();
    return result;
}

bool AudioEngine::initialize(int sampleRate, int channels, const std::string& deviceName) {
    PaError err = Pa_Initialize();
    if (err != paNoError) { m_impl->lastError = Pa_GetErrorText(err); return false; }

    // Resolve the device index
    PaDeviceIndex devIdx = Pa_GetDefaultOutputDevice();
    if (!deviceName.empty()) {
        const int count = Pa_GetDeviceCount();
        for (int i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (info && info->maxOutputChannels > 0 && info->name
                && std::string(info->name) == deviceName) {
                devIdx = i;
                break;
            }
        }
    }

    // Auto-detect channel count from the selected device when channels == 0
    if (channels <= 0) {
        channels = 2;
        if (devIdx != paNoDevice) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(devIdx);
            if (info && info->maxOutputChannels > 0)
                channels = std::min(info->maxOutputChannels, 64);
        }
    }

    m_impl->sampleRate  = sampleRate;
    m_impl->outChannels = channels;

    PaStreamParameters outParams{};
    outParams.device                    = (devIdx != paNoDevice) ? devIdx : Pa_GetDefaultOutputDevice();
    outParams.channelCount              = channels;
    outParams.sampleFormat              = paFloat32;
    outParams.suggestedLatency          = Pa_GetDeviceInfo(outParams.device)
                                          ? Pa_GetDeviceInfo(outParams.device)->defaultLowOutputLatency
                                          : 0.01;
    outParams.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&m_impl->stream, nullptr, &outParams, sampleRate,
                        paFramesPerBufferUnspecified, paNoFlag, &paCallback, m_impl.get());
    if (err != paNoError) {
        m_impl->lastError = Pa_GetErrorText(err); Pa_Terminate(); return false;
    }

    err = Pa_StartStream(m_impl->stream);
    if (err != paNoError) {
        m_impl->lastError = Pa_GetErrorText(err);
        Pa_CloseStream(m_impl->stream); m_impl->stream = nullptr;
        Pa_Terminate(); return false;
    }

    m_impl->initialized = true;
    m_impl->lastError.clear();
    return true;
}

void AudioEngine::shutdown() {
    if (m_impl->stream) {
        Pa_StopStream(m_impl->stream); Pa_CloseStream(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->initialized) { Pa_Terminate(); m_impl->initialized = false; }
    for (auto& v : m_impl->voices) { v.active = false; v.pendingReady = false; }
    m_impl->bufferPlayhead = 0;
    m_impl->enginePlayhead = 0;
}

bool AudioEngine::isInitialized() const { return m_impl->initialized; }
int  AudioEngine::sampleRate()    const { return m_impl->sampleRate; }
int  AudioEngine::channels()      const { return m_impl->outChannels; }

int AudioEngine::scheduleVoice(const float* samples, int64_t totalFrames,
                                int voiceChannels, int tag, float gain) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = m_impl->voices[i];
        if (v.active.load(std::memory_order_relaxed))       continue;
        if (v.pendingReady.load(std::memory_order_relaxed)) continue;
        v.pending = {samples, nullptr, false, totalFrames, voiceChannels, tag, gain};
        v.pendingReady.store(true, std::memory_order_release);
        return i;
    }
    return -1;
}

int AudioEngine::scheduleStreamingVoice(IAudioSource* reader, int64_t totalFrames,
                                         int voiceChannels, int tag, float gain) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = m_impl->voices[i];
        if (v.active.load(std::memory_order_relaxed))       continue;
        if (v.pendingReady.load(std::memory_order_relaxed)) continue;
        v.pending = {nullptr, reader, true, totalFrames, voiceChannels, tag, gain};
        v.pendingReady.store(true, std::memory_order_release);
        return i;
    }
    return -1;
}

void AudioEngine::softPanic(double durationSeconds) {
    if (!m_impl->initialized) { clearAllVoices(); return; }
    const int64_t now = enginePlayheadFrames();
    const int64_t dur = static_cast<int64_t>(
        std::max(0.001, durationSeconds) * m_impl->sampleRate);
    m_impl->softPanicStartFrame.store(now, std::memory_order_relaxed);
    m_impl->softPanicEndFrame.store(now + dur, std::memory_order_relaxed);
    m_impl->softPanicActive.store(true, std::memory_order_release);
}

void AudioEngine::setVoiceGain(int slotId, float gain) {
    if (slotId < 0 || slotId >= kMaxVoices) return;
    m_impl->voices[slotId].gain.store(gain, std::memory_order_relaxed);
}

void AudioEngine::clearVoice(int slotId) {
    if (slotId < 0 || slotId >= kMaxVoices) return;
    auto& v = m_impl->voices[slotId];
    v.pendingReady.store(false, std::memory_order_relaxed);
    v.pendingClear.store(true, std::memory_order_release);
}

void AudioEngine::clearVoicesByTag(int tag) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = m_impl->voices[i];
        if (v.pendingReady.load(std::memory_order_relaxed) && v.pending.tag == tag)
            v.pendingReady.store(false, std::memory_order_relaxed);
        if (v.active.load(std::memory_order_relaxed) &&
            v.activeTag.load(std::memory_order_relaxed) == tag)
            v.pendingClear.store(true, std::memory_order_release);
    }
}

void AudioEngine::clearAllVoices() {
    for (int i = 0; i < kMaxVoices; ++i) clearVoice(i);
}

bool AudioEngine::isVoiceActive(int slotId) const {
    if (slotId < 0 || slotId >= kMaxVoices) return false;
    return m_impl->voices[slotId].active.load(std::memory_order_relaxed);
}

bool AudioEngine::isVoicePending(int slotId) const {
    if (slotId < 0 || slotId >= kMaxVoices) return false;
    return m_impl->voices[slotId].pendingReady.load(std::memory_order_relaxed);
}

bool AudioEngine::anyVoiceActiveWithTag(int tag) const {
    for (const auto& v : m_impl->voices)
        if (v.active.load(std::memory_order_relaxed) &&
            v.activeTag.load(std::memory_order_relaxed) == tag)
            return true;
    return false;
}

int AudioEngine::activeVoiceCount() const {
    int n = 0;
    for (const auto& v : m_impl->voices)
        if (v.active.load(std::memory_order_relaxed)) ++n;
    return n;
}

int64_t AudioEngine::voiceStartEngineFrame(int slotId) const {
    if (slotId < 0 || slotId >= kMaxVoices) return 0;
    return m_impl->voices[slotId].startEngineFrame;
}

int64_t AudioEngine::enginePlayheadFrames() const {
    return m_impl->enginePlayhead.load(std::memory_order_acquire);
}

double AudioEngine::enginePlayheadSeconds() const {
    return m_impl->sampleRate > 0
        ? static_cast<double>(enginePlayheadFrames()) / m_impl->sampleRate : 0.0;
}

int64_t AudioEngine::bufferPlayheadFrames() const {
    return m_impl->bufferPlayhead.load(std::memory_order_relaxed);
}

const std::string& AudioEngine::lastError() const { return m_impl->lastError; }

} // namespace mcp
