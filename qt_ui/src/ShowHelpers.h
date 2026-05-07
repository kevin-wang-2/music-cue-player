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

} // namespace ShowHelpers
