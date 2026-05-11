#pragma once

#include "engine/ShowFile.h"
#include <functional>
#include <string>

// Forward declarations to avoid including AppModel.h here
class AppModel;

struct CollectOptions {
    std::string destDir;
    bool        convertToWav = false;
    int         sampleRate   = 48000;
    int         bitDepth     = 24;   // 16, 24, or 32 (float)
};

struct MissingEntry {
    int         listIdx{0};
    std::string cueType;       // "audio", "video", etc. — for display
    std::string cueNumber;
    std::string cueName;
    std::string originalPath;  // as stored in SF (relative or absolute)
    std::string resolvedPath;  // absolute path that was checked and not found
    std::string newPath;       // absolute replacement path; empty = unresolved
};

namespace ShowHelpers {

std::string nextCueNumber(const mcp::ShowFile& sf);
int         findCueByNumber(const mcp::ShowFile& sf, const std::string& num);
std::string fmtTime(double s);
std::string fmtDuration(double s);

// Ensure every targetListId / anchorMarkerListId in every CueListData is an
// absolute numericId (never -1).  Call after load and after any SF mutation
// that may have left -1 placeholders.
void normalizeListRefs(mcp::ShowFile& sf);

// Rebuild one engine CueList from sf.cueLists[listIdx].
bool rebuildCueList(AppModel& model, int listIdx, std::string& err);
// Rebuild all engine CueLists (calls syncListCount + rebuildCueList for each).
bool rebuildAllCueLists(AppModel& model, std::string& err);
// Update only the channel map on all active lists — does NOT clear or stop playback.
// Call this for mixing-only changes (fader, mute, crosspoint) that must not interrupt audio.
void applyChannelMap(AppModel& model);
// Sync SF for one list (listIdx) from its engine.  Pass -1 to use the active list.
void syncSfFromCues(AppModel& model, int listIdx = -1);
// Sync SF for ALL lists from their engines (call after rebuildAllCueLists).
void syncAllSfFromCues(AppModel& model);
void saveShow(AppModel& model);
void setCueNumberChecked(AppModel& model, int index, const std::string& num);

// Count audio cues across all lists (for progress reporting).
int  countAudioCues(const AppModel& model);

// Find all audio cues whose files do not exist on disk.
std::vector<MissingEntry> findMissingMedia(const AppModel& model);

// Write resolved newPath values back into the SF, then rebuild cue lists.
// Entries with empty newPath are skipped. Sets model.dirty = true.
void applyMediaFixes(AppModel& model, const std::vector<MissingEntry>& fixes);

// Copy (or convert) all referenced audio files into opts.destDir/audio/,
// rewrite paths to relative, and save a copy of the show file in opts.destDir.
// progress(filename) is called once per file just before it is processed.
// Returns true if all files succeeded; partial failures are appended to err.
bool collectAllFiles(AppModel& model,
                     const CollectOptions& opts,
                     std::function<void(const std::string& filename)> progress,
                     std::string& err);

// Navigate the nested ShowFile cue tree by flat engine index.
// DFS pre-order matches the engine flat list built by rebuildCueList.
// listIdx selects which CueListData to operate on.
mcp::ShowFile::CueData*       sfCueAt(mcp::ShowFile& sf, int listIdx, int flatIdx);
const mcp::ShowFile::CueData* sfCueAt(const mcp::ShowFile& sf, int listIdx, int flatIdx);

// Remove and return the CueData at flatIdx (includes group subtree if group).
// No-op (returns default) if index is out of range.
mcp::ShowFile::CueData sfRemoveAt(mcp::ShowFile& sf, int listIdx, int flatIdx);

// After removing a cue at removedFlatIdx, fix target fields across all remaining cues:
// clears targets that pointed to it, decrements targets that were after it.
void sfFixTargetsAfterRemoval(mcp::ShowFile& sf, int listIdx, int removedFlatIdx);

// After moving a block of blockSize cues from srcRow to dstRow (original pre-move position)
// in list srcListIdx, fix stale target flat-indices in EVERY list's SF that references
// that list.  Call AFTER sfRemoveAt + sfInsertBefore, before rebuildAllCueLists.
void sfFixTargetsForReorder(mcp::ShowFile& sf, int srcListIdx,
                             int srcRow, int blockSize, int dstRow);

// Insert cd immediately before the cue at beforeFlatIdx (same parent container).
// If beforeFlatIdx >= total count, appends to the top-level cues list.
void sfInsertBefore(mcp::ShowFile& sf, int listIdx, int beforeFlatIdx, mcp::ShowFile::CueData cd);

// Append cd as the last child of the group cue at groupFlatIdx.
void sfAppendToGroup(mcp::ShowFile& sf, int listIdx, int groupFlatIdx, mcp::ShowFile::CueData cd);

// ---------------------------------------------------------------------------
// Direct engine mutations (no full rebuild needed)

// Insert a single cue at flat position flatIdx in engine list listIdx.
// Negative flatIdx means append (past-end).
// If cd is a group, falls back to rebuildCueList for that list only.
// Updates both engine and sf; does NOT emit any signals.
// Returns the final flat index of the inserted cue, or -1 on error.
int insertEngineCue(AppModel& m, int listIdx, int flatIdx,
                    mcp::ShowFile::CueData cd, std::string& err);

// Remove the cue (and its subtree if Group) at flatIdx from both engine and sf.
// Stops any active voice(s). Fixes all index refs.
void removeEngineCue(AppModel& m, int listIdx, int flatIdx);

// Reload the audio file for cue[flatIdx] in place.
// The new path must already be written into sf via sfCueAt().
// Returns false if the reload fails (cue remains broken but still present).
bool reloadEngineCueAudio(AppModel& m, int listIdx, int flatIdx);

// ---------------------------------------------------------------------------
// Undo / redo helpers

// Move multiple flat-index blocks within one engine list without rebuilding.
// topLevel[i] is the original flat index of block i's header; blockSizes[i] is
// its total span (1 + childCount).  dstRow is the destination in the original
// flat index space; adjustedDst = dstRow - sum(blockSizes of blocks before dstRow).
// Falls back to rebuildAllCueLists when the move is a mixed up/down selection.
void moveEngineCues(AppModel& m, int listIdx,
                    const std::vector<int>& topLevel,
                    const std::vector<int>& blockSizes,
                    int dstRow, int adjustedDst);

// Returns true if the two SF snapshots differ in structure (list count, flat
// cue count per list, or cue type at any position).  A false result means
// only parameters changed — reapplyParamsFromSF() is sufficient to sync the
// engine without a full rebuild.
bool isStructuralChange(
    const std::vector<mcp::ShowFile::CueListData>& a,
    const std::vector<mcp::ShowFile::CueListData>& b);

// Re-apply every parameter from m.sf to the engine without rebuilding.
// Only correct when isStructuralChange() returns false (structure is identical).
void reapplyParamsFromSF(AppModel& m);

} // namespace ShowHelpers
