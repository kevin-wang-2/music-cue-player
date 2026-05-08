#pragma once

#include <string>
#include <vector>

namespace mcp {

// Returns the names of all currently available MIDI output ports.
// Uses RtMidi; returns an empty vector if no MIDI backend is available.
std::vector<std::string> midiOutputPorts();

} // namespace mcp
