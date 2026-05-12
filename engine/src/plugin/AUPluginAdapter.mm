// AUPluginAdapter.mm — AudioUnit v2 effect adapter (macOS only).
// Uses the C-level AudioToolbox / AudioUnit API exclusively; no JUCE, no NSObject.
#ifdef __APPLE__

#include "engine/plugin/AUPluginAdapter.h"
#include "engine/plugin/ProcessContext.h"

#import <AppKit/AppKit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AUCocoaUIView.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp::plugin {

// ── AUPluginAdapterImpl (fully defined here) ──────────────────────────────────

struct AUPluginAdapterImpl {
    AudioUnit  au{nullptr};

    std::string pluginId;
    std::string displayName;
    int         numChannels{2};
    double      sampleRate{48000.0};
    int         maxBlockSize{512};
    bool        prepared{false};
    uint64_t    renderSampleTime{0};

    std::vector<ParameterInfo>        params;
    std::vector<AudioUnitParameterID> paramAuIds; // parallel to params
    std::unordered_map<std::string, int> paramIndex; // strId → index in params

    // Retained handle to the AU's Cocoa editor view (nil if not yet created).
    // Stored as CFTypeRef to avoid requiring ObjC ARC in the struct.
    CFTypeRef editorViewHandle{nullptr};

    // Parameter change listener (active only while an editor window is open).
    AUEventListenerRef paramListenerRef{nullptr};
    std::function<void()> onParamChanged;
    // > 0 while notifyViewRefresh() is in flight — suppresses self-triggered callbacks.
    int suppressCount{0};

    // Bypass property listener — watches kAudioUnitProperty_BypassEffect changes
    // originating inside the plugin (e.g. a native bypass button in the AU editor).
    AUEventListenerRef bypassListenerRef{nullptr};
    std::function<void(bool)> onBypassChanged;

    // Latency property listener — watches kAudioUnitProperty_Latency changes.
    AUEventListenerRef latencyListenerRef{nullptr};
    std::function<void()> onLatencyChanged;

    // Pre-allocated input scratch: numChannels × maxBlockSize floats.
    // Filled from block.inputs before AudioUnitRender; read by render callback.
    std::vector<float>        inputScratch;
    std::vector<const float*> inputPtrs;   // one pointer per channel into inputScratch

    // Pre-allocated output ABL backing store.
    std::vector<float> outputData;
    AudioBufferList*   outputABL{nullptr}; // heap-allocated, variable-size header

    // ── Buffer helpers (no-alloc guarantee: only called from control thread) ─

    void freeOutputABL() {
        delete[] reinterpret_cast<uint8_t*>(outputABL);
        outputABL = nullptr;
    }

    void allocOutputABL(int nCh, int frames) {
        freeOutputABL();
        const size_t sz = sizeof(AudioBufferList) +
                          static_cast<size_t>(std::max(0, nCh - 1)) * sizeof(AudioBuffer);
        outputABL = reinterpret_cast<AudioBufferList*>(new uint8_t[sz]);
        outputABL->mNumberBuffers = static_cast<UInt32>(nCh);
        outputData.assign(static_cast<size_t>(nCh * frames), 0.0f);
        for (int ch = 0; ch < nCh; ++ch) {
            outputABL->mBuffers[ch].mNumberChannels = 1;
            outputABL->mBuffers[ch].mDataByteSize =
                static_cast<UInt32>(frames * static_cast<int>(sizeof(float)));
            outputABL->mBuffers[ch].mData =
                outputData.data() + static_cast<size_t>(ch * frames);
        }
    }

    void allocInputScratch(int nCh, int frames) {
        inputScratch.assign(static_cast<size_t>(nCh * frames), 0.0f);
        inputPtrs.resize(static_cast<size_t>(nCh));
        for (int ch = 0; ch < nCh; ++ch)
            inputPtrs[static_cast<size_t>(ch)] =
                inputScratch.data() + static_cast<size_t>(ch * frames);
    }
};

// ── Latency property change callback — fires on main run loop via AUEventListener ──

static void auLatencyChangedCallback(
        void* inUserData, void* /*inObject*/,
        const AudioUnitEvent* /*inEvent*/,
        UInt64 /*inEventHostTime*/, AudioUnitParameterValue /*inParameterValue*/)
{
    auto* impl = static_cast<AUPluginAdapterImpl*>(inUserData);
    if (impl->onLatencyChanged) impl->onLatencyChanged();
}

// ── Bypass property change callback — fires on main run loop via AUEventListener ──

static void auBypassChangedCallback(
        void* inUserData, void* /*inObject*/,
        const AudioUnitEvent* /*inEvent*/,
        UInt64 /*inEventHostTime*/, AudioUnitParameterValue /*inParameterValue*/)
{
    auto* impl = static_cast<AUPluginAdapterImpl*>(inUserData);
    if (!impl->onBypassChanged) return;
    UInt32 bypassVal = 0;
    UInt32 sz = sizeof(bypassVal);
    AudioUnitGetProperty(impl->au, kAudioUnitProperty_BypassEffect,
                         kAudioUnitScope_Global, 0, &bypassVal, &sz);
    impl->onBypassChanged(bypassVal != 0);
}

// ── Parameter change callback — fires on main run loop via AUEventListener ──

static void auParamChangedCallback(
        void* inUserData, void* /*inObject*/,
        const AudioUnitEvent* /*inEvent*/,
        UInt64 /*inEventHostTime*/, AudioUnitParameterValue /*inParameterValue*/)
{
    auto* impl = static_cast<AUPluginAdapterImpl*>(inUserData);
    if (impl->suppressCount > 0) return;
    if (impl->onParamChanged) impl->onParamChanged();
}

// ── Render callback — called by the AU during AudioUnitRender to pull input ──

static OSStatus auRenderCallback(
        void*                          inRefCon,
        AudioUnitRenderActionFlags*    /*ioActionFlags*/,
        const AudioTimeStamp*          /*inTimeStamp*/,
        UInt32                         /*inBusNumber*/,
        UInt32                         inNumberFrames,
        AudioBufferList*               ioData)
{
    const auto* impl = static_cast<const AUPluginAdapterImpl*>(inRefCon);
    const UInt32 frameBytes = static_cast<UInt32>(inNumberFrames * sizeof(float));

    for (UInt32 ch = 0; ch < ioData->mNumberBuffers; ++ch) {
        const size_t chIdx = static_cast<size_t>(ch);
        const bool   haveInput = (chIdx < impl->inputPtrs.size() &&
                                  impl->inputPtrs[chIdx] != nullptr);

        if (ioData->mBuffers[ch].mData == nullptr) {
            // AU passed null mData — supply a direct pointer to our scratch.
            ioData->mBuffers[ch].mData = haveInput
                ? const_cast<float*>(impl->inputPtrs[chIdx])
                : nullptr;
            ioData->mBuffers[ch].mDataByteSize = frameBytes;
        } else {
            const UInt32 copyBytes =
                std::min(frameBytes, ioData->mBuffers[ch].mDataByteSize);
            if (haveInput)
                std::memcpy(ioData->mBuffers[ch].mData,
                            impl->inputPtrs[chIdx], copyBytes);
            else
                std::memset(ioData->mBuffers[ch].mData, 0, copyBytes);
        }
    }
    return noErr;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

static void loadParams(AUPluginAdapterImpl& impl) {
    impl.params.clear();
    impl.paramAuIds.clear();
    impl.paramIndex.clear();

    UInt32  dataSize = 0;
    Boolean writable = false;
    OSStatus err = AudioUnitGetPropertyInfo(
        impl.au, kAudioUnitProperty_ParameterList,
        kAudioUnitScope_Global, 0, &dataSize, &writable);
    if (err != noErr || dataSize == 0) return;

    const size_t nParams = dataSize / sizeof(AudioUnitParameterID);
    std::vector<AudioUnitParameterID> auIds(nParams);
    err = AudioUnitGetProperty(impl.au, kAudioUnitProperty_ParameterList,
                               kAudioUnitScope_Global, 0,
                               auIds.data(), &dataSize);
    if (err != noErr) return;

    for (size_t i = 0; i < auIds.size(); ++i) {
        AudioUnitParameterInfo info{};
        UInt32 infoSz = sizeof(info);
        err = AudioUnitGetProperty(impl.au, kAudioUnitProperty_ParameterInfo,
                                   kAudioUnitScope_Global, auIds[i],
                                   &info, &infoSz);
        if (err != noErr) continue;

        ParameterInfo pi;
        pi.id           = std::to_string(auIds[i]);
        pi.minValue     = info.minValue;
        pi.maxValue     = info.maxValue;
        pi.defaultValue = info.defaultValue;
        pi.automatable  = !(info.flags & kAudioUnitParameterFlag_NonRealTime);
        pi.discrete     = (info.unit == kAudioUnitParameterUnit_Indexed ||
                           info.unit == kAudioUnitParameterUnit_Boolean);

        // Name — prefer CFString if available
        if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) &&
             info.cfNameString) {
            char buf[256] = {};
            CFStringGetCString(info.cfNameString, buf, sizeof(buf),
                               kCFStringEncodingUTF8);
            pi.name = buf;
            CFRelease(info.cfNameString);
        } else {
            pi.name = info.name; // char[52] field
        }

        // Unit string + AutoParam domain
        switch (info.unit) {
        case kAudioUnitParameterUnit_Decibels:
            pi.unit   = "dB";
            pi.domain = mcp::AutoParam::Domain::DB;
            break;
        case kAudioUnitParameterUnit_Milliseconds: pi.unit = "ms";  break;
        case kAudioUnitParameterUnit_Seconds:      pi.unit = "s";   break;
        case kAudioUnitParameterUnit_Hertz:        pi.unit = "Hz";  break;
        case kAudioUnitParameterUnit_Cents:        pi.unit = "ct";  break;
        case kAudioUnitParameterUnit_Degrees:      pi.unit = "°";   break;
        case kAudioUnitParameterUnit_Percent:      pi.unit = "%";   break;
        case kAudioUnitParameterUnit_BPM:          pi.unit = "BPM"; break;
        default:                                                     break;
        }

        // Linear normalization (min → 0, max → 1)
        const float mn    = info.minValue;
        const float range = info.maxValue - info.minValue;
        pi.toNormalized = [mn, range](float v) -> float {
            return range > 0.0f ? std::clamp((v - mn) / range, 0.0f, 1.0f) : 0.0f;
        };
        pi.fromNormalized = [mn, range](float n) -> float {
            return mn + std::clamp(n, 0.0f, 1.0f) * range;
        };

        impl.paramIndex[pi.id] = static_cast<int>(impl.params.size());
        impl.paramAuIds.push_back(auIds[i]);
        impl.params.push_back(std::move(pi));
    }
}

static bool initAUFormat(AUPluginAdapterImpl& impl, double sampleRate) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = static_cast<UInt32>(impl.numChannels);
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerPacket   = sizeof(float);
    fmt.mBytesPerFrame    = sizeof(float);

    // Ignore errors — some AUs refuse to set format before initialize
    AudioUnitSetProperty(impl.au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input,  0, &fmt, sizeof(fmt));
    AudioUnitSetProperty(impl.au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output, 0, &fmt, sizeof(fmt));

    return (AudioUnitInitialize(impl.au) == noErr);
}

// ── Factory ───────────────────────────────────────────────────────────────────

AUPluginAdapter::AUPluginAdapter()
    : m_impl(std::make_unique<AUPluginAdapterImpl>()) {}

AUPluginAdapter::~AUPluginAdapter() {
    stopWatchingLatency();
    stopWatchingBypass();
    if (m_impl->paramListenerRef) {
        AUListenerDispose(m_impl->paramListenerRef);
        m_impl->paramListenerRef = nullptr;
    }
    if (m_impl->editorViewHandle) {
        // Detach the Cocoa editor view from its host NSView BEFORE disposing the
        // AudioUnit.  CAAppleAUCustomViewBase::cleanup sends messages to the AU
        // instance; if the view is still embedded when the AU is freed, those
        // messages hit a dangling pointer and crash (EXC_BAD_ACCESS / SIGABRT).
        NSView* v = (__bridge NSView*)m_impl->editorViewHandle;
        [v removeFromSuperview];
        CFRelease(m_impl->editorViewHandle);
        m_impl->editorViewHandle = nullptr;
    }
    if (m_impl->au) {
        // Do NOT call AudioUnitUninitialize here: it kicks off asynchronous
        // AudioToolbox dispatch-source cancellations that race with the
        // AudioComponentInstanceDispose call below.  Some plugins (e.g. Waves
        // WaveShell) crash in FreeAutomation() when those two teardown paths
        // overlap.  AudioComponentInstanceDispose calls AP_Close internally
        // which handles full synchronous cleanup without the race.
        AudioComponentInstanceDispose(m_impl->au);
    }
    m_impl->freeOutputABL();
}

std::unique_ptr<AUPluginAdapter> AUPluginAdapter::create(const Descriptor& desc) {
    AudioComponentDescription acd{};
    acd.componentType         = desc.type;
    acd.componentSubType      = desc.subtype;
    acd.componentManufacturer = desc.manufacturer;

    AudioComponent comp = AudioComponentFindNext(nullptr, &acd);
    if (!comp) return nullptr;

    auto adapter = std::unique_ptr<AUPluginAdapter>(new AUPluginAdapter());
    AUPluginAdapterImpl& impl = *adapter->m_impl;
    impl.numChannels = desc.numChannels;

    // Build a stable string ID from four-char codes
    auto fcc = [](uint32_t v, char* out) {
        out[0] = static_cast<char>((v >> 24) & 0xFF);
        out[1] = static_cast<char>((v >> 16) & 0xFF);
        out[2] = static_cast<char>((v >>  8) & 0xFF);
        out[3] = static_cast<char>( v        & 0xFF);
        out[4] = '\0';
    };
    char t[5], s[5], m[5];
    fcc(desc.type, t); fcc(desc.subtype, s); fcc(desc.manufacturer, m);
    impl.pluginId = std::string("au:") + t + "/" + s + "/" + m;

    // Display name from the AudioComponent
    CFStringRef nameRef = nullptr;
    if (AudioComponentCopyName(comp, &nameRef) == noErr && nameRef) {
        char buf[256] = {};
        CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8);
        impl.displayName = buf;
        CFRelease(nameRef);
    } else {
        impl.displayName = impl.pluginId;
    }

    // Instantiate
    if (AudioComponentInstanceNew(comp, &impl.au) != noErr || !impl.au)
        return nullptr;

    // Wire render callback on input bus 0 BEFORE AudioUnitInitialize
    AURenderCallbackStruct cb{auRenderCallback, &impl};
    AudioUnitSetProperty(impl.au, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    // Initialize at default rate to make parameter metadata available
    if (!initAUFormat(impl, 48000.0)) {
        AudioComponentInstanceDispose(impl.au);
        impl.au = nullptr;
        return nullptr;
    }

    loadParams(impl);
    return adapter;
}

// ── prepare ───────────────────────────────────────────────────────────────────

void AUPluginAdapter::prepare(const ProcessContext& ctx) {
    AUPluginAdapterImpl& impl = *m_impl;
    impl.sampleRate   = ctx.sampleRate;
    impl.maxBlockSize = ctx.maxBlockSize;
    impl.numChannels  = std::min(ctx.inputChannels, ctx.outputChannels);

    // Re-initialize at the new sample rate
    impl.renderSampleTime = 0;
    AudioUnitUninitialize(impl.au);
    initAUFormat(impl, ctx.sampleRate);

    // Allocate scratch / output buffers (no allocation in process())
    impl.allocInputScratch(impl.numChannels, ctx.maxBlockSize);
    impl.allocOutputABL   (impl.numChannels, ctx.maxBlockSize);
    impl.prepared = true;
}

// ── process ───────────────────────────────────────────────────────────────────

void AUPluginAdapter::process(const AudioBlock& block, const EventBlock& events) {
    AUPluginAdapterImpl& impl = *m_impl;
    const int nCh   = std::min({block.numInputChannels,
                                 block.numOutputChannels,
                                 impl.numChannels});
    const int nSamp = block.numSamples;

    if (!impl.au || !impl.prepared || nCh <= 0 || nSamp <= 0) {
        // Pass-through when not ready to preserve signal flow
        const size_t bytes = static_cast<size_t>(nSamp) * sizeof(float);
        for (int ch = 0; ch < block.numOutputChannels && ch < block.numInputChannels; ++ch)
            std::memcpy(block.outputs[ch], block.inputs[ch], bytes);
        return;
    }

    // Block-accurate parameter events (AUv2 has no sample-accurate scheduling).
    // AudioUnitSetParameter is documented as render-thread-safe for AUv2 effects;
    // poorly-written AUs may allocate internally — that is outside our control.
    // ChannelPluginChain currently passes kNoEvents, so this path is dormant
    // until a future automation-via-EventBlock pipeline is wired up.
    for (int i = 0; i < events.numParameterEvents; ++i) {
        const auto& ev = events.parameterEvents[i];
        auto it = impl.paramIndex.find(ev.parameterId);
        if (it == impl.paramIndex.end()) continue;
        AudioUnitSetParameter(impl.au,
                              impl.paramAuIds[static_cast<size_t>(it->second)],
                              kAudioUnitScope_Global, 0,
                              static_cast<AudioUnitParameterValue>(ev.value), 0);
    }

    // Copy block inputs into pre-allocated scratch for the render callback
    const size_t frameBytes = static_cast<size_t>(nSamp) * sizeof(float);
    for (int ch = 0; ch < nCh; ++ch)
        std::memcpy(const_cast<float*>(impl.inputPtrs[static_cast<size_t>(ch)]),
                    block.inputs[ch], frameBytes);

    // Update ABL byte counts for this (possibly shorter) block
    for (int ch = 0; ch < impl.numChannels; ++ch)
        impl.outputABL->mBuffers[ch].mDataByteSize =
            static_cast<UInt32>(nSamp * static_cast<int>(sizeof(float)));

    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags      = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = static_cast<Float64>(impl.renderSampleTime);
    impl.renderSampleTime += static_cast<uint64_t>(nSamp);

    const OSStatus err = AudioUnitRender(
        impl.au, &flags, &ts,
        0,                              // output bus 0
        static_cast<UInt32>(nSamp),
        impl.outputABL);

    if (err != noErr) {
        // On render error: pass through
        for (int ch = 0; ch < block.numOutputChannels && ch < block.numInputChannels; ++ch)
            std::memcpy(block.outputs[ch], block.inputs[ch], frameBytes);
        return;
    }

    // Copy AU output to block.outputs
    for (int ch = 0; ch < nCh; ++ch)
        std::memcpy(block.outputs[ch],
                    impl.outputABL->mBuffers[ch].mData, frameBytes);
    for (int ch = nCh; ch < block.numOutputChannels; ++ch)
        std::memset(block.outputs[ch], 0, frameBytes);
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

void AUPluginAdapter::reset() {
    if (m_impl->au)
        AudioUnitReset(m_impl->au, kAudioUnitScope_Global, 0);
}
void AUPluginAdapter::suspend() { reset(); }
void AUPluginAdapter::resume()  {}

// ── identity ──────────────────────────────────────────────────────────────────

const std::string& AUPluginAdapter::pluginId()    const { return m_impl->pluginId; }
const std::string& AUPluginAdapter::displayName() const { return m_impl->displayName; }

// ── latency / tail ────────────────────────────────────────────────────────────

int AUPluginAdapter::getLatencySamples() const {
    Float64 latSec = 0.0;
    UInt32 sz = sizeof(latSec);
    AudioUnitGetProperty(m_impl->au, kAudioUnitProperty_Latency,
                         kAudioUnitScope_Global, 0, &latSec, &sz);
    return static_cast<int>(std::round(latSec * m_impl->sampleRate));
}

int AUPluginAdapter::getTailSamples() const {
    Float64 tailSec = 0.0;
    UInt32 sz = sizeof(tailSec);
    AudioUnitGetProperty(m_impl->au, kAudioUnitProperty_TailTime,
                         kAudioUnitScope_Global, 0, &tailSec, &sz);
    return static_cast<int>(std::round(tailSec * m_impl->sampleRate));
}

// ── parameters ────────────────────────────────────────────────────────────────

const std::vector<ParameterInfo>& AUPluginAdapter::getParameters() const {
    return m_impl->params;
}

float AUPluginAdapter::getParameterValue(const std::string& id) const {
    auto it = m_impl->paramIndex.find(id);
    if (it == m_impl->paramIndex.end()) return 0.0f;
    AudioUnitParameterValue val = 0.0f;
    AudioUnitGetParameter(m_impl->au,
                          m_impl->paramAuIds[static_cast<size_t>(it->second)],
                          kAudioUnitScope_Global, 0, &val);
    return static_cast<float>(val);
}

void AUPluginAdapter::setParameterValue(const std::string& id, float value) {
    auto it = m_impl->paramIndex.find(id);
    if (it == m_impl->paramIndex.end()) return;
    AudioUnitSetParameter(m_impl->au,
                          m_impl->paramAuIds[static_cast<size_t>(it->second)],
                          kAudioUnitScope_Global, 0,
                          static_cast<AudioUnitParameterValue>(value), 0);
}

float AUPluginAdapter::getNormalizedParameterValue(const std::string& id) const {
    auto it = m_impl->paramIndex.find(id);
    if (it == m_impl->paramIndex.end()) return 0.0f;
    const ParameterInfo& pi = m_impl->params[static_cast<size_t>(it->second)];
    return pi.toNormalized ? pi.toNormalized(getParameterValue(id)) : 0.0f;
}

void AUPluginAdapter::setNormalizedParameterValue(const std::string& id, float n) {
    auto it = m_impl->paramIndex.find(id);
    if (it == m_impl->paramIndex.end()) return;
    const ParameterInfo& pi = m_impl->params[static_cast<size_t>(it->second)];
    if (pi.fromNormalized) setParameterValue(id, pi.fromNormalized(n));
}

// ── state ─────────────────────────────────────────────────────────────────────

PluginState AUPluginAdapter::getState() const {
    PluginState st;
    st.pluginId = m_impl->pluginId;
    st.backend  = PluginBackend::AU;
    st.version  = 1;

    // Primary: binary-plist snapshot via kAudioUnitProperty_ClassInfo
    CFPropertyListRef classInfo = nullptr;
    UInt32 sz = sizeof(classInfo);
    if (AudioUnitGetProperty(m_impl->au, kAudioUnitProperty_ClassInfo,
                             kAudioUnitScope_Global, 0,
                             &classInfo, &sz) == noErr && classInfo) {
        CFDataRef data = CFPropertyListCreateData(
            kCFAllocatorDefault, classInfo,
            kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
        CFRelease(classInfo);
        if (data) {
            const uint8_t* bytes = CFDataGetBytePtr(data);
            st.stateData.assign(bytes, bytes + CFDataGetLength(data));
            CFRelease(data);
        }
    }

    // Supplementary flat snapshot (fallback if plist restore fails)
    for (size_t i = 0; i < m_impl->params.size(); ++i) {
        AudioUnitParameterValue val = 0.0f;
        AudioUnitGetParameter(m_impl->au, m_impl->paramAuIds[i],
                              kAudioUnitScope_Global, 0, &val);
        st.parameters[m_impl->params[i].id] = static_cast<float>(val);
    }

    return st;
}

void AUPluginAdapter::setState(const PluginState& state) {
    if (!state.stateData.empty()) {
        CFDataRef data = CFDataCreate(
            kCFAllocatorDefault,
            state.stateData.data(),
            static_cast<CFIndex>(state.stateData.size()));
        if (data) {
            CFPropertyListRef classInfo = CFPropertyListCreateWithData(
                kCFAllocatorDefault, data,
                kCFPropertyListImmutable, nullptr, nullptr);
            CFRelease(data);
            if (classInfo) {
                AudioUnitSetProperty(m_impl->au, kAudioUnitProperty_ClassInfo,
                                     kAudioUnitScope_Global, 0,
                                     &classInfo, sizeof(classInfo));
                CFRelease(classInfo);
                // No early return: fall through to apply any individual param overrides.
                // This allows partial-scope snapshot recall to override specific params
                // on top of the blob-restored baseline.
            }
        }
    }

    // Apply individual parameters — either as a standalone fallback (no blob),
    // or as per-param overrides on top of a just-restored blob state.
    for (const auto& [id, val] : state.parameters)
        setParameterValue(id, val);
}

// ── Cocoa editor view ─────────────────────────────────────────────────────────

void* AUPluginAdapter::createCocoaView(int& outW, int& outH) {
    outW = outH = 0;
    if (!m_impl->au) return nullptr;

    // Release previous view — CAAppleAUCustomViewBase::cleanup invalidates it
    // during the dialog-close NSView dealloc chain.  Reusing an invalidated
    // view on a second open causes a crash on the second close.
    if (m_impl->editorViewHandle) {
        NSView* v = (__bridge NSView*)m_impl->editorViewHandle;
        [v removeFromSuperview];
        CFRelease(m_impl->editorViewHandle);
        m_impl->editorViewHandle = nullptr;
    }

    // Query whether the AU exposes a Cocoa UI.
    UInt32   dataSize = 0;
    Boolean  writable = false;
    OSStatus err = AudioUnitGetPropertyInfo(m_impl->au,
                       kAudioUnitProperty_CocoaUI,
                       kAudioUnitScope_Global, 0,
                       &dataSize, &writable);
    if (err != noErr || dataSize < sizeof(AudioUnitCocoaViewInfo)) return nullptr;

    const UInt32 numClasses =
        (dataSize - offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass)) /
        sizeof(CFStringRef);
    if (numClasses == 0) return nullptr;

    // Heap-allocate to match the variable-length struct.
    auto* viewInfo = reinterpret_cast<AudioUnitCocoaViewInfo*>(
        ::operator new(static_cast<size_t>(dataSize)));
    err = AudioUnitGetProperty(m_impl->au,
              kAudioUnitProperty_CocoaUI,
              kAudioUnitScope_Global, 0,
              viewInfo, &dataSize);
    if (err != noErr) { ::operator delete(viewInfo); return nullptr; }

    NSURL*    bundleURL  = CFBridgingRelease(viewInfo->mCocoaAUViewBundleLocation);
    NSString* className  = CFBridgingRelease(viewInfo->mCocoaAUViewClass[0]);
    for (UInt32 i = 1; i < numClasses; ++i)
        CFRelease(viewInfo->mCocoaAUViewClass[i]);
    ::operator delete(viewInfo);

    NSBundle* bundle = [NSBundle bundleWithURL:bundleURL];
    if (!bundle || ![bundle load]) return nullptr;

    Class factoryClass = [bundle classNamed:className];
    if (!factoryClass ||
        ![factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]) return nullptr;

    id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
    if (!factory) return nullptr;

    NSView* view = [factory uiViewForAudioUnit:m_impl->au
                                      withSize:NSMakeSize(400.0, 300.0)];
    if (!view) return nullptr;

    // Retain the view for the lifetime of this adapter.
    m_impl->editorViewHandle = CFBridgingRetain(view);

    outW = static_cast<int>(view.bounds.size.width);
    outH = static_cast<int>(view.bounds.size.height);
    return (__bridge void*)view;
}

// ── Parameter watching ────────────────────────────────────────────────────────

void AUPluginAdapter::startWatchingParameters(std::function<void()> onChanged)
{
    stopWatchingParameters();
    if (!m_impl->au || m_impl->params.empty()) return;

    m_impl->onParamChanged = std::move(onChanged);
    AUEventListenerCreate(auParamChangedCallback, m_impl.get(),
                          CFRunLoopGetMain(), kCFRunLoopDefaultMode,
                          0.05f, 0.05f,
                          &m_impl->paramListenerRef);

    for (size_t i = 0; i < m_impl->paramAuIds.size(); ++i) {
        AudioUnitEvent evt{};
        evt.mEventType = kAudioUnitEvent_ParameterValueChange;
        evt.mArgument.mParameter.mAudioUnit      = m_impl->au;
        evt.mArgument.mParameter.mParameterID    = m_impl->paramAuIds[i];
        evt.mArgument.mParameter.mScope          = kAudioUnitScope_Global;
        evt.mArgument.mParameter.mElement        = 0;
        AUEventListenerAddEventType(m_impl->paramListenerRef, m_impl.get(), &evt);
    }
}

void AUPluginAdapter::stopWatchingParameters()
{
    if (m_impl->paramListenerRef) {
        AUListenerDispose(m_impl->paramListenerRef);
        m_impl->paramListenerRef = nullptr;
    }
    m_impl->onParamChanged = nullptr;
    m_impl->suppressCount  = 0;
}

void AUPluginAdapter::notifyViewRefresh()
{
    if (!m_impl->au || m_impl->params.empty()) return;

    // Suppress the onParamChanged callback so that programmatically re-sending
    // parameter values doesn't mark the plugin dirty.
    ++m_impl->suppressCount;

    for (size_t i = 0; i < m_impl->params.size(); ++i) {
        AudioUnitParameterValue v = 0.0f;
        AudioUnitGetParameter(m_impl->au, m_impl->paramAuIds[i],
                              kAudioUnitScope_Global, 0, &v);
        AudioUnitSetParameter(m_impl->au, m_impl->paramAuIds[i],
                              kAudioUnitScope_Global, 0, v, 0);
    }

    // AUEventListener dispatches with up to 50 ms granularity; clear suppress
    // after a safe margin so the callbacks fire (suppressed) before the flag resets.
    AUPluginAdapterImpl* impl = m_impl.get();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (impl->suppressCount > 0) --impl->suppressCount;
    });
}

// ── Bypass bridging ───────────────────────────────────────────────────────────

void AUPluginAdapter::setNativeBypass(bool bypass)
{
    if (!m_impl->au) return;
    UInt32 val = bypass ? 1 : 0;
    AudioUnitSetProperty(m_impl->au, kAudioUnitProperty_BypassEffect,
                         kAudioUnitScope_Global, 0, &val, sizeof(val));
}

bool AUPluginAdapter::getNativeBypass() const
{
    if (!m_impl->au) return false;
    UInt32 val = 0;
    UInt32 sz = sizeof(val);
    AudioUnitGetProperty(m_impl->au, kAudioUnitProperty_BypassEffect,
                         kAudioUnitScope_Global, 0, &val, &sz);
    return val != 0;
}

void AUPluginAdapter::startWatchingBypass(std::function<void(bool bypassed)> onChanged)
{
    stopWatchingBypass();
    if (!m_impl->au) return;

    m_impl->onBypassChanged = std::move(onChanged);
    AUEventListenerCreate(auBypassChangedCallback, m_impl.get(),
                          CFRunLoopGetMain(), kCFRunLoopDefaultMode,
                          0.05f, 0.05f,
                          &m_impl->bypassListenerRef);

    AudioUnitEvent evt{};
    evt.mEventType                          = kAudioUnitEvent_PropertyChange;
    evt.mArgument.mProperty.mAudioUnit      = m_impl->au;
    evt.mArgument.mProperty.mPropertyID     = kAudioUnitProperty_BypassEffect;
    evt.mArgument.mProperty.mScope          = kAudioUnitScope_Global;
    evt.mArgument.mProperty.mElement        = 0;
    AUEventListenerAddEventType(m_impl->bypassListenerRef, m_impl.get(), &evt);
}

void AUPluginAdapter::stopWatchingBypass()
{
    if (m_impl->bypassListenerRef) {
        AUListenerDispose(m_impl->bypassListenerRef);
        m_impl->bypassListenerRef = nullptr;
    }
    m_impl->onBypassChanged = nullptr;
}

// ── Latency watching ──────────────────────────────────────────────────────────

void AUPluginAdapter::startWatchingLatency(std::function<void()> onChanged)
{
    stopWatchingLatency();
    if (!m_impl->au) return;

    m_impl->onLatencyChanged = std::move(onChanged);
    AUEventListenerCreate(auLatencyChangedCallback, m_impl.get(),
                          CFRunLoopGetMain(), kCFRunLoopDefaultMode,
                          0.05f, 0.05f,
                          &m_impl->latencyListenerRef);

    AudioUnitEvent evt{};
    evt.mEventType                          = kAudioUnitEvent_PropertyChange;
    evt.mArgument.mProperty.mAudioUnit      = m_impl->au;
    evt.mArgument.mProperty.mPropertyID     = kAudioUnitProperty_Latency;
    evt.mArgument.mProperty.mScope          = kAudioUnitScope_Global;
    evt.mArgument.mProperty.mElement        = 0;
    AUEventListenerAddEventType(m_impl->latencyListenerRef, m_impl.get(), &evt);
}

void AUPluginAdapter::stopWatchingLatency()
{
    if (m_impl->latencyListenerRef) {
        AUListenerDispose(m_impl->latencyListenerRef);
        m_impl->latencyListenerRef = nullptr;
    }
    m_impl->onLatencyChanged = nullptr;
}

} // namespace mcp::plugin
#endif // __APPLE__
