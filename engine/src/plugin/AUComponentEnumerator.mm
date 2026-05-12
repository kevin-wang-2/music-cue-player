#include "engine/plugin/AUComponentEnumerator.h"
#ifdef __APPLE__

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <cstdio>

namespace mcp::plugin {

static std::string cfToStd(CFStringRef ref) {
    if (!ref) return {};
    if (const char* c = CFStringGetCStringPtr(ref, kCFStringEncodingUTF8))
        return c;
    char buf[512];
    if (CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8))
        return buf;
    return {};
}

static void appendType(OSType auType, const char* label,
                       std::vector<AUComponentEntry>& out)
{
    AudioComponentDescription desc{};
    desc.componentType = auType;

    AudioComponent comp = nullptr;
    while ((comp = AudioComponentFindNext(comp, &desc)) != nullptr) {
        AudioComponentDescription cd{};
        if (AudioComponentGetDescription(comp, &cd) != noErr) continue;

        AUComponentEntry e;
        e.type         = static_cast<uint32_t>(cd.componentType);
        e.subtype      = static_cast<uint32_t>(cd.componentSubType);
        e.manufacturer = static_cast<uint32_t>(cd.componentManufacturer);
        e.typeLabel    = label;

        // Component name format is usually "Vendor: Plugin Name"
        CFStringRef nameRef = nullptr;
        if (AudioComponentCopyName(comp, &nameRef) == noErr && nameRef) {
            const std::string full = cfToStd(nameRef);
            CFRelease(nameRef);
            const auto sep = full.find(':');
            if (sep != std::string::npos) {
                e.manufacturerName = full.substr(0, sep);
                size_t start = sep + 1;
                while (start < full.size() && full[start] == ' ') ++start;
                e.name = full.substr(start);
            } else {
                e.name = full;
            }
        }

        // Version packed as major(8).minor(8).bugfix(16)
        UInt32 v = 0;
        if (AudioComponentGetVersion(comp, &v) == noErr && v != 0) {
            const unsigned maj  = (v >> 24) & 0xFFu;
            const unsigned min_ = (v >> 16) & 0xFFu;
            const unsigned fix  =  v        & 0xFFFFu;
            char buf[32];
            if (fix == 0)
                snprintf(buf, sizeof(buf), "%u.%u", maj, min_);
            else
                snprintf(buf, sizeof(buf), "%u.%u.%u", maj, min_, fix);
            e.version = buf;
        }

        // AUv3 detection without instantiation
        e.supportsEditor = (cd.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;

        out.push_back(std::move(e));
    }
}

std::vector<AUComponentEntry> AUComponentEnumerator::enumerate() {
    std::vector<AUComponentEntry> result;
    appendType(kAudioUnitType_Effect,      "Effect",       result);
    appendType(kAudioUnitType_MusicEffect, "Music Effect", result);
    return result;
}

} // namespace mcp::plugin
#endif // __APPLE__
