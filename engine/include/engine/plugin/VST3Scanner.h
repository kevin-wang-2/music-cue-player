#pragma once
#ifdef MCP_HAVE_VST3

#include <string>
#include <vector>

namespace mcp::plugin {

// One discovered VST3 audio-effect class inside a .vst3 bundle.
struct VST3Entry {
    std::string name;       // display name, e.g. "Pro-L 2"
    std::string vendor;     // vendor / manufacturer
    std::string version;    // version string, may be empty
    std::string path;       // absolute path to the .vst3 bundle directory
    std::string pluginId;   // "vst3:<32-hex-char UID>"
    int         classIndex{0};  // index within the bundle's IPluginFactory
};

// Scans filesystem directories for .vst3 bundles and enumerates audio-effect
// classes by loading each bundle's factory momentarily (no plugin initialisation).
//
// Thread safety: scan() may be called from any thread; do not call concurrently.
class VST3Scanner {
public:
    // Platform-default .vst3 search directories.
    // macOS: /Library/Audio/Plug-Ins/VST3 + ~/Library/Audio/Plug-Ins/VST3
    // Windows: C:\Program Files\Common Files\VST3 + user-local
    // Linux: /usr/lib/vst3 + ~/.vst3
    static std::vector<std::string> defaultPaths();

    // Scan all dirs for .vst3 bundles.  Each bundle is loaded, its factory
    // queried for audio-effect classes, then unloaded.  Bundles that crash
    // or fail to export GetPluginFactory are silently skipped.
    static std::vector<VST3Entry> scan(const std::vector<std::string>& dirs);

    // Fast filesystem walk — no dlopen.  Returns absolute paths to all
    // .vst3 bundle directories found under the given search dirs.
    static std::vector<std::string> findBundles(const std::vector<std::string>& dirs);

    // Load one bundle, return its VST3Entry list (empty on error).
    // Public so workers can call it per-bundle from a background thread.
    static std::vector<VST3Entry> scanBundle(const std::string& bundlePath);
};

} // namespace mcp::plugin
#endif // MCP_HAVE_VST3
