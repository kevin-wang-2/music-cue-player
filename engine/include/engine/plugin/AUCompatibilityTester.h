#pragma once
#ifdef __APPLE__

#include "AUComponentEnumerator.h"
#include <functional>
#include <string>
#include <vector>

namespace mcp::plugin {

struct AUTestResult {
    enum class Status {
        Ok,
        LoadFailed,
        UnsupportedFormat,
        RenderFailed,
        ParameterFailed,
        StateFailed,
        UnknownFailed,
        Crashed,    // subprocess received a fatal signal
        TimedOut    // subprocess exceeded the timeout
    };

    static const char* statusLabel(Status s) noexcept {
        switch (s) {
            case Status::Ok:               return "ok";
            case Status::LoadFailed:       return "loadFailed";
            case Status::UnsupportedFormat:return "unsupportedFormat";
            case Status::RenderFailed:     return "renderFailed";
            case Status::ParameterFailed:  return "parameterFailed";
            case Status::StateFailed:      return "stateFailed";
            case Status::UnknownFailed:    return "unknownFailed";
            case Status::Crashed:          return "crashed";
            case Status::TimedOut:         return "timedOut";
        }
        return "unknownFailed";
    }

    std::string pluginId;   // "au:TTTT/SSSS/MMMM"
    std::string pluginName;
    std::string vendor;
    std::string version;

    Status      status{Status::Ok};
    std::string errorMessage;
    std::vector<std::string> warnings;

    int latencySamples{0};
    int tailSamples   {0};
    int parameterCount{0};
};

// Headless AU compatibility tester.
// Instantiates, prepares at 48 kHz/stereo/512, renders silence, checks state
// round-trip, then disposes.  Never opens a native editor window.
// Safe to call on any non-audio thread.  Single-plugin failures do not throw.
class AUCompatibilityTester {
public:
    // Test a single component in-process.  Never throws; errors go into result.status.
    // Do NOT call this from production code — use testIsolated() instead.
    // This function exists as the in-process worker called by the subprocess mode
    // of the host binary and for unit tests.
    static AUTestResult test(const AUComponentEntry& entry);

    // Test a single component in an isolated subprocess.  If the plugin crashes or
    // hangs, the subprocess is killed and a Crashed/TimedOut result is returned —
    // the calling process is never at risk.  timeoutMs is per-plugin (default 30 s).
    static AUTestResult testIsolated(const AUComponentEntry& entry,
                                     int timeoutMs = 30000);

    // Batch test all entries using isolated subprocesses.
    // progress(idx, total, result) is called on the calling thread after each test.
    using ProgressFn = std::function<void(int, int, const AUTestResult&)>;
    static std::vector<AUTestResult> testAll(
        const std::vector<AUComponentEntry>& entries,
        ProgressFn progress = nullptr);

    // Serialise results to compact JSON (no external library dependency).
    static std::string toJson(const std::vector<AUTestResult>& results);
};

} // namespace mcp::plugin
#endif // __APPLE__
