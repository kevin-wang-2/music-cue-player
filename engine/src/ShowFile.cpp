#include "engine/ShowFile.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── base64 helpers (for external plugin state blobs) ─────────────────────────
namespace {

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string encodeBase64(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t b = (static_cast<uint32_t>(data[i]) << 16) |
                           ((i + 1 < data.size()) ? static_cast<uint32_t>(data[i + 1]) << 8 : 0u) |
                           ((i + 2 < data.size()) ? static_cast<uint32_t>(data[i + 2])      : 0u);
        out += kB64[(b >> 18) & 63];
        out += kB64[(b >> 12) & 63];
        out += (i + 1 < data.size()) ? kB64[(b >> 6) & 63] : '=';
        out += (i + 2 < data.size()) ? kB64[ b       & 63] : '=';
    }
    return out;
}

static std::vector<uint8_t> decodeBase64(const std::string& s) {
    static const int8_t kTbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4 + 1);
    uint32_t buf  = 0;
    int      bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        const int8_t v = kTbl[c];
        if (v < 0) continue;
        buf   = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

namespace mcp {

void ShowFile::AudioSetup::normalizeMaster() {
    bool found = false;
    for (auto& d : devices) {
        if (d.masterClock) {
            if (found) d.masterClock = false;
            else       found = true;
        }
    }
    if (!found && !devices.empty())
        devices[0].masterClock = true;
}

int ShowFile::AudioSetup::masterDeviceIndex() const {
    for (int i = 0; i < (int)devices.size(); ++i)
        if (devices[i].masterClock) return i;
    return 0;
}

template<typename T>
static T jget(const json& j, const char* key, T def) {
    if (!j.contains(key)) return def;
    try { return j[key].get<T>(); } catch (...) { return def; }
}

static ShowFile::CueData parseCue(const json& j) {
    ShowFile::CueData c;
    c.type            = jget<std::string>(j, "type",            "");
    c.cueNumber       = jget<std::string>(j, "cueNumber",       "");
    c.name            = jget<std::string>(j, "name",            "");
    c.path            = jget<std::string>(j, "path",            "");
    c.target          = jget<int>        (j, "target",          -1);
    c.targetCueNumber = jget<std::string>(j, "targetCueNumber", "");
    c.targetListId    = jget<int>        (j, "targetListId",    -1);
    c.armStartTime    = jget<double>     (j, "armStartTime",    0.0);
    c.preWait         = jget<double>     (j, "preWait",         0.0);
    c.goQuantize      = jget<int>        (j, "goQuantize",      0);
    c.startTime       = jget<double>     (j, "startTime",       0.0);
    c.duration        = jget<double>     (j, "duration",        0.0);
    c.level           = jget<double>     (j, "level",           0.0);
    c.trim            = jget<double>     (j, "trim",            0.0);
    c.autoContinue    = jget<bool>       (j, "autoContinue",    false);
    c.autoFollow      = jget<bool>       (j, "autoFollow",      false);

    // Audio routing: per-output levels
    if (j.contains("outLevels") && j["outLevels"].is_array()) {
        for (const auto& v : j["outLevels"])
            c.outLevelDb.push_back(static_cast<float>(v.get<double>()));
    }
    // Audio routing: crosspoint entries
    if (j.contains("xpoint") && j["xpoint"].is_array()) {
        for (const auto& e : j["xpoint"]) {
            ShowFile::CueData::XpEntry xe;
            xe.s  = jget<int>   (e, "s",  0);
            xe.o  = jget<int>   (e, "o",  0);
            xe.db = jget<float> (e, "db", 0.0f);
            c.xpEntries.push_back(xe);
        }
    }

    // Time markers
    if (j.contains("markers") && j["markers"].is_array()) {
        for (const auto& m : j["markers"]) {
            ShowFile::CueData::TimeMarker tm;
            tm.time                 = jget<double>     (m, "time", 0.0);
            tm.name                 = jget<std::string>(m, "name", "");
            tm.anchorMarkerCueNumber = jget<std::string>(m, "anchorMarkerCueNumber", "");
            tm.anchorMarkerListId    = jget<int>        (m, "anchorMarkerListId",    -1);
            c.markers.push_back(tm);
        }
    }
    // Marker cue: which marker within target
    c.markerIndex = jget<int>(j, "markerIndex", -1);
    if (j.contains("sliceLoops") && j["sliceLoops"].is_array()) {
        for (const auto& v : j["sliceLoops"])
            c.sliceLoops.push_back(v.get<int>());
    }
    // Ensure sliceLoops has the right size (markers.size()+1).
    {
        const int want = (int)c.markers.size() + 1;
        while ((int)c.sliceLoops.size() < want) c.sliceLoops.push_back(1);
        c.sliceLoops.resize(static_cast<size_t>(want));
    }

    // Group cue parameters
    c.groupMode      = jget<std::string>(j, "groupMode",      "timeline");
    c.groupRandom    = jget<bool>       (j, "groupRandom",    false);
    c.timelineOffset = jget<double>     (j, "timelineOffset", 0.0);
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& ch : j["children"])
            c.children.push_back(parseCue(ch));
    }

    // MC inheritance link
    c.mcSourceNumber = jget<std::string>(j, "mcSourceNumber", "");

    // Music Context
    if (j.contains("musicContext") && j["musicContext"].is_object()) {
        const auto& mc = j["musicContext"];
        c.musicContext.enabled            = jget<bool>  (mc, "enabled",            false);
        c.musicContext.startOffsetSeconds = jget<double>(mc, "startOffsetSeconds", 0.0);
        c.musicContext.applyBeforeStart   = jget<bool>  (mc, "applyBeforeStart",   true);
        if (mc.contains("points") && mc["points"].is_array()) {
            for (const auto& p : mc["points"]) {
                ShowFile::CueData::MCPoint pt;
                pt.bar        = jget<int>   (p, "bar",        1);
                pt.beat       = jget<int>   (p, "beat",       1);
                pt.bpm        = jget<double>(p, "bpm",        120.0);
                pt.isRamp     = jget<bool>  (p, "isRamp",     false);
                pt.hasTimeSig = jget<bool>  (p, "hasTimeSig", true);
                pt.timeSigNum = jget<int>   (p, "timeSigNum", 4);
                pt.timeSigDen = jget<int>   (p, "timeSigDen", 4);
                c.musicContext.points.push_back(pt);
            }
        }
    }

    // Scriptlet cue
    c.scriptletCode = jget<std::string>(j, "scriptletCode", "");

    // Snapshot cue
    c.snapshotId = jget<int>(j, "snapshotId", -1);

    // Deactivate / Reactivate cues
    c.pluginChannel = jget<int>(j, "pluginChannel", -1);
    c.pluginSlot    = jget<int>(j, "pluginSlot",    -1);

    // Automation cue
    c.automationPath     = jget<std::string>(j, "automationPath", "");
    c.automationDuration = jget<double>     (j, "automationDuration", 5.0);
    if (j.contains("automationCurve") && j["automationCurve"].is_array()) {
        for (const auto& pt : j["automationCurve"]) {
            ShowFile::CueData::AutomationPoint p;
            p.time     = jget<double>(pt, "time",     0.0);
            p.value    = jget<double>(pt, "value",    0.0);
            p.isHandle = jget<bool>  (pt, "isHandle", false);
            c.automationCurve.push_back(p);
        }
    }

    // Network cue parameters
    c.networkPatchName = jget<std::string>(j, "networkPatchName", "");
    c.networkCommand   = jget<std::string>(j, "networkCommand",   "");

    // MIDI cue parameters
    c.midiPatchName   = jget<std::string>(j, "midiPatchName",   "");
    c.midiMessageType = jget<std::string>(j, "midiMessageType", "note_on");
    c.midiChannel     = jget<int>        (j, "midiChannel",     1);
    c.midiData1       = jget<int>        (j, "midiData1",       60);
    c.midiData2       = jget<int>        (j, "midiData2",       64);

    // Timecode cue parameters
    c.tcType         = jget<std::string>(j, "tcType",         "ltc");
    c.tcFps          = jget<std::string>(j, "tcFps",          "25fps");
    c.tcStartTC      = jget<std::string>(j, "tcStartTC",      "00:00:00:00");
    c.tcEndTC        = jget<std::string>(j, "tcEndTC",        "00:00:00:00");
    c.tcLtcChannel   = jget<int>        (j, "tcLtcChannel",   0);
    c.tcMidiPatchName= jget<std::string>(j, "tcMidiPatchName","");

    // Trigger data
    if (j.contains("triggers") && j["triggers"].is_object()) {
        const auto& tr = j["triggers"];
        if (tr.contains("hotkey") && tr["hotkey"].is_object()) {
            const auto& hk = tr["hotkey"];
            c.triggers.hotkey.enabled   = jget<bool>       (hk, "enabled",   false);
            c.triggers.hotkey.keyString = jget<std::string>(hk, "keyString", "");
        }
        if (tr.contains("midi") && tr["midi"].is_object()) {
            const auto& m = tr["midi"];
            c.triggers.midi.enabled = jget<bool>(m, "enabled", false);
            MidiMsgType mt; const std::string mts = jget<std::string>(m, "type", "note_on");
            midiMsgTypeFromString(mts, mt); c.triggers.midi.type = mt;
            c.triggers.midi.channel = jget<int>(m, "channel", 0);
            c.triggers.midi.data1   = jget<int>(m, "data1",   60);
            c.triggers.midi.data2   = jget<int>(m, "data2",   -1);
        }
        if (tr.contains("osc") && tr["osc"].is_object()) {
            const auto& o = tr["osc"];
            c.triggers.osc.enabled = jget<bool>       (o, "enabled", false);
            c.triggers.osc.path    = jget<std::string>(o, "path",    "");
        }
    }

    // Devamp cue parameters
    c.devampMode    = jget<int> (j, "devampMode",    0);
    c.devampPreVamp = jget<bool>(j, "devampPreVamp", false);

    // Fade cue parameters
    c.fadeCurve        = jget<std::string>(j, "fadeCurve",        "linear");
    c.fadeStopWhenDone = jget<bool>       (j, "fadeStopWhenDone", false);

    // New multi-target format
    c.fadeMasterEnabled = jget<bool> (j, "fadeMasterEnabled", false);
    c.fadeMasterTarget  = jget<float>(j, "fadeMasterTarget",  0.0f);

    // Backward compat: old single-parameter "level" fade
    if (!c.fadeMasterEnabled) {
        const std::string param = jget<std::string>(j, "fadeParameter", "");
        if (param == "level") {
            c.fadeMasterEnabled = true;
            c.fadeMasterTarget  = static_cast<float>(jget<double>(j, "fadeTargetValue", 0.0));
        }
    }

    if (j.contains("fadeOutLevels") && j["fadeOutLevels"].is_array()) {
        for (const auto& e : j["fadeOutLevels"]) {
            ShowFile::CueData::FadeOutLevel fl;
            fl.ch      = jget<int>  (e, "ch",      0);
            fl.enabled = jget<bool> (e, "enabled", false);
            fl.target  = jget<float>(e, "target",  0.0f);
            c.fadeOutLevels.push_back(fl);
        }
    }

    if (j.contains("fadeXpEntries") && j["fadeXpEntries"].is_array()) {
        for (const auto& e : j["fadeXpEntries"]) {
            ShowFile::CueData::FadeXpEntry fx;
            fx.s       = jget<int>  (e, "s",       0);
            fx.o       = jget<int>  (e, "o",       0);
            fx.enabled = jget<bool> (e, "enabled", false);
            fx.target  = jget<float>(e, "target",  0.0f);
            c.fadeXpEntries.push_back(fx);
        }
    }

    return c;
}

static json cueToJson(const ShowFile::CueData& c) {
    json j;
    j["type"]      = c.type;
    j["cueNumber"] = c.cueNumber;
    j["name"]      = c.name;
    j["preWait"]   = c.preWait;
    if (c.goQuantize != 0) j["goQuantize"] = c.goQuantize;

    if ((c.type == "audio" || (c.type == "group" && c.groupMode == "sync"))
        && !c.markers.empty()) {
        json arr = json::array();
        for (const auto& m : c.markers) {
            json mj; mj["time"] = m.time;
            if (!m.name.empty())                   mj["name"]                 = m.name;
            if (!m.anchorMarkerCueNumber.empty())  mj["anchorMarkerCueNumber"] = m.anchorMarkerCueNumber;
            if (m.anchorMarkerListId >= 0)         mj["anchorMarkerListId"]    = m.anchorMarkerListId;
            arr.push_back(mj);
        }
        j["markers"] = arr;
        // Only write sliceLoops when any differs from 1 (default)
        bool anyNonOne = false;
        for (int v : c.sliceLoops) if (v != 1) { anyNonOne = true; break; }
        if (anyNonOne) {
            json la = json::array();
            for (int v : c.sliceLoops) la.push_back(v);
            j["sliceLoops"] = la;
        }
    }

    if (c.type == "audio") {
        j["path"] = c.path;
        if (c.startTime != 0.0) j["startTime"] = c.startTime;
        if (c.duration  != 0.0) j["duration"]  = c.duration;
        if (c.level     != 0.0) j["level"]      = c.level;
        if (c.trim      != 0.0) j["trim"]       = c.trim;
        // Per-output levels (omit if all zero)
        bool anyNonZero = false;
        for (float v : c.outLevelDb) if (v != 0.0f) { anyNonZero = true; break; }
        if (anyNonZero) {
            json arr = json::array();
            for (float v : c.outLevelDb) arr.push_back(v);
            j["outLevels"] = arr;
        }
        // Crosspoint entries
        if (!c.xpEntries.empty()) {
            json arr = json::array();
            for (const auto& e : c.xpEntries) {
                json ej;
                ej["s"] = e.s; ej["o"] = e.o; ej["db"] = e.db;
                arr.push_back(ej);
            }
            j["xpoint"] = arr;
        }
    } else if (c.type == "marker") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
        if (c.targetListId >= 0) j["targetListId"] = c.targetListId;
        j["markerIndex"]     = c.markerIndex;
    } else if (c.type == "goto") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
        if (c.targetListId >= 0) j["targetListId"] = c.targetListId;
    } else if (c.type == "start" || c.type == "stop" || c.type == "arm") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
        if (c.targetListId >= 0) j["targetListId"] = c.targetListId;
        if (c.type == "arm" && c.armStartTime != 0.0)
            j["armStartTime"] = c.armStartTime;
    } else if (c.type == "devamp") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
        if (c.targetListId >= 0) j["targetListId"] = c.targetListId;
        if (c.devampMode != 0)  j["devampMode"]    = c.devampMode;
        if (c.devampPreVamp)    j["devampPreVamp"]  = true;
    } else if (c.type == "fade") {
        j["targetCueNumber"]  = c.targetCueNumber;
        j["fadeCurve"]        = c.fadeCurve;
        if (c.duration != 0.0) j["duration"] = c.duration;
        if (c.fadeStopWhenDone) j["fadeStopWhenDone"] = true;
        if (c.fadeMasterEnabled) {
            j["fadeMasterEnabled"] = true;
            j["fadeMasterTarget"]  = c.fadeMasterTarget;
        }
        if (!c.fadeOutLevels.empty()) {
            json arr = json::array();
            for (const auto& fl : c.fadeOutLevels) {
                json ej;
                ej["ch"] = fl.ch; ej["enabled"] = fl.enabled; ej["target"] = fl.target;
                arr.push_back(ej);
            }
            j["fadeOutLevels"] = arr;
        }
        if (!c.fadeXpEntries.empty()) {
            json arr = json::array();
            for (const auto& fx : c.fadeXpEntries) {
                json ej;
                ej["s"] = fx.s; ej["o"] = fx.o;
                ej["enabled"] = fx.enabled; ej["target"] = fx.target;
                arr.push_back(ej);
            }
            j["fadeXpEntries"] = arr;
        }
    }

    if (c.type == "scriptlet") {
        if (!c.scriptletCode.empty()) j["scriptletCode"] = c.scriptletCode;
    }

    if (c.type == "snapshot") {
        if (c.snapshotId >= 0) j["snapshotId"] = c.snapshotId;
    }

    if (c.type == "automation") {
        if (!c.automationPath.empty()) j["automationPath"] = c.automationPath;
        j["automationDuration"] = c.automationDuration;
        if (!c.automationCurve.empty()) {
            json arr = json::array();
            for (const auto& pt : c.automationCurve) {
                json pj; pj["time"] = pt.time; pj["value"] = pt.value;
                if (pt.isHandle) pj["isHandle"] = true;
                arr.push_back(pj);
            }
            j["automationCurve"] = arr;
        }
    }

    if (c.type == "deactivate" || c.type == "reactivate") {
        if (c.pluginChannel >= 0) j["pluginChannel"] = c.pluginChannel;
        if (c.pluginSlot    >= 0) j["pluginSlot"]    = c.pluginSlot;
    }

    if (c.type == "network") {
        if (!c.networkPatchName.empty()) j["networkPatchName"] = c.networkPatchName;
        if (!c.networkCommand.empty())   j["networkCommand"]   = c.networkCommand;
    }

    if (c.type == "midi") {
        if (!c.midiPatchName.empty())   j["midiPatchName"]   = c.midiPatchName;
        if (!c.midiMessageType.empty()) j["midiMessageType"] = c.midiMessageType;
        j["midiChannel"] = c.midiChannel;
        j["midiData1"]   = c.midiData1;
        j["midiData2"]   = c.midiData2;
    }

    if (c.type == "timecode") {
        j["tcType"]          = c.tcType;
        j["tcFps"]           = c.tcFps;
        j["tcStartTC"]       = c.tcStartTC;
        j["tcEndTC"]         = c.tcEndTC;
        j["tcLtcChannel"]    = c.tcLtcChannel;
        if (!c.tcMidiPatchName.empty()) j["tcMidiPatchName"] = c.tcMidiPatchName;
    }

    if (c.type == "group") {
        j["groupMode"] = c.groupMode;
        if (c.groupRandom) j["groupRandom"] = true;
        if (!c.children.empty()) {
            json arr = json::array();
            for (const auto& ch : c.children) arr.push_back(cueToJson(ch));
            j["children"] = arr;
        }
    }
    // "mc" type: no additional type-specific fields beyond the common fields

    // MC inheritance link
    if (!c.mcSourceNumber.empty()) j["mcSourceNumber"] = c.mcSourceNumber;

    // Music Context
    if (c.musicContext.enabled) {
        json mc;
        mc["enabled"]            = true;
        mc["startOffsetSeconds"] = c.musicContext.startOffsetSeconds;
        if (!c.musicContext.applyBeforeStart) mc["applyBeforeStart"] = false;
        json pts = json::array();
        for (const auto& p : c.musicContext.points) {
            json pj;
            pj["bar"] = p.bar; pj["beat"] = p.beat; pj["bpm"] = p.bpm;
            if (p.isRamp)       pj["isRamp"]     = true;
            if (!p.hasTimeSig)  pj["hasTimeSig"]  = false;
            else { pj["timeSigNum"] = p.timeSigNum; pj["timeSigDen"] = p.timeSigDen; }
            pts.push_back(pj);
        }
        mc["points"] = pts;
        j["musicContext"] = mc;
    }

    // Child cues inside a Timeline group carry their offset; skip default 0.
    if (c.timelineOffset != 0.0) j["timelineOffset"] = c.timelineOffset;

    if (c.autoContinue) j["autoContinue"] = true;
    if (c.autoFollow)   j["autoFollow"]   = true;

    // Triggers — only write if at least one is enabled
    const auto& tr = c.triggers;
    if (tr.hotkey.enabled || tr.midi.enabled || tr.osc.enabled) {
        json tj;
        if (tr.hotkey.enabled || !tr.hotkey.keyString.empty()) {
            json hk; hk["enabled"] = tr.hotkey.enabled; hk["keyString"] = tr.hotkey.keyString;
            tj["hotkey"] = hk;
        }
        if (tr.midi.enabled) {
            json m;
            m["enabled"] = tr.midi.enabled;
            m["type"]    = midiMsgTypeToString(tr.midi.type);
            m["channel"] = tr.midi.channel;
            m["data1"]   = tr.midi.data1;
            m["data2"]   = tr.midi.data2;
            tj["midi"] = m;
        }
        if (tr.osc.enabled || !tr.osc.path.empty()) {
            json o; o["enabled"] = tr.osc.enabled; o["path"] = tr.osc.path;
            tj["osc"] = o;
        }
        j["triggers"] = tj;
    }
    return j;
}

bool ShowFile::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open '" + path.string() + "'"; return false; }

    json root;
    try {
        root = json::parse(f, nullptr, true, true);
    } catch (const json::parse_error& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    version = jget<std::string>(root, "mcp_version", kCurrentVersion);

    if (root.contains("show") && root["show"].is_object())
        show.title = jget<std::string>(root["show"], "title", "Untitled Show");

    if (root.contains("engine") && root["engine"].is_object()) {
        engine.sampleRate = jget<int>        (root["engine"], "sampleRate", 48000);
        engine.channels   = jget<int>        (root["engine"], "channels",   2);
        engine.deviceName = jget<std::string>(root["engine"], "deviceName", "");
    }

    audioSetup = {};
    if (root.contains("audioSetup") && root["audioSetup"].is_object()) {
        const auto& as = root["audioSetup"];

        audioSetup.sampleRate = jget<int>(as, "sampleRate", 0);

        if (as.contains("devices") && as["devices"].is_array()) {
            for (const auto& dj : as["devices"]) {
                AudioSetup::Device d;
                d.name         = jget<std::string>(dj, "name",         "");
                d.channelCount = jget<int>        (dj, "channelCount", 2);
                d.bufferSize   = jget<int>        (dj, "bufferSize",   512);
                d.masterClock  = jget<bool>       (dj, "masterClock",  false);
                audioSetup.devices.push_back(d);
            }
        }

        if (as.contains("channels") && as["channels"].is_array()) {
            for (const auto& ch : as["channels"]) {
                AudioSetup::Channel c;
                c.name          = jget<std::string>(ch, "name",          "");
                c.deviceIndex   = jget<int>        (ch, "deviceIndex",   0);
                c.deviceChannel = jget<int>        (ch, "deviceChannel", -1);
                c.linkedStereo  = jget<bool>       (ch, "linkedStereo",  false);
                c.masterGainDb  = jget<float>      (ch, "masterGainDb",  0.0f);
                c.mute          = jget<bool>       (ch, "mute",          false);
                c.phaseInvert   = jget<bool>       (ch, "phaseInvert",   false);
                c.delayInSamples= jget<bool>       (ch, "delayInSamples",false);
                c.delayMs       = jget<double>     (ch, "delayMs",       0.0);
                c.delaySamples  = jget<int>        (ch, "delaySamples",  0);
                c.pdcIsolated   = jget<bool>       (ch, "pdcIsolated",   false);
                if (ch.contains("sends") && ch["sends"].is_array()) {
                    for (const auto& sj : ch["sends"]) {
                        AudioSetup::Channel::SendSlot ss;
                        ss.dstChannel = jget<int>  (sj, "dstChannel", -1);
                        ss.levelDb    = jget<float>(sj, "levelDb",    0.0f);
                        ss.panL       = jget<float>(sj, "panL",       0.0f);
                        ss.panR       = jget<float>(sj, "panR",       0.0f);
                        ss.muted      = jget<bool> (sj, "muted",      false);
                        c.sends.push_back(ss);
                    }
                }
                if (ch.contains("plugins") && ch["plugins"].is_array()) {
                    for (const auto& pj : ch["plugins"]) {
                        AudioSetup::Channel::PluginSlot slot;
                        slot.pluginId = jget<std::string>(pj, "pluginId", "");
                        if (pj.contains("parameters") && pj["parameters"].is_object()) {
                            for (const auto& [k, v] : pj["parameters"].items()) {
                                try { slot.parameters[k] = v.get<float>(); } catch (...) {}
                            }
                        }
                        // External plugin fields
                        slot.extBackend      = jget<std::string>(pj, "extBackend", "");
                        slot.extPath         = jget<std::string>(pj, "extPath",    "");
                        slot.extName         = jget<std::string>(pj, "extName",    "");
                        slot.extVendor       = jget<std::string>(pj, "extVendor",  "");
                        slot.extVersion      = jget<std::string>(pj, "extVersion", "");
                        slot.extNumChannels  = jget<int>        (pj, "extNumChannels", 2);
                        if (pj.contains("extStateBlob") && pj["extStateBlob"].is_string())
                            slot.extStateBlob = decodeBase64(pj["extStateBlob"].get<std::string>());
                        if (pj.contains("extParamSnapshot") && pj["extParamSnapshot"].is_object()) {
                            for (const auto& [k, v] : pj["extParamSnapshot"].items()) {
                                try { slot.extParamSnapshot[k] = v.get<float>(); } catch (...) {}
                            }
                        }
                        slot.bypassed      = jget<bool> (pj, "bypassed",      false);
                        slot.disabled      = jget<bool> (pj, "disabled",      false);
                        slot.loadFailCount = jget<int>  (pj, "loadFailCount", 0);
                        slot.manualTailSec = jget<float>(pj, "manualTailSec", -1.0f);
                        c.plugins.push_back(std::move(slot));
                    }
                }
                audioSetup.channels.push_back(c);
            }
        }
        if (as.contains("xpEntries") && as["xpEntries"].is_array()) {
            for (const auto& xe : as["xpEntries"]) {
                AudioSetup::XpEntry e;
                e.ch  = jget<int>  (xe, "ch",  0);
                e.out = jget<int>  (xe, "out", 0);
                e.db  = jget<float>(xe, "db",  0.0f);
                audioSetup.xpEntries.push_back(e);
            }
        }
        if (as.contains("physOutDsp") && as["physOutDsp"].is_array()) {
            for (const auto& pj : as["physOutDsp"]) {
                AudioSetup::PhysOutDsp p;
                p.phaseInvert   = jget<bool>  (pj, "phaseInvert",   false);
                p.delayInSamples= jget<bool>  (pj, "delayInSamples",false);
                p.delayMs       = jget<double>(pj, "delayMs",        0.0);
                p.delaySamples  = jget<int>   (pj, "delaySamples",   0);
                audioSetup.physOutDsp.push_back(p);
            }
        }
    }
    networkSetup = {};
    if (root.contains("networkSetup") && root["networkSetup"].is_object()) {
        const auto& ns = root["networkSetup"];
        if (ns.contains("patches") && ns["patches"].is_array()) {
            for (const auto& p : ns["patches"]) {
                NetworkSetup::Patch patch;
                patch.name        = jget<std::string>(p, "name",        "");
                patch.type        = jget<std::string>(p, "type",        "osc");
                patch.protocol    = jget<std::string>(p, "protocol",    "udp");
                patch.iface       = jget<std::string>(p, "iface",       "any");
                patch.destination = jget<std::string>(p, "destination", "");
                patch.password    = jget<std::string>(p, "password",    "");
                networkSetup.patches.push_back(patch);
            }
        }
    }

    midiSetup = {};
    if (root.contains("midiSetup") && root["midiSetup"].is_object()) {
        const auto& ms = root["midiSetup"];
        if (ms.contains("patches") && ms["patches"].is_array()) {
            for (const auto& p : ms["patches"]) {
                MidiSetup::Patch patch;
                patch.name        = jget<std::string>(p, "name",        "");
                patch.destination = jget<std::string>(p, "destination", "");
                midiSetup.patches.push_back(patch);
            }
        }
    }

    // Migration: old files have no audioSetup → create one channel per engine output
    if (audioSetup.channels.empty() && engine.channels > 0) {
        for (int i = 0; i < engine.channels; ++i) {
            AudioSetup::Channel c;
            c.name = "Ch " + std::to_string(i + 1);
            audioSetup.channels.push_back(c);
        }
    }

    // System control bindings
    systemControls = {};
    if (root.contains("systemControls") && root["systemControls"].is_array()) {
        for (const auto& ej : root["systemControls"]) {
            const std::string astr = jget<std::string>(ej, "action", "");
            ControlAction a; if (!controlActionFromString(astr, a)) continue;
            auto& entry = systemControls.get(a);
            if (ej.contains("midi") && ej["midi"].is_object()) {
                const auto& m = ej["midi"];
                entry.midi.enabled = jget<bool>(m, "enabled", false);
                MidiMsgType mt; const std::string mts = jget<std::string>(m, "type", "note_on");
                midiMsgTypeFromString(mts, mt); entry.midi.type = mt;
                entry.midi.channel = jget<int>(m, "channel", 0);
                entry.midi.data1   = jget<int>(m, "data1",   0);
                entry.midi.data2   = jget<int>(m, "data2",   -1);
            }
            if (ej.contains("osc") && ej["osc"].is_object()) {
                const auto& o = ej["osc"];
                entry.osc.enabled = jget<bool>       (o, "enabled", false);
                entry.osc.path    = jget<std::string>(o, "path",    "");
            }
        }
    }

    // OSC server settings
    oscServer = {};
    if (root.contains("oscServer") && root["oscServer"].is_object()) {
        const auto& os = root["oscServer"];
        oscServer.enabled    = jget<bool>(os, "enabled",    false);
        oscServer.listenPort = jget<int> (os, "listenPort", 14521);
        if (os.contains("accessList") && os["accessList"].is_array()) {
            for (const auto& ae : os["accessList"]) {
                OscAccessEntry e;
                e.password = jget<std::string>(ae, "password", "");
                oscServer.accessList.push_back(e);
            }
        }
    }

    scriptletLibrary = {};
    if (root.contains("scriptletLibrary") && root["scriptletLibrary"].is_array()) {
        for (const auto& ej : root["scriptletLibrary"]) {
            ScriptletLibrary::Entry e;
            e.name = jget<std::string>(ej, "name", "");
            e.code = jget<std::string>(ej, "code", "");
            if (!e.name.empty())
                scriptletLibrary.entries.push_back(std::move(e));
        }
    }

    cueLists.clear();
    if (root.contains("cueLists") && root["cueLists"].is_array()) {
        for (const auto& cl : root["cueLists"]) {
            CueListData cld;
            cld.id        = jget<std::string>(cl, "id",        "main");
            cld.name      = jget<std::string>(cl, "name",      "Main");
            cld.numericId = jget<int>        (cl, "numericId", 0);
            if (cl.contains("cues") && cl["cues"].is_array())
                for (const auto& cj : cl["cues"])
                    cld.cues.push_back(parseCue(cj));
            cueLists.push_back(std::move(cld));
        }
    }
    assignListIds();

    uiHints.clear();
    if (root.contains("ui_hints") && root["ui_hints"].is_object()) {
        for (auto& [k, v] : root["ui_hints"].items())
            if (v.is_string()) uiHints.set(k, v.get<std::string>());
    }

    snapshotList = {};
    if (root.contains("snapshots") && root["snapshots"].is_object()) {
        const auto& sj = root["snapshots"];

        if (sj.contains("pendingDirtyPaths") && sj["pendingDirtyPaths"].is_array()) {
            for (const auto& pj : sj["pendingDirtyPaths"])
                if (pj.is_string())
                    snapshotList.pendingDirtyPaths.push_back(pj.get<std::string>());
        }

        if (sj.contains("list") && sj["list"].is_array()) {
            for (const auto& sn : sj["list"]) {
                if (sn.is_null()) {
                    snapshotList.snapshots.push_back(std::nullopt);
                    continue;
                }
                SnapshotList::Snapshot snap;
                snap.id   = jget<int>        (sn, "id",   0);
                snap.name = jget<std::string>(sn, "name", "");
                if (sn.contains("scope") && sn["scope"].is_array())
                    for (const auto& pj : sn["scope"])
                        if (pj.is_string()) snap.scope.push_back(pj.get<std::string>());
                if (sn.contains("channels") && sn["channels"].is_array()) {
                    for (const auto& cj : sn["channels"]) {
                        SnapshotList::Snapshot::ChannelState cs;
                        cs.ch = jget<int>(cj, "ch", 0);
                        if (cj.contains("delayMs"))
                            cs.delayMs = jget<double>(cj, "delayMs", 0.0);
                        if (cj.contains("delayInSamples"))
                            cs.delayInSamples = jget<bool>(cj, "delayInSamples", false);
                        if (cj.contains("delaySamples"))
                            cs.delaySamples = jget<int>(cj, "delaySamples", 0);
                        if (cj.contains("polarity"))
                            cs.polarity = jget<bool>(cj, "polarity", false);
                        if (cj.contains("mute"))
                            cs.mute = jget<bool>(cj, "mute", false);
                        if (cj.contains("faderDb"))
                            cs.faderDb = jget<float>(cj, "faderDb", 0.0f);
                        if (cj.contains("xpSends") && cj["xpSends"].is_array()) {
                            for (const auto& xj : cj["xpSends"]) {
                                SnapshotList::Snapshot::XpSend xs;
                                xs.out = jget<int>  (xj, "out", 0);
                                xs.db  = jget<float>(xj, "db",  0.0f);
                                cs.xpSends.push_back(xs);
                            }
                        }
                        if (cj.contains("pluginStates") && cj["pluginStates"].is_array()) {
                            for (const auto& pj : cj["pluginStates"]) {
                                SnapshotList::Snapshot::ChannelState::PluginParamState ps;
                                ps.slot = jget<int>(pj, "slot", 0);
                                if (pj.contains("bypassed"))
                                    ps.bypassed = jget<bool>(pj, "bypassed", false);
                                if (pj.contains("parameters") && pj["parameters"].is_object())
                                    for (const auto& [k, v] : pj["parameters"].items())
                                        try { ps.parameters[k] = v.get<float>(); } catch (...) {}
                                if (pj.contains("extStateBlob") && pj["extStateBlob"].is_string())
                                    ps.extStateBlob = decodeBase64(pj["extStateBlob"].get<std::string>());
                                if (pj.contains("extParamSnapshot") && pj["extParamSnapshot"].is_object())
                                    for (const auto& [k, v] : pj["extParamSnapshot"].items())
                                        try { ps.extParamSnapshot[k] = v.get<float>(); } catch (...) {}
                                cs.pluginStates.push_back(ps);
                            }
                        }
                        if (cj.contains("sendStates") && cj["sendStates"].is_array()) {
                            for (const auto& sj : cj["sendStates"]) {
                                SnapshotList::Snapshot::ChannelState::SendState ss;
                                ss.slot = jget<int>(sj, "slot", 0);
                                if (sj.contains("muted"))   ss.muted   = jget<bool> (sj, "muted",   false);
                                if (sj.contains("levelDb")) ss.levelDb = jget<float>(sj, "levelDb", 0.0f);
                                if (sj.contains("panL"))    ss.panL    = jget<float>(sj, "panL",    0.0f);
                                if (sj.contains("panR"))    ss.panR    = jget<float>(sj, "panR",    0.0f);
                                cs.sendStates.push_back(ss);
                            }
                        }
                        snap.channels.push_back(cs);
                    }
                }
                snapshotList.snapshots.push_back(snap);
            }
        }

        const int count = static_cast<int>(snapshotList.snapshots.size());
        snapshotList.currentIndex = std::min(jget<int>(sj, "currentIndex", 0), count);
    }

    return true;
}

bool ShowFile::save(const std::filesystem::path& path, std::string& error) const {
    json root;
    root["mcp_version"]       = version;
    root["show"]["title"]     = show.title;
    root["engine"]["sampleRate"]  = engine.sampleRate;
    root["engine"]["channels"]    = engine.channels;
    if (!engine.deviceName.empty())
        root["engine"]["deviceName"] = engine.deviceName;

    {
        json as;
        if (audioSetup.sampleRate != 0)
            as["sampleRate"] = audioSetup.sampleRate;
        if (!audioSetup.devices.empty()) {
            json devArr = json::array();
            for (const auto& d : audioSetup.devices) {
                json dj;
                dj["name"]         = d.name;
                dj["channelCount"] = d.channelCount;
                if (d.bufferSize != 512) dj["bufferSize"] = d.bufferSize;
                if (d.masterClock)       dj["masterClock"] = true;
                devArr.push_back(dj);
            }
            as["devices"] = devArr;
        }
        json chArr = json::array();
        for (const auto& ch : audioSetup.channels) {
            json cj;
            cj["name"] = ch.name;
            if (ch.deviceIndex != 0)   cj["deviceIndex"]   = ch.deviceIndex;
            if (ch.deviceChannel != -1) cj["deviceChannel"] = ch.deviceChannel;
            if (ch.linkedStereo)         cj["linkedStereo"]  = true;
            if (ch.masterGainDb != 0.0f) cj["masterGainDb"]  = ch.masterGainDb;
            if (ch.mute)                 cj["mute"]          = true;
            if (ch.phaseInvert)          cj["phaseInvert"]   = true;
            if (ch.delayInSamples)       cj["delayInSamples"]= true;
            if (ch.delayMs != 0.0)       cj["delayMs"]       = ch.delayMs;
            if (ch.delaySamples != 0)    cj["delaySamples"]  = ch.delaySamples;
            if (ch.pdcIsolated)          cj["pdcIsolated"]   = true;
            if (!ch.sends.empty()) {
                json sArr = json::array();
                for (const auto& ss : ch.sends) {
                    json sj;
                    sj["dstChannel"] = ss.dstChannel;
                    if (ss.levelDb != 0.0f) sj["levelDb"] = ss.levelDb;
                    if (ss.panL    != 0.0f) sj["panL"]    = ss.panL;
                    if (ss.panR    != 0.0f) sj["panR"]    = ss.panR;
                    if (ss.muted)           sj["muted"]   = true;
                    sArr.push_back(sj);
                }
                cj["sends"] = sArr;
            }
            if (!ch.plugins.empty()) {
                json pArr = json::array();
                for (const auto& ps : ch.plugins) {
                    json pj;
                    pj["pluginId"] = ps.pluginId;
                    if (!ps.parameters.empty()) {
                        json params;
                        for (const auto& [k, v] : ps.parameters)
                            params[k] = v;
                        pj["parameters"] = params;
                    }
                    if (ps.isExternal()) {
                        pj["extBackend"]    = ps.extBackend;
                        if (!ps.extPath.empty())    pj["extPath"]    = ps.extPath;
                        pj["extName"]       = ps.extName;
                        if (!ps.extVendor.empty())  pj["extVendor"]  = ps.extVendor;
                        if (!ps.extVersion.empty()) pj["extVersion"] = ps.extVersion;
                        pj["extNumChannels"] = ps.extNumChannels;
                        if (!ps.extStateBlob.empty())
                            pj["extStateBlob"] = encodeBase64(ps.extStateBlob);
                        if (!ps.extParamSnapshot.empty()) {
                            json snap;
                            for (const auto& [k, v] : ps.extParamSnapshot) snap[k] = v;
                            pj["extParamSnapshot"] = snap;
                        }
                    }
                    if (ps.bypassed)          pj["bypassed"]      = true;
                    if (ps.disabled)          pj["disabled"]      = true;
                    if (ps.loadFailCount > 0) pj["loadFailCount"] = ps.loadFailCount;
                    if (ps.manualTailSec >= 0.0f) pj["manualTailSec"] = ps.manualTailSec;
                    pArr.push_back(pj);
                }
                cj["plugins"] = pArr;
            }
            chArr.push_back(cj);
        }
        as["channels"] = chArr;
        if (!audioSetup.xpEntries.empty()) {
            json xpArr = json::array();
            for (const auto& xe : audioSetup.xpEntries) {
                json ej;
                ej["ch"] = xe.ch; ej["out"] = xe.out; ej["db"] = xe.db;
                xpArr.push_back(ej);
            }
            as["xpEntries"] = xpArr;
        }
        {
            bool anyPhysDsp = false;
            for (const auto& p : audioSetup.physOutDsp)
                if (p.phaseInvert || p.delayMs != 0.0 || p.delaySamples != 0)
                    { anyPhysDsp = true; break; }
            if (anyPhysDsp) {
                json pArr = json::array();
                for (const auto& p : audioSetup.physOutDsp) {
                    json pj;
                    if (p.phaseInvert)    pj["phaseInvert"]   = true;
                    if (p.delayInSamples) pj["delayInSamples"]= true;
                    if (p.delayMs != 0.0) pj["delayMs"]       = p.delayMs;
                    if (p.delaySamples != 0) pj["delaySamples"] = p.delaySamples;
                    pArr.push_back(pj);
                }
                as["physOutDsp"] = pArr;
            }
        }
        root["audioSetup"] = as;
    }

    if (!networkSetup.patches.empty()) {
        json pArr = json::array();
        for (const auto& p : networkSetup.patches) {
            json pj;
            pj["name"]        = p.name;
            pj["type"]        = p.type;
            pj["protocol"]    = p.protocol;
            if (!p.iface.empty() && p.iface != "any") pj["iface"] = p.iface;
            pj["destination"] = p.destination;
            if (!p.password.empty()) pj["password"] = p.password;
            pArr.push_back(pj);
        }
        root["networkSetup"]["patches"] = pArr;
    }

    if (!midiSetup.patches.empty()) {
        json pArr = json::array();
        for (const auto& p : midiSetup.patches) {
            json pj;
            pj["name"]        = p.name;
            pj["destination"] = p.destination;
            pArr.push_back(pj);
        }
        root["midiSetup"]["patches"] = pArr;
    }

    // System control bindings
    if (!systemControls.entries.empty()) {
        json scArr = json::array();
        for (const auto& e : systemControls.entries) {
            json ej;
            ej["action"] = controlActionToString(e.action);
            if (e.midi.enabled) {
                json m;
                m["enabled"] = e.midi.enabled;
                m["type"]    = midiMsgTypeToString(e.midi.type);
                m["channel"] = e.midi.channel;
                m["data1"]   = e.midi.data1;
                m["data2"]   = e.midi.data2;
                ej["midi"] = m;
            }
            if (e.osc.enabled || !e.osc.path.empty()) {
                json o; o["enabled"] = e.osc.enabled; o["path"] = e.osc.path;
                ej["osc"] = o;
            }
            scArr.push_back(ej);
        }
        root["systemControls"] = scArr;
    }

    // OSC server settings
    if (oscServer.enabled || oscServer.listenPort != 14521 || !oscServer.accessList.empty()) {
        json os;
        os["enabled"]    = oscServer.enabled;
        os["listenPort"] = oscServer.listenPort;
        if (!oscServer.accessList.empty()) {
            json alArr = json::array();
            for (const auto& ae : oscServer.accessList) {
                json aej; aej["password"] = ae.password; alArr.push_back(aej);
            }
            os["accessList"] = alArr;
        }
        root["oscServer"] = os;
    }

    if (!scriptletLibrary.entries.empty()) {
        json libArr = json::array();
        for (const auto& e : scriptletLibrary.entries) {
            json ej;
            ej["name"] = e.name;
            ej["code"] = e.code;
            libArr.push_back(ej);
        }
        root["scriptletLibrary"] = libArr;
    }

    json clArr = json::array();
    for (const auto& cld : cueLists) {
        json cl;
        cl["id"]        = cld.id;
        cl["name"]      = cld.name;
        cl["numericId"] = cld.numericId;
        json cArr  = json::array();
        for (const auto& c : cld.cues) cArr.push_back(cueToJson(c));
        cl["cues"] = cArr;
        clArr.push_back(cl);
    }
    root["cueLists"] = clArr;

    if (!uiHints.empty()) {
        json hints = json::object();
        for (const auto& [k, v] : uiHints.data)
            hints[k] = v;
        root["ui_hints"] = hints;
    }

    if (!snapshotList.snapshots.empty() || !snapshotList.pendingDirtyPaths.empty()) {
        json sj;
        sj["currentIndex"] = snapshotList.currentIndex;

        if (!snapshotList.pendingDirtyPaths.empty()) {
            json pdArr = json::array();
            for (const auto& p : snapshotList.pendingDirtyPaths)
                pdArr.push_back(p);
            sj["pendingDirtyPaths"] = pdArr;
        }

        json listArr = json::array();
        for (const auto& slot : snapshotList.snapshots) {
            if (!slot) { listArr.push_back(nullptr); continue; }
            const auto& snap = *slot;
            json sn;
            sn["id"]   = snap.id;
            sn["name"] = snap.name;
            {
                json scopeArr = json::array();
                for (const auto& p : snap.scope) scopeArr.push_back(p);
                sn["scope"] = scopeArr;
            }
            json chArr = json::array();
            for (const auto& cs : snap.channels) {
                json cj;
                cj["ch"] = cs.ch;
                if (cs.delayMs)        cj["delayMs"]        = *cs.delayMs;
                if (cs.delayInSamples) cj["delayInSamples"] = *cs.delayInSamples;
                if (cs.delaySamples)   cj["delaySamples"]   = *cs.delaySamples;
                if (cs.polarity)       cj["polarity"]       = *cs.polarity;
                if (cs.mute)           cj["mute"]           = *cs.mute;
                if (cs.faderDb)        cj["faderDb"]        = *cs.faderDb;
                if (!cs.xpSends.empty()) {
                    json xpArr = json::array();
                    for (const auto& xs : cs.xpSends) {
                        json xj; xj["out"] = xs.out; xj["db"] = xs.db;
                        xpArr.push_back(xj);
                    }
                    cj["xpSends"] = xpArr;
                }
                if (!cs.pluginStates.empty()) {
                    json psArr = json::array();
                    for (const auto& ps : cs.pluginStates) {
                        json pj;
                        pj["slot"] = ps.slot;
                        if (ps.bypassed) pj["bypassed"] = *ps.bypassed;
                        if (!ps.parameters.empty()) {
                            json pm = json::object();
                            for (const auto& [k, v] : ps.parameters) pm[k] = v;
                            pj["parameters"] = pm;
                        }
                        if (!ps.extStateBlob.empty())
                            pj["extStateBlob"] = encodeBase64(ps.extStateBlob);
                        if (!ps.extParamSnapshot.empty()) {
                            json em = json::object();
                            for (const auto& [k, v] : ps.extParamSnapshot) em[k] = v;
                            pj["extParamSnapshot"] = em;
                        }
                        psArr.push_back(pj);
                    }
                    cj["pluginStates"] = psArr;
                }
                if (!cs.sendStates.empty()) {
                    json ssArr = json::array();
                    for (const auto& ss : cs.sendStates) {
                        json sj;
                        sj["slot"] = ss.slot;
                        if (ss.muted)   sj["muted"]   = *ss.muted;
                        if (ss.levelDb) sj["levelDb"] = *ss.levelDb;
                        if (ss.panL)    sj["panL"]    = *ss.panL;
                        if (ss.panR)    sj["panR"]    = *ss.panR;
                        ssArr.push_back(sj);
                    }
                    cj["sendStates"] = ssArr;
                }
                chArr.push_back(cj);
            }
            if (!chArr.empty()) sn["channels"] = chArr;
            listArr.push_back(sn);
        }
        sj["list"] = listArr;
        root["snapshots"] = sj;
    }

    std::ofstream f(path);
    if (!f) { error = "cannot write '" + path.string() + "'"; return false; }
    f << root.dump(2) << "\n";
    return true;
}

ShowFile ShowFile::empty(const std::string& title) {
    ShowFile sf;
    sf.show.title = title;
    CueListData cld;
    cld.id        = "main";
    cld.name      = "Main";
    cld.numericId = 1;
    sf.cueLists.push_back(std::move(cld));
    return sf;
}

void ShowFile::assignListIds() {
    // Collect already-assigned IDs
    int maxId = 0;
    for (const auto& cl : cueLists)
        if (cl.numericId > 0) maxId = std::max(maxId, cl.numericId);
    // Assign IDs to any list that doesn't have one yet
    for (auto& cl : cueLists)
        if (cl.numericId <= 0) cl.numericId = ++maxId;
}

} // namespace mcp
