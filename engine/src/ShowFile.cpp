#include "engine/ShowFile.h"
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace mcp {

// Helper: safely extract a value from j[key] if present and the right type.
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
    c.preWait         = jget<double>     (j, "preWait",         0.0);
    c.startTime       = jget<double>     (j, "startTime",       0.0);
    c.duration        = jget<double>     (j, "duration",        0.0);
    c.autoContinue    = jget<bool>       (j, "autoContinue",    false);
    c.autoFollow      = jget<bool>       (j, "autoFollow",      false);
    return c;
}

static json cueToJson(const ShowFile::CueData& c) {
    json j;
    j["type"]      = c.type;
    j["cueNumber"] = c.cueNumber;
    j["name"]      = c.name;
    j["preWait"]   = c.preWait;
    if (c.type == "audio") {
        j["path"] = c.path;
        if (c.startTime != 0.0) j["startTime"] = c.startTime;
        if (c.duration  != 0.0) j["duration"]  = c.duration;
    } else {
        j["target"]          = c.target;
        j["targetCueNumber"] = c.targetCueNumber;
    }
    if (c.autoContinue) j["autoContinue"] = true;
    if (c.autoFollow)   j["autoFollow"]   = true;
    return j;
}

bool ShowFile::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open '" + path.string() + "'"; return false; }

    json root;
    try {
        root = json::parse(f, nullptr, /*exceptions=*/true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        error = std::string("JSON parse error: ") + e.what();
        return false;
    }

    version      = jget<std::string>(root, "mcp_version", kCurrentVersion);

    if (root.contains("show") && root["show"].is_object()) {
        const auto& s = root["show"];
        show.title = jget<std::string>(s, "title", "Untitled Show");
    }

    if (root.contains("engine") && root["engine"].is_object()) {
        const auto& e = root["engine"];
        engine.sampleRate = jget<int>(e, "sampleRate", 48000);
        engine.channels   = jget<int>(e, "channels",   2);
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
    root["engine"]["sampleRate"] = engine.sampleRate;
    root["engine"]["channels"]   = engine.channels;

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
