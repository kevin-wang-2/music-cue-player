#include "MidiSender.h"
#include "engine/MidiOut.h"

#include <RtMidi.h>
#include <algorithm>
#include <vector>

namespace mcp {

std::vector<std::string> midiOutputPorts() {
    try {
        RtMidiOut mout;
        std::vector<std::string> result;
        const unsigned int count = mout.getPortCount();
        result.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
            result.push_back(mout.getPortName(i));
        return result;
    } catch (...) {
        return {};
    }
}

bool sendMidiMessage(const ShowFile::MidiSetup::Patch& patch,
                     const std::string& messageType,
                     int channel, int data1, int data2,
                     std::string& error)
{
    try {
        RtMidiOut mout;
        const unsigned int count = mout.getPortCount();
        int portIdx = -1;
        for (unsigned int i = 0; i < count; ++i) {
            if (mout.getPortName(i) == patch.destination) {
                portIdx = static_cast<int>(i);
                break;
            }
        }
        if (portIdx < 0) {
            error = "MIDI port not found: " + patch.destination;
            return false;
        }

        mout.openPort(static_cast<unsigned int>(portIdx));

        const int ch = std::max(1, std::min(16, channel)) - 1; // 0-based
        const auto clamp7 = [](int v) -> unsigned char {
            return static_cast<unsigned char>(std::max(0, std::min(127, v)));
        };

        std::vector<unsigned char> msg;
        if (messageType == "note_on") {
            msg = { static_cast<unsigned char>(0x90 | ch), clamp7(data1), clamp7(data2) };
        } else if (messageType == "note_off") {
            msg = { static_cast<unsigned char>(0x80 | ch), clamp7(data1), clamp7(data2) };
        } else if (messageType == "program_change") {
            msg = { static_cast<unsigned char>(0xC0 | ch), clamp7(data1) };
        } else if (messageType == "control_change") {
            msg = { static_cast<unsigned char>(0xB0 | ch), clamp7(data1), clamp7(data2) };
        } else if (messageType == "pitchbend") {
            const int v14 = std::max(-8192, std::min(8191, data1)) + 8192;
            msg = { static_cast<unsigned char>(0xE0 | ch),
                    static_cast<unsigned char>(v14 & 0x7F),
                    static_cast<unsigned char>((v14 >> 7) & 0x7F) };
        } else {
            error = "unknown MIDI message type: " + messageType;
            mout.closePort();
            return false;
        }

        mout.sendMessage(&msg);
        mout.closePort();
        return true;
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return false;
    } catch (...) {
        error = "unknown MIDI error";
        return false;
    }
}

} // namespace mcp
