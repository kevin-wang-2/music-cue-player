#pragma once
#include "AudioProcessor.h"
#include "PluginDescriptor.h"
#include <memory>
#include <string>
#include <vector>

namespace mcp::plugin {

// Abstract factory.  Concrete implementations: InternalPluginFactory,
// and in future Vst3PluginFactory / AuPluginFactory.
class PluginFactory {
public:
    virtual ~PluginFactory() = default;

    // Enumerate available plugins.  May be slow (filesystem scan for external
    // backends).  Not called from the audio thread.
    virtual std::vector<PluginDescriptor> scan() = 0;

    // Instantiate a plugin by its stable id.
    // Returns nullptr if pluginId is unknown or instantiation fails.
    virtual std::unique_ptr<AudioProcessor> create(const std::string& pluginId) = 0;
};

// Factory for built-in (internal) processors.
// Does not scan the filesystem; the plugin set is compiled-in.
class InternalPluginFactory : public PluginFactory {
public:
    std::vector<PluginDescriptor>   scan()   override;
    std::unique_ptr<AudioProcessor> create(const std::string& pluginId) override;
};

} // namespace mcp::plugin
