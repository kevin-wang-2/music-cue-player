#pragma once
#include "PeakRegistry.h"   // for kPeakSamplesPerBucket

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <QObject>

namespace mcp { class AudioDecoder; }  // forward-declare for unique_ptr member

// ─── SampleFrame ─────────────────────────────────────────────────────────────
// Decoded PCM for a viewport time range. Immutable after publication.

struct SampleFrame {
    std::string        path;
    std::vector<float> ch[2];   // deinterleaved, up to 2 channels
    int    nSamples{0};
    int    nChannels{0};
    double tL{0.0};             // actual decoded start (frame-aligned)
    double tR{0.0};             // actual decoded end
};

// ─── sampleFrame ─────────────────────────────────────────────────────────────
// Analogous to samplePeaks — maps a time slice to min/max in a SampleFrame.
// Returns false only if the frame has no data.
// Out-of-range tL/tR are clamped to the frame edges (safe for slight drift).
inline bool sampleFrame(const SampleFrame& sf, int ch,
                        double tL, double tR,
                        float& outMin, float& outMax)
{
    if (sf.nSamples <= 0 || sf.tR <= sf.tL) return false;
    const int safeCh = (ch < sf.nChannels) ? ch : 0;
    const double dur = sf.tR - sf.tL;
    const int sL = std::clamp(static_cast<int>((tL - sf.tL) / dur * sf.nSamples),
                              0, sf.nSamples - 1);
    const int sR = std::clamp(static_cast<int>((tR - sf.tL) / dur * sf.nSamples),
                              0, sf.nSamples - 1);
    const auto& data = sf.ch[safeCh];
    outMin = outMax = data[sL];
    for (int s = sL + 1; s <= sR; ++s) {
        outMin = std::min(outMin, data[s]);
        outMax = std::max(outMax, data[s]);
    }
    return true;
}

// ─── ViewportSampler ─────────────────────────────────────────────────────────
// Per-view helper that decodes raw PCM for the visible time range on demand.
// One background thread at QOS_CLASS_UTILITY (same QoS as PeakRegistry workers).
// Last-write-wins: a new request while decoding cancels and supersedes the old one.
// All public methods are safe to call from the GUI thread.
class ViewportSampler {
public:
    ViewportSampler();
    ~ViewportSampler();

    // Returns true when zoomed past peak-bucket resolution
    // (fewer than one full-res bucket maps to one screen pixel).
    static bool needsSampleZoom(double audioSpan, int pixelWidth,
                                 int totalBuckets, double fileDur);

    // Enqueue a decode for [tL, tR] of path.
    // No-op if the current ready frame already covers this exact viewport.
    // callback is queued on context's thread when the new frame is ready.
    void request(const std::string& path, double tL, double tR,
                 QObject* context, std::function<void()> callback);

    // Latest ready frame (null if none). Lock-free snapshot — never blocks.
    std::shared_ptr<const SampleFrame> current() const;

private:
    void workerLoop();

    struct Job {
        std::string           path;
        double                tL{0.0}, tR{0.0};
        QObject*              context{nullptr};
        std::function<void()> callback;
    };

    mutable std::mutex        m_mutex;
    std::condition_variable   m_cv;
    Job                       m_job;
    bool                      m_hasJob{false};
    std::atomic<bool>         m_stopping{false};
    std::shared_ptr<SampleFrame> m_current;   // guarded by m_mutex

    // Last submitted request — used for dedup in request().
    // Stored separately from m_current->tL/tR because the frame's tL/tR are
    // frame-aligned and will never exactly equal the continuous requested values.
    std::string m_reqPath;
    double      m_reqTL{0.0};
    double      m_reqTR{0.0};

    // Cached decoder — worker-thread-only, never touched outside workerLoop.
    // Kept alive across requests for the same path so avformat_find_stream_info
    // is called only once per file (not on every decode), eliminating the
    // 100ms–1s join() stall on app exit when the worker is mid-open.
    std::string                        m_cachedPath;
    std::unique_ptr<mcp::AudioDecoder> m_decoder;

    std::thread m_worker;   // declared last — uses all other members
};
