#include "ShowHelpers.h"
#include "AppModel.h"

#include "engine/AudioDecoder.h"
#include "engine/AudioMath.h"
#include "engine/FadeData.h"
#include "engine/Timecode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include <sndfile.h>
#include <samplerate.h>

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
            cm.xpLinear = cm.fold;
            cm.liveMasterGainDb.assign(static_cast<size_t>(numPhys), 0.0f);
            cm.liveMute.assign(static_cast<size_t>(numPhys), false);
            cm.stereoSlave.assign(static_cast<size_t>(numPhys), -1);
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
            // Save xp-only fold for live automation updates, then apply master gain.
            cm.xpLinear = cm.fold;
            cm.liveMasterGainDb.resize(static_cast<size_t>(cm.numCh), 0.0f);
            cm.liveMute.resize(static_cast<size_t>(cm.numCh), false);
            cm.stereoSlave.assign(static_cast<size_t>(cm.numCh), -1);
            // Apply channel master gain and mute
            for (int ch = 0; ch < cm.numCh; ++ch) {
                const auto& cfg = setup.channels[static_cast<size_t>(ch)];
                cm.liveMasterGainDb[static_cast<size_t>(ch)] = cfg.masterGainDb;
                cm.liveMute[static_cast<size_t>(ch)] = cfg.mute;
                if (cfg.linkedStereo && ch + 1 < cm.numCh)
                    cm.stereoSlave[static_cast<size_t>(ch)] = ch + 1;
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
                } else if (cd.type == "snapshot") {
                    cl.addSnapshotCue(cd.name, cd.preWait);
                } else if (cd.type == "automation") {
                    cl.addAutomationCue(cd.name, cd.preWait);
                } else if (cd.type == "deactivate") {
                    cl.addDeactivateCue(cd.name, cd.preWait);
                } else if (cd.type == "reactivate") {
                    cl.addReactivateCue(cd.name, cd.preWait);
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
                if (cd.type == "snapshot")
                    cl.setCueSnapshotId(myIdx, cd.snapshotId);
                if (cd.type == "automation") {
                    cl.setCueAutomationPath(myIdx, cd.automationPath);
                    cl.setCueAutomationDuration(myIdx, cd.automationDuration);
                    std::vector<mcp::Cue::AutomationPoint> pts;
                    pts.reserve(cd.automationCurve.size());
                    for (const auto& p : cd.automationCurve)
                        pts.push_back({p.time, p.value, p.isHandle});
                    cl.setCueAutomationCurve(myIdx, pts);
                }
                if (cd.type == "deactivate" || cd.type == "reactivate")
                    cl.setCuePluginSlot(myIdx, cd.pluginChannel, cd.pluginSlot);
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

                case mcp::CueType::Snapshot:
                    cd.type       = "snapshot";
                    cd.snapshotId = c->snapshotId;
                    ++i;
                    break;

                case mcp::CueType::Automation: {
                    cd.type               = "automation";
                    cd.automationPath     = c->automationPath;
                    cd.automationDuration = c->automationDuration;
                    for (const auto& p : c->automationCurve)
                        cd.automationCurve.push_back({p.time, p.value, p.isHandle});
                    ++i;
                    break;
                }

                case mcp::CueType::Deactivate:
                    cd.type          = "deactivate";
                    cd.pluginChannel = c->pluginChannel;
                    cd.pluginSlot    = c->pluginSlot;
                    ++i;
                    break;

                case mcp::CueType::Reactivate:
                    cd.type          = "reactivate";
                    cd.pluginChannel = c->pluginChannel;
                    cd.pluginSlot    = c->pluginSlot;
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

    m.markDirty();
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

// ─────────────────────────────────────────────────────────────────────────────
// Collect All Files helpers
// ─────────────────────────────────────────────────────────────────────────────

// Walk all CueData trees and call fn(cd) for every audio cue.
static void walkAudioCues(
    std::vector<mcp::ShowFile::CueData>& cues,
    const std::function<void(mcp::ShowFile::CueData&)>& fn)
{
    for (auto& cd : cues) {
        if (cd.type == "audio") fn(cd);
        if (!cd.children.empty()) walkAudioCues(cd.children, fn);
    }
}

static void walkAudioCuesConst(
    const std::vector<mcp::ShowFile::CueData>& cues,
    const std::function<void(const mcp::ShowFile::CueData&)>& fn)
{
    for (const auto& cd : cues) {
        if (cd.type == "audio") fn(cd);
        if (!cd.children.empty()) walkAudioCuesConst(cd.children, fn);
    }
}

// Transcode src to a WAV file at dst. If targetSr matches the source sample
// rate, no resampling is performed. Returns false and fills err on failure.
static bool transcodeToWav(const std::string& src, const std::string& dst,
                            int targetSr, int bitDepth, std::string& err)
{
    std::string decErr;
    auto dec = mcp::AudioDecoder::open(src, decErr);
    if (!dec) { err = decErr; return false; }

    const int srcSr = dec->nativeSampleRate();
    const int ch    = dec->nativeChannels();

    SF_INFO out{};
    out.samplerate = targetSr;
    out.channels   = ch;
    out.format     = SF_FORMAT_WAV;
    switch (bitDepth) {
        case 16: out.format |= SF_FORMAT_PCM_16; break;
        case 24: out.format |= SF_FORMAT_PCM_24; break;
        default: out.format |= SF_FORMAT_FLOAT;  break;
    }

    SNDFILE* sf = sf_open(dst.c_str(), SFM_WRITE, &out);
    if (!sf) { err = sf_strerror(nullptr); return false; }

    constexpr int kChunk = 4096;

    if (srcSr == targetSr) {
        std::vector<float> buf(static_cast<size_t>(kChunk * ch));
        int64_t got;
        while ((got = dec->readFloat(buf.data(), kChunk)) > 0)
            sf_writef_float(sf, buf.data(), static_cast<sf_count_t>(got));
    } else {
        const double ratio = static_cast<double>(targetSr) / srcSr;
        int srcErr = 0;
        SRC_STATE* resampler = src_new(SRC_SINC_BEST_QUALITY, ch, &srcErr);
        if (!resampler) {
            sf_close(sf);
            err = src_strerror(srcErr);
            return false;
        }

        const int outChunk = static_cast<int>(kChunk * ratio) + 64;
        std::vector<float> inBuf (static_cast<size_t>(kChunk   * ch));
        std::vector<float> outBuf(static_cast<size_t>(outChunk * ch));

        SRC_DATA sd{};
        sd.src_ratio     = ratio;
        sd.data_in       = inBuf.data();
        sd.data_out      = outBuf.data();
        sd.output_frames = outChunk;

        bool eof = false;
        while (!eof) {
            const int64_t got = dec->readFloat(inBuf.data(), kChunk);
            eof = (got < kChunk);
            sd.input_frames  = got;
            sd.end_of_input  = eof ? 1 : 0;
            src_process(resampler, &sd);
            if (sd.output_frames_gen > 0)
                sf_writef_float(sf, outBuf.data(),
                                static_cast<sf_count_t>(sd.output_frames_gen));
        }
        src_delete(resampler);
    }

    sf_close(sf);
    return true;
}

// ─── Missing media helpers ────────────────────────────────────────────────────

// Find an audio/media cue by both its cue number AND its current stored path.
// Matching on both fields is necessary because:
//  • Cue numbers may be empty or duplicated across the list.
//  • The path uniquely identifies which physical cue we need to fix when numbers collide.
static mcp::ShowFile::CueData* findMediaCueForFix(
    std::vector<mcp::ShowFile::CueData>& cues,
    const std::string& cueNum, const std::string& originalPath)
{
    for (auto& cd : cues) {
        if (cd.cueNumber == cueNum && cd.path == originalPath) return &cd;
        if (!cd.children.empty())
            if (auto* p = findMediaCueForFix(cd.children, cueNum, originalPath)) return p;
    }
    return nullptr;
}

// ─── public API ──────────────────────────────────────────────────────────────

// Walk all cues (any type) that have a non-empty path field.
// More generic than walkAudioCues — picks up future media cue types automatically.
static void walkMediaCues(
    const std::vector<mcp::ShowFile::CueData>& cues,
    const std::function<void(const mcp::ShowFile::CueData&)>& fn)
{
    for (const auto& cd : cues) {
        if (!cd.path.empty()) fn(cd);
        if (!cd.children.empty()) walkMediaCues(cd.children, fn);
    }
}

std::vector<MissingEntry> findMissingMedia(const AppModel& m) {
    namespace fs = std::filesystem;
    std::vector<MissingEntry> result;
    const fs::path base(m.baseDir);

    for (int li = 0; li < (int)m.sf.cueLists.size(); ++li) {
        const auto& cl = m.sf.cueLists[(size_t)li];
        walkMediaCues(cl.cues, [&](const mcp::ShowFile::CueData& cd) {
            fs::path p(cd.path);
            fs::path abs = p.is_relative() ? base / p : p;
            if (!fs::exists(abs)) {
                MissingEntry e;
                e.listIdx      = li;
                e.cueType      = cd.type;
                e.cueNumber    = cd.cueNumber;
                e.cueName      = cd.name;
                e.originalPath = cd.path;
                e.resolvedPath = abs.string();
                result.push_back(std::move(e));
            }
        });
    }
    return result;
}

void applyMediaFixes(AppModel& m, const std::vector<MissingEntry>& fixes) {
    namespace fs = std::filesystem;
    const fs::path base(m.baseDir);

    for (const auto& fix : fixes) {
        if (fix.newPath.empty()) continue;
        if (fix.listIdx >= (int)m.sf.cueLists.size()) continue;
        auto& cl = m.sf.cueLists[(size_t)fix.listIdx];
        auto* cd = findMediaCueForFix(cl.cues, fix.cueNumber, fix.originalPath);
        if (!cd) continue;

        // Store relative if the file is within baseDir, absolute otherwise
        fs::path np(fix.newPath);
        std::error_code ec;
        fs::path rel = fs::relative(np, base, ec);
        const std::string relStr = rel.string();
        if (!ec && relStr.find("..") == std::string::npos)
            cd->path = relStr;
        else
            cd->path = np.string();
    }

    // Reload audio in-place for each fixed cue — no full rebuild needed.
    for (const auto& fix : fixes) {
        if (fix.newPath.empty()) continue;
        if (fix.listIdx < 0 || fix.listIdx >= m.listCount()) continue;
        auto& cl = m.cueListAt(fix.listIdx);
        for (int fi = 0; fi < cl.cueCount(); ++fi) {
            const auto* c = cl.cueAt(fi);
            if (c && c->cueNumber == fix.cueNumber) {
                reloadEngineCueAudio(m, fix.listIdx, fi);
                break;
            }
        }
    }
    m.markDirty();
}

int countAudioCues(const AppModel& m) {
    int n = 0;
    for (const auto& cl : m.sf.cueLists)
        walkAudioCuesConst(cl.cues, [&](const mcp::ShowFile::CueData&) { ++n; });
    return n;
}

bool collectAllFiles(AppModel& m,
                     const CollectOptions& opts,
                     std::function<void(const std::string& filename)> progress,
                     std::string& err)
{
    namespace fs = std::filesystem;

    const fs::path destDir(opts.destDir);
    const fs::path audioDir = destDir / "audio";

    try {
        fs::create_directories(audioDir);
    } catch (const std::exception& e) {
        err = std::string("Cannot create destination: ") + e.what();
        return false;
    }

    // Deep-copy the ShowFile so we can rewrite paths without touching the live model.
    mcp::ShowFile sf = m.sf;
    const fs::path baseDir(m.baseDir);

    // srcAbs → relative dest path (deduplication map)
    std::map<std::string, std::string> srcToRel;
    std::set<std::string>              usedNames;
    bool anyFailed = false;

    auto resolveAbs = [&](const std::string& raw) -> fs::path {
        fs::path p(raw);
        return p.is_relative() ? baseDir / p : p;
    };

    auto uniqueRelPath = [&](const fs::path& srcAbs) -> std::string {
        const std::string key = srcAbs.string();
        auto it = srcToRel.find(key);
        if (it != srcToRel.end()) return it->second;

        const std::string stem = srcAbs.stem().string();
        const std::string ext  = opts.convertToWav ? ".wav" : srcAbs.extension().string();
        std::string name = stem + ext;
        if (usedNames.count(name)) {
            int n = 2;
            while (usedNames.count(stem + "_" + std::to_string(n) + ext)) ++n;
            name = stem + "_" + std::to_string(n) + ext;
        }
        usedNames.insert(name);
        const std::string rel = "audio/" + name;
        srcToRel[key] = rel;
        return rel;
    };

    for (auto& cl : sf.cueLists) {
        walkAudioCues(cl.cues, [&](mcp::ShowFile::CueData& cd) {
            if (cd.path.empty()) return;

            const fs::path srcAbs  = resolveAbs(cd.path);
            const std::string rel  = uniqueRelPath(srcAbs);
            const fs::path dstFile = destDir / rel;

            progress(srcAbs.filename().string());

            // Only process each source file once.
            if (!fs::exists(dstFile)) {
                std::string fileErr;
                bool ok = false;
                if (opts.convertToWav) {
                    ok = transcodeToWav(srcAbs.string(), dstFile.string(),
                                        opts.sampleRate, opts.bitDepth, fileErr);
                } else {
                    try {
                        fs::copy_file(srcAbs, dstFile, fs::copy_options::overwrite_existing);
                        ok = true;
                    } catch (const std::exception& e) {
                        fileErr = e.what();
                    }
                }
                if (!ok) {
                    if (!err.empty()) err += '\n';
                    err += srcAbs.filename().string() + ": " + fileErr;
                    anyFailed = true;
                    return;  // leave cd.path unchanged on failure
                }
            }

            cd.path = rel;  // rewrite to relative
        });
    }

    // Determine output show filename.
    const std::string showName = m.showPath.empty()
        ? "show"
        : fs::path(m.showPath).stem().string();
    const std::string outShowPath = (destDir / (showName + ".mcp")).string();

    std::string saveErr;
    sf.save(outShowPath, saveErr);
    if (!saveErr.empty()) {
        if (!err.empty()) err += '\n';
        err += "Save show file: " + saveErr;
        return false;
    }

    return !anyFailed;
}

// ---------------------------------------------------------------------------
// applyChannelMap — update the channel map on every live CueList without
// clearing or restarting cues.  Safe to call during playback.

// ---------------------------------------------------------------------------
// Direct engine mutation helpers
// ---------------------------------------------------------------------------

// Build a MusicContext from a ShowFile MCData entry (shared between rebuild
// and the direct-insert path).
static std::unique_ptr<mcp::MusicContext>
buildMCFromData(const mcp::ShowFile::CueData::MCData& mc)
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
}

// Build the engine Cue struct from a ShowFile::CueData.
// Does NOT assign stableId (insertCueAt handles that).
// target: already-resolved, post-insertion-adjusted flat index (-1 if n/a).
// parentFlatIdx: direct parent group flat index (-1 = top-level).
static mcp::Cue buildEngineCue(
    const mcp::ShowFile::CueData& cd,
    const std::filesystem::path& base,
    int target,
    int parentFlatIdx)
{
    mcp::Cue cue;
    cue.name           = cd.name.empty() ? cd.type : cd.name;
    cue.preWaitSeconds = cd.preWait;
    cue.parentIndex    = parentFlatIdx;

    if (cd.type == "audio") {
        cue.type = mcp::CueType::Audio;
        cue.path = cd.path;
        auto p = std::filesystem::path(cd.path);
        if (p.is_relative()) p = base / p;
        cue.audioFile.loadMetadata(p.string());  // silent fail → broken cue
    } else if (cd.type == "start") {
        cue.type = mcp::CueType::Start;
        cue.targetIndex = target;
    } else if (cd.type == "stop") {
        cue.type = mcp::CueType::Stop;
        cue.targetIndex = target;
    } else if (cd.type == "fade") {
        cue.type        = mcp::CueType::Fade;
        cue.duration    = 3.0;
        cue.targetIndex = target;
        cue.fadeData    = std::make_shared<mcp::FadeData>();
        cue.fadeData->targetCueNumber   = cd.targetCueNumber;
        cue.fadeData->resolvedTargetIdx = target;
        cue.fadeData->curve = (cd.fadeCurve == "equalpower")
            ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
        cue.fadeData->stopWhenDone = cd.fadeStopWhenDone;
    } else if (cd.type == "arm") {
        cue.type = mcp::CueType::Arm;
        cue.targetIndex = target;
    } else if (cd.type == "devamp") {
        cue.type = mcp::CueType::Devamp;
        cue.targetIndex = target;
    } else if (cd.type == "mc") {
        cue.type = mcp::CueType::MusicContext;
    } else if (cd.type == "marker") {
        cue.type = mcp::CueType::Marker;
        cue.targetIndex = target;
        cue.markerIndex = cd.markerIndex;
    } else if (cd.type == "goto") {
        cue.type = mcp::CueType::Goto;
        cue.targetIndex = target;
    } else if (cd.type == "memo") {
        cue.type = mcp::CueType::Memo;
    } else if (cd.type == "scriptlet") {
        cue.type = mcp::CueType::Scriptlet;
    } else if (cd.type == "snapshot") {
        cue.type = mcp::CueType::Snapshot;
    } else if (cd.type == "automation") {
        cue.type = mcp::CueType::Automation;
    } else if (cd.type == "deactivate") {
        cue.type          = mcp::CueType::Deactivate;
        cue.pluginChannel = cd.pluginChannel;
        cue.pluginSlot    = cd.pluginSlot;
    } else if (cd.type == "reactivate") {
        cue.type          = mcp::CueType::Reactivate;
        cue.pluginChannel = cd.pluginChannel;
        cue.pluginSlot    = cd.pluginSlot;
    } else if (cd.type == "network") {
        cue.type = mcp::CueType::Network;
    } else if (cd.type == "midi") {
        cue.type = mcp::CueType::Midi;
    } else if (cd.type == "timecode") {
        cue.type = mcp::CueType::Timecode;
    }
    // "group" is handled by the caller (falls back to rebuildCueList).
    return cue;
}

// Apply all per-cue setters to the cue at flatIdx after insertCueAt.
// parentFlatIdx: -1 = top-level (parentIndex already set in struct by buildEngineCue).
static void applyCueSetters(
    mcp::CueList& cl,
    int flatIdx,
    AppModel& m,
    const mcp::ShowFile::CueData& cd)
{
    cl.setCueCueNumber   (flatIdx, cd.cueNumber);
    cl.setCueStartTime   (flatIdx, cd.startTime);
    cl.setCueDuration    (flatIdx, cd.duration);
    cl.setCueLevel       (flatIdx, cd.level);
    cl.setCueTrim        (flatIdx, cd.trim);
    cl.setCueAutoContinue(flatIdx, cd.autoContinue);
    cl.setCueAutoFollow  (flatIdx, cd.autoFollow);
    cl.setCueGoQuantize  (flatIdx, cd.goQuantize);
    cl.setCueTimelineOffset(flatIdx, cd.timelineOffset);

    if (cd.type == "arm")
        cl.setCueArmStartTime(flatIdx, cd.armStartTime);
    if (cd.type == "devamp") {
        cl.setCueDevampMode   (flatIdx, cd.devampMode);
        cl.setCueDevampPreVamp(flatIdx, cd.devampPreVamp);
    }
    if (cd.type == "marker")
        cl.setCueMarkerIndex(flatIdx, cd.markerIndex);
    if (cd.type == "scriptlet")
        cl.setCueScriptletCode(flatIdx, cd.scriptletCode);
    if (cd.type == "snapshot")
        cl.setCueSnapshotId(flatIdx, cd.snapshotId);
    if (cd.type == "automation") {
        cl.setCueAutomationPath    (flatIdx, cd.automationPath);
        cl.setCueAutomationDuration(flatIdx, cd.automationDuration);
        std::vector<mcp::Cue::AutomationPoint> pts;
        pts.reserve(cd.automationCurve.size());
        for (const auto& p : cd.automationCurve)
            pts.push_back({p.time, p.value, p.isHandle});
        cl.setCueAutomationCurve(flatIdx, pts);
    }
    if (cd.type == "deactivate" || cd.type == "reactivate")
        cl.setCuePluginSlot(flatIdx, cd.pluginChannel, cd.pluginSlot);
    if (cd.type == "network") {
        int patchIdx = -1;
        for (int pi = 0; pi < (int)m.sf.networkSetup.patches.size(); ++pi) {
            if (m.sf.networkSetup.patches[static_cast<size_t>(pi)].name == cd.networkPatchName) {
                patchIdx = pi; break;
            }
        }
        cl.setCueNetworkPatch  (flatIdx, patchIdx);
        cl.setCueNetworkCommand(flatIdx, cd.networkCommand);
    }
    if (cd.type == "midi") {
        int patchIdx = -1;
        for (int pi = 0; pi < (int)m.sf.midiSetup.patches.size(); ++pi) {
            if (m.sf.midiSetup.patches[static_cast<size_t>(pi)].name == cd.midiPatchName) {
                patchIdx = pi; break;
            }
        }
        cl.setCueMidiPatch  (flatIdx, patchIdx);
        cl.setCueMidiMessage(flatIdx, cd.midiMessageType,
                             cd.midiChannel, cd.midiData1, cd.midiData2);
    }
    if (cd.type == "timecode") {
        mcp::TcFps fps = mcp::TcFps::Fps25;
        mcp::tcFpsFromString(cd.tcFps, fps);
        mcp::TcPoint startTC, endTC;
        mcp::tcFromString(cd.tcStartTC, startTC);
        mcp::tcFromString(cd.tcEndTC,   endTC);
        cl.setCueTcFps        (flatIdx, fps);
        cl.setCueTcStart      (flatIdx, startTC);
        cl.setCueTcEnd        (flatIdx, endTC);
        cl.setCueTcType       (flatIdx, cd.tcType);
        cl.setCueTcLtcChannel (flatIdx, cd.tcLtcChannel);
        int midiPatchIdx = -1;
        if (!cd.tcMidiPatchName.empty()) {
            for (int pi = 0; pi < (int)m.sf.midiSetup.patches.size(); ++pi) {
                if (m.sf.midiSetup.patches[static_cast<size_t>(pi)].name == cd.tcMidiPatchName) {
                    midiPatchIdx = pi; break;
                }
            }
        }
        cl.setCueTcMidiPatch(flatIdx, midiPatchIdx);
    }
    if (cd.type == "audio") {
        std::vector<mcp::Cue::TimeMarker> marks;
        for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
        cl.setCueMarkers   (flatIdx, marks);
        cl.setCueSliceLoops(flatIdx, cd.sliceLoops);
        for (int o = 0; o < (int)cd.outLevelDb.size(); ++o)
            cl.setCueOutLevel(flatIdx, o, cd.outLevelDb[o]);
        for (const auto& xe : cd.xpEntries)
            cl.setCueXpoint(flatIdx, xe.s, xe.o, xe.db);
    }
    if (cd.type == "fade") {
        cl.setCueFadeMasterTarget  (flatIdx, cd.fadeMasterEnabled, cd.fadeMasterTarget);
        cl.setCueFadeOutTargetCount(flatIdx, static_cast<int>(cd.fadeOutLevels.size()));
        for (const auto& fl : cd.fadeOutLevels)
            cl.setCueFadeOutTarget(flatIdx, fl.ch, fl.enabled, fl.target);
        for (const auto& fx : cd.fadeXpEntries)
            cl.setCueFadeXpTarget(flatIdx, fx.s, fx.o, fx.enabled, fx.target);
    }
    cl.setCueMusicContext(flatIdx, buildMCFromData(cd.musicContext));
}

int insertEngineCue(AppModel& m, int listIdx, int flatIdx,
                    mcp::ShowFile::CueData cd, std::string& err)
{
    if (listIdx < 0 || listIdx >= static_cast<int>(m.sf.cueLists.size())) return -1;

    auto& cl      = m.cueListAt(listIdx);
    const int n   = cl.cueCount();
    if (flatIdx < 0 || flatIdx > n) flatIdx = n;   // negative = append

    const int thisListId =
        m.sf.cueLists[static_cast<size_t>(listIdx)].numericId;

    // Groups need recursive child wiring — fall back to single-list rebuild.
    if (cd.type == "group") {
        sfInsertBefore(m.sf, listIdx, flatIdx, std::move(cd));
        return rebuildCueList(m, listIdx, err) ? flatIdx : -1;
    }

    const auto base = std::filesystem::path(m.baseDir);

    // Build cueNumber → flat engine index map (pre-insertion).
    std::vector<std::pair<std::string, int>> numToIdx;
    numToIdx.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto* c = cl.cueAt(i);
        if (c && !c->cueNumber.empty())
            numToIdx.push_back({c->cueNumber, i});
    }

    // Resolve target index.  After the insert, cues at flatIdx+ shift by +1.
    auto resolveTarget = [&]() -> int {
        const bool sameList = (cd.targetListId == -1 || cd.targetListId == thisListId);
        if (!sameList) return -1;
        int t = -1;
        if (!cd.targetCueNumber.empty()) {
            for (const auto& p : numToIdx)
                if (p.first == cd.targetCueNumber) { t = p.second; break; }
        }
        if (t < 0) t = cd.target;
        if (t >= 0 && t >= flatIdx) ++t;   // adjust for pending insertion
        return t;
    };
    const int target = resolveTarget();

    // Determine direct parent group (innermost ancestor of flatIdx).
    // A group g is an ancestor when  g < flatIdx <= g + childCount.
    // The innermost is the one with the highest g.
    int parentFlatIdx = -1;
    for (int g = 0; g < n; ++g) {
        const auto* gc = cl.cueAt(g);
        if (!gc || !gc->groupData) continue;
        if (g < flatIdx && flatIdx <= g + gc->childCount)
            parentFlatIdx = g;
    }

    // Build and insert into engine.
    mcp::Cue cue = buildEngineCue(cd, base, target, parentFlatIdx);
    cl.insertCueAt(flatIdx, std::move(cue));

    // Apply setters (cueNumber, routing, timecode, fade targets, etc.).
    applyCueSetters(cl, flatIdx, m, cd);

    // Resolve mcSourceNumber (single-cue pass).
    if (!cd.mcSourceNumber.empty()) {
        for (const auto& p : numToIdx) {
            if (p.first == cd.mcSourceNumber) {
                const int srcIdx = (p.second >= flatIdx) ? p.second + 1 : p.second;
                cl.setCueMCSource(flatIdx, srcIdx);
                break;
            }
        }
    }

    // Resolve anchorMarkerCueNumber (audio cues).
    if (cd.type == "audio") {
        for (int mi = 0; mi < static_cast<int>(cd.markers.size()); ++mi) {
            const auto& mkNum =
                cd.markers[static_cast<size_t>(mi)].anchorMarkerCueNumber;
            if (mkNum.empty()) continue;
            for (int ci = 0; ci < cl.cueCount(); ++ci) {
                const auto* cc = cl.cueAt(ci);
                if (cc && cc->cueNumber == mkNum) {
                    cl.setMarkerAnchor(flatIdx, mi, ci);
                    break;
                }
            }
        }
    }

    // Mirror the insertion in the SF tree.
    sfInsertBefore(m.sf, listIdx, flatIdx, std::move(cd));

    return flatIdx;
}

void removeEngineCue(AppModel& m, int listIdx, int flatIdx)
{
    if (listIdx < 0 || listIdx >= static_cast<int>(m.sf.cueLists.size())) return;
    auto& cl = m.cueListAt(listIdx);
    if (flatIdx < 0 || flatIdx >= cl.cueCount()) return;

    // Engine removal: stops voices, fixes all index refs.
    cl.removeCueAt(flatIdx);

    // Mirror in SF.
    sfRemoveAt(m.sf, listIdx, flatIdx);
    sfFixTargetsAfterRemoval(m.sf, listIdx, flatIdx);
}

bool reloadEngineCueAudio(AppModel& m, int listIdx, int flatIdx)
{
    if (listIdx < 0 || listIdx >= static_cast<int>(m.sf.cueLists.size())) return false;
    const auto* sfCue = sfCueAt(m.sf, listIdx, flatIdx);
    if (!sfCue) return false;

    // Build absolute path.
    const auto base = std::filesystem::path(m.baseDir);
    auto p = std::filesystem::path(sfCue->path);
    if (p.is_relative()) p = base / p;

    return m.cueListAt(listIdx).reloadCueAudio(flatIdx, p.string(), m.baseDir);
}

// ---------------------------------------------------------------------------
// Multi-block engine move (no rebuild for pure up / pure down selections)

void moveEngineCues(AppModel& m, int listIdx,
                    const std::vector<int>& topLevel,
                    const std::vector<int>& blockSizes,
                    int dstRow, int adjustedDst)
{
    const int n = static_cast<int>(topLevel.size());
    if (n == 0) return;

    // Classify: are all blocks strictly below dstRow (moving down), or all at/above?
    bool anyBelow = false, anyAtOrAbove = false;
    for (int i = 0; i < n; ++i) {
        if (topLevel[i] < dstRow) anyBelow = true;
        else anyAtOrAbove = true;
    }
    if (anyBelow && anyAtOrAbove) {
        // Mixed selection — rare edge case; fall back to rebuild.
        std::string err;
        rebuildAllCueLists(m, err);
        return;
    }

    auto& cl = m.cueListAt(listIdx);

    if (anyBelow) {
        // Pure DOWN: process from highest to lowest original index.
        // to_i = adjustedDst + sum(blockSizes[0..i]) (gives the exclusive-end destination
        // in the original index space, which is what moveCueTo expects for to > from).
        for (int i = n - 1; i >= 0; --i) {
            int cumSum = 0;
            for (int j = 0; j <= i; ++j) cumSum += blockSizes[j];
            cl.moveCueTo(topLevel[i], adjustedDst + cumSum);
        }
    } else {
        // Pure UP: process from lowest to highest original index.
        // to_i = adjustedDst + sum(blockSizes[0..i-1]) (the start position, to < from).
        for (int i = 0; i < n; ++i) {
            int cumSum = 0;
            for (int j = 0; j < i; ++j) cumSum += blockSizes[j];
            cl.moveCueTo(topLevel[i], adjustedDst + cumSum);
        }
    }
}

// ---------------------------------------------------------------------------
// Undo / redo helpers

bool isStructuralChange(
    const std::vector<mcp::ShowFile::CueListData>& a,
    const std::vector<mcp::ShowFile::CueListData>& b)
{
    if (a.size() != b.size()) return true;

    std::function<void(const std::vector<mcp::ShowFile::CueData>&,
                       std::vector<std::string>&)> flatten;
    flatten = [&](const std::vector<mcp::ShowFile::CueData>& cues,
                  std::vector<std::string>& out) {
        for (const auto& cd : cues) {
            out.push_back(cd.type);
            if (!cd.children.empty()) flatten(cd.children, out);
        }
    };

    for (size_t i = 0; i < a.size(); ++i) {
        std::vector<std::string> ta, tb;
        flatten(a[i].cues, ta);
        flatten(b[i].cues, tb);
        if (ta.size() != tb.size()) return true;
        for (size_t j = 0; j < ta.size(); ++j)
            if (ta[j] != tb[j]) return true;
    }
    return false;
}

void reapplyParamsFromSF(AppModel& m)
{
    for (int li = 0; li < m.listCount(); ++li) {
        auto& cl = m.cueListAt(li);
        const int n = cl.cueCount();
        const int thisListId = m.sf.cueLists[static_cast<size_t>(li)].numericId;

        // Build cueNumber → flatIdx map from engine.
        std::vector<std::pair<std::string, int>> numToIdx;
        numToIdx.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto* c = cl.cueAt(i);
            if (c && !c->cueNumber.empty())
                numToIdx.push_back({c->cueNumber, i});
        }

        for (int fi = 0; fi < n; ++fi) {
            const auto* cdp = sfCueAt(m.sf, li, fi);
            if (!cdp) continue;
            const auto& cd = *cdp;

            // Resolve target index.
            const bool sameList = (cd.targetListId == -1 || cd.targetListId == thisListId);
            int t = -1;
            if (sameList) {
                if (!cd.targetCueNumber.empty()) {
                    for (const auto& p : numToIdx)
                        if (p.first == cd.targetCueNumber) { t = p.second; break; }
                }
                if (t < 0) t = cd.target;
            }
            cl.setCueTarget(fi, t);

            // Re-apply all parameter setters.
            applyCueSetters(cl, fi, m, cd);

            // Resolve mcSourceNumber.
            if (!cd.mcSourceNumber.empty()) {
                for (const auto& p : numToIdx) {
                    if (p.first == cd.mcSourceNumber) {
                        cl.setCueMCSource(fi, p.second);
                        break;
                    }
                }
            }

            // Resolve anchorMarkerCueNumber for audio cues.
            if (cd.type == "audio") {
                for (int mi = 0; mi < static_cast<int>(cd.markers.size()); ++mi) {
                    const auto& mkNum =
                        cd.markers[static_cast<size_t>(mi)].anchorMarkerCueNumber;
                    if (mkNum.empty()) continue;
                    for (int ci = 0; ci < cl.cueCount(); ++ci) {
                        const auto* cc = cl.cueAt(ci);
                        if (cc && cc->cueNumber == mkNum) {
                            cl.setMarkerAnchor(fi, mi, ci);
                            break;
                        }
                    }
                }
            }
        }
    }
}

void applyChannelMap(AppModel& m) {
    const auto& setup = m.sf.audioSetup;
    const bool isMultiDevice = !setup.devices.empty();

    int numPhys = 0;
    if (isMultiDevice) {
        for (const auto& d : setup.devices) numPhys += d.channelCount;
    } else {
        numPhys = m.engineOk ? m.engine.channels() : 2;
    }

    // Build the ChannelMap exactly as rebuildCueList does but without clear().
    mcp::ChannelMap cm;
    cm.numPhys = numPhys;
    cm.numCh   = static_cast<int>(setup.channels.size());

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
        cm.numCh = numPhys;
        cm.fold.assign(static_cast<size_t>(numPhys * numPhys), 0.0f);
        for (int i = 0; i < numPhys; ++i)
            cm.fold[static_cast<size_t>(i * numPhys + i)] = 1.0f;
        cm.xpLinear = cm.fold;
        cm.liveMasterGainDb.assign(static_cast<size_t>(numPhys), 0.0f);
        cm.liveMute.assign(static_cast<size_t>(numPhys), false);
        cm.stereoSlave.assign(static_cast<size_t>(numPhys), -1);
        cm.primaryPhys.resize(static_cast<size_t>(numPhys));
        for (int i = 0; i < numPhys; ++i) cm.primaryPhys[static_cast<size_t>(i)] = i;
    } else {
        cm.fold.assign(static_cast<size_t>(cm.numCh * numPhys), 0.0f);
        for (int ch = 0; ch < cm.numCh && ch < numPhys; ++ch)
            cm.fold[static_cast<size_t>(ch * numPhys + ch)] = 1.0f;
        for (const auto& xe : setup.xpEntries) {
            if (xe.ch >= 0 && xe.ch < cm.numCh && xe.out >= 0 && xe.out < numPhys)
                cm.fold[static_cast<size_t>(xe.ch * numPhys + xe.out)] =
                    (xe.db <= -144.0f) ? 0.0f : mcp::lut::dBToLinear(xe.db);
        }
        cm.xpLinear = cm.fold;
        cm.liveMasterGainDb.resize(static_cast<size_t>(cm.numCh), 0.0f);
        cm.liveMute.resize(static_cast<size_t>(cm.numCh), false);
        cm.stereoSlave.assign(static_cast<size_t>(cm.numCh), -1);
        for (int ch = 0; ch < cm.numCh; ++ch) {
            const auto& cfg = setup.channels[static_cast<size_t>(ch)];
            cm.liveMasterGainDb[static_cast<size_t>(ch)] = cfg.masterGainDb;
            cm.liveMute[static_cast<size_t>(ch)] = cfg.mute;
            if (cfg.linkedStereo && ch + 1 < cm.numCh)
                cm.stereoSlave[static_cast<size_t>(ch)] = ch + 1;
            float g = cfg.mute ? 0.0f : mcp::lut::dBToLinear(cfg.masterGainDb);
            for (int p = 0; p < numPhys; ++p)
                cm.fold[static_cast<size_t>(ch * numPhys + p)] *= g;
        }
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

    // Push the same map to every active list.
    for (int li = 0; li < static_cast<int>(m.sf.cueLists.size()); ++li)
        m.cueListAt(li).setChannelMap(cm);   // copy — each list gets its own
}

} // namespace ShowHelpers
