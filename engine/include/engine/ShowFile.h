#pragma once

#include "TriggerData.h"
#include <filesystem>
#include <map>
#include <optional>
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
        // -1 = same list; else numericId of the target CueList.
        int         targetListId{-1};
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
            // -1 = same list as this cue; else numericId of the target CueList.
            int anchorMarkerListId{-1};
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

        // Snapshot cues
        int         snapshotId{-1};     // stable snapshot ID to recall; -1 = none

        // Automation cues
        std::string automationPath;          // parameter path, e.g. "/mixer/0/fader"
        double      automationDuration{5.0}; // total cue duration (seconds)
        struct AutomationPoint { double time{0.0}; double value{0.0}; bool isHandle{false}; };
        std::vector<AutomationPoint> automationCurve;  // breakpoints + handles: BP,H,BP,H,...,BP

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
        // Stable integer identifier, 1-based, assigned on first save/load.
        // Used by cross-list references (CueData::targetListId, TimeMarker::anchorMarkerListId).
        int                   numericId{0};
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
            // DSP — applied post-crosspoint, per logical channel.
            // For stereo pairs phase is independent; delay is shared (use master's value).
            bool   phaseInvert{false};
            bool   delayInSamples{false};  // false = ms mode, true = samples mode
            double delayMs{0.0};
            int    delaySamples{0};
            bool   pdcIsolated{false};   // PDC: own plugin latency not propagated downstream
            // Plugin slots (up to 16; order = slot index; vacant slots have empty pluginId).
            struct PluginSlot {
                std::string pluginId;                     // empty = vacant
                std::map<std::string, float> parameters;  // internal param values

                // External plugin fields — populated when pluginId starts with "au:" / "vst3:"
                // (Kept flat to avoid a dependency on ExternalPluginReference in ShowFile.h)
                std::string extBackend;            // "au", "vst3"
                std::string extPath;               // VST3: absolute path to .vst3 bundle; AU: empty
                std::string extName;               // display name
                std::string extVendor;
                std::string extVersion;
                int         extNumChannels{2};
                std::vector<uint8_t>         extStateBlob;      // binary state (AU plist, …)
                std::map<std::string, float> extParamSnapshot;   // normalized [0,1] fallback

                // ── Runtime control (persisted) ───────────────────────────────
                bool  bypassed{false};        // slot-level bypass; persisted
                bool  disabled{false};        // safe-load: skip instantiation; state preserved
                int   loadFailCount{0};       // consecutive load failures; auto-disables at 3
                float manualTailSec{-1.0f};   // tail override: -1 = use plugin value

                bool isExternal() const { return !extBackend.empty(); }
            };
            std::vector<PluginSlot> plugins;  // order = slot index; sparse is fine

            // Send slots: channel-to-channel sends (post-fader, pre-destination-DSP).
            // At most kMaxSendSlots per channel; vacant when dstChannel == -1.
            static constexpr int kMaxSendSlots = 16;
            struct SendSlot {
                int   dstChannel{-1};    // -1 = vacant; logical channel index (or master of linked pair)
                float levelDb{0.0f};     // send level in dB (0 = unity)
                float panL{0.0f};        // pan for mono src or left of stereo src (-1..+1, 0 = center)
                float panR{0.0f};        // pan for right of stereo src (stereo→stereo only)
                bool  muted{false};
                // send_type enum reserved for pre-fader variant (not implemented)
                // PrePost preFader{PostFader};

                bool isActive() const { return dstChannel >= 0; }
            };
            std::vector<SendSlot> sends;  // order = slot index; vacant slots have dstChannel == -1
        };
        std::vector<Channel> channels;
        // Sparse crosspoint: channel[ch] → physOut[out], dB.
        // Absent entry for (ch, out): diagonal (ch==out) = 0 dB, else off.
        // Set db <= -144 to explicitly disable the diagonal.
        struct XpEntry { int ch{0}; int out{0}; float db{0.0f}; };
        std::vector<XpEntry> xpEntries;

        // DSP per physical output (applied after channel DSP; combined before engine).
        struct PhysOutDsp {
            bool   phaseInvert{false};
            bool   delayInSamples{false};
            double delayMs{0.0};
            int    delaySamples{0};
        };
        std::vector<PhysOutDsp> physOutDsp;  // index = global physical output

        // Ensure at most one device has masterClock=true.
        // If none, assigns masterClock to devices[0] (no-op when devices is empty).
        void normalizeMaster();

        // Returns the index of the master-clock device, or 0 if none/legacy.
        int masterDeviceIndex() const;
    };

    // ---- UI hints ----------------------------------------------------------
    // Opaque key–value store for frontend UI state (panel visibility, sizes, etc.).
    // The engine never interprets the values — it just round-trips them through
    // the "ui_hints" JSON key so the frontend can persist layout across sessions.
    struct UiHints {
        std::map<std::string, std::string> data;

        std::string get(const std::string& key, const std::string& def = "") const {
            auto it = data.find(key);
            return it != data.end() ? it->second : def;
        }
        void set(const std::string& key, std::string value) {
            data[key] = std::move(value);
        }
        bool empty() const { return data.empty(); }
        void clear()       { data.clear(); }
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

    // ---- Snapshot list ---------------------------------------------------------
    // Stores named mixing snapshots and path-based scope tracking.
    // Each Snapshot has a stable numeric `id` so that future SnapshotCue types
    // can reference it by ID rather than by position (position changes on delete).
    //
    // Parameter path scheme (prefix-match hierarchy):
    //   /mixer/{ch}                     — entire channel
    //   /mixer/{ch}/delay               — delay (ms/samples)
    //   /mixer/{ch}/polarity            — phase invert
    //   /mixer/{ch}/mute                — mute
    //   /mixer/{ch}/fader               — master gain
    //   /mixer/{ch}/crosspoint/{out}    — one crosspoint send
    //   /mixer/{ch}/send/{slot}/mute    — send slot mute
    //   /mixer/{ch}/send/{slot}/level   — send slot level (dB)
    //   /mixer/{ch}/send/{slot}/panL    — send slot pan L
    //   /mixer/{ch}/send/{slot}/panR    — send slot pan R
    struct SnapshotList {
        struct Snapshot {
            int         id{0};       // stable identifier — referenced by future SnapshotCue
            std::string name;

            // Paths in scope for this snapshot (prefix-match: "/mixer/0" covers all sub-paths).
            std::vector<std::string> scope;

            struct XpSend { int out{0}; float db{0.0f}; };
            struct ChannelState {
                int ch{0};
                // optional<T>: present only when the corresponding path was in scope at store time.
                std::optional<double> delayMs;
                std::optional<bool>   delayInSamples;
                std::optional<int>    delaySamples;
                std::optional<bool>   polarity;
                std::optional<bool>   mute;
                std::optional<float>  faderDb;
                // Only crosspoints whose /crosspoint/{out} path was in scope at store time.
                std::vector<XpSend>   xpSends;
                // Plugin slot states whose /plugin/{slot} path was in scope at store time.
                struct PluginParamState {
                    int                          slot{0};
                    std::optional<bool>          bypassed;
                    std::map<std::string, float> parameters;       // internal plugin params
                    std::vector<uint8_t>         extStateBlob;     // AU/external state blob
                    std::map<std::string, float> extParamSnapshot; // AU param fallback
                };
                std::vector<PluginParamState> pluginStates;

                // Send slot states whose /send/{slot}/... path was in scope at store time.
                struct SendState {
                    int   slot{0};
                    std::optional<bool>  muted;
                    std::optional<float> levelDb;
                    std::optional<float> panL;
                    std::optional<float> panR;
                };
                std::vector<SendState> sendStates;
            };
            std::vector<ChannelState> channels;
        };

        // Paths of parameters changed since the last store (auto-scope for next Store).
        // Persisted so continuity survives session restarts.
        std::vector<std::string> pendingDirtyPaths;

        // Slots are sparse: std::nullopt = empty/deleted slot.
        // Deleting a snapshot sets the slot to nullopt rather than erasing it,
        // so subsequent slot numbers don't shift.
        std::vector<std::optional<Snapshot>> snapshots;
        int                                  currentIndex{0};  // cursor; == snapshots.size() → new slot

        // Returns a snapshot ID not yet used in this list.
        int nextSnapshotId() const {
            int mx = 0;
            for (const auto& s : snapshots) if (s) mx = std::max(mx, s->id);
            return mx + 1;
        }
        // Find a filled snapshot by stable ID.
        Snapshot* findById(int id) {
            for (auto& s : snapshots) if (s && s->id == id) return &s.value();
            return nullptr;
        }
        const Snapshot* findById(int id) const {
            for (const auto& s : snapshots) if (s && s->id == id) return &s.value();
            return nullptr;
        }
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
    UiHints                  uiHints;        // frontend-only; engine round-trips without reading
    SnapshotList             snapshotList;

    // ---- Helpers -----------------------------------------------------------

    // Returns a numericId not yet used by any CueListData.
    int nextListId() const {
        int mx = 0;
        for (const auto& cl : cueLists) mx = std::max(mx, cl.numericId);
        return mx + 1;
    }

    // Find a CueListData by numericId; returns nullptr if not found.
    CueListData* findList(int numericId) {
        for (auto& cl : cueLists) if (cl.numericId == numericId) return &cl;
        return nullptr;
    }
    const CueListData* findList(int numericId) const {
        for (const auto& cl : cueLists) if (cl.numericId == numericId) return &cl;
        return nullptr;
    }

    // Ensure every CueListData has a non-zero numericId. Call after load / on new list.
    void assignListIds();

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
