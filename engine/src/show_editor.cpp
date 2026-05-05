#include "engine/ShowFile.h"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Usage
//
//   show_editor new        <file.json> [title]
//   show_editor info       <file.json>
//   show_editor list       <file.json>
//   show_editor set-title  <file.json> <title>
//
//   show_editor add        <file.json> audio  <path> [name] [prewait_s]
//   show_editor add        <file.json> start  <target_index> [name] [prewait_s]
//   show_editor add        <file.json> stop   <target_index> [name] [prewait_s]
//
//   show_editor remove     <file.json> <index>
//   show_editor move       <file.json> <from_index> <to_index>
//   show_editor set-name   <file.json> <index> <name>
//   show_editor set-prewait <file.json> <index> <seconds>
//
// All operations that modify the show file are atomic: the file is only
// overwritten after the in-memory change succeeds.
// ---------------------------------------------------------------------------

namespace {

static void usage() {
    std::cerr <<
        "usage:\n"
        "  show_editor new        <file.json> [title]\n"
        "  show_editor info       <file.json>\n"
        "  show_editor list       <file.json>\n"
        "  show_editor set-title  <file.json> <title>\n"
        "\n"
        "  show_editor add        <file.json> audio  <path> [name] [prewait_s]\n"
        "  show_editor add        <file.json> start  <target> [name] [prewait_s]\n"
        "  show_editor add        <file.json> stop   <target> [name] [prewait_s]\n"
        "\n"
        "  show_editor remove        <file.json> <index>\n"
        "  show_editor move          <file.json> <from> <to>\n"
        "  show_editor set-name      <file.json> <index> <name>\n"
        "  show_editor set-prewait   <file.json> <index> <seconds>\n"
        "  show_editor set-start     <file.json> <index> <seconds>   (audio only)\n"
        "  show_editor set-duration  <file.json> <index> <seconds>   (audio only; 0=to end)\n"
        "  show_editor set-auto-continue <file.json> <index> <0|1>\n"
        "  show_editor set-auto-follow   <file.json> <index> <0|1>\n";
}

static std::string cueDesc(const mcp::ShowFile::CueData& c, int idx) {
    std::ostringstream ss;
    ss << "[" << std::setw(3) << idx << "]  " << std::setw(5) << c.type;
    if (c.type == "audio") {
        ss << "  " << std::filesystem::path(c.path).filename().string();
    } else {
        ss << "  →[" << c.target << "]";
    }
    if (!c.name.empty()) ss << "  \"" << c.name << "\"";
    if (c.preWait > 0.0) ss << "  +" << std::fixed << std::setprecision(3) << c.preWait << "s";
    return ss.str();
}

static bool loadOrDie(mcp::ShowFile& sf, const std::filesystem::path& p) {
    std::string err;
    if (!sf.load(p, err)) { std::cerr << "error: " << err << "\n"; return false; }
    return true;
}

static bool saveOrDie(const mcp::ShowFile& sf, const std::filesystem::path& p) {
    std::string err;
    if (!sf.save(p, err)) { std::cerr << "error: " << err << "\n"; return false; }
    return true;
}

static mcp::ShowFile::CueListData* mainList(mcp::ShowFile& sf) {
    if (sf.cueLists.empty()) {
        std::cerr << "error: show file has no cue lists\n";
        return nullptr;
    }
    return &sf.cueLists[0];
}

static int parseIndex(const char* s) { return std::atoi(s); }

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }

    const std::string cmd      = argv[1];
    const auto        filePath = std::filesystem::path(argv[2]);

    // ---- new ---------------------------------------------------------------
    if (cmd == "new") {
        const std::string title = (argc >= 4) ? argv[3] : "Untitled Show";
        auto sf = mcp::ShowFile::empty(title);
        std::string err;
        if (!sf.save(filePath, err)) { std::cerr << "error: " << err << "\n"; return 2; }
        std::cout << "Created " << filePath << "\n";
        return 0;
    }

    // ---- info --------------------------------------------------------------
    if (cmd == "info") {
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        std::cout << "File    : " << filePath << "\n"
                  << "Version : " << sf.version << "\n"
                  << "Title   : " << sf.show.title << "\n"
                  << "Engine  : " << sf.engine.sampleRate << " Hz  "
                                  << sf.engine.channels << " ch\n"
                  << "Lists   : " << sf.cueLists.size() << "\n";
        for (const auto& cl : sf.cueLists)
            std::cout << "  [" << cl.id << "] " << cl.name
                      << "  (" << cl.cues.size() << " cues)\n";
        return 0;
    }

    // ---- list --------------------------------------------------------------
    if (cmd == "list") {
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        const auto* cl = mainList(sf);
        if (!cl) return 3;
        if (cl->cues.empty()) { std::cout << "(no cues)\n"; return 0; }
        for (int i = 0; i < (int)cl->cues.size(); ++i)
            std::cout << cueDesc(cl->cues[i], i) << "\n";
        return 0;
    }

    // ---- set-title ---------------------------------------------------------
    if (cmd == "set-title") {
        if (argc < 4) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        sf.show.title = argv[3];
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "Title set to \"" << sf.show.title << "\"\n";
        return 0;
    }

    // ---- add ---------------------------------------------------------------
    if (cmd == "add") {
        if (argc < 5) { usage(); return 1; }
        const std::string type = argv[3];
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;

        mcp::ShowFile::CueData c;
        c.type = type;

        // Argument layout for all add sub-commands:
        //   argv[0]=show_editor argv[1]=add argv[2]=file argv[3]=type
        //   argv[4]=<path|target>  argv[5]=name(opt)  argv[6]=prewait(opt)
        if (type == "audio") {
            if (argc < 5) { std::cerr << "error: 'audio' requires a file path\n"; return 1; }
            // Store path relative to show file if possible
            auto p = std::filesystem::path(argv[4]);
            if (p.is_absolute()) {
                auto rel = std::filesystem::relative(p, filePath.parent_path());
                if (rel.string().substr(0, 2) != "..") p = rel;
            }
            c.path    = p.string();
            c.name    = (argc >= 6) ? argv[5] : std::filesystem::path(c.path).stem().string();
            c.preWait = (argc >= 7) ? std::stod(argv[6]) : 0.0;

        } else if (type == "start" || type == "stop") {
            if (argc < 5) {
                std::cerr << "error: '" << type << "' requires a target index\n";
                return 1;
            }
            c.target  = parseIndex(argv[4]);
            if (c.target < 0 || c.target >= (int)cl->cues.size()) {
                std::cerr << "error: target index " << c.target
                          << " out of range (0–" << (int)cl->cues.size() - 1 << ")\n";
                return 3;
            }
            c.name    = (argc >= 6) ? argv[5]
                                    : (type + "(" + std::to_string(c.target) + ")");
            c.preWait = (argc >= 7) ? std::stod(argv[6]) : 0.0;

        } else {
            std::cerr << "error: unknown cue type '" << type
                      << "'; expected audio | start | stop\n";
            return 1;
        }

        cl->cues.push_back(c);
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "Added " << cueDesc(c, (int)cl->cues.size() - 1) << "\n";
        return 0;
    }

    // ---- remove ------------------------------------------------------------
    if (cmd == "remove") {
        if (argc < 4) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int idx = parseIndex(argv[3]);
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        const auto desc = cueDesc(cl->cues[idx], idx);
        cl->cues.erase(cl->cues.begin() + idx);
        // Fix up start/stop targets that referenced the removed index
        for (auto& c : cl->cues) {
            if ((c.type == "start" || c.type == "stop") && c.target == idx) {
                std::cerr << "warning: cue " << cueDesc(c, -1)
                          << " targeted the removed cue — target reset to -1\n";
                c.target = -1;
            } else if ((c.type == "start" || c.type == "stop") && c.target > idx) {
                --c.target;  // shift down
            }
        }
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "Removed " << desc << "\n";
        return 0;
    }

    // ---- move --------------------------------------------------------------
    if (cmd == "move") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int from = parseIndex(argv[3]);
        const int to   = parseIndex(argv[4]);
        const int n    = (int)cl->cues.size();
        if (from < 0 || from >= n || to < 0 || to >= n) {
            std::cerr << "error: index out of range\n"; return 3;
        }
        auto c = cl->cues[from];
        cl->cues.erase(cl->cues.begin() + from);
        cl->cues.insert(cl->cues.begin() + to, c);
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "Moved [" << from << "] → [" << to << "]\n";
        return 0;
    }

    // ---- set-name ----------------------------------------------------------
    if (cmd == "set-name") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int idx = parseIndex(argv[3]);
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        cl->cues[idx].name = argv[4];
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "Renamed [" << idx << "] to \"" << argv[4] << "\"\n";
        return 0;
    }

    // ---- set-prewait -------------------------------------------------------
    if (cmd == "set-prewait") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int    idx = parseIndex(argv[3]);
        const double pw  = std::stod(argv[4]);
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        cl->cues[idx].preWait = pw;
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "[" << idx << "] preWait set to " << pw << "s\n";
        return 0;
    }

    // ---- set-start ---------------------------------------------------------
    if (cmd == "set-start") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int    idx = parseIndex(argv[3]);
        const double val = std::stod(argv[4]);
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        if (cl->cues[idx].type != "audio") {
            std::cerr << "error: set-start is only valid for audio cues\n"; return 3;
        }
        cl->cues[idx].startTime = val;
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "[" << idx << "] startTime set to " << val << "s\n";
        return 0;
    }

    // ---- set-duration ------------------------------------------------------
    if (cmd == "set-duration") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int    idx = parseIndex(argv[3]);
        const double val = std::stod(argv[4]);
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        if (cl->cues[idx].type != "audio") {
            std::cerr << "error: set-duration is only valid for audio cues\n"; return 3;
        }
        cl->cues[idx].duration = val;
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "[" << idx << "] duration set to " << val << "s\n";
        return 0;
    }

    // ---- set-auto-continue -------------------------------------------------
    if (cmd == "set-auto-continue") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int  idx = parseIndex(argv[3]);
        const bool val = std::atoi(argv[4]) != 0;
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        cl->cues[idx].autoContinue = val;
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "[" << idx << "] autoContinue set to " << (val ? "true" : "false") << "\n";
        return 0;
    }

    // ---- set-auto-follow ---------------------------------------------------
    if (cmd == "set-auto-follow") {
        if (argc < 5) { usage(); return 1; }
        mcp::ShowFile sf;
        if (!loadOrDie(sf, filePath)) return 2;
        auto* cl = mainList(sf);
        if (!cl) return 3;
        const int  idx = parseIndex(argv[3]);
        const bool val = std::atoi(argv[4]) != 0;
        if (idx < 0 || idx >= (int)cl->cues.size()) {
            std::cerr << "error: index " << idx << " out of range\n"; return 3;
        }
        cl->cues[idx].autoFollow = val;
        if (!saveOrDie(sf, filePath)) return 2;
        std::cout << "[" << idx << "] autoFollow set to " << (val ? "true" : "false") << "\n";
        return 0;
    }

    std::cerr << "error: unknown command '" << cmd << "'\n\n";
    usage();
    return 1;
}
