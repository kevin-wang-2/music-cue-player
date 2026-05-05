#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mcp {

struct AudioEngineImpl;

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
    static constexpr int kMaxVoices = 64;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool initialize(int sampleRate = 48000, int channels = 2);
    void shutdown();
    bool isInitialized() const;

    int sampleRate() const;
    int channels() const;

    // --- Voice pool ---------------------------------------------------------

    // Schedule a voice in the first free slot. Returns the slot ID [0, kMaxVoices)
    // or -1 if the pool is full. The caller must keep `samples` alive until the
    // voice finishes or is explicitly cleared.
    // `tag` is an arbitrary identifier (e.g., cue index) used for bulk operations.
    int scheduleVoice(const float* samples, int64_t totalFrames,
                      int voiceChannels, int tag = -1);

    void clearVoice(int slotId);
    void clearVoicesByTag(int tag);
    void clearAllVoices();

    // --- Queries (safe to call from any thread) ----------------------------

    bool isVoiceActive(int slotId) const;
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

    const std::string& lastError() const;

private:
    std::unique_ptr<AudioEngineImpl> m_impl;
};

} // namespace mcp
