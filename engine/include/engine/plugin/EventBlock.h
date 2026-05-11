#pragma once
#include <string>

namespace mcp::plugin {

// A single parameter automation event for one block.
// parameterId uses SSO for typical short IDs ("gain_db", "delay_ms") so
// construction does not heap-allocate in practice.
struct ParameterEvent {
    std::string parameterId;
    float       value       {0.0f};  // real (not normalized) value
    int         sampleOffset{0};     // 0 <= sampleOffset < numSamples
};

// Non-owning view of events for the current block.
// Events MUST be sorted ascending by sampleOffset before process() is called.
// process() must not modify or retain this data.
struct EventBlock {
    const ParameterEvent* parameterEvents   {nullptr};
    int                   numParameterEvents{0};
};

} // namespace mcp::plugin
