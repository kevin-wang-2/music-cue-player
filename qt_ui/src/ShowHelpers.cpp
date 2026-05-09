#include "ShowHelpers.h"
#include "AppModel.h"

#include "engine/AudioMath.h"
#include "engine/FadeData.h"
#include "engine/Timecode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace ShowHelpers {

void normalizeListRefs(mcp::ShowFile& sf) {
    for (auto& cl : sf.cueLists) {
        const int listId = cl.numericId;
        if (listId == 0) continue;  // not yet assigned — skip
        std::function<void(std::vector<mcp::ShowFile::CueData>&)> fix;
        fix = [&](std::vector<mcp::ShowFile::CueData>& cues) {
            for (auto& cd : cues) {
                if (cd.targetListId == -1) cd.targetListId = listId;
                for (auto& mk : cd.markers)
                    if (mk.anchorMarkerListId == -1) mk.anchorMarkerListId = listId;
                if (!cd.children.empty()) fix(cd.children);
            }
        };
        fix(cl.cues);
    }
}

std::string nextCueNumber(const mcp::ShowFile& sf) {
    // Collect all existing cue numbers from ALL lists (globally unique numbering).
    std::vector<std::string> taken;
    std::function<void(const std::vector<mcp::ShowFile::CueData>&)> collect;
    collect = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
        for (const auto& cd : cues) {
            if (!cd.cueNumber.empty()) taken.push_back(cd.cueNumber);
            if (!cd.children.empty()) collect(cd.children);
        }
    };
    for (const auto& cl : sf.cueLists) collect(cl.cues);

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
    // Search the first (active) list only — caller resolves cross-list refs separately.
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

bool rebuildCueList(AppModel& m, int listIdx, std::string& /*err*/) {
    mcp::CueList& cl = m.cueListAt(listIdx);
    cl.clear();
    if (m.sf.cueLists.empty()) return true;
    const auto& topCues = m.sf.cueLists[static_cast<size_t>(listIdx)].cues;
    const auto  base    = std::filesystem::path(m.baseDir);

    // Install network and MIDI patches so their cues can fire.
    cl.setNetworkPatches(m.sf.networkSetup.patches);
    cl.setMidiPatches(m.sf.midiSetup.patches);

    // Build and install the channel map so applyRoutingToReader folds correctly.
    {
        const auto& setup = m.sf.audioSetup;
        const bool isMultiDevice = !setup.devices.empty();

        // Total physical output count: sum of all device channel counts in
        // multi-device mode, or the single engine output count in legacy mode.
        int numPhys = 0;
        if (isMultiDevice) {
            for (const auto& d : setup.devices) numPhys += d.channelCount;
        } else {
            numPhys = m.engineOk ? m.engine.channels() : 2;
        }

        mcp::ChannelMap cm;
        cm.numPhys = numPhys;
        cm.numCh = static_cast<int>(setup.channels.size());

        // Multi-device: fill physDevice / physLocalCh / devicePhysCount.
        if (isMultiDevice) {
            const int numDev = static_cast<int>(setup.devices.size());
            cm.devicePhysCount.resize(static_cast<size_t>(numDev), 0);
            cm.physDevice.resize(static_cast<size_t>(numPhys), 0);
            cm.physLocalCh.resize(static_cast<size_t>(numPhys), 0);
            int gp = 0;
            for (int d = 0; d < numDev; ++d) {
                const int cnt = setup.devices[static_cast<size_t>(d)].channelCount;
                cm.devicePhysCount[static_cast<size_t>(d)] = cnt;
                for (int lp = 0; lp < cnt; ++lp, ++gp) {
                    cm.physDevice[static_cast<size_t>(gp)]  = d;
                    cm.physLocalCh[static_cast<size_t>(gp)] = lp;
                }
            }
        }

        if (cm.numCh == 0) {
            // No audio setup: identity map (legacy / new empty show)
            cm.numCh = numPhys;
            cm.fold.assign(static_cast<size_t>(numPhys * numPhys), 0.0f);
            for (int i = 0; i < numPhys; ++i)
                cm.fold[static_cast<size_t>(i * numPhys + i)] = 1.0f;
            cm.primaryPhys.resize(static_cast<size_t>(numPhys));
            for (int i = 0; i < numPhys; ++i) cm.primaryPhys[static_cast<size_t>(i)] = i;
        } else {
            cm.fold.assign(static_cast<size_t>(cm.numCh * numPhys), 0.0f);
            // Default: diagonal (channel ch → physOut ch if in range)
            for (int ch = 0; ch < cm.numCh && ch < numPhys; ++ch)
                cm.fold[static_cast<size_t>(ch * numPhys + ch)] = 1.0f;
            // Apply explicit crosspoint entries
            for (const auto& xe : setup.xpEntries) {
                if (xe.ch >= 0 && xe.ch < cm.numCh && xe.out >= 0 && xe.out < numPhys)
                    cm.fold[static_cast<size_t>(xe.ch * numPhys + xe.out)] =
                        (xe.db <= -144.0f) ? 0.0f : mcp::lut::dBToLinear(xe.db);
            }
            // Apply channel master gain and mute
            for (int ch = 0; ch < cm.numCh; ++ch) {
                const auto& cfg = setup.channels[static_cast<size_t>(ch)];
                float g = cfg.mute ? 0.0f : mcp::lut::dBToLinear(cfg.masterGainDb);
                for (int p = 0; p < numPhys; ++p)
                    cm.fold[static_cast<size_t>(ch * numPhys + p)] *= g;
            }
            // Compute primary physOut per channel (argmax of fold row)
            cm.primaryPhys.resize(static_cast<size_t>(cm.numCh), 0);
            for (int ch = 0; ch < cm.numCh; ++ch) {
                float maxG = -1.0f;
                int   maxP = (ch < numPhys) ? ch : 0;
                for (int p = 0; p < numPhys; ++p) {
                    float g = cm.fold[static_cast<size_t>(ch * numPhys + p)];
                    if (g > maxG) { maxG = g; maxP = p; }
                }
                cm.primaryPhys[static_cast<size_t>(ch)] = maxP;
            }
        }
        cl.setChannelMap(std::move(cm));
    }

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
    // targetListId must match this list's numericId for within-list resolution;
    // cross-list targets (-1 after normalizeListRefs means unassigned, any other
    // mismatching ID means another list) return -1 — the engine doesn't support
    // cross-list targeting yet.
    const int thisListId = m.sf.cueLists[static_cast<size_t>(listIdx)].numericId;
    auto resolveTarget = [&](const mcp::ShowFile::CueData& cd) -> int {
        // -1 is legacy "same list" kept for files written before normalizeListRefs.
        const bool sameList = (cd.targetListId == -1 || cd.targetListId == thisListId);
        if (!sameList) return -1;  // cross-list: not supported in engine yet
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
            const int myIdx = cl.cueCount();

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

                cl.addGroupCue(mode, cd.groupRandom, cd.name, cd.preWait);
                cl.setCueCueNumber   (myIdx, cd.cueNumber);
                cl.setCueAutoContinue(myIdx, cd.autoContinue);
                cl.setCueAutoFollow  (myIdx, cd.autoFollow);
                cl.setCueChildCount  (myIdx, countAll(cd.children));
                if (parentFlatIdx >= 0) cl.setCueParentIndex(myIdx, parentFlatIdx);
                cl.setCueTimelineOffset(myIdx, cd.timelineOffset);

                // SyncGroup: load its own markers and slice loops
                if (cd.groupMode == "sync") {
                    std::vector<mcp::Cue::TimeMarker> marks;
                    for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
                    cl.setCueMarkers   (myIdx, marks);
                    cl.setCueSliceLoops(myIdx, cd.sliceLoops);
                }

                cl.setCueMusicContext(myIdx, buildMC(cd.musicContext));

                // Recursively add children immediately after the group header.
                process(cd.children, myIdx);
            } else {
                // Non-group cue
                const int target = resolveTarget(cd);

                if (cd.type == "audio") {
                    auto p = std::filesystem::path(cd.path);
                    if (p.is_relative()) p = base / p;
                    if (!cl.addCue(p.string(), cd.name, cd.preWait))
                        cl.addBrokenAudioCue(p.string(), cd.name, cd.preWait);
                } else if (cd.type == "start") {
                    cl.addStartCue(target, cd.name, cd.preWait);
                } else if (cd.type == "stop") {
                    cl.addStopCue(target, cd.name, cd.preWait);
                } else if (cd.type == "fade") {
                    const auto curve = (cd.fadeCurve == "equalpower")
                        ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
                    cl.addFadeCue(target, cd.targetCueNumber, curve,
                                      cd.fadeStopWhenDone, cd.name, cd.preWait);
                } else if (cd.type == "arm") {
                    cl.addArmCue(target, cd.name, cd.preWait);
                } else if (cd.type == "devamp") {
                    cl.addDevampCue(target, cd.name, cd.preWait, cd.devampMode);
                } else if (cd.type == "mc") {
                    cl.addMCCue(cd.name, cd.preWait);
                } else if (cd.type == "marker") {
                    cl.addMarkerCue(target, cd.markerIndex, cd.name, cd.preWait);
                } else if (cd.type == "goto") {
                    cl.addGotoCue(target, cd.name, cd.preWait);
                } else if (cd.type == "memo") {
                    cl.addMemoCue(cd.name, cd.preWait);
                } else if (cd.type == "scriptlet") {
                    cl.addScriptletCue(cd.name, cd.preWait);
                } else if (cd.type == "network") {
                    cl.addNetworkCue(cd.name, cd.preWait);
                } else if (cd.type == "midi") {
                    cl.addMidiCue(cd.name, cd.preWait);
                } else if (cd.type == "timecode") {
                    cl.addTimecodeCue(cd.name, cd.preWait);
                }

                cl.setCueCueNumber   (myIdx, cd.cueNumber);
                cl.setCueStartTime   (myIdx, cd.startTime);
                cl.setCueDuration    (myIdx, cd.duration);
                cl.setCueLevel       (myIdx, cd.level);
                cl.setCueTrim        (myIdx, cd.trim);
                cl.setCueAutoContinue(myIdx, cd.autoContinue);
                cl.setCueAutoFollow  (myIdx, cd.autoFollow);
                cl.setCueGoQuantize  (myIdx, cd.goQuantize);
                if (parentFlatIdx >= 0) cl.setCueParentIndex(myIdx, parentFlatIdx);
                cl.setCueTimelineOffset(myIdx, cd.timelineOffset);

                if (cd.type == "arm")
                    cl.setCueArmStartTime(myIdx, cd.armStartTime);
                if (cd.type == "devamp") {
                    cl.setCueDevampMode   (myIdx, cd.devampMode);
                    cl.setCueDevampPreVamp(myIdx, cd.devampPreVamp);
                }
                if (cd.type == "marker")
                    cl.setCueMarkerIndex(myIdx, cd.markerIndex);
                if (cd.type == "scriptlet")
                    cl.setCueScriptletCode(myIdx, cd.scriptletCode);
                if (cd.type == "network") {
                    // Resolve patch name → index
                    int patchIdx = -1;
                    for (int pi = 0; pi < (int)m.sf.networkSetup.patches.size(); ++pi) {
                        if (m.sf.networkSetup.patches[static_cast<size_t>(pi)].name == cd.networkPatchName) {
                            patchIdx = pi;
                            break;
                        }
                    }
                    cl.setCueNetworkPatch  (myIdx, patchIdx);
                    cl.setCueNetworkCommand(myIdx, cd.networkCommand);
                }
                if (cd.type == "midi") {
                    int patchIdx = -1;
                    for (int pi = 0; pi < (int)m.sf.midiSetup.patches.size(); ++pi) {
                        if (m.sf.midiSetup.patches[static_cast<size_t>(pi)].name == cd.midiPatchName) {
                            patchIdx = pi;
                            break;
                        }
                    }
                    cl.setCueMidiPatch  (myIdx, patchIdx);
                    cl.setCueMidiMessage(myIdx, cd.midiMessageType,
                                             cd.midiChannel, cd.midiData1, cd.midiData2);
                }
                if (cd.type == "timecode") {
                    mcp::TcFps fps = mcp::TcFps::Fps25;
                    mcp::tcFpsFromString(cd.tcFps, fps);
                    mcp::TcPoint startTC, endTC;
                    mcp::tcFromString(cd.tcStartTC, startTC);
                    mcp::tcFromString(cd.tcEndTC,   endTC);
                    cl.setCueTcFps  (myIdx, fps);
                    cl.setCueTcStart(myIdx, startTC);
                    cl.setCueTcEnd  (myIdx, endTC);
                    cl.setCueTcType (myIdx, cd.tcType);
                    cl.setCueTcLtcChannel(myIdx, cd.tcLtcChannel);
                    // Resolve MTC MIDI patch name → index
                    int midiPatchIdx = -1;
                    if (!cd.tcMidiPatchName.empty()) {
                        for (int pi = 0; pi < (int)m.sf.midiSetup.patches.size(); ++pi) {
                            if (m.sf.midiSetup.patches[static_cast<size_t>(pi)].name == cd.tcMidiPatchName) {
                                midiPatchIdx = pi; break;
                            }
                        }
                    }
                    cl.setCueTcMidiPatch(myIdx, midiPatchIdx);
                }
                if (cd.type == "audio") {
                    std::vector<mcp::Cue::TimeMarker> marks;
                    for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
                    cl.setCueMarkers   (myIdx, marks);
                    cl.setCueSliceLoops(myIdx, cd.sliceLoops);
                    for (int o = 0; o < (int)cd.outLevelDb.size(); ++o)
                        cl.setCueOutLevel(myIdx, o, cd.outLevelDb[o]);
                    for (const auto& xe : cd.xpEntries)
                        cl.setCueXpoint(myIdx, xe.s, xe.o, xe.db);
                }
                if (cd.type == "fade") {
                    cl.setCueFadeMasterTarget  (myIdx, cd.fadeMasterEnabled, cd.fadeMasterTarget);
                    cl.setCueFadeOutTargetCount(myIdx, (int)cd.fadeOutLevels.size());
                    for (const auto& fl : cd.fadeOutLevels)
                        cl.setCueFadeOutTarget(myIdx, fl.ch, fl.enabled, fl.target);
                    for (const auto& fx : cd.fadeXpEntries)
                        cl.setCueFadeXpTarget(myIdx, fx.s, fx.o, fx.enabled, fx.target);
                }
                cl.setCueMusicContext(myIdx, buildMC(cd.musicContext));
            }
        }
    };

    process(topCues, -1);

    // Second pass: resolve mcSourceNumber → setCueMCSource.
    // Done after process() so forward-references (source cue comes later) work.
    {
        std::unordered_map<std::string, int> numToFlatIdx;
        for (int i = 0; i < cl.cueCount(); ++i) {
            const auto* c = cl.cueAt(i);
            if (c && !c->cueNumber.empty()) numToFlatIdx[c->cueNumber] = i;
        }
        // Walk the CueData tree to find pending mcSourceNumber entries.
        std::function<void(const std::vector<mcp::ShowFile::CueData>&, int&)> resolveSource;
        resolveSource = [&](const std::vector<mcp::ShowFile::CueData>& cues, int& flatIdx) {
            for (const auto& cd : cues) {
                const int myIdx = flatIdx++;
                if (!cd.mcSourceNumber.empty()) {
                    auto it = numToFlatIdx.find(cd.mcSourceNumber);
                    if (it != numToFlatIdx.end())
                        cl.setCueMCSource(myIdx, it->second);
                }
                if (!cd.children.empty()) resolveSource(cd.children, flatIdx);
            }
        };
        int idx = 0;
        resolveSource(topCues, idx);
    }

    // Third pass: resolve anchorMarkerCueNumber → setMarkerAnchor.
    {
        std::function<void(const std::vector<mcp::ShowFile::CueData>&, int&)> resolveAnchors;
        resolveAnchors = [&](const std::vector<mcp::ShowFile::CueData>& cues, int& flatIdx) {
            for (const auto& cd : cues) {
                const int myIdx = flatIdx++;
                if (cd.type == "audio") {
                    for (int mi = 0; mi < (int)cd.markers.size(); ++mi) {
                        const auto& mkNum = cd.markers[static_cast<size_t>(mi)].anchorMarkerCueNumber;
                        if (mkNum.empty()) continue;
                        // Find the Marker cue with this cue number
                        for (int ci = 0; ci < cl.cueCount(); ++ci) {
                            const auto* cc = cl.cueAt(ci);
                            if (cc && cc->cueNumber == mkNum)
                                cl.setMarkerAnchor(myIdx, mi, ci);
                        }
                    }
                }
                if (!cd.children.empty()) resolveAnchors(cd.children, flatIdx);
            }
        };
        int idx2 = 0;
        resolveAnchors(topCues, idx2);
    }

    return true;
}

bool rebuildAllCueLists(AppModel& m, std::string& err) {
    m.syncListCount();
    bool ok = true;
    for (int li = 0; li < static_cast<int>(m.sf.cueLists.size()); ++li)
        if (!rebuildCueList(m, li, err)) ok = false;
    // Resolve any -1 "same list" placeholders to absolute numericIds so that
    // cut+paste across lists never misidentifies the source list.
    normalizeListRefs(m.sf);

    // Second pass: wire up cross-list targets now that all engine lists are built.
    for (int li = 0; li < static_cast<int>(m.sf.cueLists.size()); ++li) {
        const int thisListId = m.sf.cueLists[static_cast<size_t>(li)].numericId;
        auto& cl = m.cueListAt(li);

        int counter = 0;
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> visit;
        visit = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& cd : cues) {
                const int myFlatIdx = counter++;
                const bool crossList = (cd.targetListId != -1 && cd.targetListId != thisListId);
                if (crossList && (cd.type == "start" || cd.type == "stop")) {
                    // Find the target list's engine.
                    int tgtListIdx = -1;
                    for (int tli = 0; tli < static_cast<int>(m.sf.cueLists.size()); ++tli) {
                        if (m.sf.cueLists[static_cast<size_t>(tli)].numericId == cd.targetListId) {
                            tgtListIdx = tli; break;
                        }
                    }
                    if (tgtListIdx >= 0 && tgtListIdx < m.listCount()) {
                        auto& tgtEng = m.cueListAt(tgtListIdx);
                        int tgtFlatIdx = -1;
                        // Prefer cue-number lookup (stable), fall back to stored flat index.
                        if (!cd.targetCueNumber.empty()) {
                            for (int ti = 0; ti < tgtEng.cueCount(); ++ti) {
                                const auto* tc = tgtEng.cueAt(ti);
                                if (tc && tc->cueNumber == cd.targetCueNumber) {
                                    tgtFlatIdx = ti; break;
                                }
                            }
                        }
                        if (tgtFlatIdx < 0 && cd.target >= 0 && cd.target < tgtEng.cueCount())
                            tgtFlatIdx = cd.target;
                        if (tgtFlatIdx >= 0) {
                            cl.setCueCrossListTarget(myFlatIdx, cd.targetListId, tgtFlatIdx);
                            // Back-fill targetCueNumber in SF so future syncs preserve it.
                            if (auto* sfCue = sfCueAt(m.sf, li, myFlatIdx)) {
                                if (sfCue->targetCueNumber.empty()) {
                                    const auto* tc = tgtEng.cueAt(tgtFlatIdx);
                                    if (tc && !tc->cueNumber.empty())
                                        sfCue->targetCueNumber = tc->cueNumber;
                                }
                            }
                        }
                    }
                }
                if (!cd.children.empty()) visit(cd.children);
            }
        };
        visit(m.sf.cueLists[static_cast<size_t>(li)].cues);
    }

    // Set up cross-list callbacks on each list so fire() can delegate.
    for (int li = 0; li < m.listCount(); ++li) {
        m.cueListAt(li).setCrossListStartCallback([&m](int numericId, int flatIdx) {
            for (int tli = 0; tli < m.listCount(); ++tli) {
                if (tli < static_cast<int>(m.sf.cueLists.size()) &&
                    m.sf.cueLists[static_cast<size_t>(tli)].numericId == numericId) {
                    m.cueListAt(tli).start(flatIdx);
                    return;
                }
            }
        });
        m.cueListAt(li).setCrossListStopCallback([&m](int numericId, int flatIdx) {
            for (int tli = 0; tli < m.listCount(); ++tli) {
                if (tli < static_cast<int>(m.sf.cueLists.size()) &&
                    m.sf.cueLists[static_cast<size_t>(tli)].numericId == numericId) {
                    m.cueListAt(tli).stop(flatIdx);
                    return;
                }
            }
        });
    }

    return ok;
}

// Reconstruct the nested ShowFile structure from the flat engine CueList.
// Group headers and their descendants are re-nested using childCount for range tracking.
void syncSfFromCues(AppModel& m, int listIdx) {
    if (m.sf.cueLists.empty()) return;
    if (listIdx < 0) listIdx = m.activeListIdx();
    if (listIdx < 0 || listIdx >= static_cast<int>(m.sf.cueLists.size())) return;

    auto& cueList = m.cueListAt(listIdx);
    const int activeListNumericId = m.sf.cueLists[static_cast<size_t>(listIdx)].numericId;
    const int total               = cueList.cueCount();

    // Build a helper to look up a cue's number by flat index.
    auto cueNumAt = [&](int idx) -> std::string {
        if (idx < 0 || idx >= total) return "";
        const auto* c = cueList.cueAt(idx);
        return c ? c->cueNumber : "";
    };

    auto fillCommon = [&](mcp::ShowFile::CueData& cd, const mcp::Cue& c) {
        cd.cueNumber      = c.cueNumber;
        cd.name           = c.name;
        cd.preWait        = c.preWaitSeconds;
        cd.goQuantize     = c.goQuantize;
        cd.autoContinue   = c.autoContinue;
        cd.autoFollow     = c.autoFollow;
        cd.timelineOffset = c.timelineOffset;
        if (c.mcSourceIdx >= 0) {
            // Inherited MC: store the source cue number, not the MC data itself.
            cd.mcSourceNumber = cueNumAt(c.mcSourceIdx);
        } else if (c.musicContext) {
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

    // Resolve target reference from engine fields — cross-list or same-list.
    auto resolveEngineTargetRef = [&](const mcp::Cue* c, mcp::ShowFile::CueData& cd) {
        if (c->crossListNumericId != -1) {
            cd.targetListId = c->crossListNumericId;
            cd.target       = c->crossListFlatIdx;
            for (int tli = 0; tli < m.listCount(); ++tli) {
                if (tli < (int)m.sf.cueLists.size() &&
                    m.sf.cueLists[static_cast<size_t>(tli)].numericId == c->crossListNumericId) {
                    const auto* tc = m.cueListAt(tli).cueAt(c->crossListFlatIdx);
                    if (tc) cd.targetCueNumber = tc->cueNumber;
                    break;
                }
            }
        } else {
            cd.targetListId    = activeListNumericId;
            cd.target          = c->targetIndex;
            cd.targetCueNumber = cueNumAt(c->targetIndex);
        }
    };

    // Recursively reconstruct nesting.
    // Processes flat indices [startIdx, startIdx+count) into dest.
    std::function<void(int, int, std::vector<mcp::ShowFile::CueData>&)> extract;
    extract = [&](int startIdx, int count, std::vector<mcp::ShowFile::CueData>& dest) {
        for (int i = startIdx; i < startIdx + count && i < total; ) {
            const auto* c = cueList.cueAt(i);
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
                    for (const auto& mk : c->markers) {
                        mcp::ShowFile::CueData::TimeMarker tm;
                        tm.time = mk.time; tm.name = mk.name;
                        if (mk.anchorMarkerCueIdx >= 0)
                            tm.anchorMarkerCueNumber = cueNumAt(mk.anchorMarkerCueIdx);
                        cd.markers.push_back(std::move(tm));
                    }
                    cd.sliceLoops = c->sliceLoops;
                    cd.outLevelDb = c->routing.outLevelDb;
                    for (int s = 0; s < (int)c->routing.xpoint.size(); ++s)
                        for (int o = 0; o < (int)c->routing.xpoint[s].size(); ++o)
                            if (c->routing.xpoint[s][o].has_value())
                                cd.xpEntries.push_back({s, o, *c->routing.xpoint[s][o]});
                    ++i;
                    break;

                case mcp::CueType::Start:
                    cd.type = "start";
                    resolveEngineTargetRef(c, cd);
                    ++i;
                    break;

                case mcp::CueType::Stop:
                    cd.type = "stop";
                    resolveEngineTargetRef(c, cd);
                    ++i;
                    break;

                case mcp::CueType::Arm:
                    cd.type         = "arm";
                    cd.armStartTime = c->armStartTime;
                    resolveEngineTargetRef(c, cd);
                    ++i;
                    break;

                case mcp::CueType::Devamp:
                    cd.type          = "devamp";
                    cd.devampMode    = c->devampMode;
                    cd.devampPreVamp = c->devampPreVamp;
                    resolveEngineTargetRef(c, cd);
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
                            for (const auto& mk : c->markers) {
                                mcp::ShowFile::CueData::TimeMarker tm;
                                tm.time = mk.time; tm.name = mk.name;
                                if (mk.anchorMarkerCueIdx >= 0)
                                    tm.anchorMarkerCueNumber = cueNumAt(mk.anchorMarkerCueIdx);
                                cd.markers.push_back(std::move(tm));
                            }
                            cd.sliceLoops = c->sliceLoops;
                        }
                    }
                    if (c->childCount > 0)
                        extract(i + 1, c->childCount, cd.children);
                    i += c->childCount + 1;  // skip group header + all descendants
                    break;

                case mcp::CueType::MusicContext:
                    cd.type = "mc";
                    ++i;
                    break;

                case mcp::CueType::Marker:
                    cd.type        = "marker";
                    cd.markerIndex = c->markerIndex;
                    resolveEngineTargetRef(c, cd);
                    ++i;
                    break;

                case mcp::CueType::Network:
                    cd.type              = "network";
                    cd.networkPatchName  = cueList.networkPatchName(c->networkPatchIdx);
                    cd.networkCommand    = c->networkCommand;
                    ++i;
                    break;

                case mcp::CueType::Midi:
                    cd.type            = "midi";
                    cd.midiPatchName   = cueList.midiPatchName(c->midiPatchIdx);
                    cd.midiMessageType = c->midiMessageType;
                    cd.midiChannel     = c->midiChannel;
                    cd.midiData1       = c->midiData1;
                    cd.midiData2       = c->midiData2;
                    break;
                case mcp::CueType::Timecode:
                    cd.type            = "timecode";
                    cd.tcType          = c->tcType;
                    cd.tcFps           = mcp::tcFpsToString(c->tcFps);
                    cd.tcStartTC       = mcp::tcToString(c->tcStartTC);
                    cd.tcEndTC         = mcp::tcToString(c->tcEndTC);
                    cd.tcLtcChannel    = c->tcLtcChannel;
                    cd.tcMidiPatchName = cueList.midiPatchName(c->tcMidiPatchIdx);
                    ++i;
                    break;

                case mcp::CueType::Goto:
                    cd.type = "goto";
                    resolveEngineTargetRef(c, cd);
                    ++i;
                    break;

                case mcp::CueType::Memo:
                    cd.type = "memo";
                    ++i;
                    break;

                case mcp::CueType::Scriptlet:
                    cd.type           = "scriptlet";
                    cd.scriptletCode  = c->scriptletCode;
                    ++i;
                    break;
            }

            dest.push_back(std::move(cd));
        }
    };

    // Snapshot SF-only fields (not tracked by the engine) indexed by cue number.
    // These are lost when we clear and rebuild from engine state, so we preserve
    // them here and restore them after the rebuild.
    struct SfExtra {
        mcp::CueTriggers triggers;
        std::vector<int> markerAnchorListIds;  // per marker, by index
    };
    std::unordered_map<std::string, SfExtra> extra;
    {
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> snap;
        snap = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& cd : cues) {
                if (!cd.cueNumber.empty()) {
                    SfExtra& e = extra[cd.cueNumber];
                    e.triggers = cd.triggers;
                    for (const auto& mk : cd.markers)
                        e.markerAnchorListIds.push_back(mk.anchorMarkerListId);
                }
                if (!cd.children.empty()) snap(cd.children);
            }
        };
        snap(m.sf.cueLists[static_cast<size_t>(listIdx)].cues);
    }

    m.sf.cueLists[static_cast<size_t>(listIdx)].cues.clear();
    extract(0, total, m.sf.cueLists[static_cast<size_t>(listIdx)].cues);

    // Restore SF-only fields by cue number.
    {
        std::function<void(std::vector<mcp::ShowFile::CueData>&)> restore;
        restore = [&](std::vector<mcp::ShowFile::CueData>& cues) {
            for (auto& cd : cues) {
                auto it = extra.find(cd.cueNumber);
                if (it != extra.end()) {
                    cd.triggers = it->second.triggers;
                    for (int mi = 0; mi < (int)cd.markers.size() &&
                                     mi < (int)it->second.markerAnchorListIds.size(); ++mi)
                        cd.markers[static_cast<size_t>(mi)].anchorMarkerListId =
                            it->second.markerAnchorListIds[static_cast<size_t>(mi)];
                }
                if (!cd.children.empty()) restore(cd.children);
            }
        };
        restore(m.sf.cueLists[static_cast<size_t>(listIdx)].cues);
    }

    m.dirty = true;
}

void syncAllSfFromCues(AppModel& m) {
    for (int li = 0; li < static_cast<int>(m.sf.cueLists.size()); ++li)
        syncSfFromCues(m, li);
}

// ---------------------------------------------------------------------------
// SF navigation helpers (DFS pre-order, matching rebuildCueList insertion order)

mcp::ShowFile::CueData* sfCueAt(mcp::ShowFile& sf, int listIdx, int flatIdx) {
    if (listIdx < 0 || listIdx >= (int)sf.cueLists.size() || flatIdx < 0) return nullptr;
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
    return find(sf.cueLists[listIdx].cues);
}

const mcp::ShowFile::CueData* sfCueAt(const mcp::ShowFile& sf, int listIdx, int flatIdx) {
    return sfCueAt(const_cast<mcp::ShowFile&>(sf), listIdx, flatIdx);
}

mcp::ShowFile::CueData sfRemoveAt(mcp::ShowFile& sf, int listIdx, int flatIdx) {
    if (listIdx < 0 || listIdx >= (int)sf.cueLists.size() || flatIdx < 0) return {};
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
    rem(sf.cueLists[listIdx].cues);
    return removed;
}

void sfFixTargetsAfterRemoval(mcp::ShowFile& sf, int listIdx, int removedFlatIdx) {
    if (listIdx < 0 || listIdx >= (int)sf.cueLists.size()) return;
    std::function<void(std::vector<mcp::ShowFile::CueData>&)> fix;
    fix = [&](std::vector<mcp::ShowFile::CueData>& cues) {
        for (auto& cd : cues) {
            if (cd.target == removedFlatIdx)       cd.target = -1;
            else if (cd.target > removedFlatIdx)   --cd.target;
            if (!cd.children.empty()) fix(cd.children);
        }
    };
    fix(sf.cueLists[listIdx].cues);
}

void sfFixTargetsForReorder(mcp::ShowFile& sf, int srcListIdx,
                             int srcRow, int blockSize, int dstRow) {
    if (srcListIdx < 0 || srcListIdx >= (int)sf.cueLists.size()) return;
    const int srcListId = sf.cueLists[static_cast<size_t>(srcListIdx)].numericId;

    // Map old flat index T (in the reordered list) to the new flat index after the move.
    auto newIdx = [&](int T) -> int {
        if (T < 0) return T;
        if (dstRow > srcRow) {  // forward move: block ends up at [dstRow-blockSize, dstRow)
            if (T >= srcRow && T < srcRow + blockSize)
                return T + (dstRow - srcRow - blockSize);   // moved block
            if (T >= srcRow + blockSize && T < dstRow)
                return T - blockSize;                       // shifted left
        } else if (dstRow < srcRow) {  // backward move: block ends up at [dstRow, dstRow+blockSize)
            if (T >= srcRow && T < srcRow + blockSize)
                return T + (dstRow - srcRow);               // moved block
            if (T >= dstRow && T < srcRow)
                return T + blockSize;                       // shifted right
        }
        return T;
    };

    for (int li = 0; li < (int)sf.cueLists.size(); ++li) {
        const int thisListId = sf.cueLists[static_cast<size_t>(li)].numericId;
        std::function<void(std::vector<mcp::ShowFile::CueData>&)> fix;
        fix = [&](std::vector<mcp::ShowFile::CueData>& cues) {
            for (auto& cd : cues) {
                const bool refersToSrc = (cd.targetListId == srcListId ||
                    (cd.targetListId == -1 && thisListId == srcListId));
                if (refersToSrc && cd.target >= 0)
                    cd.target = newIdx(cd.target);
                if (!cd.children.empty()) fix(cd.children);
            }
        };
        fix(sf.cueLists[static_cast<size_t>(li)].cues);
    }
}

void sfInsertBefore(mcp::ShowFile& sf, int listIdx, int beforeFlatIdx, mcp::ShowFile::CueData cd) {
    if (listIdx < 0 || listIdx >= (int)sf.cueLists.size()) return;
    int counter = 0;
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
    if (!ins(sf.cueLists[listIdx].cues))
        sf.cueLists[listIdx].cues.push_back(std::move(cd));   // past-end → append top-level
}

void sfAppendToGroup(mcp::ShowFile& sf, int listIdx, int groupFlatIdx, mcp::ShowFile::CueData cd) {
    auto* group = sfCueAt(sf, listIdx, groupFlatIdx);
    if (group) group->children.push_back(std::move(cd));
}

void saveShow(AppModel& m) {
    if (m.showPath.empty()) return;
    std::string err;
    m.sf.save(m.showPath, err);
    if (err.empty()) { m.dirty = false; emit m.dirtyChanged(false); }
}

void setCueNumberChecked(AppModel& m, int index, const std::string& num) {
    if (!num.empty()) {
        for (int i = 0; i < m.cues().cueCount(); ++i) {
            if (i == index) continue;
            const auto* other = m.cues().cueAt(i);
            if (other && other->cueNumber == num) {
                m.cues().setCueCueNumber(index, "");
                syncSfFromCues(m);
                return;
            }
        }
    }
    m.cues().setCueCueNumber(index, num);
    syncSfFromCues(m);
}

} // namespace ShowHelpers
