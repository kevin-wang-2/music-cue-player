#include "ShowHelpers.h"
#include "AppModel.h"

#include "engine/FadeData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ShowHelpers {

std::string nextCueNumber(const mcp::ShowFile& sf) {
    const auto& cues = sf.cueLists.empty()
        ? std::vector<mcp::ShowFile::CueData>{}
        : sf.cueLists[0].cues;
    int n = 1;
    while (true) {
        std::string candidate = std::to_string(n);
        bool taken = false;
        for (const auto& cd : cues)
            if (cd.cueNumber == candidate) { taken = true; break; }
        if (!taken) return candidate;
        ++n;
    }
}

int findCueByNumber(const mcp::ShowFile& sf, const std::string& num) {
    if (num.empty() || sf.cueLists.empty()) return -1;
    for (int i = 0; i < (int)sf.cueLists[0].cues.size(); ++i)
        if (sf.cueLists[0].cues[i].cueNumber == num) return i;
    return -1;
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
    auto& cds  = m.sf.cueLists[0].cues;
    const auto base = std::filesystem::path(m.baseDir);

    // Pass 1: resolve targetCueNumber → index only when cd.target is unset.
    // If cd.target >= 0 it was written by the runtime (e.g. drag-to-set-target)
    // and is authoritative; don't overwrite it with a potentially stale number lookup.
    for (auto& cd : cds) {
        if ((cd.type == "start" || cd.type == "stop" || cd.type == "fade"
             || cd.type == "arm" || cd.type == "devamp")
            && cd.target < 0 && !cd.targetCueNumber.empty()) {
            int idx = findCueByNumber(m.sf, cd.targetCueNumber);
            if (idx >= 0) cd.target = idx;
        }
    }

    // Pass 2: build CueList
    for (int i = 0; i < (int)cds.size(); ++i) {
        const auto& cd = cds[i];
        bool ok = true;
        if (cd.type == "audio") {
            auto p = std::filesystem::path(cd.path);
            if (p.is_relative()) p = base / p;
            ok = m.cues.addCue(p.string(), cd.name, cd.preWait);
            if (!ok) { m.cues.addBrokenAudioCue(p.string(), cd.name, cd.preWait); ok = true; }
        } else if (cd.type == "start")  { m.cues.addStartCue (cd.target, cd.name, cd.preWait); }
        else if (cd.type == "stop")     { m.cues.addStopCue  (cd.target, cd.name, cd.preWait); }
        else if (cd.type == "fade") {
            const auto curve = (cd.fadeCurve == "equalpower")
                ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear;
            m.cues.addFadeCue(cd.target, cd.targetCueNumber, curve,
                              cd.fadeStopWhenDone, cd.name, cd.preWait);
        } else if (cd.type == "arm")    { m.cues.addArmCue   (cd.target, cd.name, cd.preWait); }
        else if (cd.type == "devamp")   { m.cues.addDevampCue(cd.target, cd.name, cd.preWait, cd.devampMode); }

        if (ok) {
            m.cues.setCueCueNumber  (i, cd.cueNumber);
            m.cues.setCueStartTime  (i, cd.startTime);
            m.cues.setCueDuration   (i, cd.duration);
            m.cues.setCueLevel      (i, cd.level);
            m.cues.setCueTrim       (i, cd.trim);
            m.cues.setCueAutoContinue(i, cd.autoContinue);
            m.cues.setCueAutoFollow  (i, cd.autoFollow);
            if (cd.type == "arm")    m.cues.setCueArmStartTime(i, cd.armStartTime);
            if (cd.type == "devamp") {
                m.cues.setCueDevampMode   (i, cd.devampMode);
                m.cues.setCueDevampPreVamp(i, cd.devampPreVamp);
            }
            if (cd.type == "audio") {
                std::vector<mcp::Cue::TimeMarker> marks;
                for (const auto& mk : cd.markers) marks.push_back({mk.time, mk.name});
                m.cues.setCueMarkers  (i, marks);
                m.cues.setCueSliceLoops(i, cd.sliceLoops);
                for (int o = 0; o < (int)cd.outLevelDb.size(); ++o)
                    m.cues.setCueOutLevel(i, o, cd.outLevelDb[o]);
                for (const auto& xe : cd.xpEntries)
                    m.cues.setCueXpoint(i, xe.s, xe.o, xe.db);
            }
            if (cd.type == "fade") {
                m.cues.setCueFadeMasterTarget  (i, cd.fadeMasterEnabled, cd.fadeMasterTarget);
                m.cues.setCueFadeOutTargetCount(i, (int)cd.fadeOutLevels.size());
                for (const auto& fl : cd.fadeOutLevels)
                    m.cues.setCueFadeOutTarget(i, fl.ch, fl.enabled, fl.target);
                for (const auto& fx : cd.fadeXpEntries)
                    m.cues.setCueFadeXpTarget(i, fx.s, fx.o, fx.enabled, fx.target);
            }
        }
    }
    return true;
}

void syncSfFromCues(AppModel& m) {
    if (m.sf.cueLists.empty()) return;
    auto& cds = m.sf.cueLists[0].cues;
    for (int i = 0; i < m.cues.cueCount() && i < (int)cds.size(); ++i) {
        const auto* c = m.cues.cueAt(i);
        if (!c) continue;
        cds[i].cueNumber    = c->cueNumber;
        cds[i].name         = c->name;
        cds[i].preWait      = c->preWaitSeconds;
        cds[i].startTime    = c->startTime;
        cds[i].duration     = c->duration;
        cds[i].level        = c->level;
        cds[i].trim         = c->trim;
        cds[i].autoContinue = c->autoContinue;
        cds[i].autoFollow   = c->autoFollow;
        if (c->type == mcp::CueType::Arm)
            cds[i].armStartTime = c->armStartTime;
        // Sync target index for control cue types (index only; no number derivation)
        if (c->type == mcp::CueType::Start || c->type == mcp::CueType::Stop ||
            c->type == mcp::CueType::Arm)
            cds[i].target = c->targetIndex;
        if (c->type == mcp::CueType::Devamp) {
            cds[i].target        = c->targetIndex;
            cds[i].devampMode    = c->devampMode;
            cds[i].devampPreVamp = c->devampPreVamp;
        }
        if (c->type == mcp::CueType::Audio) {
            cds[i].markers.clear();
            for (const auto& mk : c->markers)
                cds[i].markers.push_back({mk.time, mk.name});
            cds[i].sliceLoops = c->sliceLoops;
            cds[i].outLevelDb = c->routing.outLevelDb;
            cds[i].xpEntries.clear();
            for (int s = 0; s < (int)c->routing.xpoint.size(); ++s)
                for (int o = 0; o < (int)c->routing.xpoint[s].size(); ++o)
                    if (c->routing.xpoint[s][o].has_value())
                        cds[i].xpEntries.push_back({s, o, *c->routing.xpoint[s][o]});
        }
        if (c->type == mcp::CueType::Fade && c->fadeData) {
            const auto& fd = *c->fadeData;
            cds[i].targetCueNumber   = fd.targetCueNumber;
            cds[i].target            = fd.resolvedTargetIdx;
            cds[i].fadeStopWhenDone  = fd.stopWhenDone;
            cds[i].fadeCurve         = (fd.curve == mcp::FadeData::Curve::EqualPower)
                                       ? "equalpower" : "linear";
            cds[i].fadeMasterEnabled = fd.masterLevel.enabled;
            cds[i].fadeMasterTarget  = fd.masterLevel.targetDb;
            cds[i].fadeOutLevels.clear();
            for (int o = 0; o < (int)fd.outLevels.size(); ++o)
                cds[i].fadeOutLevels.push_back({o, fd.outLevels[o].enabled,
                                                fd.outLevels[o].targetDb});
            cds[i].fadeXpEntries.clear();
            for (int s = 0; s < (int)fd.xpTargets.size(); ++s)
                for (int o = 0; o < (int)fd.xpTargets[s].size(); ++o)
                    cds[i].fadeXpEntries.push_back(
                        {s, o,
                         fd.xpTargets[s][o].enabled,
                         fd.xpTargets[s][o].targetDb});
        }
    }
    m.dirty = true;
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
