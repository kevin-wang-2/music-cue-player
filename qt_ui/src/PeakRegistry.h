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

static constexpr int kPeakSamplesPerBucket           = 256;  // full-res: 256 audio samples → 1 bucket
static constexpr int kPeakNumLOD                     = 3;    // number of LOD levels
static constexpr int kPeakLODSizes[kPeakNumLOD]      = {16384, 4096, 1024};  // fine → coarse

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
//   Worker: write fileDur/fileCh/totalBuckets/lodFactor[] + allocate vectors,
//           then metadataReady.store(true, release).
//   Worker: write minPk/maxPk[0..bi), then nFilled.store(bi, release).
//   Worker: write lodMin/lodMax[][k][0..li), then nFilledLOD[k].store(li, release).
//   Renderer: load metadataReady(acquire) before reading metadata fields.
//             load nFilled(acquire) before reading minPk/maxPk[0..nFilled).
//             load nFilledLOD[k](acquire) before reading lodMin/lodMax[][k][0..nFilledLOD[k]).

struct PeakData {
    PeakData() noexcept {
        for (int k = 0; k < kPeakNumLOD; ++k) {
            lodFactor[k] = 1;
            nFilledLOD[k].store(0, std::memory_order_relaxed);
        }
    }
    PeakData(const PeakData&) = delete;
    PeakData& operator=(const PeakData&) = delete;

    // Immutable once metadataReady is set (safe to read after acquire-load on metadataReady)
    int    totalBuckets{0};
    int    lodFactor[kPeakNumLOD];  // totalBuckets / kPeakLODSizes[k] (ceiling div), init in ctor
    double fileDur{0.0};
    int    fileCh{0};

    // Full-res peak arrays [totalBuckets] — written by worker, published via nFilled
    std::vector<float> minPk[2];
    std::vector<float> maxPk[2];

    // Multi-level LOD arrays [channel][lod_level][entries] — fine→coarse
    // Level sizes: kPeakLODSizes[0]=16384, [1]=4096, [2]=1024
    std::vector<float> lodMin[2][kPeakNumLOD];
    std::vector<float> lodMax[2][kPeakNumLOD];

    std::atomic<int>        nFilled{0};              // full-res buckets ready (release/acquire)
    std::atomic<int>        nFilledLOD[kPeakNumLOD]; // LOD entries ready per level (init in ctor)
    std::atomic<bool>       metadataReady{false};     // set after alloc + metadata written
    std::atomic<ScanStatus> status{ScanStatus::Pending};
};

// ─── samplePeaks ──────────────────────────────────────────────────────────────
// Shared rendering helper used by all waveform views.
// nFull must be acquired once per paint (acquire-load on nFilled) and passed here.
// LOD level nFilledLOD[k] is acquired internally for each qualifying level.
// Returns false if tL is beyond loaded data (stop drawing).
inline bool samplePeaks(const PeakData& pd, int ch,
                         double tL, double tR,
                         int nFull,
                         float& outMin, float& outMax)
{
    if (pd.fileDur <= 0.0 || pd.totalBuckets <= 0 || nFull <= 0) return false;

    const double invDur = 1.0 / pd.fileDur;
    const int bL = static_cast<int>(tL * invDur * pd.totalBuckets);
    if (bL >= nFull) return false;  // not loaded yet → stop
    const int bR = static_cast<int>(tR * invDur * pd.totalBuckets);
    const int range = bR - bL;

    // Try LOD levels fine → coarse; use finest qualifying level.
    // A level qualifies when range * 4 >= lodFactor (1 LOD entry spans ≤ 4 screen pixels).
    for (int k = 0; k < kPeakNumLOD; ++k) {
        if (range < pd.lodFactor[k]) continue;
        const int nLODk = pd.nFilledLOD[k].load(std::memory_order_acquire);
        if (nLODk <= 0) continue;
        // Map time → LOD entry using the same formula as the scanner:
        //   li = bi / lodFactor[k],  bi = t/fileDur * totalBuckets
        // so  li = t/fileDur * totalBuckets / lodFactor[k]
        const double lodScale = static_cast<double>(pd.totalBuckets) / pd.lodFactor[k];
        const int lL = std::clamp(static_cast<int>(tL * invDur * lodScale), 0, nLODk - 1);
        const int lR = std::clamp(static_cast<int>(tR * invDur * lodScale), 0, nLODk - 1);
        outMin = pd.lodMin[ch][k][lL];
        outMax = pd.lodMax[ch][k][lL];
        for (int l = lL + 1; l <= lR; ++l) {
            outMin = std::min(outMin, pd.lodMin[ch][k][l]);
            outMax = std::max(outMax, pd.lodMax[ch][k][l]);
        }
        return true;
    }

    // Full-res fallback
    const int bLc = std::clamp(bL, 0, nFull - 1);
    const int bRc = std::clamp(bR, 0, nFull - 1);
    outMin = pd.minPk[ch][bLc];
    outMax = pd.maxPk[ch][bLc];
    for (int b = bLc + 1; b <= bRc; ++b) {
        outMin = std::min(outMin, pd.minPk[ch][b]);
        outMax = std::max(outMax, pd.maxPk[ch][b]);
    }
    return true;
}

// ─── samplePeaksDbg ───────────────────────────────────────────────────────────
// Debug variant: same as samplePeaks but also increments per-call counters and
// records which LOD level was used (-1 = full-res, 0..kPeakNumLOD-1 = LOD level).
inline bool samplePeaksDbg(const PeakData& pd, int ch,
                             double tL, double tR,
                             int nFull,
                             float& outMin, float& outMax,
                             int& lastLODLevel,
                             int& lodCalls, int& lodBuckets,
                             int& fullCalls, int& fullBuckets)
{
    if (pd.fileDur <= 0.0 || pd.totalBuckets <= 0 || nFull <= 0) return false;

    const double invDur = 1.0 / pd.fileDur;
    const int bL = static_cast<int>(tL * invDur * pd.totalBuckets);
    if (bL >= nFull) return false;
    const int bR = static_cast<int>(tR * invDur * pd.totalBuckets);
    const int range = bR - bL;

    for (int k = 0; k < kPeakNumLOD; ++k) {
        if (range < pd.lodFactor[k]) continue;
        const int nLODk = pd.nFilledLOD[k].load(std::memory_order_acquire);
        if (nLODk <= 0) continue;
        const double lodScale = static_cast<double>(pd.totalBuckets) / pd.lodFactor[k];
        const int lL = std::clamp(static_cast<int>(tL * invDur * lodScale), 0, nLODk - 1);
        const int lR = std::clamp(static_cast<int>(tR * invDur * lodScale), 0, nLODk - 1);
        outMin = pd.lodMin[ch][k][lL];
        outMax = pd.lodMax[ch][k][lL];
        for (int l = lL + 1; l <= lR; ++l) {
            outMin = std::min(outMin, pd.lodMin[ch][k][l]);
            outMax = std::max(outMax, pd.lodMax[ch][k][l]);
        }
        lastLODLevel = k;
        ++lodCalls;
        lodBuckets += lR - lL + 1;
        return true;
    }

    const int bLc = std::clamp(bL, 0, nFull - 1);
    const int bRc = std::clamp(bR, 0, nFull - 1);
    outMin = pd.minPk[ch][bLc];
    outMax = pd.maxPk[ch][bLc];
    for (int b = bLc + 1; b <= bRc; ++b) {
        outMin = std::min(outMin, pd.minPk[ch][b]);
        outMax = std::max(outMax, pd.maxPk[ch][b]);
    }
    lastLODLevel = -1;
    ++fullCalls;
    fullBuckets += bRc - bLc + 1;
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
