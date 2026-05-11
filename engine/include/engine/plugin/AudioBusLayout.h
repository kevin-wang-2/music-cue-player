#pragma once
#include <string>
#include <vector>

namespace mcp::plugin {

struct AudioBus {
    std::string name;
    int  numChannels{0};
    bool isMain{false};
};

struct AudioBusLayout {
    std::vector<AudioBus> inputs;
    std::vector<AudioBus> outputs;

    static AudioBusLayout mono() {
        AudioBusLayout l;
        l.inputs .push_back({"Main", 1, true});
        l.outputs.push_back({"Main", 1, true});
        return l;
    }
    static AudioBusLayout stereo() {
        AudioBusLayout l;
        l.inputs .push_back({"Main", 2, true});
        l.outputs.push_back({"Main", 2, true});
        return l;
    }
};

} // namespace mcp::plugin
