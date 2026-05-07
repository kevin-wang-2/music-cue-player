#include "ShowHelpers.h"
#include "AppModel.h"

#include "engine/FadeData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

namespace ShowHelpers {

std::string nextCueNumber(const mcp::ShowFile& sf) {
    // Collect all existing cue numbers (recursively through group children).
    std::vector<std::string> taken;
    std::function<void(const std::vector<mcp::ShowFile::CueData>&)> collect;
    collect = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
        for (const auto& cd : cues) {
            if (!cd.cueNumber.empty()) taken.push_back(cd.cueNumber);
            if (!cd.children.empty()) collect(cd.children);
        }
    };
    if (!sf.cueLists.empty()) collect(sf.cueLists[0].cues);

    int n = 1;
    while (true) {
        std::string candidate = std::to_string(n);
        bool found = false;
        for (const auto& s : taken) if (s == candidate) { found = true; break; }
        if (!found) return candidate;
        ++n;
    }
}

// Returns the flat engine index corresponding to a cueNumber, or -1 if not found.
// DFS pre-order: parent counted before its children — same ordering as rebuildCueList.
int findCueByNumber(const mcp::ShowFile& sf, const std::string& num) {
    if (num.empty() || sf.cueLists.empty()) return -1;
    int flatIdx = 0;
    std::function<int(const std::vector<mcp::ShowFile::CueData>&)> search;
    search = [&](const std::vector<mcp::ShowFile::CueData>& cues) -> int {
        for (const auto& cd : cues) {
            const int myFlatIdx = flatIdx++;
            if (cd.cueNumber == num) return myFlatIdx;
            if (!cd.children.empty()) {
                const int found = search(cd.children);
                if (found >= 0) return found;
            }
        }
        return -1;
    };
    return search(sf.cueLists[0].cues);
}

std::string fmtTime(double s) {
    if (s < 0.0) s = 0.0;
    const int totalMs  = static_cast<int>(std::round(s * 1000.0));
    const int ms       = totalMs % 1000;
    const int totalSec = totalMs / 1000;
    const int sec      = totalSec % 60;
    const int min      = totalSec / 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d.%03d", min, sec, ms);
    return buf;
}

std::string fmtDuration(double s) {
    if (s <= 0.0) return "--:--.---";
    return fmtTime(s);
}

bool rebuildCueList(AppModel& m, std::string& /*err*/) {
    m.cues.clear();
    if (m.sf.cueLists.empty()) return true;
    const auto& topCues = m.sf.cueLists[0].cues;
    const auto  base    = std::filesystem::path(m.baseDir);

    // Build cueNumber → flat engine index map (DFS pre-order = insertion order).
    std::vector<std::pair<std::string, int>> numToIdx;
    {
        int counter = 0;
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> collectNums;
        collectNums = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& cd : cues) {
                if (!cd.cueNumber.empty())
                    numToIdx.push_back({cd.cueNumber, counter});
                ++counter;
                if (!cd.children.empty()) collectNums(cd.children);
            }
        };
        collectNums(topCues);
    }

    // Resolve target index: prefer cue-number lookup (stable across rebuilds) over
    // the stored flat index (which goes stale when groups are inserted or removed).
    auto resolveTarget = [&](const mcp::ShowFile::CueData& cd) -> int {
        if (!cd.targetCueNumber.empty()) {
            for (const auto& p : numToIdx)
                if (p.first == cd.targetCueNumber) return p.second;
        }
        return cd.target;
    };

    // Convert CueData::MCData to a heap-allocated MusicContext (or nullptr).
    auto buildMC = [](const mcp::ShowFile::CueData::MCData& mc)
        -> std::unique_ptr<mcp::MusicContext>
    {
        if (!mc.enabled || mc.points.empty()) return nullptr;
        auto ctx = std::make_unique<mcp::MusicContext>();
        ctx->startOffsetSeconds = mc.startOffsetSeconds;
        ctx->applyBeforeStart   = mc.applyBeforeStart;
        for (const auto& p : mc.points) {
            mcp::MusicContext::Point pt;
            pt.bar        = p.bar;
            pt.beat       = p.beat;
            pt.bpm        = p.bpm;
            pt.isRamp     = p.isRamp;
            pt.hasTimeSig = p.hasTimeSig;
            pt.timeSigNum = p.timeSigNum;
            pt.timeSigDen = p.timeSigDen;
            ctx->points.push_back(pt);
        }
        return ctx;
    };

    // Recursively add cues into the engine flat list.
    // parentFlatIdx: flat index of the enclosing Group cue, or -1 for top-level.
    std::function<void(const std::vector<mcp::ShowFile::CueData>&, int)> process;
    process = [&](const std::vector<mcp::ShowFile::CueData>& cues, int parentFlatIdx) {
        for (const auto& cd : cues) {
            const int myIdx = m.cues.cueCount();

            if (cd.type == "group") {
                // Count ALL descendants (recursive) to set childCount correctly.
                std::function<int(const std::vector<mcp::ShowFile::CueData>&)> countAll;
                countAll = [&](const std::vector<mcp::ShowFile::CueData>& children) -> int {
                    int total = 0;
                    for (const auto& c : children) {
                        ++total;
                        if (!c.children.empty()) total += countAll(c.children);
                    }
                    return total;
                };

                const auto mode =
                    (cd.groupMode == "playlist")   ? mcp::GroupData::Mode::Playlist  :
                    (cd.groupMode == "startfirst") ? mcp::GroupData::Mode::StartFirst :
                    (cd.groupMode == "sync")       ? mcp::GroupData::Mode::Sync       :
                                                     mcp::GroupData::Mode::Timeline;

                m.cues.addGroupCue(mode, cd.groupRandom, cd.name, cd.preWait);
                m.cues.setCueCueNumber   (myIdx, cd.cueNumber);
                m.cues.setCueAutoContinue(myIdx, cd.autoContinue);
                m.cues.setCueAutoFollow  (myIdx, cd.autoFollow);
                m.cues.setCueChildCount  (myIdx, countAll(cd.children));
                if (parentFlatIdx >= 0) m.cues.setCueParentIndex(myIdx, parentFlatIdx);
                m.cues.setCueTimelineOffset(myIdx, cd.timelineOffset);

                // SyncGroup: load its own markers and slice loops
                if (cd.groupMode == "sync") {
                    std::vector<mcp::Cue::TimeMarker> marks;
                    for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
                    m.cues.setCueMarkers   (myIdx, marks);
                    m.cues.setCueSliceLoops(myIdx, cd.sliceLoops);
                }

                m.cues.setCueMusicContext(myIdx, buildMC(cd.musicContext));

                // Recursively add children immediately after the group header.
                process(cd.children, myIdx);
            } else {
                // Non-group cue
                const int target = resolveTarget(cd);

                if (cd.type == "audio") {
                    auto p = std::filesystem::path(cd.path);
                    if (p.is_relative()) p = base / p;
                    if (!m.cues.addCue(p.string(), cd.name, cd.preWait))
                        m.cues.addBrokenAudioCue(p.string(), cd.name, cd.preWait);
                } else if (cd.type == "start") {
                    m.cues.addStartCue(target, cd.name, cd.preWait);
                } else if (cd.type == "stop") {
                    m.cues.addStopCue(target, cd.name, cd.preWait);
                } else if (cd.type == "fade") {
                    const auto curve = (cd.fadeCurve == "equalpower")
                        ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
                    m.cues.addFadeCue(target, cd.targetCueNumber, curve,
                                      cd.fadeStopWhenDone, cd.name, cd.preWait);
                } else if (cd.type == "arm") {
                    m.cues.addArmCue(target, cd.name, cd.preWait);
                } else if (cd.type == "devamp") {
                    m.cues.addDevampCue(target, cd.name, cd.preWait, cd.devampMode);
                }

                m.cues.setCueCueNumber   (myIdx, cd.cueNumber);
                m.cues.setCueStartTime   (myIdx, cd.startTime);
                m.cues.setCueDuration    (myIdx, cd.duration);
                m.cues.setCueLevel       (myIdx, cd.level);
                m.cues.setCueTrim        (myIdx, cd.trim);
                m.cues.setCueAutoContinue(myIdx, cd.autoContinue);
                m.cues.setCueAutoFollow  (myIdx, cd.autoFollow);
                m.cues.setCueGoQuantize  (myIdx, cd.goQuantize);
                if (parentFlatIdx >= 0) m.cues.setCueParentIndex(myIdx, parentFlatIdx);
                m.cues.setCueTimelineOffset(myIdx, cd.timelineOffset);

                if (cd.type == "arm")
                    m.cues.setCueArmStartTime(myIdx, cd.armStartTime);
                if (cd.type == "devamp") {
                    m.cues.setCueDevampMode   (myIdx, cd.devampMode);
                    m.cues.setCueDevampPreVamp(myIdx, cd.devampPreVamp);
                }
                if (cd.type == "audio") {
                    std::vector<mcp::Cue::TimeMarker> marks;
                    for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
                    m.cues.setCueMarkers   (myIdx, marks);
                    m.cues.setCueSliceLoops(myIdx, cd.sliceLoops);
                    for (int o = 0; o < (int)cd.outLevelDb.size(); ++o)
                        m.cues.setCueOutLevel(myIdx, o, cd.outLevelDb[o]);
                    for (const auto& xe : cd.xpEntries)
                        m.cues.setCueXpoint(myIdx, xe.s, xe.o, xe.db);
                }
                if (cd.type == "fade") {
                    m.cues.setCueFadeMasterTarget  (myIdx, cd.fadeMasterEnabled, cd.fadeMasterTarget);
                    m.cues.setCueFadeOutTargetCount(myIdx, (int)cd.fadeOutLevels.size());
                    for (const auto& fl : cd.fadeOutLevels)
                        m.cues.setCueFadeOutTarget(myIdx, fl.ch, fl.enabled, fl.target);
                    for (const auto& fx : cd.fadeXpEntries)
                        m.cues.setCueFadeXpTarget(myIdx, fx.s, fx.o, fx.enabled, fx.target);
                }
                m.cues.setCueMusicContext(myIdx, buildMC(cd.musicContext));
            }
        }
    };

    process(topCues, -1);
    return true;
}

// Reconstruct the nested ShowFile structure from the flat engine CueList.
// Group headers and their descendants are re-nested using childCount for range tracking.
void syncSfFromCues(AppModel& m) {
    if (m.sf.cueLists.empty()) return;

    const int total = m.cues.cueCount();

    auto fillCommon = [](mcp::ShowFile::CueData& cd, const mcp::Cue& c) {
        cd.cueNumber      = c.cueNumber;
        cd.name           = c.name;
        cd.preWait        = c.preWaitSeconds;
        cd.goQuantize     = c.goQuantize;
        cd.autoContinue   = c.autoContinue;
        cd.autoFollow     = c.autoFollow;
        cd.timelineOffset = c.timelineOffset;
        if (c.musicContext) {
            cd.musicContext.enabled            = true;
            cd.musicContext.startOffsetSeconds = c.musicContext->startOffsetSeconds;
            cd.musicContext.applyBeforeStart   = c.musicContext->applyBeforeStart;
            for (const auto& p : c.musicContext->points) {
                mcp::ShowFile::CueData::MCPoint pt;
                pt.bar        = p.bar;
                pt.beat       = p.beat;
                pt.bpm        = p.bpm;
                pt.isRamp     = p.isRamp;
                pt.hasTimeSig = p.hasTimeSig;
                pt.timeSigNum = p.timeSigNum;
                pt.timeSigDen = p.timeSigDen;
                cd.musicContext.points.push_back(pt);
            }
        }
    };

    // Build a helper to look up a cue's number by flat index.
    auto cueNumAt = [&](int idx) -> std::string {
        if (idx < 0 || idx >= total) return "";
        const auto* c = m.cues.cueAt(idx);
        return c ? c->cueNumber : "";
    };

    // Recursively reconstruct nesting.
    // Processes flat indices [startIdx, startIdx+count) into dest.
    std::function<void(int, int, std::vector<mcp::ShowFile::CueData>&)> extract;
    extract = [&](int startIdx, int count, std::vector<mcp::ShowFile::CueData>& dest) {
        for (int i = startIdx; i < startIdx + count && i < total; ) {
            const auto* c = m.cues.cueAt(i);
            if (!c) { ++i; continue; }

            mcp::ShowFile::CueData cd;
            fillCommon(cd, *c);

            switch (c->type) {
                case mcp::CueType::Audio:
                    cd.type      = "audio";
                    cd.path      = c->path;
                    cd.startTime = c->startTime;
                    cd.duration  = c->duration;
                    cd.level     = c->level;
                    cd.trim      = c->trim;
                    for (const auto& mk : c->markers)
                        cd.markers.push_back({mk.time, mk.name});
                    cd.sliceLoops = c->sliceLoops;
                    cd.outLevelDb = c->routing.outLevelDb;
                    for (int s = 0; s < (int)c->routing.xpoint.size(); ++s)
                        for (int o = 0; o < (int)c->routing.xpoint[s].size(); ++o)
                            if (c->routing.xpoint[s][o].has_value())
                                cd.xpEntries.push_back({s, o, *c->routing.xpoint[s][o]});
                    ++i;
                    break;

                case mcp::CueType::Start:
                    cd.type            = "start";
                    cd.target          = c->targetIndex;
                    cd.targetCueNumber = cueNumAt(c->targetIndex);
                    ++i;
                    break;

                case mcp::CueType::Stop:
                    cd.type            = "stop";
                    cd.target          = c->targetIndex;
                    cd.targetCueNumber = cueNumAt(c->targetIndex);
                    ++i;
                    break;

                case mcp::CueType::Arm:
                    cd.type            = "arm";
                    cd.target          = c->targetIndex;
                    cd.armStartTime    = c->armStartTime;
                    cd.targetCueNumber = cueNumAt(c->targetIndex);
                    ++i;
                    break;

                case mcp::CueType::Devamp:
                    cd.type            = "devamp";
                    cd.target          = c->targetIndex;
                    cd.devampMode      = c->devampMode;
                    cd.devampPreVamp   = c->devampPreVamp;
                    cd.targetCueNumber = cueNumAt(c->targetIndex);
                    ++i;
                    break;

                case mcp::CueType::Fade:
                    cd.type     = "fade";
                    cd.duration = c->duration;
                    if (c->fadeData) {
                        const auto& fd      = *c->fadeData;
                        cd.targetCueNumber  = fd.targetCueNumber;
                        cd.target           = fd.resolvedTargetIdx;
                        cd.fadeStopWhenDone = fd.stopWhenDone;
                        cd.fadeCurve        = (fd.curve == mcp::FadeData::Curve::EqualPower)
                                              ? "equalpower" : "linear";
                        cd.fadeMasterEnabled = fd.masterLevel.enabled;
                        cd.fadeMasterTarget  = fd.masterLevel.targetDb;
                        for (int o = 0; o < (int)fd.outLevels.size(); ++o)
                            cd.fadeOutLevels.push_back({o, fd.outLevels[o].enabled,
                                                        fd.outLevels[o].targetDb});
                        for (int s = 0; s < (int)fd.xpTargets.size(); ++s)
                            for (int o = 0; o < (int)fd.xpTargets[s].size(); ++o)
                                cd.fadeXpEntries.push_back(
                                    {s, o, fd.xpTargets[s][o].enabled,
                                     fd.xpTargets[s][o].targetDb});
                    }
                    ++i;
                    break;

                case mcp::CueType::Group:
                    cd.type = "group";
                    if (c->groupData) {
                        cd.groupMode =
                            (c->groupData->mode == mcp::GroupData::Mode::Playlist)   ? "playlist"  :
                            (c->groupData->mode == mcp::GroupData::Mode::StartFirst) ? "startfirst" :
                            (c->groupData->mode == mcp::GroupData::Mode::Sync)       ? "sync"       :
                                                                                        "timeline";
                        cd.groupRandom = c->groupData->random;
                        // SyncGroup: serialize its own markers and slice loops
                        if (c->groupData->mode == mcp::GroupData::Mode::Sync) {
                            for (const auto& mk : c->markers)
                                cd.markers.push_back({mk.time, mk.name});
                            cd.sliceLoops = c->sliceLoops;
                        }
                    }
                    if (c->childCount > 0)
                        extract(i + 1, c->childCount, cd.children);
                    i += c->childCount + 1;  // skip group header + all descendants
                    break;
            }

            dest.push_back(std::move(cd));
        }
    };

    m.sf.cueLists[0].cues.clear();
    extract(0, total, m.sf.cueLists[0].cues);
    m.dirty = true;
}

// ---------------------------------------------------------------------------
// SF navigation helpers (DFS pre-order, matching rebuildCueList insertion order)

mcp::ShowFile::CueData* sfCueAt(mcp::ShowFile& sf, int flatIdx) {
    if (sf.cueLists.empty() || flatIdx < 0) return nullptr;
    int counter = 0;
    std::function<mcp::ShowFile::CueData*(std::vector<mcp::ShowFile::CueData>&)> find;
    find = [&](std::vector<mcp::ShowFile::CueData>& cues) -> mcp::ShowFile::CueData* {
        for (auto& cd : cues) {
            if (counter++ == flatIdx) return &cd;
            if (!cd.children.empty()) {
                if (auto* p = find(cd.children)) return p;
            }
        }
        return nullptr;
    };
    return find(sf.cueLists[0].cues);
}

const mcp::ShowFile::CueData* sfCueAt(const mcp::ShowFile& sf, int flatIdx) {
    return sfCueAt(const_cast<mcp::ShowFile&>(sf), flatIdx);
}

mcp::ShowFile::CueData sfRemoveAt(mcp::ShowFile& sf, int flatIdx) {
    if (sf.cueLists.empty() || flatIdx < 0) return {};
    int counter = 0;
    mcp::ShowFile::CueData removed;
    std::function<bool(std::vector<mcp::ShowFile::CueData>&)> rem;
    rem = [&](std::vector<mcp::ShowFile::CueData>& cues) -> bool {
        for (int i = 0; i < (int)cues.size(); ++i) {
            if (counter++ == flatIdx) {
                removed = std::move(cues[i]);
                cues.erase(cues.begin() + i);
                return true;
            }
            if (!cues[i].children.empty())
                if (rem(cues[i].children)) return true;
        }
        return false;
    };
    rem(sf.cueLists[0].cues);
    return removed;
}

void sfInsertBefore(mcp::ShowFile& sf, int beforeFlatIdx, mcp::ShowFile::CueData cd) {
    if (sf.cueLists.empty()) return;
    int counter = 0;
    bool inserted = false;
    std::function<bool(std::vector<mcp::ShowFile::CueData>&)> ins;
    ins = [&](std::vector<mcp::ShowFile::CueData>& cues) -> bool {
        for (int i = 0; i < (int)cues.size(); ++i) {
            if (counter++ == beforeFlatIdx) {
                cues.insert(cues.begin() + i, std::move(cd));
                return true;
            }
            if (!cues[i].children.empty())
                if (ins(cues[i].children)) return true;
        }
        return false;
    };
    if (!ins(sf.cueLists[0].cues))
        sf.cueLists[0].cues.push_back(std::move(cd));   // past-end → append top-level
}

void saveShow(AppModel& m) {
    if (m.showPath.empty()) return;
    std::string err;
    m.sf.save(m.showPath, err);
    if (err.empty()) { m.dirty = false; emit m.dirtyChanged(false); }
}

void setCueNumberChecked(AppModel& m, int index, const std::string& num) {
    if (!num.empty()) {
        for (int i = 0; i < m.cues.cueCount(); ++i) {
            if (i == index) continue;
            const auto* other = m.cues.cueAt(i);
            if (other && other->cueNumber == num) {
                m.cues.setCueCueNumber(index, "");
                syncSfFromCues(m);
                return;
            }
        }
    }
    m.cues.setCueCueNumber(index, num);
    syncSfFromCues(m);
}

} // namespace ShowHelpers
