#pragma once
#ifdef __APPLE__

#include <cstdint>
#include <string>
#include <vector>

namespace mcp::plugin {

// One discovered AU effect / music-effect component.
// Populated entirely from the macOS component registry — no instantiation required.
struct AUComponentEntry {
    uint32_t    type{0};
    uint32_t    subtype{0};
    uint32_t    manufacturer{0};

    std::string name;              // plugin display name, e.g. "Limiter"
    std::string manufacturerName;  // vendor, e.g. "Apple"
    std::string version;           // "major.minor[.bugfix]" — empty if unavailable
    std::string typeLabel;         // "Effect" | "Music Effect"

    // True only for confirmed AUv3 (kAudioComponentFlag_IsV3AudioUnit set).
    // AUv2 plugins: false means "unknown", not "no editor".
    bool supportsEditor{false};
};

// Enumerates all system-registered 'aufx' (effect) and 'aumf' (music effect)
// AU components using AudioComponentFindNext.  Never instantiates any plugin.
// Enumeration failures are silently skipped; returns whatever was found.
class AUComponentEnumerator {
public:
    static std::vector<AUComponentEntry> enumerate();
};

} // namespace mcp::plugin
#endif // __APPLE__
