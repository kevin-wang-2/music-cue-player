#include "engine/plugin/VST3Scanner.h"
#ifdef MCP_HAVE_VST3

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#  include <dlfcn.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace mcp::plugin {

using GetPluginFactoryFn = Steinberg::IPluginFactory*(*)();

// ── Platform: open/close library + get factory ───────────────────────────────

// Scanner only needs GetPluginFactory() to read metadata — plain dlopen suffices.
// CFBundle + bundleEntry is only needed in VST3PluginAdapter where the plugin
// is fully initialized and needs its own bundle for resource lookups.
static void* openLib(const std::string& libPath) {
#if defined(__APPLE__) || defined(__linux__)
    return dlopen(libPath.c_str(), RTLD_LAZY | RTLD_LOCAL);
#elif defined(_WIN32)
    return (void*)LoadLibraryA(libPath.c_str());
#else
    return nullptr;
#endif
}

static void closeLib(void* handle) {
    if (!handle) return;
#if defined(__APPLE__) || defined(__linux__)
    dlclose(handle);
#elif defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#endif
}

static GetPluginFactoryFn getFactoryFn(void* handle) {
    if (!handle) return nullptr;
#if defined(__APPLE__) || defined(__linux__)
    return (GetPluginFactoryFn)dlsym(handle, "GetPluginFactory");
#elif defined(_WIN32)
    return (GetPluginFactoryFn)GetProcAddress((HMODULE)handle, "GetPluginFactory");
#else
    return nullptr;
#endif
}

// ── Bundle binary path ────────────────────────────────────────────────────────

static std::string bundleBinaryPath(const std::string& bundlePath) {
    const std::string name =
        std::filesystem::path(bundlePath).stem().string();
#if defined(__APPLE__)
    return bundlePath + "/Contents/MacOS/" + name;
#elif defined(__linux__)
    return bundlePath + "/Contents/x86_64-linux/" + name + ".so";
#elif defined(_WIN32)
    return bundlePath + "/Contents/x86_64-win/" + name + ".vst3";
#else
    return {};
#endif
}

// ── TUID → hex string ─────────────────────────────────────────────────────────

static std::string tuidToHex(const Steinberg::TUID tuid) {
    char buf[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(buf + i * 2, 3, "%02x",
                      static_cast<unsigned char>(tuid[i]));
    buf[32] = '\0';
    return buf;
}

// ── scanBundle ────────────────────────────────────────────────────────────────

std::vector<VST3Entry> VST3Scanner::scanBundle(const std::string& bundlePath)
{
    std::vector<VST3Entry> out;

    const std::string binPath = bundleBinaryPath(bundlePath);
    if (binPath.empty() || !std::filesystem::exists(binPath))
        return out;

    void* handle = openLib(binPath);
    if (!handle) return out;

    auto factoryFn = getFactoryFn(handle);
    if (!factoryFn) { closeLib(handle); return out; }

    Steinberg::IPluginFactory* factory = factoryFn();
    if (!factory) { closeLib(handle); return out; }

    Steinberg::PFactoryInfo factoryInfo;
    if (factory->getFactoryInfo(&factoryInfo) != Steinberg::kResultOk) {
        factory->release();
        closeLib(handle);
        return out;
    }
    const std::string vendor = factoryInfo.vendor;

    const int count = factory->countClasses();
    for (int i = 0; i < count; ++i) {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(i, &ci) != Steinberg::kResultOk) continue;

        // Only enumerate audio effects (kVstAudioEffectClass)
        if (std::string(ci.category) != kVstAudioEffectClass) continue;

        VST3Entry entry;
        entry.name       = ci.name;
        entry.vendor     = vendor;
        entry.path       = bundlePath;
        entry.classIndex = i;
        entry.pluginId   = "vst3:" + tuidToHex(ci.cid);

        // Try extended factory for version string
        Steinberg::IPluginFactory2* factory2 = nullptr;
        if (factory->queryInterface(Steinberg::IPluginFactory2::iid,
                                    (void**)&factory2) == Steinberg::kResultOk
            && factory2)
        {
            Steinberg::PClassInfo2 ci2;
            if (factory2->getClassInfo2(i, &ci2) == Steinberg::kResultOk)
                entry.version = ci2.version;
            factory2->release();
        }

        out.push_back(std::move(entry));
    }

    factory->release();
    closeLib(handle);
    return out;
}

// ── findBundles ───────────────────────────────────────────────────────────────

std::vector<std::string> VST3Scanner::findBundles(const std::vector<std::string>& dirs)
{
    std::vector<std::string> bundles;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) continue;

        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (ec) break;
            const auto& p = entry.path();
            if (p.extension() == ".vst3" && std::filesystem::is_directory(p, ec))
                bundles.push_back(p.string());
        }
    }
    return bundles;
}

// ── scan ─────────────────────────────────────────────────────────────────────

std::vector<VST3Entry> VST3Scanner::scan(const std::vector<std::string>& dirs)
{
    std::vector<VST3Entry> results;
    for (const auto& bundlePath : findBundles(dirs)) {
        auto entries = scanBundle(bundlePath);
        for (auto& e : entries) results.push_back(std::move(e));
    }
    return results;
}

// ── defaultPaths ─────────────────────────────────────────────────────────────

std::vector<std::string> VST3Scanner::defaultPaths()
{
#if defined(__APPLE__)
    std::vector<std::string> paths;
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
    const char* home = std::getenv("HOME");
    if (home)
        paths.push_back(std::string(home) + "/Library/Audio/Plug-Ins/VST3");
    return paths;
#elif defined(__linux__)
    std::vector<std::string> paths;
    paths.push_back("/usr/lib/vst3");
    paths.push_back("/usr/local/lib/vst3");
    const char* home = std::getenv("HOME");
    if (home)
        paths.push_back(std::string(home) + "/.vst3");
    return paths;
#elif defined(_WIN32)
    std::vector<std::string> paths;
    paths.push_back("C:\\Program Files\\Common Files\\VST3");
    const char* local = std::getenv("LOCALAPPDATA");
    if (local)
        paths.push_back(std::string(local) + "\\Programs\\Common\\VST3");
    return paths;
#else
    return {};
#endif
}

} // namespace mcp::plugin
#endif // MCP_HAVE_VST3
