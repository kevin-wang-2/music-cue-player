#include "engine/AudioEngine.h"
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
        const float* samples{nullptr};
        int64_t      totalFrames{0};
        int          channels{0};
        int          tag{-1};
        float        gain{1.0f};
    } pending;

    std::atomic<bool>  pendingReady{false};
    std::atomic<bool>  pendingClear{false};

    // gain is written by the main thread (setVoiceGain) and read by the audio
    // thread every callback; atomic<float> provides the necessary ordering.
    std::atomic<float> gain{1.0f};

    // Written by audio thread at activation; readable by the main thread.
    const float*      samples{nullptr};
    int64_t           totalFrames{0};
    int               channels{0};
    int64_t           startEngineFrame{0};  // engine-time coordinate
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

    // bufferPlayhead — incremented each callback by this device's frame count.
    // enginePlayhead — the master clock; written here because this is the sole
    //                  (master) device. In a multi-device setup only the master
    //                  device's callback would update this.
    std::atomic<int64_t> bufferPlayhead{0};
    std::atomic<int64_t> enginePlayhead{0};
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
        // The start is recorded in engine time so it stays valid across devices.
        if (v.pendingReady.load(std::memory_order_acquire)) {
            v.samples          = v.pending.samples;
            v.totalFrames      = v.pending.totalFrames;
            v.channels         = v.pending.channels;
            v.startEngineFrame = engStart;
            v.gain.store(v.pending.gain, std::memory_order_relaxed);
            v.activeTag.store(v.pending.tag, std::memory_order_relaxed);
            v.active.store(true, std::memory_order_relaxed);
            v.pendingReady.store(false, std::memory_order_release);
        }

        if (!v.active.load(std::memory_order_relaxed)) continue;

        if (v.channels != impl->outChannels) {
            v.active.store(false, std::memory_order_release);
            continue;
        }

        // Voice position: how many frames have elapsed since this voice started,
        // expressed in engine time. For this device: bufStart == engStart, so
        // localStart = bufStart - v.startEngineFrame.
        // A future slave device would compute:
        //   localStart = (bufStart + slaveOffset) - v.startEngineFrame
        // where slaveOffset maps buffer time → engine time for that device.
        const int64_t localStart = engStart - v.startEngineFrame;
        if (localStart < 0) { v.active.store(false, std::memory_order_release); continue; }

        const int64_t remaining = v.totalFrames - localStart;
        const int64_t toRead    = std::min(static_cast<int64_t>(frameCount), remaining);

        const float* src  = v.samples + localStart * v.channels;
        const float  gain = v.gain.load(std::memory_order_relaxed);
        for (int64_t f = 0; f < toRead; ++f)
            for (int ch = 0; ch < impl->outChannels; ++ch)
                out[f * impl->outChannels + ch] += src[f * v.channels + ch] * gain;

        if (toRead < static_cast<int64_t>(frameCount))
            v.active.store(false, std::memory_order_release);
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

bool AudioEngine::initialize(int sampleRate, int channels) {
    PaError err = Pa_Initialize();
    if (err != paNoError) { m_impl->lastError = Pa_GetErrorText(err); return false; }

    m_impl->sampleRate  = sampleRate;
    m_impl->outChannels = channels;

    err = Pa_OpenDefaultStream(&m_impl->stream, 0, channels, paFloat32, sampleRate,
                               paFramesPerBufferUnspecified, &paCallback, m_impl.get());
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
        v.pending = {samples, totalFrames, voiceChannels, tag, gain};
        v.pendingReady.store(true, std::memory_order_release);
        return i;
    }
    return -1;
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
