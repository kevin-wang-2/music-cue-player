#pragma once

#include "engine/ShowFile.h"
#include <string>

// Forward declarations to avoid including AppModel.h here
struct AppModel;

namespace ShowHelpers {

std::string nextCueNumber(const mcp::ShowFile& sf);
int         findCueByNumber(const mcp::ShowFile& sf, const std::string& num);
std::string fmtTime(double s);
std::string fmtDuration(double s);

// Returns true if any cue list entry was modified.
bool rebuildCueList(AppModel& model, std::string& err);
void syncSfFromCues(AppModel& model);
void saveShow(AppModel& model);
void setCueNumberChecked(AppModel& model, int index, const std::string& num);

// Navigate the nested ShowFile cue tree by flat engine index.
// DFS pre-order matches the engine flat list built by rebuildCueList.
mcp::ShowFile::CueData*       sfCueAt(mcp::ShowFile& sf, int flatIdx);
const mcp::ShowFile::CueData* sfCueAt(const mcp::ShowFile& sf, int flatIdx);

// Remove and return the CueData at flatIdx (includes group subtree if group).
// No-op (returns default) if index is out of range.
mcp::ShowFile::CueData sfRemoveAt(mcp::ShowFile& sf, int flatIdx);

// After removing a cue at removedFlatIdx, fix target fields across all remaining cues:
// clears targets that pointed to it, decrements targets that were after it.
void sfFixTargetsAfterRemoval(mcp::ShowFile& sf, int removedFlatIdx);

// Insert cd immediately before the cue at beforeFlatIdx (same parent container).
// If beforeFlatIdx >= total count, appends to the top-level cues list.
void sfInsertBefore(mcp::ShowFile& sf, int beforeFlatIdx, mcp::ShowFile::CueData cd);

// Append cd as the last child of the group cue at groupFlatIdx.
void sfAppendToGroup(mcp::ShowFile& sf, int groupFlatIdx, mcp::ShowFile::CueData cd);

} // namespace ShowHelpers
