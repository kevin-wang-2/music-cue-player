#pragma once

#include "ShowFile.h"

namespace mcp {

// Manages snapshot capture, recall, cursor navigation, and dirty tracking.
// All business logic lives here so future SnapshotCue types can call
// recallById() directly without going through the UI layer.
//
// Call init() once before using any other method (e.g. in AppModel ctor).
class SnapshotManager {
public:
    using SnapshotList = ShowFile::SnapshotList;
    using AudioSetup   = ShowFile::AudioSetup;

    void init(ShowFile& sf);

    // Mark a parameter path as changed since the last store.
    // Builds the auto-scope for the next store() call.
    // Path examples: "/mixer/0/fader", "/mixer/0/crosspoint/2"
    void markDirty(const std::string& path);

    // Capture current AudioSetup state at currentIndex using pendingDirty as scope.
    // If the slot is empty or new, a snapshot is created there.
    // Clears pendingDirty after storing.
    void store();

    // Capture ALL channels and ALL parameters regardless of dirty state.
    // Useful for a "full scene snapshot" without relying on change tracking.
    void storeAll();

    // Apply the snapshot at currentIndex to AudioSetup.
    // Returns false if currentIndex is an empty slot (nothing applied).
    bool recall();

    // Apply a snapshot by its stable ID. Returns false if not found.
    // Safe to call from any non-audio thread (e.g. future SnapshotCue).
    bool recallById(int id);

    // Cursor navigation — update currentIndex without recalling.
    void navigatePrev();
    void navigateNext();
    void setCurrentIndex(int idx);

    int  currentIndex()  const;
    int  snapshotCount() const;
    bool isEmptySlot()   const;  // true when cursor is past the last stored snapshot

private:
    ShowFile* m_sf{nullptr};
};

} // namespace mcp
