#pragma once
#ifdef __APPLE__

#include "engine/plugin/AudioProcessor.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace mcp::plugin {

// Opaque implementation struct — defined in AUPluginAdapter.mm
struct AUPluginAdapterImpl;

// Adapter that wraps a macOS AudioUnit v2 (Component) effect as an AudioProcessor.
//
// ── First-spike limitations ──────────────────────────────────────────────────
//   - Mono or stereo main bus only; no sidechain, no multi-bus.
//   - Block-accurate parameter automation.  AUv2 has no sample-accurate event
//     delivery; ParameterEvents are applied at block start.
//   - State saved via kAudioUnitProperty_ClassInfo (binary plist).  Not all AUs
//     guarantee perfect round-trip fidelity; setParameterValue fallback is used
//     when plist restore fails.
//   - getLatencySamples() / getTailSamples() read the AU property once after
//     prepare(); full PDC compensation is not implemented.
//   - No GUI/editor.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//   prepare() and reset() must be called from the control thread.
//   process() must be called from the audio thread.
class AUPluginAdapter final : public AudioProcessor {
public:
    struct Descriptor {
        uint32_t type;              // Component type        (e.g. 'aufx')
        uint32_t subtype;           // Component subtype
        uint32_t manufacturer;      // Manufacturer four-char-code
        int      numChannels{2};    // 1 = mono, 2 = stereo
    };

    // Returns nullptr if the component cannot be found or instantiated.
    static std::unique_ptr<AUPluginAdapter> create(const Descriptor& desc);
    ~AUPluginAdapter() override;

    // ── AudioProcessor ───────────────────────────────────────────────────────
    void prepare(const ProcessContext& context) override;
    void process(const AudioBlock& block, const EventBlock& events) override;
    void reset() override;

    void suspend() override;
    void resume()  override;

    int getLatencySamples() const override;
    int getTailSamples()    const override;

    const std::string& pluginId()    const;
    const std::string& displayName() const;

    const std::vector<ParameterInfo>& getParameters() const override;
    float getParameterValue          (const std::string& id) const override;
    void  setParameterValue          (const std::string& id, float value) override;
    float getNormalizedParameterValue(const std::string& id) const override;
    void  setNormalizedParameterValue(const std::string& id, float normalized) override;

    PluginState getState()                    const override;
    void        setState(const PluginState& state)  override;

    // Returns the AU's native Cocoa NSView* as void* (non-owning).
    // The view is retained internally and lives as long as this adapter.
    // outW/outH are set to the view's preferred size (may be 0 if not reported).
    // Returns nullptr if the AU has no Cocoa UI.
    void* createCocoaView(int& outW, int& outH);

    // Register an AUEventListener for all parameters.  `onChanged` is called
    // (on the main thread) whenever the AU's parameter values change — e.g. when
    // the user moves a knob in the native AU editor.  Call stopWatchingParameters()
    // to unregister (automatically suppressed during notifyViewRefresh()).
    void startWatchingParameters(std::function<void()> onChanged);
    void stopWatchingParameters();

    // Re-read all current AU parameter values and re-send them via
    // AudioUnitSetParameter so the AU editor NSView refreshes its knobs/sliders.
    // Does NOT fire the onChanged callback (suppressed internally).
    void notifyViewRefresh();

    // ── Bypass bridging ──────────────────────────────────────────────────────
    // Sync the AU's native kAudioUnitProperty_BypassEffect with the host bypass.
    // Call setNativeBypass() whenever our PluginWrapper::setBypassed() changes.
    void setNativeBypass(bool bypass);
    bool getNativeBypass() const;

    // Watch for kAudioUnitProperty_BypassEffect changes originating from within
    // the plugin (e.g. a bypass button in the native editor).  The callback fires
    // on the main thread.  Call with nullptr / stopWatchingBypass() to unregister.
    void startWatchingBypass(std::function<void(bool bypassed)> onChanged);
    void stopWatchingBypass();

    // Watch for kAudioUnitProperty_Latency changes (e.g. when an AU changes its
    // internal buffer size).  The callback fires on the main thread so the host
    // can recompute PDC.  Call stopWatchingLatency() to unregister.
    void startWatchingLatency(std::function<void()> onChanged);
    void stopWatchingLatency();

private:
    explicit AUPluginAdapter();
    std::unique_ptr<AUPluginAdapterImpl> m_impl;
};

} // namespace mcp::plugin
#endif // __APPLE__
