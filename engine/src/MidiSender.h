#pragma once

#include "engine/ShowFile.h"

#include <string>

namespace mcp {

// Send a single MIDI message to the port named by patch.destination.
// messageType: "note_on" | "note_off" | "program_change" | "control_change" | "pitchbend"
// channel:  1–16
// data1:    note / program / controller number; for pitchbend: signed value -8192..8191
// data2:    velocity / CC value (unused for program_change and pitchbend)
// Returns true on success; sets error on failure.
bool sendMidiMessage(const ShowFile::MidiSetup::Patch& patch,
                     const std::string& messageType,
                     int channel, int data1, int data2,
                     std::string& error);

} // namespace mcp
