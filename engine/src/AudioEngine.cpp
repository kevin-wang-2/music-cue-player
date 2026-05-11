#include "engine/AudioEngine.h"
#include "engine/IAudioSource.h"
#include <portaudio.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace mcp {

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
struct AudioEngineImpl;

// ---------------------------------------------------------------------------
// Per-stream callback context — stored inside the heap-allocated DeviceStream
// so its address is stable for the lifetime of the stream.
// ---------------------------------------------------------------------------
struct DeviceCallbackData {
    AudioEngineImpl* impl{nullptr};
    int              streamIndex{0};
};

// ---------------------------------------------------------------------------
// Voice slot
// ---------------------------------------------------------------------------
struct VoiceSlot {
    struct Pending {
        const float*   samples{nullptr};
        IAudioSource*  streamReader{nullptr};
        bool           streaming{false};
        int64_t        totalFrames{0};
        int            channels{0};
        int            tag{-1};
        float          gain{1.0f};
        int            deviceIndex{0};
        bool           bypassChannelFold{false};
    } pending;

    std::atomic<bool>  pendingReady{false};
    std::atomic<bool>  pendingClear{false};

    // Written by main thread (setVoiceGain); read every callback.
    std::atomic<float> gain{1.0f};

    // Active-voice state — written by audio callback at activation.
    const float*   samples{nullptr};
    IAudioSource*  streamReader{nullptr};
    bool           streaming{false};
    int64_t        totalFrames{0};
    int            channels{0};
    int64_t        startEngineFrame{0};
    int            deviceIndex{0};   // which stream renders this voice
    bool           bypassChannelFold{false};
    std::atomic<int>  activeTag{-1};
    std::atomic<bool> active{false};
};

// ---------------------------------------------------------------------------
// Channel-fold buffer for one device: fold[ch * numPhys + p] = gain from logical
// channel ch to this device's local physical output p.  Double-buffered for
// lock-free main-thread updates; callback reads the active half.
struct FoldBuffer {
    std::vector<float> data;
    int numCh{0};
    int numPhys{0};
};

// Maximum frames per callback buffer we pre-allocate chanBuf for.
static constexpr int kMaxChanBufFrames = 8192;

// ---------------------------------------------------------------------------
// Per-device stream state.
// std::atomic members make this non-copyable/non-movable.
// Always heap-allocate via make_unique; never move.
// ---------------------------------------------------------------------------
struct DeviceStream {
    PaStream*            stream{nullptr};
    DeviceCallbackData   cbData;
    int                  outChannels{2};
    bool                 masterClock{false};
    // Local frame counter — incremented by this stream's callback.
    std::atomic<int64_t> renderedFrames{0};
    // Master enginePlayhead captured when this slave stream was opened.
    int64_t              masterFrameAtZero{0};

    // Channel bus: non-bypass voices accumulate here (numCh channels × frame).
    // Main thread allocates/grows (kMaxChanBufFrames × capacity); callback reads
    // via atomic pointer.  chanBufNumCh is the active channel count (loop bound).
    std::atomic<float*> chanBufPtr{nullptr};
    std::atomic<int>    chanBufNumCh{0};
    int                 chanBufCapacity{0};  // written by main thread only

    // Fold matrix double-buffer — same pattern as dspBuf.
    FoldBuffer           foldBuf[2];
    std::atomic<int>     foldBufIdx{0};
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
// Per-output-channel DSP config (phase + delay), stored in a double-buffer.
// Main thread writes the inactive buffer then atomically swaps the active index.
// Callback reads the active buffer without locking.
struct OutDspSlot {
    bool phaseInvert{false};
    int  delaySamples{0};
};

struct AudioEngineImpl {
    // heap-allocated entries; addresses stable for the lifetime of the engine.
    std::vector<std::unique_ptr<DeviceStream>> streams;

    bool        initialized{false};
    int         sampleRate{48000};
    std::string lastError;

    std::array<VoiceSlot, AudioEngine::kMaxVoices> voices;

    // Authoritative system clock; written only by the master callback.
    std::atomic<int64_t> enginePlayhead{0};

    std::atomic<bool>    softPanicActive{false};
    std::atomic<int64_t> softPanicStartFrame{0};
    std::atomic<int64_t> softPanicEndFrame{0};

    // Channel DSP — double-buffered; applied to chanBuf per logical channel, pre-fold.
    struct ChanDspSlot { bool phaseInvert{false}; int delaySamples{0}; };
    std::vector<ChanDspSlot> chanDspBuf[2];
    std::atomic<int>         chanDspBufIdx{0};
    // Per-logical-channel delay rings; grown in setDeviceChannelFold() as numCh increases.
    static constexpr int kDelayMask = AudioEngine::kMaxDelaySamples - 1;
    std::vector<std::vector<float>> chanDelayRings;  // [numCh][kMaxDelaySamples]
    std::vector<int>                chanDelayWrPos;  // [numCh]

    // Output DSP — double-buffered for lock-free main-thread updates.
    // Index [0/1] flips atomically; callback reads active, main writes inactive.
    std::vector<OutDspSlot> dspBuf[2];       // [globalOutCh]
    std::atomic<int>        dspBufIdx{0};    // which buffer is active

    // Delay rings — pre-allocated at initialize(); never resized afterwards.
    std::vector<std::vector<float>> delayRings;  // [globalOutCh][kMaxDelaySamples]
    std::vector<int>                delayWrPos;  // [globalOutCh]

    // Per-logical-channel peak amplitudes measured from chanBuf (pre-fold).
    // Fixed maximum size; chanPeakLevelCount is the active channel count.
    std::unique_ptr<std::atomic<float>[]> chanPeakLevels;
    std::atomic<int>                      chanPeakLevelCount{0};

    // Old chanBuf allocations — kept alive until shutdown to avoid use-after-free.
    // Main thread appends; freed in closeAllStreams() after callbacks are stopped.
    std::vector<float*> oldChanBufs;

    int masterIdx() const {
        for (int i = 0; i < (int)streams.size(); ++i)
            if (streams[i]->masterClock) return i;
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Audio callback — shared by all device streams.
//
// Phase 2 (skeleton): all voices are rendered on every stream.
// Phase 3 will add per-device voice filtering via VoiceSlot::deviceIndex.
// ---------------------------------------------------------------------------
static int paDeviceCallback(const void* /*input*/, void* output,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo* /*timeInfo*/,
                            PaStreamCallbackFlags /*statusFlags*/,
                            void* userData) {
    auto* cbd  = static_cast<DeviceCallbackData*>(userData);
    auto* impl = cbd->impl;
    auto& ds   = *impl->streams[cbd->streamIndex];

    // Engine-time coordinate for the start of this buffer.
    const int64_t localFrames = ds.renderedFrames.load(std::memory_order_relaxed);
    const int64_t engStart = ds.masterClock
        ? impl->enginePlayhead.load(std::memory_order_relaxed)
        : ds.masterFrameAtZero + localFrames;

    auto* out = static_cast<float*>(output);
    std::memset(out, 0, frameCount * static_cast<size_t>(ds.outChannels) * sizeof(float));

    // Channel bus: non-bypass streaming voices mix here first, then fold → out[].
    float* chanBuf   = ds.chanBufPtr.load(std::memory_order_acquire);
    const int activeCh = ds.chanBufNumCh.load(std::memory_order_acquire);
    const auto& fold = ds.foldBuf[ds.foldBufIdx.load(std::memory_order_acquire)];
    const bool hasChanBus = (chanBuf != nullptr && activeCh > 0 && fold.numCh > 0);

    if (hasChanBus)
        std::memset(chanBuf, 0, static_cast<size_t>(frameCount) * static_cast<size_t>(activeCh) * sizeof(float));

    for (auto& v : impl->voices) {
        // pendingClear: any callback can handle it (exchange guarantees once-only).
        if (v.pendingClear.exchange(false, std::memory_order_acq_rel)) {
            v.active.store(false, std::memory_order_relaxed);
            v.activeTag.store(-1, std::memory_order_relaxed);
        }

        // pendingReady: only the target device's callback activates the voice.
        if (v.pendingReady.load(std::memory_order_acquire) &&
            v.pending.deviceIndex == cbd->streamIndex &&
            v.pendingReady.exchange(false, std::memory_order_acq_rel)) {
            v.deviceIndex        = v.pending.deviceIndex;
            v.samples            = v.pending.samples;
            v.streamReader       = v.pending.streamReader;
            v.streaming          = v.pending.streaming;
            v.totalFrames        = v.pending.totalFrames;
            v.channels           = v.pending.channels;
            v.startEngineFrame   = engStart;
            v.bypassChannelFold  = v.pending.bypassChannelFold;
            v.gain.store(v.pending.gain, std::memory_order_relaxed);
            v.activeTag.store(v.pending.tag, std::memory_order_relaxed);
            v.active.store(true, std::memory_order_relaxed);
        }

        if (!v.active.load(std::memory_order_relaxed)) continue;
        if (v.deviceIndex != cbd->streamIndex) continue;

        if (!v.streaming && v.channels != ds.outChannels) {
            v.active.store(false, std::memory_order_release);
            continue;
        }

        const int64_t localStart = engStart - v.startEngineFrame;
        if (localStart < 0) { v.active.store(false, std::memory_order_release); continue; }

        const int64_t remaining = v.totalFrames - localStart;
        const int64_t toRead    = std::min(static_cast<int64_t>(frameCount), remaining);
        const float   gain      = v.gain.load(std::memory_order_relaxed);

        if (v.streaming) {
            if (v.streamReader && toRead > 0) {
                // Non-bypass voices accumulate into chanBuf (channel space);
                // bypass voices (e.g. LTC) mix directly to physical out[].
                const bool useChannelBus = hasChanBus && !v.bypassChannelFold;
                float*     dest    = useChannelBus ? chanBuf : out;
                const int  destCh  = useChannelBus ? activeCh : ds.outChannels;
                const int64_t got = v.streamReader->read(dest, toRead, destCh, gain);
                if (got < toRead && v.streamReader->isDone())
                    v.active.store(false, std::memory_order_release);
            }
            if (toRead < static_cast<int64_t>(frameCount))
                v.active.store(false, std::memory_order_release);
        } else {
            // In-memory voices always go directly to physical out[].
            const float* src = v.samples + localStart * v.channels;
            for (int64_t f = 0; f < toRead; ++f)
                for (int ch = 0; ch < ds.outChannels; ++ch)
                    out[f * ds.outChannels + ch] += src[f * v.channels + ch] * gain;
            if (toRead < static_cast<int64_t>(frameCount))
                v.active.store(false, std::memory_order_release);
        }
    }

    // Per-logical-channel DSP: phase inversion and delay, applied to chanBuf pre-fold.
    if (hasChanBus) {
        const auto& cdsp = impl->chanDspBuf[impl->chanDspBufIdx.load(std::memory_order_acquire)];
        for (int ch = 0; ch < activeCh; ++ch) {
            if (ch >= static_cast<int>(cdsp.size())) break;
            const bool inv   = cdsp[static_cast<size_t>(ch)].phaseInvert;
            const int  delay = cdsp[static_cast<size_t>(ch)].delaySamples;
            if (!inv && delay <= 0) continue;

            if (ch >= static_cast<int>(impl->chanDelayRings.size())) continue;
            auto& ring = impl->chanDelayRings[static_cast<size_t>(ch)];
            auto& wp   = impl->chanDelayWrPos[static_cast<size_t>(ch)];
            if (ring.empty()) continue;

            for (unsigned long f = 0; f < frameCount; ++f) {
                float s = chanBuf[f * static_cast<unsigned long>(activeCh) + static_cast<unsigned long>(ch)];
                ring[static_cast<size_t>(wp)] = s;
                if (delay > 0) {
                    const int rp = (wp - delay + AudioEngine::kMaxDelaySamples) & AudioEngineImpl::kDelayMask;
                    s = ring[static_cast<size_t>(rp)];
                }
                wp = (wp + 1) & AudioEngineImpl::kDelayMask;
                if (inv) s = -s;
                chanBuf[f * static_cast<unsigned long>(activeCh) + static_cast<unsigned long>(ch)] = s;
            }
        }
    }

    // Measure chanBuf peaks (logical channels, pre-fold).
    if (hasChanBus && impl->chanPeakLevels) {
        const int measCh = std::min(activeCh,
            std::min(impl->chanPeakLevelCount.load(std::memory_order_relaxed),
                     AudioEngine::kMaxChannels));
        for (int ch = 0; ch < measCh; ++ch) {
            float peak = 0.0f;
            for (unsigned long f = 0; f < frameCount; ++f) {
                const float s = chanBuf[f * static_cast<unsigned long>(activeCh) + static_cast<unsigned long>(ch)];
                const float a = s < 0.0f ? -s : s;
                if (a > peak) peak = a;
            }
            float prev = impl->chanPeakLevels[static_cast<size_t>(ch)].load(std::memory_order_relaxed);
            while (peak > prev &&
                   !impl->chanPeakLevels[static_cast<size_t>(ch)].compare_exchange_weak(
                       prev, peak, std::memory_order_relaxed)) {}
        }
    }

    // Apply channel fold: accumulate chanBuf (channel space) into out[] (physical space).
    if (hasChanBus) {
        const int numCh   = std::min(activeCh, fold.numCh);
        const int numPhys = std::min(ds.outChannels, fold.numPhys);
        for (int ch = 0; ch < numCh; ++ch) {
            for (unsigned long f = 0; f < frameCount; ++f) {
                const float s = chanBuf[f * static_cast<unsigned long>(activeCh) + static_cast<unsigned long>(ch)];
                if (s == 0.0f) continue;
                for (int p = 0; p < numPhys; ++p) {
                    out[f * static_cast<unsigned long>(ds.outChannels) + static_cast<unsigned long>(p)]
                        += s * fold.data[static_cast<size_t>(ch * fold.numPhys + p)];
                }
            }
        }
    }

    // Per-output-channel DSP: phase inversion and delay.
    // Compute this stream's base in the global output-channel index.
    {
        int globalBase = 0;
        for (int si = 0; si < cbd->streamIndex; ++si)
            globalBase += impl->streams[static_cast<size_t>(si)]->outChannels;

        const auto& dsp = impl->dspBuf[impl->dspBufIdx.load(std::memory_order_acquire)];
        for (int c = 0; c < ds.outChannels; ++c) {
            const int gi = globalBase + c;
            if (gi >= (int)dsp.size()) break;
            const bool inv   = dsp[static_cast<size_t>(gi)].phaseInvert;
            const int  delay = dsp[static_cast<size_t>(gi)].delaySamples;
            if (!inv && delay <= 0) continue;

            auto& ring = impl->delayRings[static_cast<size_t>(gi)];
            auto& wp   = impl->delayWrPos[static_cast<size_t>(gi)];
            if (ring.empty()) continue;

            for (unsigned long f = 0; f < frameCount; ++f) {
                float s = out[f * static_cast<unsigned long>(ds.outChannels) + static_cast<unsigned long>(c)];
                ring[static_cast<size_t>(wp)] = s;
                if (delay > 0) {
                    const int rp = (wp - delay + AudioEngine::kMaxDelaySamples) & AudioEngineImpl::kDelayMask;
                    s = ring[static_cast<size_t>(rp)];
                }
                wp = (wp + 1) & AudioEngineImpl::kDelayMask;
                if (inv) s = -s;
                out[f * static_cast<unsigned long>(ds.outChannels) + static_cast<unsigned long>(c)] = s;
            }
        }
    }

    // Soft-panic fade: ramp to silence then clear all voices.
    // Only the master stream clears voices to avoid races.
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
            for (int ch = 0; ch < ds.outChannels; ++ch)
                out[f * static_cast<unsigned long>(ds.outChannels) + ch] *= t;
        }
        if (allDone && ds.masterClock) {
            for (auto& v : impl->voices) {
                v.active.store(false, std::memory_order_relaxed);
                v.pendingReady.store(false, std::memory_order_relaxed);
            }
            impl->softPanicActive.store(false, std::memory_order_release);
        }
    }

    // Advance device counter; master also advances the shared engine clock.
    ds.renderedFrames.fetch_add(static_cast<int64_t>(frameCount), std::memory_order_relaxed);
    if (ds.masterClock) {
        impl->enginePlayhead.store(engStart + static_cast<int64_t>(frameCount),
                                   std::memory_order_release);
    }
    return paContinue;
}

// ---------------------------------------------------------------------------
// Internal helper: resolve a PortAudio device and open the stream.
// Allocates and appends a new DeviceStream into impl->streams.
// Pa_Initialize() must already have been called.
// Returns the new streamIndex or -1 on failure.
// ---------------------------------------------------------------------------
static int openOneStream(AudioEngineImpl* impl,
                         int sampleRate, int channels,
                         const std::string& deviceName,
                         bool masterClock, int bufferSize) {
    const int idx = static_cast<int>(impl->streams.size());
    impl->streams.push_back(std::make_unique<DeviceStream>());
    DeviceStream& ds = *impl->streams.back();
    ds.cbData      = {impl, idx};
    ds.masterClock = masterClock;

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

    if (channels <= 0) {
        channels = 2;
        if (devIdx != paNoDevice) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(devIdx);
            if (info && info->maxOutputChannels > 0)
                channels = std::min(info->maxOutputChannels, 64);
        }
    }
    ds.outChannels = channels;

    PaStreamParameters outParams{};
    outParams.device       = (devIdx != paNoDevice) ? devIdx : Pa_GetDefaultOutputDevice();
    outParams.channelCount = channels;
    outParams.sampleFormat = paFloat32;
    const PaDeviceInfo* di = Pa_GetDeviceInfo(outParams.device);
    outParams.suggestedLatency          = di ? di->defaultLowOutputLatency : 0.01;
    outParams.hostApiSpecificStreamInfo = nullptr;

    const unsigned long framesPerBuffer = bufferSize > 0
        ? static_cast<unsigned long>(bufferSize)
        : paFramesPerBufferUnspecified;

    const PaError err = Pa_OpenStream(&ds.stream, nullptr, &outParams,
                                      sampleRate, framesPerBuffer, paNoFlag,
                                      &paDeviceCallback, &ds.cbData);
    if (err != paNoError) {
        impl->lastError = (deviceName.empty() ? std::string("default") : deviceName)
                          + ": " + Pa_GetErrorText(err);
        impl->streams.pop_back();
        return -1;
    }
    return idx;
}

// ---------------------------------------------------------------------------
static void closeAllStreams(AudioEngineImpl* impl) {
    // Stop and close all PA streams first so callbacks are no longer running.
    for (auto& ds : impl->streams) {
        if (ds->stream) {
            Pa_StopStream(ds->stream);
            Pa_CloseStream(ds->stream);
            ds->stream = nullptr;
        }
    }
    // Callbacks are now stopped — safe to free chanBufs.
    for (auto& ds : impl->streams) {
        float* p = ds->chanBufPtr.exchange(nullptr, std::memory_order_relaxed);
        delete[] p;
    }
    for (float* p : impl->oldChanBufs) delete[] p;
    impl->oldChanBufs.clear();
    impl->streams.clear();
}

// ---------------------------------------------------------------------------
AudioEngine::AudioEngine() : m_impl(std::make_unique<AudioEngineImpl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

std::vector<DeviceInfo> AudioEngine::listOutputDevices() {
    std::vector<DeviceInfo> result;
    if (Pa_Initialize() != paNoError) return result;

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

// ---------------------------------------------------------------------------
bool AudioEngine::initialize(int sampleRate, int channels, const std::string& deviceName) {
    PaError err = Pa_Initialize();
    if (err != paNoError) { m_impl->lastError = Pa_GetErrorText(err); return false; }

    m_impl->sampleRate = sampleRate;
    if (openOneStream(m_impl.get(), sampleRate, channels, deviceName,
                      /*masterClock=*/true, /*bufferSize=*/0) < 0) {
        Pa_Terminate();
        return false;
    }

    err = Pa_StartStream(m_impl->streams[0]->stream);
    if (err != paNoError) {
        m_impl->lastError = Pa_GetErrorText(err);
        Pa_CloseStream(m_impl->streams[0]->stream);
        m_impl->streams.clear();
        Pa_Terminate();
        return false;
    }

    // Allocate per-output-channel DSP resources.
    {
        int totalOut = 0;
        for (const auto& ds : m_impl->streams) totalOut += ds->outChannels;
        m_impl->delayRings.assign(static_cast<size_t>(totalOut),
                                  std::vector<float>(kMaxDelaySamples, 0.0f));
        m_impl->delayWrPos.assign(static_cast<size_t>(totalOut), 0);
        m_impl->dspBuf[0].assign(static_cast<size_t>(totalOut), OutDspSlot{});
        m_impl->dspBuf[1].assign(static_cast<size_t>(totalOut), OutDspSlot{});
        m_impl->dspBufIdx.store(0, std::memory_order_relaxed);
        m_impl->chanPeakLevels = std::make_unique<std::atomic<float>[]>(kMaxChannels);
        for (int i = 0; i < kMaxChannels; ++i)
            m_impl->chanPeakLevels[static_cast<size_t>(i)].store(0.0f, std::memory_order_relaxed);
    }

    m_impl->initialized = true;
    m_impl->lastError.clear();
    return true;
}

// ---------------------------------------------------------------------------
bool AudioEngine::initialize(int sampleRate, const std::vector<DeviceSpec>& devices) {
    if (devices.empty()) { m_impl->lastError = "no devices specified"; return false; }

    PaError err = Pa_Initialize();
    if (err != paNoError) { m_impl->lastError = Pa_GetErrorText(err); return false; }

    m_impl->sampleRate = sampleRate;
    const int n = static_cast<int>(devices.size());

    // Find master (first with masterClock=true, or device 0).
    int masterSpecIdx = 0;
    for (int i = 0; i < n; ++i)
        if (devices[i].masterClock) { masterSpecIdx = i; break; }

    // Open master first so slaves can read enginePlayhead at their start time.
    const auto& ms = devices[masterSpecIdx];
    if (openOneStream(m_impl.get(), sampleRate, ms.channelCount, ms.name,
                      /*masterClock=*/true, ms.bufferSize) < 0) {
        closeAllStreams(m_impl.get()); Pa_Terminate(); return false;
    }
    // streams[0] is the master — but its logical index inside the streams array
    // is used by the callback; masterSpecIdx maps to streams[0] after this call.
    err = Pa_StartStream(m_impl->streams.back()->stream);
    if (err != paNoError) {
        m_impl->lastError = Pa_GetErrorText(err);
        closeAllStreams(m_impl.get()); Pa_Terminate(); return false;
    }

    // Open and start each slave.
    for (int i = 0; i < n; ++i) {
        if (i == masterSpecIdx) continue;
        const auto& spec = devices[i];
        if (openOneStream(m_impl.get(), sampleRate, spec.channelCount, spec.name,
                          /*masterClock=*/false, spec.bufferSize) < 0) {
            closeAllStreams(m_impl.get()); Pa_Terminate(); return false;
        }
        auto& ds = *m_impl->streams.back();
        // Snapshot master clock so the slave can estimate engine time locally.
        ds.masterFrameAtZero = m_impl->enginePlayhead.load(std::memory_order_acquire);
        err = Pa_StartStream(ds.stream);
        if (err != paNoError) {
            m_impl->lastError = Pa_GetErrorText(err);
            closeAllStreams(m_impl.get()); Pa_Terminate(); return false;
        }
    }

    // Allocate per-output-channel DSP resources.
    {
        int totalOut = 0;
        for (const auto& ds : m_impl->streams) totalOut += ds->outChannels;
        m_impl->delayRings.assign(static_cast<size_t>(totalOut),
                                  std::vector<float>(kMaxDelaySamples, 0.0f));
        m_impl->delayWrPos.assign(static_cast<size_t>(totalOut), 0);
        m_impl->dspBuf[0].assign(static_cast<size_t>(totalOut), OutDspSlot{});
        m_impl->dspBuf[1].assign(static_cast<size_t>(totalOut), OutDspSlot{});
        m_impl->dspBufIdx.store(0, std::memory_order_relaxed);
        m_impl->chanPeakLevels = std::make_unique<std::atomic<float>[]>(kMaxChannels);
        for (int i = 0; i < kMaxChannels; ++i)
            m_impl->chanPeakLevels[static_cast<size_t>(i)].store(0.0f, std::memory_order_relaxed);
    }

    m_impl->initialized = true;
    m_impl->lastError.clear();
    return true;
}

// ---------------------------------------------------------------------------
void AudioEngine::shutdown() {
    closeAllStreams(m_impl.get());
    if (m_impl->initialized) { Pa_Terminate(); m_impl->initialized = false; }
    for (auto& v : m_impl->voices) {
        v.active.store(false, std::memory_order_relaxed);
        v.pendingReady.store(false, std::memory_order_relaxed);
    }
    m_impl->enginePlayhead.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
bool AudioEngine::isInitialized() const { return m_impl->initialized; }

int AudioEngine::sampleRate() const { return m_impl->sampleRate; }

int AudioEngine::channels() const {
    const int idx = m_impl->masterIdx();
    return (idx < (int)m_impl->streams.size())
        ? m_impl->streams[idx]->outChannels : 2;
}

// ---------------------------------------------------------------------------
int AudioEngine::scheduleVoice(const float* samples, int64_t totalFrames,
                                int voiceChannels, int tag, float gain, int deviceIndex) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = m_impl->voices[i];
        if (v.active.load(std::memory_order_relaxed))       continue;
        if (v.pendingReady.load(std::memory_order_relaxed)) continue;
        v.pending = {samples, nullptr, false, totalFrames, voiceChannels, tag, gain, deviceIndex, false};
        v.pendingReady.store(true, std::memory_order_release);
        return i;
    }
    return -1;
}

int AudioEngine::scheduleStreamingVoice(IAudioSource* reader, int64_t totalFrames,
                                         int voiceChannels, int tag, float gain,
                                         int deviceIndex, bool bypassChannelFold) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = m_impl->voices[i];
        if (v.active.load(std::memory_order_relaxed))       continue;
        if (v.pendingReady.load(std::memory_order_relaxed)) continue;
        v.pending = {nullptr, reader, true, totalFrames, voiceChannels,
                     tag, gain, deviceIndex, bypassChannelFold};
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
    const int idx = m_impl->masterIdx();
    return (idx < (int)m_impl->streams.size())
        ? m_impl->streams[idx]->renderedFrames.load(std::memory_order_relaxed) : 0;
}

const std::string& AudioEngine::lastError() const { return m_impl->lastError; }

void AudioEngine::setChannelDsp(std::vector<ChannelDsp> config) {
    const int inactive = 1 - m_impl->chanDspBufIdx.load(std::memory_order_relaxed);
    const int n = static_cast<int>(config.size());
    m_impl->chanDspBuf[inactive].resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        m_impl->chanDspBuf[inactive][static_cast<size_t>(i)].phaseInvert  = config[static_cast<size_t>(i)].phaseInvert;
        m_impl->chanDspBuf[inactive][static_cast<size_t>(i)].delaySamples =
            std::min(config[static_cast<size_t>(i)].delaySamples, kMaxDelaySamples - 1);
    }
    m_impl->chanDspBufIdx.store(inactive, std::memory_order_release);
}

void AudioEngine::setOutputDsp(std::vector<OutputDsp> config) {
    // Write to the inactive buffer, then atomically swap the active index.
    const int inactive = 1 - m_impl->dspBufIdx.load(std::memory_order_relaxed);
    const int n = std::min(static_cast<int>(config.size()),
                           static_cast<int>(m_impl->dspBuf[inactive].size()));
    for (int i = 0; i < n; ++i) {
        m_impl->dspBuf[inactive][static_cast<size_t>(i)].phaseInvert  = config[static_cast<size_t>(i)].phaseInvert;
        m_impl->dspBuf[inactive][static_cast<size_t>(i)].delaySamples =
            std::min(config[static_cast<size_t>(i)].delaySamples, kMaxDelaySamples - 1);
    }
    // Zero out any trailing slots not covered by the new config.
    for (int i = n; i < (int)m_impl->dspBuf[inactive].size(); ++i)
        m_impl->dspBuf[inactive][static_cast<size_t>(i)] = OutDspSlot{};
    m_impl->dspBufIdx.store(inactive, std::memory_order_release);
}

void AudioEngine::setDeviceChannelFold(int deviceIdx, const float* fold, int numCh, int physCh) {
    if (deviceIdx < 0 || deviceIdx >= (int)m_impl->streams.size()) return;
    auto& ds = *m_impl->streams[static_cast<size_t>(deviceIdx)];

    // Write fold to inactive buffer and swap.
    const int inactive = 1 - ds.foldBufIdx.load(std::memory_order_relaxed);
    auto& fb = ds.foldBuf[inactive];
    fb.numCh  = numCh;
    fb.numPhys = physCh;
    fb.data.assign(fold, fold + numCh * physCh);
    ds.foldBufIdx.store(inactive, std::memory_order_release);

    // Grow chanBuf if needed (main thread only — callback only reads chanBufPtr atomically).
    if (numCh > ds.chanBufCapacity) {
        const int newCap = std::max(numCh, ds.chanBufCapacity * 2);
        auto* newBuf = new float[static_cast<size_t>(kMaxChanBufFrames) * static_cast<size_t>(newCap)]();
        float* old = ds.chanBufPtr.exchange(nullptr, std::memory_order_relaxed);
        if (old) m_impl->oldChanBufs.push_back(old);
        ds.chanBufCapacity = newCap;
        ds.chanBufPtr.store(newBuf, std::memory_order_release);
    }
    ds.chanBufNumCh.store(numCh, std::memory_order_release);
    m_impl->chanPeakLevelCount.store(numCh, std::memory_order_release);

    // Grow per-channel delay rings if numCh increased (main thread only).
    if (numCh > static_cast<int>(m_impl->chanDelayRings.size())) {
        m_impl->chanDelayRings.resize(static_cast<size_t>(numCh),
                                      std::vector<float>(kMaxDelaySamples, 0.0f));
        m_impl->chanDelayWrPos.resize(static_cast<size_t>(numCh), 0);
    }
}

std::vector<float> AudioEngine::takeChannelPeaks() {
    if (!m_impl->chanPeakLevels) return {};
    const int n = m_impl->chanPeakLevelCount.load(std::memory_order_relaxed);
    if (n <= 0) return {};
    std::vector<float> result(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i)
        result[static_cast<size_t>(i)] =
            m_impl->chanPeakLevels[static_cast<size_t>(i)].exchange(0.0f, std::memory_order_relaxed);
    return result;
}

} // namespace mcp
