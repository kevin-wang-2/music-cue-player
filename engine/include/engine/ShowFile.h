#pragma once

#include "TriggerData.h"
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace mcp {

// Serialisable, version-tagged representation of a show.
// Loaded from / saved to JSON.
//
// Extensibility contract
// ----------------------
//   - Unknown JSON keys at any level are silently ignored on load, so files
//     written by a newer version can be opened by an older binary (unknown
//     features are dropped on re-save, but nothing is corrupted).
//   - Optional fields default to sensible values when absent, so files written
//     by an older version can be opened by a newer binary.
//   - Bump kCurrentVersion minor when adding optional fields, major when
//     making a structural breaking change.
//   - CueData::type drives dispatch; unknown types produce a warning and are
//     skipped by consumers — adding a new cue type never breaks old readers.
//
// Layout (JSON top-level keys)
// ----------------------------
//   mcp_version  — format version string, e.g. "1.0"
//   show         — show metadata (title, …)
//   engine       — preferred engine settings (sampleRate, channels, …)
//   cueLists     — array of named cue lists (currently one, future: many)
struct ShowFile {
    static constexpr const char* kCurrentVersion = "1.0";

    // ---- Cue data ----------------------------------------------------------
    struct CueData {
        std::string type;
        std::string cueNumber;
        std::string name;
        // Audio cues
        std::string path;
        // Start / Stop / Arm / Fade cues
        int         target{-1};
        std::string targetCueNumber;
        // Arm cues: pre-load the target from this offset (seconds)
        double      armStartTime{0.0};
        // All types
        double      preWait{0.0};
        int         goQuantize{0};   // 0=none, 1=next bar, 2=next beat
        bool        autoContinue{false};
        bool        autoFollow{false};
        // Audio cues: playback region
        double      startTime{0.0};
        double      duration{0.0};
        // Audio cues: gain
        double      level{0.0};
        double      trim{0.0};
        // Audio cues: per-output-channel levels (dB); index = output channel
        std::vector<float> outLevelDb;
        // Audio cues: crosspoint matrix entries (only enabled cells are stored)
        struct XpEntry { int s{0}; int o{0}; float db{0.0f}; };
        std::vector<XpEntry> xpEntries;
        // Time markers (audio cues only) — absolute file positions, sorted by time
        struct TimeMarker {
            double time{0.0};
            std::string name;
            // Cue number of the Marker cue anchored to this marker ("" = none).
            std::string anchorMarkerCueNumber;
        };
        std::vector<TimeMarker> markers;
        // Marker cues only: which marker within the target cue (-1 = cue start)
        int markerIndex{-1};
        std::vector<int>        sliceLoops;  // [i] = loops for slice i; 0 = infinite

        // Devamp cues
        int         devampMode{0};        // 0=NextSlice, 1=go()+StopCurrent, 2=go()+KeepCurrent
        bool        devampPreVamp{false}; // skip subsequent looping slices after devamp

        // Scriptlet cues
        std::string scriptletCode;      // Python source code to execute

        // Network cues
        std::string networkPatchName;   // name of the target NetworkSetup patch
        std::string networkCommand;     // command string (OSC or plain text)

        // MIDI cues
        std::string midiPatchName;      // name of the target MidiSetup patch
        std::string midiMessageType;    // "note_on"|"note_off"|"program_change"|"control_change"|"pitchbend"
        int         midiChannel{1};     // 1–16
        int         midiData1{60};      // note/program/controller number; pitchbend value LSB
        int         midiData2{64};      // velocity/value (unused for program_change and pitchbend)

        // Timecode cues
        std::string tcType;             // "ltc" | "mtc"
        std::string tcFps;              // "24fps"|"25fps"|"30fps_nd"|"30fps_df"|"23.976fps"|"24.975fps"|"29.97fps_nd"|"29.97fps_df"
        std::string tcStartTC;          // "hh:mm:ss:ff"
        std::string tcEndTC;            // "hh:mm:ss:ff"
        int         tcLtcChannel{0};    // LTC: 0-based physical output channel
        std::string tcMidiPatchName;    // MTC: MIDI patch name

        // Fade cues
        std::string fadeCurve{"linear"};
        bool        fadeStopWhenDone{false};
        bool        fadeMasterEnabled{false};
        float       fadeMasterTarget{0.0f};
        struct FadeOutLevel { int ch{0}; bool enabled{false}; float target{0.0f}; };
        std::vector<FadeOutLevel> fadeOutLevels;
        struct FadeXpEntry { int s{0}; int o{0}; bool enabled{false}; float target{0.0f}; };
        std::vector<FadeXpEntry> fadeXpEntries;

        // Group cues
        std::string groupMode{"timeline"};   // "timeline" | "playlist" | "startfirst"
        bool        groupRandom{false};      // Playlist: randomise child order
        // For child cues: position in the parent Timeline (seconds).
        double      timelineOffset{0.0};
        // Nested children — populated for group cues only.
        // Serialised as a recursive JSON array under the key "children".
        std::vector<CueData> children;

        // MC inheritance link: when non-empty, this cue's MC is sourced from
        // the cue with the given cue number.  Mutually exclusive with musicContext.
        std::string mcSourceNumber;

        // Music Context (serialised as "musicContext" when enabled is true)
        struct MCPoint {
            int    bar{1};
            int    beat{1};
            double bpm{120.0};
            bool   isRamp{false};
            bool   hasTimeSig{true};
            int    timeSigNum{4};
            int    timeSigDen{4};
        };
        struct MCData {
            bool               enabled{false};
            std::vector<MCPoint> points;
            double             startOffsetSeconds{0.0};
            bool               applyBeforeStart{true};
        };
        MCData musicContext;

        // External triggers (hotkey / MIDI / OSC)
        CueTriggers triggers;
    };

    // ---- Cue list ----------------------------------------------------------
    struct CueListData {
        std::string           id{"main"};
        std::string           name{"Main"};
        std::vector<CueData>  cues;
    };

    // ---- Engine hints (optional; consumers may ignore) ---------------------
    struct EngineHints {
        int         sampleRate{48000};
        int         channels{2};
        std::string deviceName;
    };

    // ---- Show metadata -----------------------------------------------------
    struct ShowMeta {
        std::string title{"Untitled Show"};
    };

    // ---- MIDI patch setup --------------------------------------------------
    // Defines named MIDI output patches used by MIDI cues.
    struct MidiSetup {
        struct Patch {
            std::string name;
            std::string destination;  // MIDI output port name
        };
        std::vector<Patch> patches;
    };

    // ---- Network patch setup -----------------------------------------------
    // Defines named network output patches used by Network cues.
    struct NetworkSetup {
        struct Patch {
            std::string name;
            std::string type;         // "osc" | "plaintext"
            std::string protocol;     // "udp" | "tcp"
            std::string iface;        // network interface ("any", "eth0", …)
            std::string destination;  // "host:port"
            std::string password;     // optional (for OSC password auth)
        };
        std::vector<Patch> patches;
    };

    // ---- Audio channel setup -----------------------------------------------
    // Defines named logical channels that sit between cue outputs and the
    // device's physical outputs.  Cue routing indices reference channel indices,
    // not physical output indices.
    //
    // Multi-device mode: when devices is non-empty each Channel carries a
    // deviceIndex + deviceChannel that pins it to a specific physical output on
    // a specific device.  When devices is empty the legacy single-device path
    // is used (AudioSetup::Channel::deviceIndex is ignored).
    struct AudioSetup {
        // ---- Per-device descriptor (multi-device mode only) ----------------
        struct Device {
            std::string name;
            int  channelCount{2};   // number of physical outputs on this device
            int  bufferSize{512};   // preferred buffer size (frames)
            bool masterClock{false};// this device is the clock source
        };
        // Empty  = legacy single-device mode.
        std::vector<Device> devices;
        // Override sample rate for all devices.  0 = inherit EngineHints.sampleRate.
        int sampleRate{0};

        struct Channel {
            std::string name;
            // Multi-device mode: which device and physical output this channel maps to.
            int  deviceIndex{0};    // index into devices[]; 0 in legacy mode
            int  deviceChannel{-1}; // physical out on that device; -1 = use channel list index
            bool linkedStereo{false};  // UI hint only — no effect on routing
            float masterGainDb{0.0f};  // channel master fader (0 = unity)
            bool  mute{false};
        };
        std::vector<Channel> channels;
        // Sparse crosspoint: channel[ch] → physOut[out], dB.
        // Absent entry for (ch, out): diagonal (ch==out) = 0 dB, else off.
        // Set db <= -144 to explicitly disable the diagonal.
        struct XpEntry { int ch{0}; int out{0}; float db{0.0f}; };
        std::vector<XpEntry> xpEntries;

        // Ensure at most one device has masterClock=true.
        // If none, assigns masterClock to devices[0] (no-op when devices is empty).
        void normalizeMaster();

        // Returns the index of the master-clock device, or 0 if none/legacy.
        int masterDeviceIndex() const;
    };

    // ---- Scriptlet library -------------------------------------------------
    // Named Python modules available as `import mcp.library.<name>` in scriptlets.
    struct ScriptletLibrary {
        struct Entry {
            std::string name;   // valid Python identifier; must be unique within the library
            std::string code;
        };
        std::vector<Entry> entries;
    };

    // ---- Top-level fields --------------------------------------------------
    std::string              version{kCurrentVersion};
    ShowMeta                 show;
    EngineHints              engine;
    AudioSetup               audioSetup;
    NetworkSetup             networkSetup;
    MidiSetup                midiSetup;
    SystemControlBindings    systemControls;
    OscServerSettings        oscServer;
    ScriptletLibrary         scriptletLibrary;
    std::vector<CueListData> cueLists;

    // ---- I/O ---------------------------------------------------------------

    // Parse JSON from path.  On failure, error contains a human-readable
    // message and the object is left in an empty-but-valid state.
    bool load(const std::filesystem::path& path, std::string& error);

    // Write pretty-printed JSON to path (creates or overwrites).
    bool save(const std::filesystem::path& path, std::string& error) const;

    // Return a ShowFile with one empty cue list.
    static ShowFile empty(const std::string& title = "Untitled Show");
};

} // namespace mcp
