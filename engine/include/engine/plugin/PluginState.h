#pragma once
#include "PluginDescriptor.h"   // for PluginBackend
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp::plugin {

// Serialized state for one processor instance.
//
// Internal processors encode their state as a UTF-8 JSON string stored in
// stateData.  Future VST3/AU processors may store an opaque binary blob.
//
// The optional `parameters` map carries a plain float snapshot; setState()
// implementations should prefer stateData but may fall back to parameters for
// parameters not present in stateData.  Unknown keys in either representation
// must be silently ignored — never crash on unexpected input.
struct PluginState {
    std::string   pluginId;
    PluginBackend backend {PluginBackend::Internal};
    int           version {1};

    // UTF-8 JSON for internal; binary blob for external.
    std::vector<uint8_t> stateData;

    // Flat parameter snapshot (supplementary).
    std::unordered_map<std::string, float> parameters;
};

} // namespace mcp::plugin
