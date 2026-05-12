#pragma once
#include <set>
#include <string>

// Tracks AU plugin vendors known to be incompatible with AudioToolbox parameter
// watching (AUEventListenerAddEventType). Persisted as app-level metadata —
// not part of the show file.
//
// Default seeds: "Waves Audio", "Waves" (crash-on-startWatchingParameters).
// Additional entries are added automatically when testIsolated() returns Crashed.
class DangerousPluginList {
public:
    DangerousPluginList();  // loads from disk, seeds defaults

    bool contains(const std::string& vendorName) const;
    void add(const std::string& vendorName);   // adds + saves immediately
    void save() const;

private:
    void load();

    std::set<std::string> m_vendors;
    std::string           m_path;  // absolute path to JSON file on disk
};
