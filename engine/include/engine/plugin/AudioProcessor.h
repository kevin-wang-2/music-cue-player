#pragma once
#include "AudioBlock.h"
#include "EventBlock.h"
#include "ParameterInfo.h"
#include "PluginState.h"
#include "ProcessContext.h"
#include <string>
#include <vector>

namespace mcp::plugin {

// Pure virtual interface for all audio processors (internal and future external).
//
// Lifecycle:
//   prepare()  → process() × N  → [reset()]  → [prepare() again]
//
// Thread safety: the host calls prepare/reset from the control thread and
// process() from the audio thread.  Implementations must not hold shared
// locks across both threads.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    // Called before the first process() and whenever sample rate / block size
    // changes.  May allocate buffers.  Not called from the audio thread.
    virtual void prepare(const ProcessContext& context) = 0;

    // Realtime-safe audio callback.  Must not: allocate heap memory, acquire
    // locks, perform I/O, or access UI objects.
    virtual void process(const AudioBlock& block, const EventBlock& events) = 0;

    // Clear internal state (ring buffers, envelopes, …) without re-preparing.
    // Not called from the audio thread.
    virtual void reset() = 0;

    // Optional lifecycle hooks for external plugin hosts.
    virtual void suspend() {}
    virtual void resume()  {}

    // Algorithmic latency introduced by this processor, in samples.
    virtual int getLatencySamples() const { return 0; }

    // Tail length after audio input ceases (e.g. reverb tail).  0 = no tail.
    virtual int getTailSamples() const { return 0; }

    // ── Parameter access ─────────────────────────────────────────────────────
    // Not required to be realtime-safe; intended for control-thread use.

    virtual const std::vector<ParameterInfo>& getParameters() const = 0;

    virtual float getParameterValue          (const std::string& id) const = 0;
    virtual void  setParameterValue          (const std::string& id, float value) = 0;
    virtual float getNormalizedParameterValue(const std::string& id) const = 0;
    virtual void  setNormalizedParameterValue(const std::string& id, float normalized) = 0;

    // ── State serialization ───────────────────────────────────────────────────

    virtual PluginState getState() const = 0;
    virtual void        setState(const PluginState& state) = 0;
};

} // namespace mcp::plugin
