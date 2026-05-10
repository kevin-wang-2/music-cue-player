#include "engine/ShowFile.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

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
