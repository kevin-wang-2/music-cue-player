#pragma once
#include "AudioBlock.h"
#include "AudioProcessor.h"
#include "EventBlock.h"
#include "PluginState.h"
#include "ProcessContext.h"
#include <memory>
#include <vector>

namespace mcp::plugin {

// Wraps an AudioProcessor with host-level conveniences: bypass, deactivation,
// input/output gain staging.  process() is realtime-safe: no allocation, no
// locking.
//
// ── Bypass ───────────────────────────────────────────────────────────────────
// Native bypass (AU; VST3 with kIsBypass param):
//   setNativeBypass() queues the param; process() is still called every block
//   so the plugin handles the transition and any reverb tail itself.
//
// Host bypass (VST3 without kIsBypass, internal DSP):
//   A per-channel ring buffer (sized to getLatencySamples()) routes dry audio
//   through the same delay as the plugin so PDC alignment is preserved.
//   A short crossfade (kXfadeLen samples) on every bypass state change
//   eliminates the click that would otherwise occur on high-latency plugins.
//
// ── Deactivation ─────────────────────────────────────────────────────────────
// deactivate(): destroys the plugin instance (CPU/memory savings) while keeping
//   the PDC ring alive.  Dry audio continues to pass through the correct delay.
// reactivate(): accepts a new processor instance and crossfades back to it
//   without resetting the ring, so the PDC signal path is never interrupted.
class PluginWrapper {
public:
    enum class Status { Idle, Prepared, Error };

    // Takes ownership of processor; stored internally as shared_ptr.
    explicit PluginWrapper(std::unique_ptr<AudioProcessor> processor);

    // Control-thread methods.
    void prepare(const ProcessContext& context);
    void reset();

    // Realtime-safe.
    void process(const AudioBlock& block, const EventBlock& events);

    // Bypass: plugin instance stays loaded; processing is skipped (host bypass)
    // or delegated to the plugin (native bypass).
    bool isBypassed()    const { return m_bypassed; }
    void setBypassed(bool v);

    // Deactivation: releases the plugin instance while the PDC ring is preserved.
    // Call getProcessor()->getState() before deactivating to save state for reload.
    // After deactivate(), dry audio passes through the ring at the correct delay.
    void deactivate();

    // Restore a new plugin instance.  The processor is prepared with the last
    // context and a crossfade from the ring back to the plugin output is started.
    // Apply the saved PluginState to the new processor before or after this call.
    void reactivate(std::unique_ptr<AudioProcessor> processor);

    bool isDeactivated() const { return m_deactivated; }

    // State saved at deactivate() time so reactivate() can restore it.
    // Also used by AppModel::syncPluginStatesToShowFile() to keep the show
    // file accurate for deactivated slots.
    const PluginState& savedState() const { return m_savedState; }

    // Gain staging (dB).  Default 0 dB (unity).
    float getInputGainDb()  const;
    void  setInputGainDb (float db);
    float getOutputGainDb() const;
    void  setOutputGainDb(float db);

    int    getLatencySamples() const;
    Status getStatus()         const { return m_status; }

    // Direct access to the wrapped processor for parameter/state operations.
    // Returns nullptr when deactivated.
    AudioProcessor*                    getProcessor()       const { return m_processor.get(); }
    std::shared_ptr<AudioProcessor>    getProcessorShared() const { return m_processor; }

private:
    static float dbToLinear(float db);

    // Routes dry audio through the PDC ring (or direct copy if zero latency)
    // and writes the result to block.outputs.  Called from process() for the
    // bypass and deactivated paths.
    void computeBypassInto(const AudioBlock& block);

    // Ensures the PDC ring and output scratch are allocated with at least
    // latencyHint samples depth and numOutCh channels.  Idempotent if already
    // sized correctly; does not clear existing ring content.
    void ensureRingAndScratch(int latencyHint, int numOutCh);

    std::shared_ptr<AudioProcessor> m_processor;
    Status m_status          {Status::Idle};
    bool   m_bypassed        {false};
    bool   m_deactivated     {false};
    PluginState m_savedState; // snapshot taken in deactivate(), restored in reactivate()
    float  m_inputGainLinear {1.0f};
    float  m_outputGainLinear{1.0f};
    int    m_numInputChannels {0};
    int    m_numOutputChannels{0};
    int    m_maxBlockSize     {0};

    // Last context passed to prepare(), reused by reactivate().
    ProcessContext m_lastContext;

    // Pre-allocated scratch buffers for input gain scaling.
    std::vector<float>        m_inputScratchData;
    std::vector<float*>       m_inputScratchPtrs;
    std::vector<const float*> m_inputConstPtrs;

    bool m_hasNativeBypass{false};  // set in prepare(); drives processing strategy

    // PDC ring: per-channel circular buffer, length = plugin latency in samples.
    // Used for host-bypass and deactivated paths.  Always preserved across
    // bypass transitions and deactivate/reactivate to avoid PDC gaps.
    std::vector<std::vector<float>> m_bypassRings;
    int m_bypassRingLen {0};
    int m_bypassRingHead{0};

    // Crossfade state: eliminates click on bypass/deactivation transitions.
    // m_xfadePos >= 0 while a crossfade is in progress.
    // m_xfadeToBypass: true = active→bypass/deactivated; false = back to active.
    int  m_xfadePos     {-1};
    bool m_xfadeToBypass{false};

    // Per-channel output scratch for crossfade blending (plugin output side).
    std::vector<float>  m_outputScratchData;
    std::vector<float*> m_outputScratchPtrs;
};

} // namespace mcp::plugin
