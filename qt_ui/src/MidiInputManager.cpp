#include "MidiInputManager.h"

#include <QMetaObject>

MidiInputManager::MidiInputManager(QObject* parent) : QObject(parent) {}

MidiInputManager::~MidiInputManager() { closeAll(); }

void MidiInputManager::openAll() {
    closeAll();
    RtMidiIn probe;
    const unsigned int n = probe.getPortCount();
    for (unsigned int i = 0; i < n; ++i) {
        Port p;
        p.name = probe.getPortName(i);
        p.midi = std::make_unique<RtMidiIn>();
        try {
            p.midi->openPort(i);
            p.midi->ignoreTypes(false, true, true); // don't ignore sysex-less clocks/AT
            p.midi->setCallback(&MidiInputManager::rtCallback, this);
            m_ports.push_back(std::move(p));
        } catch (...) {}
    }
}

void MidiInputManager::closeAll() {
    for (auto& p : m_ports) {
        try { p.midi->closePort(); } catch (...) {}
    }
    m_ports.clear();
}

std::vector<std::string> MidiInputManager::portNames() const {
    std::vector<std::string> names;
    for (const auto& p : m_ports) names.push_back(p.name);
    return names;
}

void MidiInputManager::armCapture(CaptureCallback cb) {
    m_capturing = true;
    m_captureCallback = std::move(cb);
}

void MidiInputManager::cancelCapture() {
    m_capturing = false;
    m_captureCallback = nullptr;
}

// ---------------------------------------------------------------------------
// Static RtMidi callback — called from a background thread.
void MidiInputManager::rtCallback(double /*ts*/,
                                   std::vector<unsigned char>* msg,
                                   void* userData) {
    if (!msg || msg->empty()) return;
    static_cast<MidiInputManager*>(userData)->handleMessage(*msg);
}

void MidiInputManager::handleMessage(const std::vector<unsigned char>& msg) {
    if (msg.empty()) return;
    const unsigned char status = msg[0];
    const int ch   = (status & 0x0F) + 1;  // 1-16
    const int d1   = msg.size() > 1 ? msg[1] : 0;
    const int d2   = msg.size() > 2 ? msg[2] : 0;
    const int type = (status >> 4) & 0x0F;

    mcp::MidiMsgType mt;
    switch (type) {
        case 0x9: mt = (d2 == 0) ? mcp::MidiMsgType::NoteOff : mcp::MidiMsgType::NoteOn; break;
        case 0x8: mt = mcp::MidiMsgType::NoteOff;         break;
        case 0xB: mt = mcp::MidiMsgType::ControlChange;   break;
        case 0xC: mt = mcp::MidiMsgType::ProgramChange;   break;
        case 0xE: mt = mcp::MidiMsgType::PitchBend;       break;
        default: return;
    }

    if (m_capturing) {
        m_capturing = false;
        auto cb = std::move(m_captureCallback);
        m_captureCallback = nullptr;
        // Invoke on main thread
        QMetaObject::invokeMethod(this, [cb, mt, ch, d1, d2]() {
            if (cb) cb(mt, ch, d1, d2);
        }, Qt::QueuedConnection);
        return;
    }

    QMetaObject::invokeMethod(this, [this, mt, ch, d1, d2]() {
        emit midiReceived(mt, ch, d1, d2);
    }, Qt::QueuedConnection);
}
