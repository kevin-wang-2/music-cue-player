#pragma once

namespace mcp {

// Abstract automation parameter target.
//
// Heavy initialisation (path parsing, index resolution, plugin handshake, etc.)
// belongs in the constructor or a factory.  setValue() is called ~200 times/sec
// from the scheduler thread and must be as lightweight as possible.
//
// Built-in implementations (mixer fader/mute/crosspoint) live in CueList.cpp
// and access channel-map state directly through friend access.
// Third-party plugin parameters subclass AutoParam and manage their own storage.
class AutoParam {
public:
    // Value domain: determines how the parameter value is interpreted.
    // Linear  = raw linear value (mute: 0/1, polarity: 0/1)
    // DB      = decibels (crosspoint level)
    // FaderTaper = fader dB with log taper (channel fader)
    enum class Domain { Linear, DB, FaderTaper };

    virtual ~AutoParam() = default;

    // Apply a new value.  Called from the scheduler thread; must be lock-free
    // or use only very brief critical sections.
    virtual void setValue(double value) = 0;

    virtual Domain domain() const { return Domain::Linear; }
};

} // namespace mcp
