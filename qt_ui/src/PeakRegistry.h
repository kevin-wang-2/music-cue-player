#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <QObject>

// ─── constants ────────────────────────────────────────────────────────────────

static constexpr int kPeakSamplesPerBucket = 256;   // full-res: 256 audio samples → 1 bucket
static constexpr int kPeakLODSize          = 1024;  // coarse LOD: fixed 1024 entries per file

// ─── ScanStatus ───────────────────────────────────────────────────────────────

enum class ScanStatus : uint8_t {
    Pending  = 0,  // D: queued, not yet started (or suspended and re-queued)
    Scanning = 1,  // C: actively being scanned by a worker
    Complete = 2,  // A: scan done, data in memory
    Evicted  = 3,  // B: scan done, data released (reserved for future use)
};

// ─── PeakData ─────────────────────────────────────────────────────────────────
// Heap-allocated, owned by PeakRegistry; views hold shared_ptr<PeakData>.
// Worker writes sequentially; renderer reads using release/acquire on nFilled/nFilledLOD.
//
// Memory ordering contract:
//   Worker: write fileDur/fileCh/totalBuckets/lodFactor + allocate vectors,
//           then metadataReady.store(true, release).
//   Worker: write minPk/maxPk[0..bi), then nFilled.store(bi, release).
//   Worker: write lodMin/lodMax[0..li), then nFilledLOD.store(li, release).
//   Renderer: load metadataReady(acquire) before reading metadata fields.
//             load nFilled(acquire) before reading minPk/maxPk[0..nFilled).
//             load nFilledLOD(acquire) before reading lodMin/lodMax[0..nFilledLOD).

struct PeakData {
    // Immutable once metadataReady is set (safe to read after acquire-load on metadataReady)
    int    totalBuckets{0};
    int    lodFactor{1};    // totalBuckets / kPeakLODSize
    double fileDur{0.0};
    int    fileCh{0};

    // Full-res peak arrays [totalBuckets] — written by worker, published via nFilled
    std::vector<float> minPk[2];
    std::vector<float> maxPk[2];

    // Coarse LOD arrays [kPeakLODSize] — written by worker, published via nFilledLOD
    std::vector<float> lodMin[2];
    std::vector<float> lodMax[2];

    std::atomic<int>        nFilled{0};           // full-res buckets ready (release/acquire)
    std::atomic<int>        nFilledLOD{0};        // LOD entries ready (release/acquire)
    std::atomic<bool>       metadataReady{false}; // set after alloc + metadata written
    std::atomic<ScanStatus> status{ScanStatus::Pending};
};

// ─── samplePeaks ──────────────────────────────────────────────────────────────
// Shared rendering helper used by all waveform views.
// nFull / nLOD must be acquired once per paint (acquire-load), then passed here.
// Returns false if tL is beyond loaded data (stop drawing).
inline bool samplePeaks(const PeakData& pd, int ch,
                         double tL, double tR,
                         int nFull, int nLOD,
                         float& outMin, float& outMax)
{
    if (pd.fileDur <= 0.0 || pd.totalBuckets <= 0 || nFull <= 0) return false;

    const double invDur = 1.0 / pd.fileDur;
    const int bL = static_cast<int>(tL * invDur * pd.totalBuckets);
    if (bL >= nFull) return false;                              // not loaded yet → stop
    const int bR = static_cast<int>(tR * invDur * pd.totalBuckets);

    const int range = bR - bL;

    if (range >= pd.lodFactor && nLOD > 0) {
        // Use coarse LOD
        const int lL = std::clamp(static_cast<int>(tL * invDur * kPeakLODSize), 0, nLOD - 1);
        const int lR = std::clamp(static_cast<int>(tR * invDur * kPeakLODSize), 0, nLOD - 1);
        outMin = pd.lodMin[ch][lL];
        outMax = pd.lodMax[ch][lL];
        for (int l = lL + 1; l <= lR; ++l) {
            outMin = std::min(outMin, pd.lodMin[ch][l]);
            outMax = std::max(outMax, pd.lodMax[ch][l]);
        }
    } else {
        // Use full-res
        const int bLc = std::clamp(bL, 0, nFull - 1);
        const int bRc = std::clamp(bR, 0, nFull - 1);
        outMin = pd.minPk[ch][bLc];
        outMax = pd.maxPk[ch][bLc];
        for (int b = bLc + 1; b <= bRc; ++b) {
            outMin = std::min(outMin, pd.minPk[ch][b]);
            outMax = std::max(outMax, pd.maxPk[ch][b]);
        }
    }
    return true;
}

// ─── PeakRegistry ─────────────────────────────────────────────────────────────
// Singleton. Manages background scanning of audio files and distributes
// PeakData to subscribers.
class PeakRegistry : public QObject {
    Q_OBJECT
public:
    static PeakRegistry& instance();

    // Returns (or creates) PeakData for path; enqueues a scan job if new.
    // Jobs are ordered shortest-first (by file size as a proxy for duration).
    std::shared_ptr<PeakData> requestScan(const std::string& path);

    // Like requestScan but also pushes the job to the front of the LIFO so it
    // is picked up before anything in the normal priority queue.  Call this from
    // views that are actively displaying a cue — view activation implies the
    // user is waiting for this data right now.
    std::shared_ptr<PeakData> boostScan(const std::string& path);

    // Subscribe to progress/completion updates for path.
    // callback is invoked (Qt::QueuedConnection) on context's thread.
    // If metadata is already available, fires once immediately.
    // Returns a token for unsubscription.
    int subscribe(const std::string& path, QObject* context,
                  std::function<void()> callback);

    // Cancel all queued (Pending) scan jobs and remove their m_data entries.
    // Call before loading a new project to avoid scanning files from the old
    // project.  In-flight Scanning jobs complete normally.
    void cancelPendingScans();

    // Remove a subscription (idempotent).
    void unsubscribe(int token);

    // Pause all worker threads at the next bucket boundary.
    // In-progress scans are re-queued from their current nFilled position.
    // Async — workers may complete their current bucket before stopping.
    void suspend();

    // Resume scanning after suspend().
    void resume();

    // Stop all worker threads and clear subscriptions.
    // Connected automatically to QCoreApplication::aboutToQuit so workers are
    // stopped before any Qt objects are destroyed.  Idempotent; safe to call
    // explicitly before that signal fires.
    void shutdown();

private:
    explicit PeakRegistry(QObject* parent = nullptr);
    ~PeakRegistry() override;

    void workerLoop();
    void scanFile(const std::string& path, const std::shared_ptr<PeakData>& pd);
    void notifySubscribers(const std::string& path);

    // LRU helpers — all must be called under m_mutex.
    void   touchLRU(const std::string& path);
    void   addToLRU(const std::string& path, size_t bytes);
    void   evictIfNeeded();
    static size_t peakDataMemBytes(const PeakData& pd);

    struct ScanJob {
        int         priority{0};
        std::string path;
        bool operator<(const ScanJob& o) const { return priority < o.priority; }
    };

    struct Subscription {
        std::string           path;
        int                   token{0};
        QObject*              context{nullptr};
        std::function<void()> callback;
    };

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;

    std::unordered_map<std::string, std::shared_ptr<PeakData>> m_data;
    std::priority_queue<ScanJob>                                m_queue;
    std::unordered_map<int, Subscription>                       m_subs;

    // LRU eviction (front = most recently used, back = least recently used).
    // Only Complete entries are tracked; eviction replaces them with Evicted markers.
    static constexpr size_t kMaxMemBytes = 1ULL * 1024 * 1024 * 1024;  // 1 GB
    std::list<std::string>                                             m_lruList;
    std::unordered_map<std::string, std::list<std::string>::iterator>  m_lruIter;
    size_t                                                             m_memUsed{0};

    // LIFO urgent queue — workers drain this before the normal priority queue.
    // Entries are pushed by boostScan (front) and dropped from the back when full.
    static constexpr size_t     kLifoMaxSize = 32;
    std::deque<std::string>     m_lifo;

    std::atomic<int>         m_nextToken{1};
    std::vector<std::thread> m_workers;
    std::atomic<bool>        m_stopping{false};
    std::atomic<bool>        m_suspended{false};
};
