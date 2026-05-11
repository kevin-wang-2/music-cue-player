#pragma once

namespace mcp::plugin {

enum class LatencyMode { Live, Offline };

struct ProcessContext {
    double      sampleRate   {44100.0};
    int         maxBlockSize {512};
    int         inputChannels {2};
    int         outputChannels{2};
    LatencyMode latencyMode  {LatencyMode::Live};
};

} // namespace mcp::plugin
