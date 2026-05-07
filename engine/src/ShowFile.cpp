#include "engine/ShowFile.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace mcp {

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
            tm.time = jget<double>     (m, "time", 0.0);
            tm.name = jget<std::string>(m, "name", "");
            c.markers.push_back(tm);
        }
    }
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
            if (!m.name.empty()) mj["name"] = m.name;
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
    } else if (c.type == "start" || c.type == "stop" || c.type == "arm") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
        if (c.type == "arm" && c.armStartTime != 0.0)
            j["armStartTime"] = c.armStartTime;
    } else if (c.type == "devamp") {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
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

    cueLists.clear();
    if (root.contains("cueLists") && root["cueLists"].is_array()) {
        for (const auto& cl : root["cueLists"]) {
            CueListData cld;
            cld.id   = jget<std::string>(cl, "id",   "main");
            cld.name = jget<std::string>(cl, "name", "Main");
            if (cl.contains("cues") && cl["cues"].is_array())
                for (const auto& cj : cl["cues"])
                    cld.cues.push_back(parseCue(cj));
            cueLists.push_back(std::move(cld));
        }
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

    json clArr = json::array();
    for (const auto& cld : cueLists) {
        json cl;
        cl["id"]   = cld.id;
        cl["name"] = cld.name;
        json cArr  = json::array();
        for (const auto& c : cld.cues) cArr.push_back(cueToJson(c));
        cl["cues"] = cArr;
        clArr.push_back(cl);
    }
    root["cueLists"] = clArr;

    std::ofstream f(path);
    if (!f) { error = "cannot write '" + path.string() + "'"; return false; }
    f << root.dump(2) << "\n";
    return true;
}

ShowFile ShowFile::empty(const std::string& title) {
    ShowFile sf;
    sf.show.title = title;
    sf.cueLists.push_back({"main", "Main", {}});
    return sf;
}

} // namespace mcp
