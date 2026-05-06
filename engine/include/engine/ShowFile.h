#pragma once

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
        struct TimeMarker { double time{0.0}; std::string name; };
        std::vector<TimeMarker> markers;
        std::vector<int>        sliceLoops;  // [i] = loops for slice i; 0 = infinite

        // Devamp cues
        int         devampMode{0};        // 0=NextSlice, 1=go()+StopCurrent, 2=go()+KeepCurrent
        bool        devampPreVamp{false}; // skip subsequent looping slices after devamp

        // Fade cues
        std::string fadeCurve{"linear"};
        bool        fadeStopWhenDone{false};
        bool        fadeMasterEnabled{false};
        float       fadeMasterTarget{0.0f};
        struct FadeOutLevel { int ch{0}; bool enabled{false}; float target{0.0f}; };
        std::vector<FadeOutLevel> fadeOutLevels;
        struct FadeXpEntry { int s{0}; int o{0}; bool enabled{false}; float target{0.0f}; };
        std::vector<FadeXpEntry> fadeXpEntries;
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

    // ---- Top-level fields --------------------------------------------------
    std::string              version{kCurrentVersion};
    ShowMeta                 show;
    EngineHints              engine;
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
