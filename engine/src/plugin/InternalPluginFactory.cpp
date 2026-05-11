#include "engine/plugin/PluginFactory.h"
#include "DelayProcessor.h"
#include "TrimProcessor.h"

namespace mcp::plugin {

std::vector<PluginDescriptor> InternalPluginFactory::scan() {
    std::vector<PluginDescriptor> list;

    auto addTrim = [&](int ch) {
        PluginDescriptor d;
        d.id       = (ch == 1) ? "internal.trim.mono" : "internal.trim.stereo";
        d.name     = (ch == 1) ? "Trim (Mono)"        : "Trim (Stereo)";
        d.vendor   = "Internal";
        d.version  = "1.0";
        d.backend  = PluginBackend::Internal;
        d.category = "utility";
        d.supportsAutomation = true;
        d.hasEditor          = false;
        d.isInstrument       = false;
        d.supportedLayouts.push_back((ch == 1) ? AudioBusLayout::mono()
                                                : AudioBusLayout::stereo());
        list.push_back(std::move(d));
    };

    auto addDelay = [&](int ch) {
        PluginDescriptor d;
        d.id       = (ch == 1) ? "internal.delay.mono" : "internal.delay.stereo";
        d.name     = (ch == 1) ? "Delay (Mono)"        : "Delay (Stereo)";
        d.vendor   = "Internal";
        d.version  = "1.0";
        d.backend  = PluginBackend::Internal;
        d.category = "delay";
        d.supportsAutomation = true;
        d.hasEditor          = false;
        d.isInstrument       = false;
        d.supportedLayouts.push_back((ch == 1) ? AudioBusLayout::mono()
                                                : AudioBusLayout::stereo());
        list.push_back(std::move(d));
    };

    addTrim (1); addTrim (2);
    addDelay(1); addDelay(2);
    return list;
}

std::unique_ptr<AudioProcessor> InternalPluginFactory::create(const std::string& pluginId) {
    if (pluginId == "internal.trim.mono")    return std::make_unique<TrimProcessor> (1);
    if (pluginId == "internal.trim.stereo")  return std::make_unique<TrimProcessor> (2);
    if (pluginId == "internal.delay.mono")   return std::make_unique<DelayProcessor>(1);
    if (pluginId == "internal.delay.stereo") return std::make_unique<DelayProcessor>(2);
    return nullptr;
}

} // namespace mcp::plugin
