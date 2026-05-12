#include "engine/plugin/VST3PluginAdapter.h"
#ifdef MCP_HAVE_VST3

// VST3 SDK interfaces (pluginterfaces — header-only, no SDK .cpp needed)
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "engine/plugin/ParameterInfo.h"
#include "engine/plugin/PluginState.h"
// Include our own types with explicit namespace to avoid conflicts with VST3 ProcessContext
#include "engine/plugin/ProcessContext.h"
#include "engine/plugin/AudioBlock.h"
#include "engine/plugin/EventBlock.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#  include <dlfcn.h>
#elif defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// Platform view type string for IPlugView::attached()
#if defined(__APPLE__)
#  define MCP_VST3_PLATFORM_TYPE  Steinberg::kPlatformTypeNSView
#elif defined(_WIN32)
#  define MCP_VST3_PLATFORM_TYPE  Steinberg::kPlatformTypeHWND
#elif defined(__linux__)
#  define MCP_VST3_PLATFORM_TYPE  Steinberg::kPlatformTypeX11EmbedWindowID
#endif

// Alias VST3 namespace to avoid global pollution while keeping code readable
namespace vst3 = Steinberg::Vst;

namespace mcp::plugin {

// ── MemoryStream — minimal IBStream backed by a byte vector ──────────────────

class MemoryStream final : public Steinberg::IBStream {
public:
    explicit MemoryStream() = default;
    explicit MemoryStream(const std::vector<uint8_t>& data) : m_data(data) {}

    Steinberg::tresult queryInterface(const Steinberg::TUID, void** obj) override
        { *obj = nullptr; return Steinberg::kNoInterface; }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    Steinberg::tresult read(void* buf, Steinberg::int32 n, Steinberg::int32* nr) override {
        const Steinberg::int32 avail =
            static_cast<Steinberg::int32>(m_data.size()) - m_pos;
        const Steinberg::int32 got = std::min(n, avail);
        if (got > 0) std::memcpy(buf, m_data.data() + m_pos, static_cast<size_t>(got));
        m_pos += got;
        if (nr) *nr = got;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult write(void* buf, Steinberg::int32 n,
                             Steinberg::int32* nw) override {
        if (n <= 0) { if (nw) *nw = 0; return Steinberg::kResultOk; }
        const size_t end = static_cast<size_t>(m_pos + n);
        if (end > m_data.size()) m_data.resize(end);
        std::memcpy(m_data.data() + m_pos, buf, static_cast<size_t>(n));
        m_pos += n;
        if (nw) *nw = n;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult seek(Steinberg::int64 pos, Steinberg::int32 mode,
                            Steinberg::int64* result) override {
        Steinberg::int64 np = m_pos;
        switch (mode) {
            case IBStream::kIBSeekSet: np = pos; break;
            case IBStream::kIBSeekCur: np = m_pos + pos; break;
            case IBStream::kIBSeekEnd: np = static_cast<Steinberg::int64>(m_data.size()) + pos; break;
            default: return Steinberg::kInvalidArgument;
        }
        if (np < 0) np = 0;
        m_pos = static_cast<Steinberg::int32>(np);
        if (result) *result = m_pos;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult tell(Steinberg::int64* pos) override {
        if (pos) *pos = m_pos;
        return Steinberg::kResultOk;
    }

    const std::vector<uint8_t>& data() const { return m_data; }
    void rewind() { m_pos = 0; }

private:
    std::vector<uint8_t>   m_data;
    Steinberg::int32       m_pos{0};
    std::atomic<int>       m_ref{1};
};

// ── HostApplication — minimal IHostApplication ───────────────────────────────

class HostApplication final : public vst3::IHostApplication {
public:
    Steinberg::tresult queryInterface(const Steinberg::TUID iid,
                                      void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, vst3::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this; addRef(); return Steinberg::kResultOk;
        }
        *obj = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    Steinberg::tresult getName(vst3::String128 name) override {
        static const Steinberg::char16 kName[] = {
            'M','u','s','i','c',' ','C','u','e',' ','P','l','a','y','e','r',0
        };
        std::memcpy(name, kName, sizeof(kName));
        return Steinberg::kResultOk;
    }
    Steinberg::tresult createInstance(Steinberg::TUID cid, Steinberg::TUID iid,
                                      void** obj) override;  // defined below MessageImpl

private:
    std::atomic<int> m_ref{1};
};

// ── IMessage / IAttributeList implementation ──────────────────────────────────
// Plugins use these to pass data between component and controller via
// IConnectionPoint::notify() — e.g. FabFilter uses this for spectrum data.

class AttributeListImpl final : public vst3::IAttributeList {
public:
    using AttrID = const char*;

    Steinberg::tresult queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, vst3::IAttributeList::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this; addRef(); return Steinberg::kResultOk;
        }
        *obj = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    Steinberg::tresult setInt(AttrID id, Steinberg::int64 v) override {
        auto& a = m_attrs[id]; a.type = Type::Int; a.i = v;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult getInt(AttrID id, Steinberg::int64& v) override {
        auto it = m_attrs.find(id);
        if (it == m_attrs.end() || it->second.type != Type::Int) return Steinberg::kResultFalse;
        v = it->second.i; return Steinberg::kResultOk;
    }
    Steinberg::tresult setFloat(AttrID id, double v) override {
        auto& a = m_attrs[id]; a.type = Type::Float; a.f = v;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult getFloat(AttrID id, double& v) override {
        auto it = m_attrs.find(id);
        if (it == m_attrs.end() || it->second.type != Type::Float) return Steinberg::kResultFalse;
        v = it->second.f; return Steinberg::kResultOk;
    }
    Steinberg::tresult setString(AttrID id, const vst3::TChar* s) override {
        auto& a = m_attrs[id]; a.type = Type::String;
        a.str.clear();
        while (*s) a.str.push_back(*s++);
        a.str.push_back(0);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult getString(AttrID id, vst3::TChar* buf,
                                  Steinberg::uint32 sizeBytes) override {
        auto it = m_attrs.find(id);
        if (it == m_attrs.end() || it->second.type != Type::String) return Steinberg::kResultFalse;
        const auto& s = it->second.str;
        const size_t n = std::min(sizeBytes / sizeof(vst3::TChar), s.size());
        std::memcpy(buf, s.data(), n * sizeof(vst3::TChar));
        if (n < sizeBytes / sizeof(vst3::TChar)) buf[n] = 0;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult setBinary(AttrID id, const void* data,
                                  Steinberg::uint32 sizeBytes) override {
        auto& a = m_attrs[id]; a.type = Type::Binary;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        a.bin.assign(p, p + sizeBytes);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult getBinary(AttrID id, const void*& data,
                                  Steinberg::uint32& sizeBytes) override {
        auto it = m_attrs.find(id);
        if (it == m_attrs.end() || it->second.type != Type::Binary) return Steinberg::kResultFalse;
        data      = it->second.bin.data();
        sizeBytes = static_cast<Steinberg::uint32>(it->second.bin.size());
        return Steinberg::kResultOk;
    }

private:
    enum class Type { Int, Float, String, Binary };
    struct AttrVal {
        Type type{Type::Int};
        Steinberg::int64             i{0};
        double                       f{0.0};
        std::vector<vst3::TChar>     str;
        std::vector<uint8_t>         bin;
    };
    std::map<std::string, AttrVal> m_attrs;
    std::atomic<int> m_ref{1};
};

class MessageImpl final : public vst3::IMessage {
public:
    Steinberg::tresult queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, vst3::IMessage::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this; addRef(); return Steinberg::kResultOk;
        }
        *obj = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    const char* PLUGIN_API getMessageID() override { return m_id.c_str(); }
    void PLUGIN_API setMessageID(const char* id) override { m_id = id ? id : ""; }
    vst3::IAttributeList* PLUGIN_API getAttributes() override { return &m_attrs; }

private:
    std::string        m_id;
    AttributeListImpl  m_attrs;
    std::atomic<int>   m_ref{1};
};

// HostApplication::createInstance — out-of-line so MessageImpl is complete
inline Steinberg::tresult HostApplication::createInstance(
        Steinberg::TUID cid, Steinberg::TUID iid, void** obj)
{
    // Plugins allocate IMessage objects here for IConnectionPoint communication.
    if (Steinberg::FUnknownPrivate::iidEqual(cid, vst3::IMessage::iid) &&
        Steinberg::FUnknownPrivate::iidEqual(iid, vst3::IMessage::iid)) {
        *obj = static_cast<vst3::IMessage*>(new MessageImpl);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNotImplemented;
}

// ── PlugFrame — IPlugFrame (host resizes container on plugin request) ─────────

class PlugFrameImpl final : public Steinberg::IPlugFrame {
public:
    std::function<void(int w, int h)> onResize;

    Steinberg::tresult queryInterface(const Steinberg::TUID iid,
                                      void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this; addRef(); return Steinberg::kResultOk;
        }
        *obj = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* /*view*/,
                                              Steinberg::ViewRect* newSize) override {
        if (newSize && onResize) {
            const int w = newSize->right  - newSize->left;
            const int h = newSize->bottom - newSize->top;
            if (w > 0 && h > 0) onResize(w, h);
        }
        return Steinberg::kResultOk;
    }

private:
    std::atomic<int> m_ref{1};
};

// ── ComponentHandler ─────────────────────────────────────────────────────────

class ComponentHandler final : public vst3::IComponentHandler {
public:
    // Called by the plugin UI for every knob/parameter gesture.
    // Host must forward (id, value) to inputParameterChanges in the next process().
    std::function<void(vst3::ParamID, double)> onEdit;
    // Fired after any performEdit so the host UI can refresh its display.
    std::function<void()>                      onChanged;
    // Fired when the plugin requests a host-side restart (e.g. preset load).
    std::function<void(Steinberg::int32)>      onRestart;

    Steinberg::tresult queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, vst3::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = this; addRef(); return Steinberg::kResultOk;
        }
        *obj = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 addRef()  override { return ++m_ref; }
    Steinberg::uint32 release() override {
        if (--m_ref == 0) { delete this; return 0; }
        return m_ref;
    }

    Steinberg::tresult beginEdit(vst3::ParamID) override { return Steinberg::kResultOk; }

    Steinberg::tresult performEdit(vst3::ParamID id, vst3::ParamValue value) override {
        if (onEdit)    onEdit(id, value);    // enqueue for next process()
        if (onChanged) onChanged();          // notify host UI
        return Steinberg::kResultOk;
    }

    Steinberg::tresult endEdit(vst3::ParamID) override { return Steinberg::kResultOk; }

    Steinberg::tresult restartComponent(Steinberg::int32 flags) override {
        if (onRestart) onRestart(flags);
        return Steinberg::kResultOk;
    }

private:
    std::atomic<int> m_ref{1};
};

// ── Platform helpers ──────────────────────────────────────────────────────────

// On macOS we load the .vst3 bundle via CFBundle so the plugin's own
// NSBundle/CFBundle lookups (for resources, etc.) resolve correctly.
// Raw dlopen on the Mach-O inside Contents/MacOS/ skips bundle registration,
// causing resource-not-found failures in plugins like FabFilter Pro-L 2.
static void* platformOpenLib(const std::string& path) {
#if defined(__APPLE__)
    CFStringRef cfStr = CFStringCreateWithCString(
        kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
    if (!cfStr) return nullptr;
    CFURLRef url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, cfStr, kCFURLPOSIXPathStyle, /*isDirectory=*/true);
    CFRelease(cfStr);
    if (!url) return nullptr;
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    if (!bundle) return nullptr;
    // Only call bundleEntry if we freshly load the executable.
    // If the binary was already loaded (e.g. by the scanner via dlopen and not
    // yet unloaded), CFBundleIsExecutableLoaded returns true and we skip both
    // CFBundleLoadExecutable and bundleEntry to avoid a double-initialisation crash.
    const bool alreadyLoaded = CFBundleIsExecutableLoaded(bundle);
    if (!alreadyLoaded && !CFBundleLoadExecutable(bundle)) {
        CFRelease(bundle);
        return nullptr;
    }
    if (!alreadyLoaded) {
        // Call the plugin's bundleEntry(CFBundleRef) if it exports one.
        // This is how VST3 plugins cache their own bundle ref for resource lookups
        // (e.g. CFBundleCopyResourceURL). The VST3 SDK's module_mac.mm does the same.
        using BundleEntryFn = bool(*)(CFBundleRef);
        CFStringRef entryName = CFStringCreateWithCString(
            kCFAllocatorDefault, "bundleEntry", kCFStringEncodingUTF8);
        auto bundleEntryFn = reinterpret_cast<BundleEntryFn>(
            CFBundleGetFunctionPointerForName(bundle, entryName));
        CFRelease(entryName);
        if (bundleEntryFn && !bundleEntryFn(bundle)) {
            fprintf(stderr, "[MCP VST3] bundleEntry() returned false — plugin refused to load\n");
            CFBundleUnloadExecutable(bundle);
            CFRelease(bundle);
            return nullptr;
        }
    }
    return static_cast<void*>(bundle);
#elif defined(__linux__)
    return dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
#elif defined(_WIN32)
    return (void*)LoadLibraryA(path.c_str());
#else
    return nullptr;
#endif
}
static void platformCloseLib(void* h) {
    if (!h) return;
#if defined(__APPLE__)
    {
        CFBundleRef bundle = static_cast<CFBundleRef>(h);
        using BundleExitFn = bool(*)();
        CFStringRef exitName = CFStringCreateWithCString(
            kCFAllocatorDefault, "bundleExit", kCFStringEncodingUTF8);
        auto bundleExitFn = reinterpret_cast<BundleExitFn>(
            CFBundleGetFunctionPointerForName(bundle, exitName));
        CFRelease(exitName);
        if (bundleExitFn) bundleExitFn();
        CFBundleUnloadExecutable(bundle);
        CFRelease(bundle);
    }
#elif defined(__linux__)
    dlclose(h);
#elif defined(_WIN32)
    FreeLibrary((HMODULE)h);
#endif
}
static void* platformGetSym(void* h, const char* sym) {
    if (!h) return nullptr;
#if defined(__APPLE__)
    CFStringRef cfSym = CFStringCreateWithCString(
        kCFAllocatorDefault, sym, kCFStringEncodingUTF8);
    if (!cfSym) return nullptr;
    void* fn = CFBundleGetFunctionPointerForName(static_cast<CFBundleRef>(h), cfSym);
    CFRelease(cfSym);
    return fn;
#elif defined(__linux__)
    return dlsym(h, sym);
#elif defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)h, sym);
#else
    return nullptr;
#endif
}

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

// ── OutputParamChanges ────────────────────────────────────────────────────────
// Writable IParameterChanges given to ProcessData::outputParameterChanges.
// The processor calls addParameterData()/addPoint() to push output values
// (spectrum bins, gain-reduction meters, etc.) back to the host each block.
// Kept as a persistent impl member so its internal vector doesn't reallocate
// on every audio callback after the first few calls have grown the capacity.

class OutputParamChanges final : public vst3::IParameterChanges {
public:
    Steinberg::tresult queryInterface(const Steinberg::TUID, void** o) override
        { *o = nullptr; return Steinberg::kNoInterface; }
    Steinberg::uint32 addRef()  override { return 1; }
    Steinberg::uint32 release() override { return 1; }

    Steinberg::int32 getParameterCount() override
        { return static_cast<Steinberg::int32>(m_queues.size()); }

    vst3::IParamValueQueue* getParameterData(Steinberg::int32 idx) override {
        if (idx < 0 || idx >= static_cast<Steinberg::int32>(m_queues.size()))
            return nullptr;
        return &m_queues[static_cast<size_t>(idx)];
    }

    vst3::IParamValueQueue* addParameterData(const vst3::ParamID& id,
                                              Steinberg::int32& outIdx) override {
        for (Steinberg::int32 i = 0; i < static_cast<Steinberg::int32>(m_queues.size()); ++i) {
            if (m_queues[static_cast<size_t>(i)].getParameterId() == id) {
                outIdx = i;
                return &m_queues[static_cast<size_t>(i)];
            }
        }
        outIdx = static_cast<Steinberg::int32>(m_queues.size());
        m_queues.push_back(WritableQueue{id});
        return &m_queues.back();
    }

    void reset() { m_queues.clear(); }  // call at the start of each process() block

private:
    struct WritableQueue final : public vst3::IParamValueQueue {
        explicit WritableQueue(vst3::ParamID i) : m_id(i) {}

        Steinberg::tresult queryInterface(const Steinberg::TUID, void** o) override
            { *o = nullptr; return Steinberg::kNoInterface; }
        Steinberg::uint32 addRef()  override { return 1; }
        Steinberg::uint32 release() override { return 1; }

        vst3::ParamID    getParameterId() override { return m_id; }
        Steinberg::int32 getPointCount()  override { return m_hasPoint ? 1 : 0; }

        Steinberg::tresult getPoint(Steinberg::int32, Steinberg::int32& sampleOffset,
                                    vst3::ParamValue& v) override {
            if (!m_hasPoint) return Steinberg::kResultFalse;
            sampleOffset = 0; v = m_lastVal;
            return Steinberg::kResultOk;
        }

        Steinberg::tresult addPoint(Steinberg::int32, vst3::ParamValue v,
                                    Steinberg::int32& outIdx) override {
            m_lastVal = v; m_hasPoint = true; outIdx = 0;
            return Steinberg::kResultOk;
        }

    private:
        vst3::ParamID m_id{0};
        double        m_lastVal{0.0};
        bool          m_hasPoint{false};
    };

    std::vector<WritableQueue> m_queues;
};

// ── VST3PluginAdapterImpl ─────────────────────────────────────────────────────

struct VST3PluginAdapterImpl {
    void*            libHandle{nullptr};
    std::string      bundlePath;
    int              classIndex{0};

    vst3::IComponent*      component{nullptr};
    vst3::IAudioProcessor* processor{nullptr};
    vst3::IEditController* controller{nullptr};
    bool                   controllerSeparate{false};  // separate class (owns terminate+release)
    bool                   active{false};     // component setActive(true) called
    bool                   processing{false}; // processor setProcessing(true) called
    bool                   prepared{false};   // setupProcessing done

    Steinberg::IPlugView*          plugView{nullptr};
    PlugFrameImpl*                 plugFrame{nullptr};   // IPlugFrame sent to the view

    // IConnectionPoint pair kept alive for disconnect on shutdown.
    vst3::IConnectionPoint* compConnPoint{nullptr};
    vst3::IConnectionPoint* ctrlConnPoint{nullptr};

    HostApplication*  hostApp{nullptr};
    ComponentHandler* compHandler{nullptr};

    std::vector<ParameterInfo>   paramInfos;
    std::vector<vst3::ParamID>   paramIds;
    std::unordered_map<std::string, size_t>  paramIndexMap;  // string id → index, O(1) lookup
    std::unordered_map<vst3::ParamID, float> paramValueCache; // VST3 ParamID → normalized value

    vst3::ParamID bypassParamId{vst3::kNoParamId};
    bool          nativeBypassed{false};

    std::string pluginId;
    std::string displayName;
    int         numChannels{2};

    double sampleRate{44100.0};
    int    maxBlockSize{512};

    struct PendingParam { vst3::ParamID id; double normalized; };
    std::vector<PendingParam> pendingParams;
    std::mutex                paramsMutex;         // guards pendingParams (UI vs audio thread)
    std::atomic<bool>         needsFullParamSync{false}; // set by restartComponent(kParamValuesChanged)
    OutputParamChanges        outParamChanges;     // reused each block — avoids per-callback heap alloc
    std::function<void()>        onParamChanged;
    std::function<void(int,int)> onViewResize;

    void cleanup() {
        if (plugView) { plugView->removed(); plugView->release(); plugView = nullptr; }
        if (plugFrame) { plugFrame->onResize = nullptr; plugFrame->release(); plugFrame = nullptr; }
        if (processing) { processor->setProcessing(false); processing = false; }
        if (active)    { component->setActive(false); active = false; }
        // Disconnect IConnectionPoint pair before terminating controller/component.
        if (compConnPoint && ctrlConnPoint) {
            try { compConnPoint->disconnect(ctrlConnPoint); } catch (...) {}
            try { ctrlConnPoint->disconnect(compConnPoint); } catch (...) {}
        }
        if (compConnPoint) { compConnPoint->release(); compConnPoint = nullptr; }
        if (ctrlConnPoint) { ctrlConnPoint->release(); ctrlConnPoint = nullptr; }
        if (controller) {
            if (controllerSeparate) {
                try { controller->terminate(); } catch (...) {}
            }
            controller->release();
            controller = nullptr;
        }
        if (component) {
            try { component->terminate(); } catch (...) {}
            component->release();
            component = nullptr;
        }
        processor = nullptr;
        if (compHandler) { compHandler->release(); compHandler = nullptr; }
        if (hostApp)     { hostApp->release();     hostApp = nullptr; }
        if (libHandle)   { platformCloseLib(libHandle); libHandle = nullptr; }
    }

    int findParamIndex(const std::string& id) const {
        auto it = paramIndexMap.find(id);
        return it == paramIndexMap.end() ? -1 : static_cast<int>(it->second);
    }
};

// ── create ────────────────────────────────────────────────────────────────────

std::unique_ptr<VST3PluginAdapter> VST3PluginAdapter::create(
    const std::string& bundlePath, int classIndex, int numChannels)
{
    const std::string binPath = bundleBinaryPath(bundlePath);
    if (binPath.empty() || !std::filesystem::exists(binPath))
        return nullptr;

    // On macOS, open the .vst3 bundle directory (not the inner Mach-O) so that
    // CFBundle/NSBundle resource lookups work correctly inside the plugin.
#if defined(__APPLE__)
    void* handle = platformOpenLib(bundlePath);
#else
    void* handle = platformOpenLib(binPath);
#endif
    if (!handle) return nullptr;

    using FactoryFn = Steinberg::IPluginFactory*(*)();
    auto* factoryFn = (FactoryFn)platformGetSym(handle, "GetPluginFactory");
    if (!factoryFn) { platformCloseLib(handle); return nullptr; }

    Steinberg::IPluginFactory* factory = factoryFn();
    if (!factory || classIndex >= factory->countClasses()) {
        if (factory) factory->release();
        platformCloseLib(handle);
        return nullptr;
    }

    Steinberg::PClassInfo ci;
    if (factory->getClassInfo(classIndex, &ci) != Steinberg::kResultOk) {
        factory->release(); platformCloseLib(handle); return nullptr;
    }

    // All plugin vtable calls below are wrapped in individual try-catch blocks
    // with stderr logging so crash reports point to the exact failing call.
#define MCP_VST3_LOG(msg) \
    fprintf(stderr, "[MCP VST3] %-40s  " msg "\n", ci.name)
#define MCP_VST3_LOGF(fmt, ...) \
    fprintf(stderr, "[MCP VST3] %-40s  " fmt "\n", ci.name, __VA_ARGS__)

    vst3::IComponent* component = nullptr;
    MCP_VST3_LOG("createInstance");
    try {
        if (factory->createInstance(ci.cid, vst3::IComponent::iid,
                                    (void**)&component) != Steinberg::kResultOk
            || !component)
        {
            MCP_VST3_LOG("createInstance FAILED");
            factory->release(); platformCloseLib(handle); return nullptr;
        }
    } catch (...) {
        MCP_VST3_LOG("createInstance THREW");
        if (component) component->release();
        factory->release(); platformCloseLib(handle); return nullptr;
    }
    MCP_VST3_LOG("createInstance OK");

    auto* impl         = new VST3PluginAdapterImpl;
    impl->libHandle    = handle;
    impl->bundlePath   = bundlePath;
    impl->classIndex   = classIndex;
    impl->component    = component;
    impl->numChannels  = numChannels;
    impl->displayName  = ci.name;
    impl->hostApp      = new HostApplication;
    impl->compHandler  = new ComponentHandler;

    // pluginId: "vst3:<32-hex-char UID>"
    char hexbuf[33];
    for (int k = 0; k < 16; ++k)
        std::snprintf(hexbuf + k*2, 3, "%02x",
                      static_cast<unsigned char>(ci.cid[k]));
    hexbuf[32] = '\0';
    impl->pluginId = std::string("vst3:") + hexbuf;

    // ── Step 1: IComponent::initialize ───────────────────────────────────────
    MCP_VST3_LOG("component::initialize");
    {
        Steinberg::tresult r = Steinberg::kResultFalse;
        try { r = component->initialize(impl->hostApp); } catch (...) {
            MCP_VST3_LOG("component::initialize THREW");
        }
        if (r != Steinberg::kResultOk) {
            MCP_VST3_LOGF("component::initialize FAILED (result=%d)", (int)r);
            factory->release(); impl->cleanup(); delete impl; return nullptr;
        }
    }
    MCP_VST3_LOG("component::initialize OK");

    // ── Step 2: IAudioProcessor QI ───────────────────────────────────────────
    MCP_VST3_LOG("queryInterface IAudioProcessor");
    try {
        if (component->queryInterface(vst3::IAudioProcessor::iid,
                                      (void**)&impl->processor) != Steinberg::kResultOk
            || !impl->processor)
        {
            MCP_VST3_LOG("queryInterface IAudioProcessor FAILED");
            factory->release(); impl->cleanup(); delete impl; return nullptr;
        }
        // QI adds one ref; processor shares the component object — release the extra ref.
        impl->processor->release();
    } catch (...) {
        MCP_VST3_LOG("queryInterface IAudioProcessor THREW");
        factory->release(); impl->cleanup(); delete impl; return nullptr;
    }
    MCP_VST3_LOG("queryInterface IAudioProcessor OK");

    // ── Step 3: IEditController — combined first, then separate class ─────────
    MCP_VST3_LOG("IEditController lookup");
    try {
        if (component->queryInterface(vst3::IEditController::iid,
                                      (void**)&impl->controller) != Steinberg::kResultOk
            || !impl->controller)
        {
            impl->controller = nullptr;
            Steinberg::TUID ctrlTuid;
            if (component->getControllerClassId(ctrlTuid) == Steinberg::kResultOk) {
                MCP_VST3_LOG("separate controller class found");
                vst3::IEditController* ctrl = nullptr;
                if (factory->createInstance(ctrlTuid, vst3::IEditController::iid,
                                            (void**)&ctrl) == Steinberg::kResultOk && ctrl)
                {
                    MCP_VST3_LOG("controller::initialize");
                    Steinberg::tresult r = Steinberg::kResultFalse;
                    try { r = ctrl->initialize(impl->hostApp); } catch (...) {
                        MCP_VST3_LOG("controller::initialize THREW");
                    }
                    if (r == Steinberg::kResultOk) {
                        impl->controller = ctrl;
                        impl->controllerSeparate = true;
                        MCP_VST3_LOG("controller::initialize OK (separate)");
                    } else {
                        MCP_VST3_LOGF("controller::initialize FAILED (result=%d)", (int)r);
                        ctrl->release();
                    }
                }
            } else {
                MCP_VST3_LOG("no IEditController (no editor, no params)");
            }
        } else {
            MCP_VST3_LOG("IEditController OK (combined)");
        }
    } catch (...) {
        MCP_VST3_LOG("IEditController lookup THREW — continuing without controller");
        if (impl->controller && !impl->controllerSeparate) {
            impl->controller->release();
            impl->controller = nullptr;
        }
    }

    factory->release();

    if (impl->controller) {
        // Forward UI edits into the pending-param queue so process() picks them up.
        impl->compHandler->onEdit = [impl](vst3::ParamID id, double v) {
            // Update host-side cache immediately (UI thread) so getParameterValue()
            // returns the new value without calling back into the plugin.
            impl->paramValueCache[id] = static_cast<float>(v);
            std::lock_guard<std::mutex> lk(impl->paramsMutex);
            impl->pendingParams.push_back({id, v});
        };
        impl->compHandler->onChanged = [impl]() {
            if (impl->onParamChanged) impl->onParamChanged();
        };
        // Handle plugin-initiated restarts:
        //   kParamValuesChanged — re-read all params from controller (e.g. after preset load).
        //   kIoChanged / kReloadComponent — log only; handled in a later pass.
        impl->compHandler->onRestart = [impl](Steinberg::int32 flags) {
            if (flags & vst3::kParamValuesChanged)
                // Don't read all N params here (UI thread, mutex held for N calls = lag).
                // Set a flag; process() will drain them on the audio thread next block.
                impl->needsFullParamSync.store(true, std::memory_order_relaxed);
            if (flags & (vst3::kIoChanged | vst3::kReloadComponent))
                fprintf(stderr, "[MCP VST3] restartComponent: kIoChanged/kReloadComponent"
                                " requested — deferred (flags=0x%x)\n", (unsigned)flags);
        };
        try {
            impl->controller->setComponentHandler(impl->compHandler);
        } catch (...) { MCP_VST3_LOG("setComponentHandler THREW (ignored)"); }

        // ── Step 4: IConnectionPoint — connect component ↔ controller ────────
        if (impl->controllerSeparate) {
            MCP_VST3_LOG("IConnectionPoint connect");
            vst3::IConnectionPoint* compCP = nullptr;
            vst3::IConnectionPoint* ctrlCP = nullptr;
            try {
                component->queryInterface(vst3::IConnectionPoint::iid, (void**)&compCP);
                impl->controller->queryInterface(vst3::IConnectionPoint::iid, (void**)&ctrlCP);
                if (compCP && ctrlCP) {
                    compCP->connect(ctrlCP);
                    ctrlCP->connect(compCP);
                    impl->compConnPoint = compCP;
                    impl->ctrlConnPoint = ctrlCP;
                    MCP_VST3_LOG("IConnectionPoint connect OK");
                } else {
                    if (compCP) compCP->release();
                    if (ctrlCP) ctrlCP->release();
                    MCP_VST3_LOG("IConnectionPoint not supported by plugin");
                }
            } catch (...) {
                MCP_VST3_LOG("IConnectionPoint connect THREW (ignored)");
                if (compCP && compCP != impl->compConnPoint) compCP->release();
                if (ctrlCP && ctrlCP != impl->ctrlConnPoint) ctrlCP->release();
            }
        }

        // ── Step 5: setComponentState — sync controller with initial state ────
        // Required before getParameterCount(); some controllers return 0 without it.
        MCP_VST3_LOG("setComponentState");
        {
            auto* stream = new MemoryStream;
            try {
                if (component->getState(stream) == Steinberg::kResultOk) {
                    stream->rewind();
                    impl->controller->setComponentState(stream);
                    MCP_VST3_LOG("setComponentState OK");
                } else {
                    MCP_VST3_LOG("setComponentState skipped (getState returned non-OK)");
                }
            } catch (...) { MCP_VST3_LOG("setComponentState THREW (ignored)"); }
            stream->release();
        }

        // ── Step 6: enumerate parameters ─────────────────────────────────────
        MCP_VST3_LOG("getParameterCount");
        auto str16to8 = [](const Steinberg::Vst::TChar* s16) {
            std::string out;
            for (int k = 0; s16[k]; ++k)
                out += static_cast<char>(s16[k] & 0xFF);
            return out;
        };

        try {
            const Steinberg::int32 pcount = impl->controller->getParameterCount();
            MCP_VST3_LOGF("getParameterCount = %d", (int)pcount);
            impl->paramInfos.reserve(static_cast<size_t>(pcount));
            impl->paramIds.reserve(static_cast<size_t>(pcount));

            for (Steinberg::int32 p = 0; p < pcount; ++p) {
                vst3::ParameterInfo vpi;
                if (impl->controller->getParameterInfo(p, vpi) != Steinberg::kResultOk)
                    continue;

                ParameterInfo pi;
                pi.id           = "vst3param:" + std::to_string(static_cast<unsigned>(vpi.id));
                pi.name         = str16to8(vpi.title);
                pi.unit         = str16to8(vpi.units);
                pi.minValue     = 0.0f;
                pi.maxValue     = 1.0f;
                pi.defaultValue = static_cast<float>(vpi.defaultNormalizedValue);
                pi.automatable  = (vpi.flags & vst3::ParameterInfo::kCanAutomate) != 0;
                pi.toNormalized   = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
                pi.fromNormalized = [](float n) { return std::clamp(n, 0.0f, 1.0f); };

                if (vpi.flags & vst3::ParameterInfo::kIsBypass)
                    impl->bypassParamId = vpi.id;

                const size_t newIdx = impl->paramInfos.size();
                impl->paramIndexMap[pi.id] = newIdx;
                // Seed cache with actual current controller value (not just the default),
                // so getParameterValue() is correct from the start without plugin calls.
                impl->paramValueCache[vpi.id] = static_cast<float>(
                    impl->controller->getParamNormalized(vpi.id));
                impl->paramInfos.push_back(std::move(pi));
                impl->paramIds.push_back(vpi.id);
            }
        } catch (...) {
            MCP_VST3_LOG("parameter enumeration THREW (partial list kept)");
        }
    }
    MCP_VST3_LOGF("create() complete -- %zu params", impl->paramInfos.size());
#undef MCP_VST3_LOG

    auto adapter = std::unique_ptr<VST3PluginAdapter>(new VST3PluginAdapter);
    adapter->m_impl.reset(impl);
    return adapter;
}

// ── ctor / dtor ───────────────────────────────────────────────────────────────

VST3PluginAdapter::VST3PluginAdapter() = default;

VST3PluginAdapter::~VST3PluginAdapter() {
    if (m_impl) m_impl->cleanup();
}

// ── prepare ───────────────────────────────────────────────────────────────────

void VST3PluginAdapter::prepare(const mcp::plugin::ProcessContext& ctx) {
    auto* impl = m_impl.get();

    if (impl->processing) { impl->processor->setProcessing(false); impl->processing = false; }
    if (impl->active)     { impl->component->setActive(false);     impl->active     = false; }

    impl->sampleRate   = ctx.sampleRate;
    impl->maxBlockSize = ctx.maxBlockSize;

    // Query how many audio buses the plugin actually has.
    // Instruments have 0 input buses; passing numIn=1 to setBusArrangements
    // causes some plugins to fail or throw.
    const int numInBuses  = impl->component->getBusCount(vst3::kAudio, vst3::kInput);
    const int numOutBuses = impl->component->getBusCount(vst3::kAudio, vst3::kOutput);

    vst3::SpeakerArrangement spk =
        (impl->numChannels == 2) ? vst3::SpeakerArr::kStereo : vst3::SpeakerArr::kMono;
    vst3::SpeakerArrangement inArr[1]  = {spk};
    vst3::SpeakerArrangement outArr[1] = {spk};
    impl->processor->setBusArrangements(
        numInBuses  > 0 ? inArr  : nullptr, numInBuses  > 0 ? 1 : 0,
        numOutBuses > 0 ? outArr : nullptr, numOutBuses > 0 ? 1 : 0);

    vst3::ProcessSetup setup;
    setup.processMode        = vst3::kRealtime;
    setup.symbolicSampleSize = vst3::kSample32;
    setup.maxSamplesPerBlock = ctx.maxBlockSize;
    setup.sampleRate         = ctx.sampleRate;
    impl->processor->setupProcessing(setup);

    if (numInBuses  > 0) impl->component->activateBus(vst3::kAudio, vst3::kInput,  0, true);
    if (numOutBuses > 0) impl->component->activateBus(vst3::kAudio, vst3::kOutput, 0, true);
    impl->component->setActive(true);
    impl->active = true;

    impl->processor->setProcessing(true);
    impl->processing = true;
    impl->prepared   = true;
}

// ── reset ─────────────────────────────────────────────────────────────────────

void VST3PluginAdapter::reset() {
    auto* impl = m_impl.get();
    if (!impl->processing) return;
    impl->processor->setProcessing(false);
    impl->processor->setProcessing(true);
}

void VST3PluginAdapter::suspend() {
    auto* impl = m_impl.get();
    if (impl->processing) { impl->processor->setProcessing(false); impl->processing = false; }
}

void VST3PluginAdapter::resume() {
    auto* impl = m_impl.get();
    if (impl->prepared && !impl->processing) {
        impl->processor->setProcessing(true);
        impl->processing = true;
    }
}

// ── IParameterChanges + IEventList minimal stubs ─────────────────────────────

class SimpleParamChanges final : public vst3::IParameterChanges {
public:
    Steinberg::tresult queryInterface(const Steinberg::TUID, void** o) override
        { *o = nullptr; return Steinberg::kNoInterface; }
    Steinberg::uint32 addRef()  override { return 1; }
    Steinberg::uint32 release() override { return 1; }

    Steinberg::int32 getParameterCount() override
        { return static_cast<Steinberg::int32>(m_queues.size()); }

    vst3::IParamValueQueue* getParameterData(Steinberg::int32 idx) override
        { return (idx < static_cast<Steinberg::int32>(m_queues.size()))
                  ? &m_queues[static_cast<size_t>(idx)] : nullptr; }

    vst3::IParamValueQueue* addParameterData(const vst3::ParamID&,
                                             Steinberg::int32&) override
        { return nullptr; }

    void setEntries(const std::vector<VST3PluginAdapterImpl::PendingParam>& pp) {
        m_queues.clear();
        m_queues.reserve(pp.size());
        for (const auto& p : pp) { SimpleQueue q; q.id = p.id; q.val = p.normalized; m_queues.push_back(q); }
    }

private:
    struct SimpleQueue final : public vst3::IParamValueQueue {
        vst3::ParamID id{0};
        double        val{0.0};

        Steinberg::tresult queryInterface(const Steinberg::TUID, void** o) override
            { *o = nullptr; return Steinberg::kNoInterface; }
        Steinberg::uint32 addRef()  override { return 1; }
        Steinberg::uint32 release() override { return 1; }

        vst3::ParamID       getParameterId() override { return id; }
        Steinberg::int32    getPointCount()  override { return 1; }
        Steinberg::tresult  getPoint(Steinberg::int32, Steinberg::int32& ofs,
                                     vst3::ParamValue& v) override
            { ofs = 0; v = val; return Steinberg::kResultOk; }
        Steinberg::tresult  addPoint(Steinberg::int32, vst3::ParamValue,
                                     Steinberg::int32&) override
            { return Steinberg::kNotImplemented; }
    };
    std::vector<SimpleQueue> m_queues;
};

class EmptyEventList final : public vst3::IEventList {
public:
    Steinberg::tresult queryInterface(const Steinberg::TUID, void** o) override
        { *o = nullptr; return Steinberg::kNoInterface; }
    Steinberg::uint32 addRef()  override { return 1; }
    Steinberg::uint32 release() override { return 1; }

    Steinberg::int32  getEventCount()                      override { return 0; }
    Steinberg::tresult getEvent(Steinberg::int32, vst3::Event&) override
        { return Steinberg::kResultFalse; }
    Steinberg::tresult addEvent(vst3::Event&) override
        { return Steinberg::kNotImplemented; }
};

// ── process ───────────────────────────────────────────────────────────────────

void VST3PluginAdapter::process(const AudioBlock& block,
                                 const EventBlock& events)
{
    auto* impl = m_impl.get();
    if (!impl->processing || !impl->processor) return;

    const int nCh = impl->numChannels;
    float** inPtrs  = const_cast<float**>(block.inputs);
    float** outPtrs = block.outputs;

    vst3::AudioBusBuffers inBus, outBus;
    inBus.numChannels      = nCh;
    inBus.silenceFlags     = 0;
    inBus.channelBuffers32 = inPtrs;
    outBus.numChannels      = nCh;
    outBus.silenceFlags     = 0;
    outBus.channelBuffers32 = outPtrs;

    SimpleParamChanges paramChanges;

    // Merge automation events + UI-driven pending params under one lock.
    // performEdit (UI thread) and onRestart (plugin thread) also push to pendingParams,
    // so all accesses must be protected by paramsMutex.
    {
        std::lock_guard<std::mutex> lk(impl->paramsMutex);

        // Full re-sync requested by restartComponent(kParamValuesChanged) — e.g. preset load.
        // Deferred from the UI thread to here so the UI-thread call is O(1) (just a flag set).
        // This still runs N getParamNormalized() calls, but only once per restart event,
        // and on the audio thread where the block budget is known.
        if (impl->needsFullParamSync.exchange(false, std::memory_order_relaxed)
                && impl->controller) {
            for (const auto& pid : impl->paramIds)
                impl->pendingParams.push_back(
                    {pid, impl->controller->getParamNormalized(pid)});
        }

        if (events.numParameterEvents > 0) {
            for (int e = 0; e < events.numParameterEvents; ++e) {
                const auto& ev = events.parameterEvents[e];
                const int idx = impl->findParamIndex(ev.parameterId);
                if (idx < 0) continue;
                const double norm = static_cast<double>(
                    impl->paramInfos[static_cast<size_t>(idx)].toNormalized(ev.value));
                impl->pendingParams.push_back(
                    {impl->paramIds[static_cast<size_t>(idx)], norm});
            }
        }
        if (!impl->pendingParams.empty()) {
            paramChanges.setEntries(impl->pendingParams);
            impl->pendingParams.clear();
        }
    }

    EmptyEventList emptyEvents;

    impl->outParamChanges.reset();

    vst3::ProcessData data{};
    data.processMode             = vst3::kRealtime;
    data.symbolicSampleSize      = vst3::kSample32;
    data.numSamples              = block.numSamples;
    data.numInputs               = 1;
    data.numOutputs              = 1;
    data.inputs                  = &inBus;
    data.outputs                 = &outBus;
    data.inputParameterChanges   = &paramChanges;
    data.outputParameterChanges  = &impl->outParamChanges;
    data.inputEvents             = &emptyEvents;

    impl->processor->process(data);

    // Forward output parameter changes (spectrum data, meters, etc.) to the
    // controller so the plugin editor can refresh its display.
    if (impl->controller) {
        const Steinberg::int32 n = impl->outParamChanges.getParameterCount();
        for (Steinberg::int32 i = 0; i < n; ++i) {
            auto* q = impl->outParamChanges.getParameterData(i);
            if (!q || q->getPointCount() == 0) continue;
            Steinberg::int32 sampleOffset = 0;
            vst3::ParamValue v = 0.0;
            if (q->getPoint(q->getPointCount() - 1, sampleOffset, v) == Steinberg::kResultOk)
                impl->controller->setParamNormalized(q->getParameterId(), v);
        }
    }
}

// ── latency / tail ────────────────────────────────────────────────────────────

int VST3PluginAdapter::getLatencySamples() const {
    if (!m_impl->processor) return 0;
    return static_cast<int>(m_impl->processor->getLatencySamples());
}
int VST3PluginAdapter::getTailSamples() const {
    if (!m_impl->processor) return 0;
    return static_cast<int>(m_impl->processor->getTailSamples());
}

// ── parameters ────────────────────────────────────────────────────────────────

const std::vector<ParameterInfo>& VST3PluginAdapter::getParameters() const {
    return m_impl->paramInfos;
}

float VST3PluginAdapter::getNormalizedParameterValue(const std::string& id) const {
    const int idx = m_impl->findParamIndex(id);
    if (idx < 0) return 0.0f;
    // Read from host-side cache (O(1), no plugin call).
    // Cache is kept in sync by onEdit and setNormalizedParameterValue.
    auto it = m_impl->paramValueCache.find(m_impl->paramIds[static_cast<size_t>(idx)]);
    return it != m_impl->paramValueCache.end() ? it->second : 0.0f;
}

void VST3PluginAdapter::setNormalizedParameterValue(const std::string& id,
                                                     float normalized)
{
    const int idx = m_impl->findParamIndex(id);
    if (idx < 0 || !m_impl->controller) return;
    const vst3::ParamID pid = m_impl->paramIds[static_cast<size_t>(idx)];
    m_impl->controller->setParamNormalized(pid, static_cast<double>(normalized));
    m_impl->paramValueCache[pid] = normalized;
    std::lock_guard<std::mutex> lk(m_impl->paramsMutex);
    m_impl->pendingParams.push_back({pid, static_cast<double>(normalized)});
}

float VST3PluginAdapter::getParameterValue(const std::string& id) const {
    const int idx = m_impl->findParamIndex(id);
    if (idx < 0) return 0.0f;
    return m_impl->paramInfos[static_cast<size_t>(idx)].fromNormalized(
               getNormalizedParameterValue(id));
}

void VST3PluginAdapter::setParameterValue(const std::string& id, float value) {
    const int idx = m_impl->findParamIndex(id);
    if (idx < 0) return;
    setNormalizedParameterValue(
        id, m_impl->paramInfos[static_cast<size_t>(idx)].toNormalized(value));
}

// ── state ─────────────────────────────────────────────────────────────────────

PluginState VST3PluginAdapter::getState() const {
    PluginState st;
    st.pluginId = m_impl->pluginId;
    st.backend  = PluginBackend::VST3;
    st.version  = 1;
    if (!m_impl->component) return st;

    auto* stream = new MemoryStream;
    if (m_impl->component->getState(stream) == Steinberg::kResultOk)
        st.stateData = stream->data();
    stream->release();

    // Export individual parameter values from the host-side cache.
    // The binary blob alone may not reflect parameters changed via
    // inputParameterChanges if the plugin doesn't persist them back to getState(),
    // so snapshots must also store the per-param values explicitly.
    for (size_t i = 0; i < m_impl->paramIds.size(); ++i) {
        auto it = m_impl->paramValueCache.find(m_impl->paramIds[i]);
        if (it != m_impl->paramValueCache.end())
            st.parameters[m_impl->paramInfos[i].id] = it->second;
    }

    return st;
}

void VST3PluginAdapter::setState(const PluginState& state) {
    if (!m_impl->component) return;

    if (!state.stateData.empty()) {
        auto* stream = new MemoryStream(state.stateData);
        if (m_impl->component->setState(stream) == Steinberg::kResultOk
            && m_impl->controller)
        {
            stream->rewind();
            m_impl->controller->setComponentState(stream);
        }
        stream->release();
        // After a state blob restore the controller holds new values for all params.
        // Refresh the host-side cache so getParameterValue() stays accurate.
        if (m_impl->controller) {
            for (size_t i = 0; i < m_impl->paramIds.size(); ++i)
                m_impl->paramValueCache[m_impl->paramIds[i]] = static_cast<float>(
                    m_impl->controller->getParamNormalized(m_impl->paramIds[i]));
        }
    }

    for (const auto& [id, norm] : state.parameters)
        setNormalizedParameterValue(id, norm);
}

// ── metadata ─────────────────────────────────────────────────────────────────

const std::string& VST3PluginAdapter::pluginId()    const { return m_impl->pluginId; }
const std::string& VST3PluginAdapter::displayName() const { return m_impl->displayName; }

// ── native editor ─────────────────────────────────────────────────────────────

void* VST3PluginAdapter::createEditorView(void* parentView, int& outW, int& outH) {
    outW = 0; outH = 0;
    auto* impl = m_impl.get();
    if (!impl->controller) return nullptr;
    if (impl->plugView) destroyEditorView();

    impl->plugView = impl->controller->createView(vst3::ViewType::kEditor);
    if (!impl->plugView) return nullptr;

#ifdef MCP_VST3_PLATFORM_TYPE
    if (impl->plugView->isPlatformTypeSupported(MCP_VST3_PLATFORM_TYPE)
            != Steinberg::kResultOk) {
        impl->plugView->release(); impl->plugView = nullptr;
        return nullptr;
    }

    // Provide IPlugFrame so the plugin can request view resizes.
    // Must be set BEFORE attached() — strict VST3 plugins (e.g. FabFilter)
    // throw if they cannot resolve resizeView() during initialisation.
    if (!impl->plugFrame)
        impl->plugFrame = new PlugFrameImpl;
    impl->plugFrame->onResize = impl->onViewResize;
    impl->plugView->setFrame(impl->plugFrame);

    // attached() can throw if the parent view is not yet in a live window.
    // Wrap so a plugin exception never propagates to std::terminate().
    Steinberg::tresult attachRes = Steinberg::kResultFalse;
    try {
        attachRes = impl->plugView->attached(parentView, MCP_VST3_PLATFORM_TYPE);
    } catch (...) {
        impl->plugView->release(); impl->plugView = nullptr;
        return nullptr;
    }
    if (attachRes != Steinberg::kResultOk) {
        impl->plugView->release(); impl->plugView = nullptr;
        return nullptr;
    }

    Steinberg::ViewRect rect{};
    if (impl->plugView->getSize(&rect) == Steinberg::kResultOk) {
        outW = rect.getWidth();
        outH = rect.getHeight();
    }
    return parentView;
#else
    impl->plugView->release(); impl->plugView = nullptr;
    return nullptr;
#endif
}

void VST3PluginAdapter::destroyEditorView() {
    auto* impl = m_impl.get();
    if (!impl->plugView) return;
    impl->plugView->removed();
    impl->plugView->release();
    impl->plugView = nullptr;
}

// ── parameter watching ────────────────────────────────────────────────────────

void VST3PluginAdapter::startWatchingParameters(std::function<void()> cb) {
    m_impl->onParamChanged = std::move(cb);
}
void VST3PluginAdapter::stopWatchingParameters() {
    m_impl->onParamChanged = nullptr;
}

void VST3PluginAdapter::setResizeCallback(std::function<void(int w, int h)> cb) {
    m_impl->onViewResize = std::move(cb);
    if (m_impl->plugFrame)
        m_impl->plugFrame->onResize = m_impl->onViewResize;
}

// ── bypass ────────────────────────────────────────────────────────────────────

bool VST3PluginAdapter::hasNativeBypass() const {
    return m_impl->bypassParamId != vst3::kNoParamId;
}

void VST3PluginAdapter::setNativeBypass(bool bypass) {
    auto* impl = m_impl.get();
    if (impl->bypassParamId == vst3::kNoParamId || !impl->controller) return;
    const double norm = bypass ? 1.0 : 0.0;
    // Update controller so the plugin editor reflects the new state immediately.
    impl->controller->setParamNormalized(impl->bypassParamId, norm);
    impl->paramValueCache[impl->bypassParamId] = static_cast<float>(norm);
    // Queue for delivery to the audio processor on the next process() call.
    {
        std::lock_guard<std::mutex> lk(impl->paramsMutex);
        impl->pendingParams.push_back({impl->bypassParamId, norm});
    }
    impl->nativeBypassed = bypass;
    // Notify host UI watchers so they can refresh their parameter display.
    if (impl->onParamChanged) impl->onParamChanged();
}
bool VST3PluginAdapter::getNativeBypass() const { return m_impl->nativeBypassed; }

} // namespace mcp::plugin
#endif // MCP_HAVE_VST3
