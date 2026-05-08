#pragma once

#include "engine/TriggerData.h"

#include <RtMidi.h>
#include <QObject>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Manages all MIDI input ports.
// Emits midiReceived for routing to cue triggers and system actions.
// Capture mode: the next incoming message is forwarded to a one-shot callback
// (used by the "Capture" button in the Triggers inspector tab).
class MidiInputManager : public QObject {
    Q_OBJECT
public:
    explicit MidiInputManager(QObject* parent = nullptr);
    ~MidiInputManager() override;

    // Open / refresh all available MIDI input ports.
    void openAll();
    void closeAll();

    // Returns list of currently open port names.
    std::vector<std::string> portNames() const;

    // Arm capture mode: the next MIDI message calls cb once, then capture stops.
    using CaptureCallback = std::function<void(mcp::MidiMsgType, int ch, int d1, int d2)>;
    void armCapture(CaptureCallback cb);
    void cancelCapture();
    bool isCapturing() const { return m_capturing; }

signals:
    // Fired on the Qt main thread for every incoming MIDI message.
    void midiReceived(mcp::MidiMsgType type, int channel, int data1, int data2);

private:
    struct Port {
        std::string           name;
        std::unique_ptr<RtMidiIn> midi;
    };
    std::vector<Port> m_ports;

    bool           m_capturing{false};
    CaptureCallback m_captureCallback;

    static void rtCallback(double, std::vector<unsigned char>*, void*);
    void handleMessage(const std::vector<unsigned char>& msg);
};
