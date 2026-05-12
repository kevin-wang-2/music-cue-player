#pragma once
#ifdef MCP_HAVE_VST3

#include "engine/plugin/AudioProcessor.h"
#include <functional>
#include <memory>
#include <string>

namespace mcp::plugin {

// Opaque impl — all VST3 SDK types stay out of this header.
struct VST3PluginAdapterImpl;

// Adapter that wraps a VST3 audio-effect class as an AudioProcessor.
//
// Limitations (first-spike):
//   - Mono or stereo main bus only (no sidechain / multi-bus).
//   - Block-accurate parameter automation via IEditController.
//   - State saved via IComponent::getState / IAudioProcessor::getState.
//   - Editor (IPlugView): createEditorView() returns a platform-native view
//     pointer (NSView* on macOS) for embedding in the host window.
//
// Thread safety:
//   prepare(), reset(), and all editor/parameter calls must be made from the
//   control thread.  process() is called from the audio thread.
class VST3PluginAdapter final : public AudioProcessor {
public:
    // Returns nullptr if the bundle cannot be loaded or the class is not found.
    static std::unique_ptr<VST3PluginAdapter> create(
        const std::string& bundlePath, int classIndex, int numChannels = 2);

    ~VST3PluginAdapter() override;

    // ── AudioProcessor ───────────────────────────────────────────────────────
    void prepare(const ProcessContext& context) override;
    void process(const AudioBlock& block, const EventBlock& events) override;
    void reset() override;
    void suspend() override;
    void resume()  override;

    int getLatencySamples() const override;
    int getTailSamples()    const override;

    const std::vector<ParameterInfo>& getParameters() const override;
    float getParameterValue          (const std::string& id) const override;
    void  setParameterValue          (const std::string& id, float value) override;
    float getNormalizedParameterValue(const std::string& id) const override;
    void  setNormalizedParameterValue(const std::string& id, float normalized) override;

    PluginState getState()                   const override;
    void        setState(const PluginState& state) override;

    // ── Metadata ─────────────────────────────────────────────────────────────
    const std::string& pluginId()    const;
    const std::string& displayName() const;

    // ── Native editor ────────────────────────────────────────────────────────
    // Returns the IPlugView attached to a parent NSView (macOS) / HWND (Win).
    // parentView must remain alive while the editor is open.
    // outW / outH are set to the preferred view size (0 if not reported).
    // Returns nullptr if no editor is available.
    void* createEditorView(void* parentView, int& outW, int& outH);
    // Detach and destroy the editor view (safe to call even if none was created).
    void destroyEditorView();

    // Register a callback that fires (on the calling thread) whenever a
    // parameter changes via the native editor UI.
    void startWatchingParameters(std::function<void()> onChanged);
    void stopWatchingParameters();

    // Set a callback invoked when the plugin requests a view resize via IPlugFrame.
    // Pass nullptr to clear. Must be called before createEditorView() to take effect.
    void setResizeCallback(std::function<void(int w, int h)> cb);

    // ── Bypass bridging ──────────────────────────────────────────────────────
    void setNativeBypass(bool bypass);
    bool getNativeBypass() const;

private:
    explicit VST3PluginAdapter();
    std::unique_ptr<VST3PluginAdapterImpl> m_impl;
};

} // namespace mcp::plugin
#endif // MCP_HAVE_VST3
