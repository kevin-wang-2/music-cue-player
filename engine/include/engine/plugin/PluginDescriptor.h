#pragma once
#include "AudioBusLayout.h"
#include <string>
#include <vector>

namespace mcp::plugin {

enum class PluginBackend { Internal, VST3, AU };

struct PluginDescriptor {
    std::string   id;
    std::string   name;
    std::string   vendor;
    std::string   version;
    PluginBackend backend           {PluginBackend::Internal};
    std::string   category;         // "utility", "dynamics", "delay", …
    bool          supportsAutomation{true};
    bool          hasEditor         {false};
    bool          isInstrument      {false};
    std::vector<AudioBusLayout> supportedLayouts;
};

} // namespace mcp::plugin
