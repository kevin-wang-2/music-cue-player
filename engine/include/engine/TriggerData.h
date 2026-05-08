#pragma once
#include <string>
#include <vector>

namespace mcp {

// Shared MIDI message type enum used by both MIDI cues and MIDI triggers.
// String tokens used in ShowFile JSON:  "note_on" | "note_off" |
//   "control_change" | "program_change" | "pitchbend"
enum class MidiMsgType {
    NoteOn, NoteOff, ControlChange, ProgramChange, PitchBend
};

inline std::string midiMsgTypeToString(MidiMsgType t) {
    switch (t) {
        case MidiMsgType::NoteOn:         return "note_on";
        case MidiMsgType::NoteOff:        return "note_off";
        case MidiMsgType::ControlChange:  return "control_change";
        case MidiMsgType::ProgramChange:  return "program_change";
        case MidiMsgType::PitchBend:      return "pitchbend";
    }
    return "note_on";
}
inline bool midiMsgTypeFromString(const std::string& s, MidiMsgType& out) {
    if (s == "note_on")         { out = MidiMsgType::NoteOn;        return true; }
    if (s == "note_off")        { out = MidiMsgType::NoteOff;       return true; }
    if (s == "control_change")  { out = MidiMsgType::ControlChange; return true; }
    if (s == "program_change")  { out = MidiMsgType::ProgramChange; return true; }
    if (s == "pitchbend")       { out = MidiMsgType::PitchBend;     return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Per-cue trigger settings

struct HotkeyTrigger {
    bool        enabled{false};
    std::string keyString;   // Qt key sequence string, e.g. "Ctrl+F1"
};

struct MidiTrigger {
    bool        enabled{false};
    MidiMsgType type{MidiMsgType::NoteOn};
    int         channel{0};  // 0 = any, 1-16 = specific
    int         data1{60};   // note / CC number / program
    int         data2{-1};   // -1 = any; 0-127 = exact velocity / CC value
};

struct OscTrigger {
    bool        enabled{false};
    std::string path;        // custom OSC path; must not be in system vocabulary
};

struct CueTriggers {
    HotkeyTrigger hotkey;
    MidiTrigger   midi;
    OscTrigger    osc;
};

// ---------------------------------------------------------------------------
// System-action bindings (stored at show level)

enum class ControlAction {
    Go, Arm, PanicSelected, SelectionUp, SelectionDown, PanicAll
};

inline std::string controlActionToString(ControlAction a) {
    switch (a) {
        case ControlAction::Go:             return "go";
        case ControlAction::Arm:            return "arm";
        case ControlAction::PanicSelected:  return "panic_selected";
        case ControlAction::SelectionUp:    return "selection_up";
        case ControlAction::SelectionDown:  return "selection_down";
        case ControlAction::PanicAll:       return "panic_all";
    }
    return "go";
}
inline bool controlActionFromString(const std::string& s, ControlAction& out) {
    if (s == "go")             { out = ControlAction::Go;            return true; }
    if (s == "arm")            { out = ControlAction::Arm;           return true; }
    if (s == "panic_selected") { out = ControlAction::PanicSelected; return true; }
    if (s == "selection_up")   { out = ControlAction::SelectionUp;   return true; }
    if (s == "selection_down") { out = ControlAction::SelectionDown; return true; }
    if (s == "panic_all")      { out = ControlAction::PanicAll;      return true; }
    return false;
}

struct ControlMidiBinding {
    bool        enabled{false};
    MidiMsgType type{MidiMsgType::NoteOn};
    int         channel{0};  // 0 = any
    int         data1{0};
    int         data2{-1};   // -1 = any
};

struct ControlOscBinding {
    bool        enabled{false};
    std::string path;
};

// One entry per ControlAction — use controlActionToString() as map key in JSON.
struct SystemControlBindings {
    // keyed by ControlAction (stored as vector of pairs for simplicity)
    struct Entry {
        ControlAction     action;
        ControlMidiBinding midi;
        ControlOscBinding  osc;
    };
    std::vector<Entry> entries;

    Entry* find(ControlAction a) {
        for (auto& e : entries) if (e.action == a) return &e;
        return nullptr;
    }
    const Entry* find(ControlAction a) const {
        for (const auto& e : entries) if (e.action == a) return &e;
        return nullptr;
    }
    Entry& get(ControlAction a) {
        if (auto* e = find(a)) return *e;
        entries.push_back({a, {}, {}});
        return entries.back();
    }
};

// ---------------------------------------------------------------------------
// OSC server / access control settings

struct OscAccessEntry {
    std::string password;    // empty string = no-password (open access)
};

struct OscServerSettings {
    bool enabled{false};
    int  listenPort{14521};  // default avoids QLab(53000), TheatreMix(32000)
    std::vector<OscAccessEntry> accessList;  // empty = reject all; add entry to allow

    bool requiresPassword() const {
        for (const auto& e : accessList) if (e.password.empty()) return false;
        return !accessList.empty();
    }
    bool acceptsPassword(const std::string& pw) const {
        for (const auto& e : accessList)
            if (e.password.empty() || e.password == pw) return true;
        return false;
    }
};

// System OSC vocabulary — cue OscTrigger paths must not use these.
inline bool isSystemOscPath(const std::string& path) {
    return path == "/go"    || path == "/start" || path == "/stop"  ||
           path == "/panic" || path == "/prev"  || path == "/next"  ||
           path == "/goto";
}

} // namespace mcp
