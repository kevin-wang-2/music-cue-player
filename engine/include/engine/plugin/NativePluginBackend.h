#pragma once
#include "engine/plugin/AudioProcessor.h"
#include "engine/plugin/ExternalPluginReference.h"
#include <cstdint>
#include <memory>

namespace mcp::plugin {

// Backend factory for external native plugins (AUv2 on macOS; VST3 reserved).
//
// Design contract:
//   1. AU / VST3 types must NOT appear in any header included by the main engine.
//      NativePluginBackend is the only ABI boundary that knows about them.
//   2. load() always returns a valid AudioProcessor — either the real plugin or a
//      MissingPluginProcessor.  It never returns nullptr.
//   3. Consumers only see AudioProcessor / ExternalPluginReference /
//      PluginRuntimeStatus.
//
// State restoration on load():
//   - Tries stateBlob first (AU ClassInfo plist, VST3 chunk).
//   - Falls back to paramSnapshot (normalized) if stateBlob is absent or fails.
//   - StateRestoreFailed status is set if neither method succeeded but the plugin
//     otherwise loaded OK.
class NativePluginBackend {
public:
    NativePluginBackend() = default;

    // Load a plugin from a reference.  Always returns non-null.
    std::unique_ptr<AudioProcessor> load(const ExternalPluginReference& ref) const;

    // ── Manual reference construction (no scanning) ──────────────────────────

#ifdef __APPLE__
    // Build a reference from raw AU four-char codes.
    // Queries AudioComponent for display name if the plugin is present.
    // Sets runtimeStatus = Missing if not found.
    static ExternalPluginReference makeAUReference(
        uint32_t type, uint32_t subtype, uint32_t manufacturer,
        int numChannels = 2);
#endif

    // ── Runtime status query ─────────────────────────────────────────────────

    // Returns the runtime status of any processor produced by load().
    // Returns Ok for processors not produced by this backend (internal plugins).
    static PluginRuntimeStatus statusOf(const AudioProcessor& proc);

private:
    std::unique_ptr<AudioProcessor> loadAU  (const ExternalPluginReference& ref) const;
    std::unique_ptr<AudioProcessor> loadVST3(const ExternalPluginReference& ref) const;
};

} // namespace mcp::plugin
