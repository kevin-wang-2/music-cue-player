#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mcp { class IAudioSource; }  // forward-declare to avoid circular includes

namespace mcp {

struct AudioEngineImpl;

struct DeviceInfo {
    int         index{-1};
    std::string name;
    int         maxOutputChannels{0};
};

// Low-level audio output engine. Manages a PortAudio stream and a pool of
// kMaxVoices voice slots. All active voices are summed into the output each
// callback.
//
// Two playhead concepts are maintained:
//
//   bufferPlayhead  — this device's raw sample counter, incremented by the
//                     audio callback on every buffer. Device-local; not
//                     directly comparable across devices.
//
//   enginePlayhead  — the authoritative system clock shared by all devices.
//                     Updated here because this is currently the only (master)
//                     device. With multiple devices, only the master's callback
//                     would write this; slaves would read it and derive their
//                     offset against their own bufferPlayhead.
//
// All voice start times and position calculations are expressed in engine time.
// Because we have a single device today, enginePlayhead == bufferPlayhead, but
// the public API and internal storage already use the distinction so that
// adding a second device later only requires plumbing — not a redesign.
//
// Thread model:
//   Main thread  — scheduleVoice / clearVoice / clearVoicesByTag / clearAllVoices
//   Audio thread — callback: applies pending changes, mixes, advances playheads
class AudioEngine {
public:
    static constexpr int kMaxVoices = 128;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // List all PortAudio output devices. Safe to call before initialize().
    // Returns empty vector if PortAudio cannot be initialized.
    static std::vector<DeviceInfo> listOutputDevices();

    // channels = 0 → auto-detect from the selected (or default) output device.
    // deviceName = "" → use default PortAudio output device.
    bool initialize(int sampleRate = 48000, int channels = 0,
                    const std::string& deviceName = "");

    // Multi-device initialisation.  Each DeviceSpec describes one PortAudio
    // output stream.  Exactly one entry should have masterClock=true (the one
    // that drives enginePlayhead); if none is marked the first is used.
    // All streams share the same sample rate; buffer sizes may differ.
    struct DeviceSpec {
        std::string name;          // PortAudio device name; "" = default output
        int         channelCount{0};  // 0 = auto-detect from device
        int         bufferSize{0};    // 0 = let PortAudio choose
        bool        masterClock{false};
    };
    bool initialize(int sampleRate, const std::vector<DeviceSpec>& devices);

    void shutdown();
    bool isInitialized() const;

    int sampleRate() const;
    int channels() const;

    // --- Voice pool ---------------------------------------------------------

    // Schedule an in-memory voice. Returns slot ID or -1 if pool is full.
    // The caller must keep `samples` alive until the voice finishes.
    // `tag`         — arbitrary identifier (e.g., cue index) used for bulk operations.
    // `gain`        — linear amplitude gain applied every sample (1.0 = unity).
    // `deviceIndex` — which stream (device) renders this voice; 0 = master/default.
    int scheduleVoice(const float* samples, int64_t totalFrames,
                      int voiceChannels, int tag = -1, float gain = 1.0f,
                      int deviceIndex = 0);

    // Schedule a streaming voice backed by any IAudioSource.
    // The raw pointer must remain valid (kept alive externally) until
    // isVoiceActive(slot) returns false.
    // `deviceIndex`        — which stream (device) renders this voice; 0 = master/default.
    // `bypassChannelFold`  — when true, voice mixes directly to physical out[] bypassing
    //                        the channel bus (use for LTC and other special-purpose voices).
    int scheduleStreamingVoice(IAudioSource* reader, int64_t totalFrames,
                               int voiceChannels, int tag = -1, float gain = 1.0f,
                               int deviceIndex = 0, bool bypassChannelFold = false);

    // Update the gain of a running voice. Safe to call from the main thread
    // while the audio thread is running (atomic store, no locking).
    void setVoiceGain(int slotId, float gain);

    void clearVoice(int slotId);
    void clearVoicesByTag(int tag);
    void clearAllVoices();

    // Linearly fade all active voices to silence over durationSeconds, then
    // clear them.  Subsequent voices scheduled while fading are not affected.
    void softPanic(double durationSeconds = 0.5);

    // --- Queries (safe to call from any thread) ----------------------------

    bool isVoiceActive(int slotId) const;
    // True while the voice is scheduled but the callback hasn't activated it yet.
    bool isVoicePending(int slotId) const;
    bool anyVoiceActiveWithTag(int tag) const;
    int  activeVoiceCount() const;

    // Engine frame at which a slot was last activated (engine-time coordinate).
    // Undefined if the slot has never been used.
    int64_t voiceStartEngineFrame(int slotId) const;

    // Master system clock — all callers that need "current time" should use this.
    int64_t enginePlayheadFrames() const;
    double  enginePlayheadSeconds() const;

    // This device's raw sample counter. Same value as enginePlayheadFrames()
    // for the single-device case; exposed separately for future multi-device use.
    int64_t bufferPlayheadFrames() const;

    // --- Output DSP ---------------------------------------------------------

    // DSP applied per logical channel, post-chanBuf-mix, pre-fold, in the audio callback.
    // `config[i]` applies to logical channel i.
    struct ChannelDsp {
        bool phaseInvert{false};
        int  delaySamples{0};    // 0 = no delay; clamped to kMaxDelaySamples-1
    };
    void setChannelDsp(std::vector<ChannelDsp> config);

    // DSP applied per physical output channel, post-fold, in the audio callback.
    // `config[i]` applies to global output channel i (sequential across streams).
    // Can be called at any time; takes effect within one callback buffer.
    struct OutputDsp {
        bool phaseInvert{false};
        int  delaySamples{0};    // 0 = no delay; clamped to kMaxDelaySamples-1
    };
    void setOutputDsp(std::vector<OutputDsp> config);

    // Push a channel-fold matrix to one device stream.
    // fold[ch * physCh + p] = linear gain from logical channel ch to device-local physical output p.
    // numCh must equal the number of logical channels in the channel map.
    // Allocates/grows the per-device chanBuf as needed (main thread only).
    void setDeviceChannelFold(int deviceIdx, const float* fold, int numCh, int physCh);

    // Poll and reset per-logical-channel peak amplitudes measured from the
    // channel bus (chanBuf), pre-fold.  Returns one entry per logical channel
    // (same count as numCh passed to setDeviceChannelFold).
    // Returns empty vector if the channel bus is not set up.
    std::vector<float> takeChannelPeaks();

    // Maximum number of logical channels supported for peak metering.
    static constexpr int kMaxChannels = 256;

    // Maximum supported delay in samples (power of 2 for fast ring indexing).
    static constexpr int kMaxDelaySamples = 131072;  // ~2.73 s at 48 kHz

    const std::string& lastError() const;

private:
    std::unique_ptr<AudioEngineImpl> m_impl;
};

} // namespace mcp
