#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include "engine/ShowFile.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// cue_runner — loads a show file and runs an optional event timeline.
//
//   cue_runner [--debug] <show.json> [events.txt]
//
// If no events file is given the show is loaded, verified, and listed.
//
// Event file format (one event per line):
//   <time_s>  go               — fire selected cue, advance selection
//   <time_s>  start <index>    — fire cue[index], selection unchanged
//   <time_s>  stop  <index>    — stop cue[index] immediately
//   <time_s>  panic            — cancel pending fires + stop all voices
//   # comment / blank lines ignored
// ---------------------------------------------------------------------------

namespace {

using Clock   = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

struct Event {
    double      time{0.0};
    std::string command;
    int         arg{-1};
};

static std::string trim(std::string s) {
    if (auto p = s.find('#'); p != std::string::npos) s.erase(p);
    auto ns = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
    return s;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

static std::vector<Event> parseEvents(const std::filesystem::path& path) {
    std::vector<Event> events;
    std::ifstream f(path);
    if (!f) { std::cerr << "error: cannot open events file '" << path << "'\n"; return {}; }

    std::string raw;
    while (std::getline(f, raw)) {
        const std::string line = trim(raw);
        if (line.empty()) continue;

        std::istringstream ss(line);
        Event ev;
        if (!(ss >> ev.time >> ev.command)) {
            std::cerr << "warning: ignoring malformed event: " << line << "\n";
            continue;
        }
        ev.command = lower(ev.command);
        ss >> ev.arg;
        if (ss.fail()) ev.arg = -1;
        events.push_back(ev);
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b){ return a.time < b.time; });
    return events;
}

static std::string cueLabel(const mcp::ShowFile& sf, int idx) {
    if (sf.cueLists.empty()) return "?";
    const auto& cues = sf.cueLists[0].cues;
    if (idx < 0 || idx >= (int)cues.size()) return "?";
    const auto& c = cues[idx];
    if (!c.name.empty()) return c.name;
    if (c.type == "audio") return std::filesystem::path(c.path).filename().string();
    return c.type + "(" + std::to_string(c.target) + ")";
}

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cue_runner [--debug] <show.json> [events.txt]\n";
        return 1;
    }

    bool debugLog  = false;
    int  nextArg   = 1;
    if (std::string(argv[nextArg]) == "--debug") { debugLog = true; ++nextArg; }
    if (nextArg >= argc) {
        std::cerr << "usage: cue_runner [--debug] <show.json> [events.txt]\n";
        return 1;
    }

    const auto showPath = std::filesystem::absolute(argv[nextArg++]);

    // ---- Load show file ----------------------------------------------------
    mcp::ShowFile sf;
    {
        std::string err;
        if (!sf.load(showPath, err)) { std::cerr << "error: " << err << "\n"; return 1; }
    }
    if (sf.cueLists.empty() || sf.cueLists[0].cues.empty()) {
        std::cerr << "error: show file has no cues\n"; return 1;
    }

    std::cout << "Show    : " << sf.show.title << "\n"
              << "Version : " << sf.version    << "\n\n";

    // ---- Engine + CueList setup --------------------------------------------
    mcp::AudioEngine engine;
    if (!engine.initialize(sf.engine.sampleRate, sf.engine.channels)) {
        std::cerr << "error: AudioEngine: " << engine.lastError() << "\n"; return 1;
    }

    mcp::Scheduler scheduler(engine);
    scheduler.setDebugLog(debugLog);
    mcp::CueList cues(engine, scheduler);

    const auto& cueListData = sf.cueLists[0];
    const auto  baseDir     = showPath.parent_path();

    std::cout << "Cues (" << cueListData.name << "):\n";
    for (int i = 0; i < (int)cueListData.cues.size(); ++i) {
        const auto& cd = cueListData.cues[i];
        bool ok = true;

        if (cd.type == "audio") {
            auto p = std::filesystem::path(cd.path);
            if (p.is_relative()) p = baseDir / p;
            ok = cues.addCue(p.string(), cd.name, cd.preWait);
            if (ok && (cd.startTime != 0.0 || cd.duration != 0.0)) {
                cues.setCueStartTime(i, cd.startTime);
                cues.setCueDuration (i, cd.duration);
            }
        } else if (cd.type == "start") {
            cues.addStartCue(cd.target, cd.name, cd.preWait);
        } else if (cd.type == "stop") {
            cues.addStopCue(cd.target, cd.name, cd.preWait);
        } else {
            std::cerr << "  [" << i << "] warning: unknown type '" << cd.type
                      << "' — skipped\n";
            // Still push a placeholder so indices stay consistent.
            // For now just skip; future: cues.addUnknownCue(cd)
            continue;
        }

        if (ok) {
            cues.setCueAutoContinue(i, cd.autoContinue);
            cues.setCueAutoFollow  (i, cd.autoFollow);
        }

        if (!ok) {
            std::cerr << "  [" << i << "] FAILED: " << cueLabel(sf, i) << "\n";
            return 1;
        }

        std::cout << "  [" << i << "]  " << std::setw(5) << cd.type << "  "
                  << cueLabel(sf, i);
        if (cd.preWait      > 0.0)  std::cout << "  (+" << std::fixed << std::setprecision(3) << cd.preWait << "s)";
        if (cd.startTime    > 0.0)  std::cout << "  from=" << cd.startTime << "s";
        if (cd.duration     > 0.0)  std::cout << "  dur=" << cd.duration << "s";
        if (cd.autoContinue)        std::cout << "  [AC]";
        if (cd.autoFollow)          std::cout << "  [AF]";
        std::cout << "\n";
    }

    // ---- Events (optional) -------------------------------------------------
    if (nextArg >= argc) {
        std::cout << "\n(no events file — show loaded OK)\n";
        return 0;
    }

    const auto eventsPath = std::filesystem::absolute(argv[nextArg]);
    const auto events     = parseEvents(eventsPath);
    if (events.empty()) { std::cerr << "error: no events in '" << eventsPath << "'\n"; return 1; }

    std::cout << "\nEvents:\n";
    for (const auto& ev : events) {
        std::cout << "  " << std::fixed << std::setprecision(3) << ev.time << "s  "
                  << ev.command;
        if (ev.arg >= 0) std::cout << " " << ev.arg;
        std::cout << "\n";
    }
    std::cout << "\n";

    // ---- Playback loop -----------------------------------------------------
    const auto t0    = Clock::now();
    std::size_t next = 0;

    auto elapsed = [&]{ return Seconds(Clock::now() - t0).count(); };

    while (next < events.size() || cues.isAnyCuePlaying() || scheduler.pendingCount() > 0) {
        const double now = elapsed();

        // Snapshot engine frame once for all co-temporal events so prewait
        // deadlines are identical even if the calls take a few microseconds.
        const int64_t batchOrigin = engine.enginePlayheadFrames();

        while (next < events.size() && now >= events[next].time) {
            const Event& ev = events[next];
            bool ok = true;

            std::cout << "\rT+" << std::fixed << std::setprecision(3) << now << "s  ";

            if (ev.command == "go") {
                const int sel = cues.selectedIndex();
                ok = cues.go(batchOrigin);
                std::cout << "go → [" << sel << "] " << cueLabel(sf, sel);
            } else if (ev.command == "start") {
                ok = cues.start(ev.arg, batchOrigin);
                std::cout << "start [" << ev.arg << "] " << cueLabel(sf, ev.arg);
            } else if (ev.command == "stop") {
                cues.stop(ev.arg);
                std::cout << "stop [" << ev.arg << "] " << cueLabel(sf, ev.arg);
            } else if (ev.command == "panic") {
                cues.panic();
                std::cout << "panic";
            } else {
                std::cout << "? unknown '" << ev.command << "'";
                ok = false;
            }

            if (!ok) std::cout << "  (failed)";
            std::cout << "\n";
            ++next;
        }

        if (cues.isAnyCuePlaying() || scheduler.pendingCount() > 0) {
            std::cout << "\r  voices:" << engine.activeVoiceCount()
                      << "  pending:" << scheduler.pendingCount()
                      << "  sel:" << cues.selectedIndex()
                      << "      " << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    std::cout << "\rDone.                                   \n";
    return 0;
}
