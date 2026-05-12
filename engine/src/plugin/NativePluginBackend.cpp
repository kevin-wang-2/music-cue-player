#include "engine/plugin/NativePluginBackend.h"
#include "engine/plugin/MissingPluginProcessor.h"
#include "engine/plugin/PluginState.h"

#ifdef __APPLE__
#  include "engine/plugin/AUPluginAdapter.h"
#endif

#include <cstring>
#include <string>

namespace mcp::plugin {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<MissingPluginProcessor> makeMissing(
        const ExternalPluginReference& ref,
        PluginRuntimeStatus reason,
        const std::string& msg)
{
    ExternalPluginReference r = ref;
    r.runtimeStatus  = reason;
    r.runtimeMessage = msg;
    return std::make_unique<MissingPluginProcessor>(std::move(r), reason, msg);
}

// ── AU loading ────────────────────────────────────────────────────────────────

std::unique_ptr<AudioProcessor> NativePluginBackend::loadAU(
        const ExternalPluginReference& ref) const
{
#ifndef __APPLE__
    return makeMissing(ref, PluginRuntimeStatus::Missing,
                       "AU plugins are only available on macOS");
#else
    // Parse "au:TTTT/SSSS/MMMM" → Descriptor four-char codes
    if (ref.pluginId.size() < 15 || ref.pluginId.compare(0, 3, "au:") != 0)
        return makeMissing(ref, PluginRuntimeStatus::Failed,
                           "Cannot parse AU plugin ID: " + ref.pluginId);

    const std::string fcc = ref.pluginId.substr(3); // "TTTT/SSSS/MMMM"
    const size_t s1 = fcc.find('/');
    const size_t s2 = (s1 != std::string::npos) ? fcc.find('/', s1 + 1) : std::string::npos;
    if (s1 == std::string::npos || s2 == std::string::npos ||
        s1 != 4 || (s2 - s1 - 1) != 4 || (fcc.size() - s2 - 1) != 4)
        return makeMissing(ref, PluginRuntimeStatus::Failed,
                           "Malformed AU plugin ID: " + ref.pluginId);

    auto toFCC = [](const std::string& s) -> uint32_t {
        return (static_cast<uint32_t>(s[0]) << 24) |
               (static_cast<uint32_t>(s[1]) << 16) |
               (static_cast<uint32_t>(s[2]) <<  8) |
                static_cast<uint32_t>(s[3]);
    };
    AUPluginAdapter::Descriptor desc{
        toFCC(fcc.substr(0, 4)),
        toFCC(fcc.substr(5, 4)),
        toFCC(fcc.substr(10, 4)),
        ref.numChannels
    };

    auto adapter = AUPluginAdapter::create(desc);
    if (!adapter)
        return makeMissing(ref, PluginRuntimeStatus::Missing,
                           "AudioComponent not found: " + ref.pluginId);

    // Restore state: try stateBlob first, fall back to paramSnapshot
    if (!ref.stateBlob.empty() || !ref.paramSnapshot.empty()) {
        PluginState st;
        st.pluginId  = ref.pluginId;
        st.backend   = PluginBackend::AU;
        st.version   = 1;
        st.stateData = ref.stateBlob;

        // Denormalize snapshot entries into the parameters fallback map
        for (const auto& info : adapter->getParameters()) {
            auto it = ref.paramSnapshot.find(info.id);
            if (it != ref.paramSnapshot.end() && info.fromNormalized)
                st.parameters[info.id] = info.fromNormalized(it->second);
        }
        adapter->setState(st);
    }

    return adapter;
#endif
}

// ── VST3 loading (reserved, not implemented in MVP) ──────────────────────────

std::unique_ptr<AudioProcessor> NativePluginBackend::loadVST3(
        const ExternalPluginReference& ref) const
{
    // TODO(vst3-mvp): implement VST3 backend
    return makeMissing(ref, PluginRuntimeStatus::Missing,
                       "VST3 backend not yet implemented");
}

// ── load (dispatcher) ─────────────────────────────────────────────────────────

std::unique_ptr<AudioProcessor> NativePluginBackend::load(
        const ExternalPluginReference& ref) const
{
    if (ref.backend == "au")   return loadAU(ref);
    if (ref.backend == "vst3") return loadVST3(ref);
    return makeMissing(ref, PluginRuntimeStatus::Failed,
                       "Unknown plugin backend: " + ref.backend);
}

// ── makeAUReference ───────────────────────────────────────────────────────────

#ifdef __APPLE__
ExternalPluginReference NativePluginBackend::makeAUReference(
        uint32_t type, uint32_t subtype, uint32_t manufacturer, int numChannels)
{
    ExternalPluginReference ref;
    ref.backend     = "au";
    ref.numChannels = numChannels;

    auto fcc = [](uint32_t v, char* out) {
        out[0] = static_cast<char>((v >> 24) & 0xFF);
        out[1] = static_cast<char>((v >> 16) & 0xFF);
        out[2] = static_cast<char>((v >>  8) & 0xFF);
        out[3] = static_cast<char>( v        & 0xFF);
        out[4] = '\0';
    };
    char t[5], s[5], m[5];
    fcc(type, t); fcc(subtype, s); fcc(manufacturer, m);
    ref.pluginId = std::string("au:") + t + "/" + s + "/" + m;

    // Probe the adapter to obtain display name; destroy it after.
    AUPluginAdapter::Descriptor desc{type, subtype, manufacturer, numChannels};
    if (auto adapter = AUPluginAdapter::create(desc)) {
        ref.name             = adapter->displayName();
        ref.runtimeStatus    = PluginRuntimeStatus::Ok;
    } else {
        ref.runtimeStatus    = PluginRuntimeStatus::Missing;
        ref.runtimeMessage   = "AudioComponent not found for " + ref.pluginId;
    }

    return ref;
}
#endif

// ── statusOf ─────────────────────────────────────────────────────────────────

PluginRuntimeStatus NativePluginBackend::statusOf(const AudioProcessor& proc) {
    if (const auto* mp = dynamic_cast<const MissingPluginProcessor*>(&proc))
        return mp->runtimeStatus();
    return PluginRuntimeStatus::Ok;
}

} // namespace mcp::plugin
