#pragma once

#include <filesystem>
#include <string>
#include <vector>

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
        std::string type;         // "audio" | "start" | "stop" | … (extensible)
        std::string cueNumber;    // user-visible Q number ("1", "2", "1a", …) — independent of array index
        std::string name;
        // Audio cues
        std::string path;         // relative to the show file's directory
        // Start / Stop cues
        int         target{-1};         // resolved array index (internal)
        std::string targetCueNumber;    // user-visible reference stored in file
        // All types
        double      preWait{0.0};
        bool        autoContinue{false};
        bool        autoFollow{false};
        // Audio cues: playback region (0 = default = start/end of file)
        double      startTime{0.0};
        double      duration{0.0};
    };

    // ---- Cue list ----------------------------------------------------------
    struct CueListData {
        std::string           id{"main"};
        std::string           name{"Main"};
        std::vector<CueData>  cues;
    };

    // ---- Engine hints (optional; consumers may ignore) ---------------------
    struct EngineHints {
        int sampleRate{48000};
        int channels{2};
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
