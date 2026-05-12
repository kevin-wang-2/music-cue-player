#pragma once
#include "engine/plugin/PluginRuntimeStatus.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mcp::plugin {

// A fully self-contained, serialisable description of one external plugin slot.
//
// This struct travels between three layers:
//   ShowFile  ←→  ExternalPluginReference  ←→  NativePluginBackend
//
// The ShowFile layer flattens the fields into PluginSlot (no circular include).
// NativePluginBackend uses it as the only input to create an AudioProcessor.
//
// State storage strategy (belt-and-suspenders):
//   stateBlob     — primary; native binary (AU ClassInfo plist, future VST3 chunk).
//                   Opaque; round-trip fidelity is backend-dependent.
//   paramSnapshot — fallback; normalized [0,1] per paramId.
//                   Used when stateBlob is absent or fails to restore.
//                   paramId is the stable numeric string, NOT a display name.
struct ExternalPluginReference {
    std::string backend;            // "au", "vst3"
    std::string pluginId;           // stable ID, e.g. "au:aufx/lmtr/appl"
    std::string path;               // VST3: absolute path to .vst3 bundle; AU: empty
    std::string name;               // display name (for UI and error messages)
    std::string vendor;             // manufacturer / vendor name
    std::string version;            // plugin version string (informational)
    int         numChannels{2};     // 1 = mono, 2 = stereo

    // Primary saved state: native binary blob.
    // May be empty if the plugin has never been successfully initialized.
    std::vector<uint8_t> stateBlob;

    // Fallback normalized parameter snapshot.
    // Keys are stable paramIds; values are in [0,1].
    std::map<std::string, float> paramSnapshot;

    // ── Runtime-only (not serialised) ────────────────────────────────────────
    PluginRuntimeStatus runtimeStatus {PluginRuntimeStatus::Ok};
    std::string         runtimeMessage;
};

} // namespace mcp::plugin
