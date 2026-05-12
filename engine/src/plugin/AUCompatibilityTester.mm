#include "engine/plugin/AUCompatibilityTester.h"
#ifdef __APPLE__

#include "engine/plugin/AUPluginAdapter.h"
#include "engine/plugin/AudioBlock.h"
#include "engine/plugin/EventBlock.h"
#include "engine/plugin/ProcessContext.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mcp::plugin {

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string fccStr(uint32_t v) {
    char c[5];
    c[0] = static_cast<char>((v >> 24) & 0xFF);
    c[1] = static_cast<char>((v >> 16) & 0xFF);
    c[2] = static_cast<char>((v >>  8) & 0xFF);
    c[3] = static_cast<char>( v        & 0xFF);
    c[4] = '\0';
    return c;
}

// ── test ─────────────────────────────────────────────────────────────────────

AUTestResult AUCompatibilityTester::test(const AUComponentEntry& entry) {
    AUTestResult res;
    res.pluginId   = "au:" + fccStr(entry.type) + "/" +
                     fccStr(entry.subtype) + "/" + fccStr(entry.manufacturer);
    res.pluginName = entry.name;
    res.vendor     = entry.manufacturerName;
    res.version    = entry.version;

    try {
        // 1. Instantiate — prefer stereo
        AUPluginAdapter::Descriptor desc{entry.type, entry.subtype, entry.manufacturer, 2};
        auto adapter = AUPluginAdapter::create(desc);
        if (!adapter) {
            res.status       = AUTestResult::Status::LoadFailed;
            res.errorMessage = "AudioComponent not found";
            return res;
        }

        // 2. Prepare: stereo, 48 kHz, 512-frame blocks
        constexpr double kSR    = 48000.0;
        constexpr int    kBlock = 512;
        constexpr int    kCh    = 2;

        ProcessContext ctx;
        ctx.sampleRate     = kSR;
        ctx.maxBlockSize   = kBlock;
        ctx.inputChannels  = kCh;
        ctx.outputChannels = kCh;

        try {
            adapter->prepare(ctx);
        } catch (const std::exception& ex) {
            res.status       = AUTestResult::Status::UnsupportedFormat;
            res.errorMessage = std::string("prepare() threw: ") + ex.what();
            return res;
        } catch (...) {
            res.status       = AUTestResult::Status::UnsupportedFormat;
            res.errorMessage = "prepare() threw unknown exception";
            return res;
        }

        // 3. Gather metadata
        const auto& params = adapter->getParameters();
        res.parameterCount = static_cast<int>(params.size());
        res.latencySamples = adapter->getLatencySamples();
        res.tailSamples    = adapter->getTailSamples();

        // 4. Baseline getState
        try {
            adapter->getState();
        } catch (...) {
            res.warnings.push_back("getState() threw on baseline");
        }

        // 5. Allocate planar silence buffers
        std::vector<float> bufData(static_cast<size_t>(kBlock * kCh), 0.0f);
        std::vector<const float*> inPtrs (static_cast<size_t>(kCh));
        std::vector<float*>       outPtrs(static_cast<size_t>(kCh));
        for (int c = 0; c < kCh; ++c) {
            inPtrs [static_cast<size_t>(c)] = bufData.data() + c * kBlock;
            outPtrs[static_cast<size_t>(c)] = bufData.data() + c * kBlock;
        }
        AudioBlock block;
        block.inputs            = inPtrs.data();
        block.outputs           = outPtrs.data();
        block.numInputChannels  = kCh;
        block.numOutputChannels = kCh;
        block.numSamples        = kBlock;

        EventBlock evts{};

        // 6. Render ~1 second of silence
        const int kTotalBlocks = static_cast<int>(kSR) / kBlock;
        try {
            for (int b = 0; b < kTotalBlocks; ++b)
                adapter->process(block, evts);
        } catch (const std::exception& ex) {
            res.status       = AUTestResult::Status::RenderFailed;
            res.errorMessage = std::string("process() threw: ") + ex.what();
            return res;
        } catch (...) {
            res.status       = AUTestResult::Status::RenderFailed;
            res.errorMessage = "process() threw unknown exception";
            return res;
        }

        // 7. Exercise one automatable parameter (read current, write it back)
        if (!params.empty()) {
            const ParameterInfo* target = nullptr;
            for (const auto& p : params)
                if (p.automatable) { target = &p; break; }
            if (target) {
                try {
                    const float cur = adapter->getParameterValue(target->id);
                    adapter->setParameterValue(target->id, cur);
                } catch (const std::exception& ex) {
                    res.warnings.push_back(
                        std::string("parameter set/get threw: ") + ex.what());
                } catch (...) {
                    res.warnings.push_back("parameter set/get threw unknown exception");
                }
            }
        }

        // 8. Render again after parameter touch
        try {
            for (int b = 0; b < kTotalBlocks; ++b)
                adapter->process(block, evts);
        } catch (...) {
            res.warnings.push_back("process() threw on second render pass");
        }

        // 9. Final getState
        try {
            adapter->getState();
        } catch (...) {
            res.status       = AUTestResult::Status::StateFailed;
            res.errorMessage = "getState() threw after render";
            return res;
        }

        // adapter destructor cleans up AU resources
    } catch (const std::exception& ex) {
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = std::string("unexpected exception: ") + ex.what();
    } catch (...) {
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = "unexpected unknown exception";
    }

    return res;
}

// ── testAll ───────────────────────────────────────────────────────────────────

std::vector<AUTestResult> AUCompatibilityTester::testAll(
        const std::vector<AUComponentEntry>& entries,
        ProgressFn progress)
{
    std::vector<AUTestResult> out;
    out.reserve(entries.size());
    const int total = static_cast<int>(entries.size());
    for (int i = 0; i < total; ++i) {
        auto res = testIsolated(entries[static_cast<size_t>(i)]);
        if (progress) progress(i, total, res);
        out.push_back(std::move(res));
    }
    return out;
}

// ── testIsolated — subprocess helpers ────────────────────────────────────────

// Encode a FCC uint32 as exactly 8 uppercase hex digits.
static std::string fccHex(uint32_t v) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// Parse 8 hex digits → uint32.
static uint32_t hexFcc(const char* s) {
    return static_cast<uint32_t>(strtoul(s, nullptr, 16));
}

// Read from fd until EOF or timeout, returning all data received.
static std::string pipeReadWithTimeout(int fd, int timeoutMs) {
    std::string out;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        struct pollfd pfd = { fd, POLLIN, 0 };
        // Poll in 100 ms slices so we re-check the deadline regularly.
        int pollMs = static_cast<int>(std::min(remaining, (long long)100));
        int ret    = poll(&pfd, 1, pollMs);
        if (ret < 0) break;  // EINTR or error

        char buf[8192];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;   // 0 = EOF (child exited), <0 = error
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// Minimal field extractor for our deterministic toJson() output.
static std::string jsonStr(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\": \"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') break;
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += esc;  break;
            }
        } else {
            result += c;
        }
    }
    return result;
}

static int jsonInt(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\": ";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    if (pos >= json.size() || (!std::isdigit(json[pos]) && json[pos] != '-')) return 0;
    return std::stoi(json.substr(pos));
}

static AUTestResult::Status statusFromLabel(const std::string& s) {
    if (s == "ok")               return AUTestResult::Status::Ok;
    if (s == "loadFailed")       return AUTestResult::Status::LoadFailed;
    if (s == "unsupportedFormat")return AUTestResult::Status::UnsupportedFormat;
    if (s == "renderFailed")     return AUTestResult::Status::RenderFailed;
    if (s == "parameterFailed")  return AUTestResult::Status::ParameterFailed;
    if (s == "stateFailed")      return AUTestResult::Status::StateFailed;
    if (s == "crashed")          return AUTestResult::Status::Crashed;
    if (s == "timedOut")         return AUTestResult::Status::TimedOut;
    return AUTestResult::Status::UnknownFailed;
}

AUTestResult AUCompatibilityTester::testIsolated(const AUComponentEntry& entry,
                                                  int timeoutMs)
{
    // Pre-fill metadata from caller (subprocess can't know display name/vendor).
    AUTestResult res;
    res.pluginId   = "au:" + fccStr(entry.type) + "/" +
                     fccStr(entry.subtype) + "/" + fccStr(entry.manufacturer);
    res.pluginName = entry.name;
    res.vendor     = entry.manufacturerName;
    res.version    = entry.version;

    // ── 1. Locate this binary ──────────────────────────────────────────────────
    char execPath[4096] = {};
    uint32_t execPathLen = sizeof(execPath);
    if (_NSGetExecutablePath(execPath, &execPathLen) != 0) {
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = "could not determine executable path";
        return res;
    }

    // ── 2. Build the subprocess argument: "TTTTTTTT,SSSSSSSS,MMMMMMMM" ────────
    std::string pluginArg = fccHex(entry.type) + "," +
                            fccHex(entry.subtype) + "," +
                            fccHex(entry.manufacturer);

    // ── 3. Pipe for capturing child stdout ────────────────────────────────────
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = "pipe() failed";
        return res;
    }

    // ── 4. posix_spawn file actions ───────────────────────────────────────────
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2 (&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    // Silence plugin log spam on stderr.
    posix_spawn_file_actions_addopen (&actions, STDERR_FILENO,
                                      "/dev/null", O_WRONLY, 0);

    // ── 5. Launch subprocess ──────────────────────────────────────────────────
    char* const args[] = {
        execPath,
        const_cast<char*>("--au-test-plugin"),
        const_cast<char*>(pluginArg.c_str()),
        nullptr
    };
    pid_t pid = -1;
    int spawnErr = posix_spawn(&pid, execPath, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]); // parent doesn't write

    if (spawnErr != 0) {
        close(pipefd[0]);
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = "posix_spawn failed (errno " + std::to_string(spawnErr) + ")";
        return res;
    }

    // ── 6. Read subprocess output with timeout ────────────────────────────────
    std::string output = pipeReadWithTimeout(pipefd[0], timeoutMs);
    close(pipefd[0]);

    // ── 7. Collect child exit status ──────────────────────────────────────────
    // Check if child is still alive (timed out in the read loop).
    int wstatus = 0;
    pid_t wr = waitpid(pid, &wstatus, WNOHANG);
    if (wr == 0) {
        // Still running — we exceeded the timeout; kill it.
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        res.status       = AUTestResult::Status::TimedOut;
        res.errorMessage = "plugin test timed out (" +
                           std::to_string(timeoutMs / 1000) + " s)";
        return res;
    }

    if (WIFSIGNALED(wstatus)) {
        res.status       = AUTestResult::Status::Crashed;
        res.errorMessage = "plugin crashed (signal " +
                           std::to_string(WTERMSIG(wstatus)) + ")";
        return res;
    }

    // ── 8. Parse JSON output ──────────────────────────────────────────────────
    if (output.empty()) {
        res.status       = AUTestResult::Status::UnknownFailed;
        res.errorMessage = "subprocess produced no output";
        return res;
    }

    res.status         = statusFromLabel(jsonStr(output, "status"));
    res.errorMessage   = jsonStr(output, "errorMessage");
    res.parameterCount = jsonInt(output, "parameterCount");
    res.latencySamples = jsonInt(output, "latencySamples");
    res.tailSamples    = jsonInt(output, "tailSamples");
    // warnings: skip (not critical for the tester UI)
    return res;
}

// ── toJson ────────────────────────────────────────────────────────────────────

static std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

std::string AUCompatibilityTester::toJson(const std::vector<AUTestResult>& results) {
    std::string out;
    out.reserve(results.size() * 200);
    out += "[\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        char num[64];
        out += "  {\n";
        out += "    \"pluginId\": \""     + jsonEsc(r.pluginId)   + "\",\n";
        out += "    \"name\": \""         + jsonEsc(r.pluginName) + "\",\n";
        out += "    \"vendor\": \""       + jsonEsc(r.vendor)     + "\",\n";
        out += "    \"version\": \""      + jsonEsc(r.version)    + "\",\n";
        out += "    \"status\": \""       + std::string(AUTestResult::statusLabel(r.status)) + "\",\n";
        snprintf(num, sizeof(num), "%d",  r.parameterCount); out += "    \"parameterCount\": " + std::string(num) + ",\n";
        snprintf(num, sizeof(num), "%d",  r.latencySamples); out += "    \"latencySamples\": " + std::string(num) + ",\n";
        snprintf(num, sizeof(num), "%d",  r.tailSamples);    out += "    \"tailSamples\": "    + std::string(num) + ",\n";
        out += "    \"errorMessage\": \"" + jsonEsc(r.errorMessage) + "\",\n";
        out += "    \"warnings\": [";
        for (size_t wi = 0; wi < r.warnings.size(); ++wi) {
            if (wi > 0) out += ", ";
            out += "\"" + jsonEsc(r.warnings[wi]) + "\"";
        }
        out += "]\n  }";
        if (i + 1 < results.size()) out += ",";
        out += "\n";
    }
    out += "]\n";
    return out;
}

} // namespace mcp::plugin
#endif // __APPLE__
