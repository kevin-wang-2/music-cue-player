#include "engine/plugin/NativePluginBackend.h"
#include "engine/plugin/MissingPluginProcessor.h"
#include "engine/plugin/PluginState.h"

#ifdef __APPLE__
#  include "engine/plugin/AUPluginAdapter.h"
#endif
#ifdef MCP_HAVE_VST3
#  include "engine/plugin/VST3PluginAdapter.h"
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#ifdef MCP_HAVE_VST3
#  include "pluginterfaces/base/ipluginbase.h"
#  if defined(__APPLE__) || defined(__linux__)
#    include <dlfcn.h>
#  elif defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#  endif
#endif

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

    // Restore state: blob takes priority; params are fallback when no blob.
    // Never pass both together: setState() now applies params after the blob,
    // but the paramSnapshot here contains raw AU values stored in the show file
    // which would wrongly override the blob-restored state.
    if (!ref.stateBlob.empty()) {
        PluginState st;
        st.pluginId  = ref.pluginId;
        st.backend   = PluginBackend::AU;
        st.version   = 1;
        st.stateData = ref.stateBlob;
        adapter->setState(st);
    } else if (!ref.paramSnapshot.empty()) {
        PluginState st;
        st.pluginId = ref.pluginId;
        st.backend  = PluginBackend::AU;
        st.version  = 1;
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

// ── VST3 loading ──────────────────────────────────────────────────────────────

std::unique_ptr<AudioProcessor> NativePluginBackend::loadVST3(
        const ExternalPluginReference& ref) const
{
#ifndef MCP_HAVE_VST3
    return makeMissing(ref, PluginRuntimeStatus::Missing,
                       "VST3 support was not compiled in");
#else
    if (ref.path.empty())
        return makeMissing(ref, PluginRuntimeStatus::Failed,
                           "VST3 plugin has no bundle path: " + ref.pluginId);

    // Parse class index from pluginId "vst3:<hex>" — we need the classIndex
    // which is stored in the ref (via VST3Entry.classIndex → extNumChannels field
    // encodes it; we re-scan to find it).  Simpler: try classIndex 0..15 and
    // match by UID stored in pluginId.
    const std::string uid = (ref.pluginId.size() > 5)
                            ? ref.pluginId.substr(5) : "";  // strip "vst3:"

    // Try loading from classIndex embedded in ref (extNumChannels is channels,
    // not the index).  We scan the bundle to find the matching class.
    int classIndex = 0;
#  if defined(__APPLE__) || defined(__linux__)
    {
        // Quick scan to find correct classIndex by UID match
        std::string binPath = ref.path;
        const std::string name =
            std::filesystem::path(ref.path).stem().string();
#    ifdef __APPLE__
        binPath = ref.path + "/Contents/MacOS/" + name;
#    else
        binPath = ref.path + "/Contents/x86_64-linux/" + name + ".so";
#    endif
        void* h = nullptr;
        if (std::filesystem::exists(binPath))
            h = dlopen(binPath.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            using FactFn = Steinberg::IPluginFactory*(*)();
            auto* fn = (FactFn)dlsym(h, "GetPluginFactory");
            if (fn) {
                auto* fac = fn();
                if (fac) {
                    char hexbuf[33];
                    for (int ci = 0; ci < fac->countClasses(); ++ci) {
                        Steinberg::PClassInfo info;
                        if (fac->getClassInfo(ci, &info) != Steinberg::kResultOk) continue;
                        for (int k = 0; k < 16; ++k)
                            std::snprintf(hexbuf + k*2, 3, "%02x",
                                          static_cast<unsigned char>(info.cid[k]));
                        hexbuf[32] = '\0';
                        if (uid == hexbuf) { classIndex = ci; break; }
                    }
                    fac->release();
                }
            }
            dlclose(h);
        }
    }
#  elif defined(_WIN32)
    {
        std::string binPath = ref.path + "/Contents/x86_64-win/" +
            std::filesystem::path(ref.path).stem().string() + ".vst3";
        HMODULE h = LoadLibraryA(binPath.c_str());
        if (h) {
            using FactFn = Steinberg::IPluginFactory*(*)();
            auto* fn = (FactFn)GetProcAddress(h, "GetPluginFactory");
            if (fn) {
                auto* fac = fn();
                if (fac) {
                    char hexbuf[33];
                    for (int ci = 0; ci < fac->countClasses(); ++ci) {
                        Steinberg::PClassInfo info;
                        if (fac->getClassInfo(ci, &info) != Steinberg::kResultOk) continue;
                        for (int k = 0; k < 16; ++k)
                            std::snprintf(hexbuf + k*2, 3, "%02x",
                                          static_cast<unsigned char>(info.cid[k]));
                        hexbuf[32] = '\0';
                        if (uid == hexbuf) { classIndex = ci; break; }
                    }
                    fac->release();
                }
            }
            FreeLibrary(h);
        }
    }
#  endif

    auto adapter = VST3PluginAdapter::create(ref.path, classIndex, ref.numChannels);
    if (!adapter)
        return makeMissing(ref, PluginRuntimeStatus::Missing,
                           "Failed to load VST3 plugin: " + ref.path);

    // Restore state
    if (!ref.stateBlob.empty() || !ref.paramSnapshot.empty()) {
        PluginState st;
        st.pluginId  = ref.pluginId;
        st.backend   = PluginBackend::VST3;
        st.version   = 1;
        st.stateData = ref.stateBlob;
        for (const auto& [id, norm] : ref.paramSnapshot)
            st.parameters[id] = norm;
        adapter->setState(st);
    }

    return adapter;
#endif
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
