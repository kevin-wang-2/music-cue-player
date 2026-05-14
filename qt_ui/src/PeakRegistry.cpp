#include "PeakRegistry.h"
#include "PeakDiskCache.h"
#include "engine/AudioDecoder.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <limits>
#include <pthread.h>

// ─── singleton ────────────────────────────────────────────────────────────────

PeakRegistry& PeakRegistry::instance() {
    static PeakRegistry inst;
    return inst;
}

PeakRegistry::PeakRegistry(QObject* parent) : QObject(parent) {
    const unsigned int hc = std::thread::hardware_concurrency();
    const int n = (hc >= 3) ? std::min(4, (int)hc - 2) : 1;
    m_workers.reserve(n);
    for (int i = 0; i < n; ++i)
        m_workers.emplace_back(&PeakRegistry::workerLoop, this);

    if (auto* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &PeakRegistry::shutdown,
                Qt::DirectConnection);
}

PeakRegistry::~PeakRegistry() {
    shutdown();
}

// ─── public API ───────────────────────────────────────────────────────────────

std::shared_ptr<PeakData> PeakRegistry::requestScan(const std::string& path) {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto it = m_data.find(path);
    if (it != m_data.end()) {
        const auto st = it->second->status.load(std::memory_order_acquire);
        if (st == ScanStatus::Complete) {
            touchLRU(path);
            return it->second;
        }
        if (st == ScanStatus::Pending || st == ScanStatus::Scanning)
            return it->second;
        // Evicted: re-queue for disk reload.
        m_queue.push({0, path});
        m_cv.notify_one();
        return it->second;
    }

    auto pd = std::make_shared<PeakData>();
    m_data[path] = pd;
    m_queue.push({0, path});
    m_cv.notify_one();
    return pd;
}

int PeakRegistry::subscribe(const std::string& path, QObject* context,
                              std::function<void()> callback) {
    bool fireNow = false;
    int token;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        token = m_nextToken.fetch_add(1, std::memory_order_relaxed);
        m_subs[token] = {path, token, context, callback};

        auto it = m_data.find(path);
        if (it != m_data.end() &&
            it->second->metadataReady.load(std::memory_order_acquire))
            fireNow = true;
    }
    if (fireNow)
        QMetaObject::invokeMethod(context, std::move(callback), Qt::QueuedConnection);
    return token;
}

void PeakRegistry::unsubscribe(int token) {
    if (token <= 0) return;
    std::unique_lock<std::mutex> lock(m_mutex);
    m_subs.erase(token);
}

void PeakRegistry::suspend() {
    m_suspended.store(true, std::memory_order_release);
}

void PeakRegistry::resume() {
    m_suspended.store(false, std::memory_order_release);
    m_cv.notify_all();
}

void PeakRegistry::shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stopping.exchange(true)) return;
        m_suspended.store(false);
        m_subs.clear();
    }
    m_cv.notify_all();
    for (auto& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
}

// ─── LRU helpers (all called under m_mutex) ───────────────────────────────────

size_t PeakRegistry::peakDataMemBytes(const PeakData& pd) {
    size_t n = 0;
    for (int c = 0; c < 2; ++c) {
        n += pd.minPk[c].capacity()  * sizeof(float);
        n += pd.maxPk[c].capacity()  * sizeof(float);
        n += pd.lodMin[c].capacity() * sizeof(float);
        n += pd.lodMax[c].capacity() * sizeof(float);
    }
    return n;
}

void PeakRegistry::touchLRU(const std::string& path) {
    auto it = m_lruIter.find(path);
    if (it != m_lruIter.end())
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
}

void PeakRegistry::addToLRU(const std::string& path, size_t bytes) {
    if (m_lruIter.count(path)) {
        touchLRU(path);
        return;
    }
    m_lruList.push_front(path);
    m_lruIter[path] = m_lruList.begin();
    m_memUsed += bytes;
}

void PeakRegistry::evictIfNeeded() {
    while (m_memUsed > kMaxMemBytes && !m_lruList.empty()) {
        const std::string evictPath = m_lruList.back();
        m_lruList.pop_back();
        m_lruIter.erase(evictPath);

        auto it = m_data.find(evictPath);
        if (it == m_data.end()) continue;

        auto& old = it->second;
        if (old->status.load(std::memory_order_acquire) != ScanStatus::Complete)
            continue;  // cannot evict Scanning/Pending; just drop from LRU tracking

        const size_t bytes = peakDataMemBytes(*old);
        m_memUsed = (m_memUsed >= bytes) ? m_memUsed - bytes : 0;

        // Replace registry entry with a lightweight Evicted marker.
        // The old PeakData stays alive (and usable) until all callers release it.
        auto fresh = std::make_shared<PeakData>();
        fresh->status.store(ScanStatus::Evicted, std::memory_order_release);
        it->second = fresh;
    }
}

// ─── worker ───────────────────────────────────────────────────────────────────

void PeakRegistry::workerLoop() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    while (true) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return (!m_queue.empty() && !m_suspended.load(std::memory_order_relaxed))
                    || m_stopping.load(std::memory_order_relaxed);
            });
            if (m_stopping.load() && m_queue.empty()) break;
            if (m_suspended.load() || m_queue.empty()) continue;
            path = m_queue.top().path;
            m_queue.pop();
        }

        std::shared_ptr<PeakData> pd;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_data.find(path);
            if (it == m_data.end()) continue;
            pd = it->second;
        }

        const auto curStatus = pd->status.load(std::memory_order_acquire);
        if (curStatus == ScanStatus::Complete) continue;

        // ── Try disk cache (both fresh Pending and Evicted re-loads) ─────────
        if (curStatus == ScanStatus::Pending || curStatus == ScanStatus::Evicted) {
            const int64_t mtime = PeakDiskCache::fileMtime(path);
            if (mtime > 0 && PeakDiskCache::read(path, mtime, *pd)) {
                pd->status.store(ScanStatus::Complete, std::memory_order_release);
                if (!m_stopping.load(std::memory_order_acquire)) {
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        addToLRU(path, peakDataMemBytes(*pd));
                        evictIfNeeded();
                    }
                    notifySubscribers(path);
                }
                continue;
            }
        }

        // ── Disk miss → scan from audio ───────────────────────────────────────
        pd->status.store(ScanStatus::Scanning, std::memory_order_release);
        scanFile(path, pd);

        // ── Post-scan: write to disk and register in LRU ──────────────────────
        if (pd->status.load(std::memory_order_acquire) == ScanStatus::Complete
                && !m_stopping.load(std::memory_order_acquire)) {
            const int64_t mtime = PeakDiskCache::fileMtime(path);
            if (mtime > 0) PeakDiskCache::write(path, mtime, *pd);
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                addToLRU(path, peakDataMemBytes(*pd));
                evictIfNeeded();
            }
        }
    }
}

void PeakRegistry::scanFile(const std::string& path, const std::shared_ptr<PeakData>& pd) {
    std::string err;
    auto dec = mcp::AudioDecoder::open(path, err);
    if (!dec) return;

    constexpr float kMax    = std::numeric_limits<float>::max();
    const bool      resuming = pd->metadataReady.load(std::memory_order_acquire);

    int totalBuckets, lodFactor, fileCh;

    if (!resuming) {
        const int     fc     = dec->nativeChannels();
        const int     sr     = dec->nativeSampleRate();
        const int64_t frames = dec->nativeFrameCount();
        if (fc <= 0 || sr <= 0 || frames <= 0) return;

        totalBuckets = std::max(1, static_cast<int>(frames / kPeakSamplesPerBucket));
        lodFactor    = std::max(1, totalBuckets / kPeakLODSize);
        fileCh       = fc;

        pd->fileDur      = static_cast<double>(frames) / sr;
        pd->fileCh       = fc;
        pd->totalBuckets = totalBuckets;
        pd->lodFactor    = lodFactor;

        for (int c = 0; c < 2; ++c) {
            pd->minPk[c].assign(static_cast<size_t>(totalBuckets),  kMax);
            pd->maxPk[c].assign(static_cast<size_t>(totalBuckets), -kMax);
            pd->lodMin[c].assign(kPeakLODSize,  kMax);
            pd->lodMax[c].assign(kPeakLODSize, -kMax);
        }

        pd->metadataReady.store(true, std::memory_order_release);
        notifySubscribers(path);
    } else {
        totalBuckets = pd->totalBuckets;
        lodFactor    = pd->lodFactor;
        fileCh       = pd->fileCh;
    }

    const int dispCh = std::min(fileCh, 2);

    // ── Seek to resume position ───────────────────────────────────────────────
    int bi = pd->nFilled.load(std::memory_order_acquire);
    if (bi > 0) {
        if (!dec->seekToFrame(static_cast<int64_t>(bi) * kPeakSamplesPerBucket)) {
            bi = 0;
            pd->nFilled.store(0, std::memory_order_release);
            pd->nFilledLOD.store(0, std::memory_order_release);
            for (int c = 0; c < 2; ++c) {
                pd->minPk[c].assign(static_cast<size_t>(totalBuckets),  kMax);
                pd->maxPk[c].assign(static_cast<size_t>(totalBuckets), -kMax);
                pd->lodMin[c].assign(kPeakLODSize,  kMax);
                pd->lodMax[c].assign(kPeakLODSize, -kMax);
            }
        }
    }

    // ── Scan ─────────────────────────────────────────────────────────────────
    constexpr int64_t kBufBytes = 1 << 20;
    const int64_t kBuf = std::max(int64_t(1024),
                                   kBufBytes / static_cast<int64_t>(sizeof(float) * fileCh));
    std::vector<float> buf(static_cast<size_t>(kBuf * fileCh));

    int  samples   = 0;
    bool suspended = false;

    using Clock = std::chrono::steady_clock;
    auto lastReport = Clock::now();

    while (bi < totalBuckets && !suspended) {
        const int64_t got = dec->readFloat(buf.data(), kBuf);
        if (got <= 0) break;

        const float* p = buf.data();
        for (int64_t f = 0; f < got && bi < totalBuckets && !suspended; ++f, p += fileCh) {
            for (int c = 0; c < dispCh; ++c) {
                const float s = p[c];
                if (s < pd->minPk[c][bi]) pd->minPk[c][bi] = s;
                if (s > pd->maxPk[c][bi]) pd->maxPk[c][bi] = s;
            }

            if (++samples == kPeakSamplesPerBucket) {
                samples = 0;

                const int li = bi / lodFactor;
                if (li < kPeakLODSize) {
                    for (int c = 0; c < dispCh; ++c) {
                        if (pd->minPk[c][bi] < pd->lodMin[c][li])
                            pd->lodMin[c][li] = pd->minPk[c][bi];
                        if (pd->maxPk[c][bi] > pd->lodMax[c][li])
                            pd->lodMax[c][li] = pd->maxPk[c][bi];
                    }
                }

                ++bi;
                pd->nFilled.store(bi, std::memory_order_release);

                if (bi % lodFactor == 0)
                    pd->nFilledLOD.store(bi / lodFactor, std::memory_order_release);

                if (m_suspended.load(std::memory_order_acquire)
                        || m_stopping.load(std::memory_order_acquire))
                    suspended = true;
            }
        }

        if (!suspended) {
            const auto now = Clock::now();
            if (now - lastReport >= std::chrono::milliseconds(250)) {
                lastReport = now;
                notifySubscribers(path);
            }
        }
    }

    // ── Handle suspension ─────────────────────────────────────────────────────
    if (suspended) {
        if (bi < totalBuckets) {
            for (int c = 0; c < 2; ++c) {
                pd->minPk[c][bi] =  kMax;
                pd->maxPk[c][bi] = -kMax;
            }
        }
        pd->status.store(ScanStatus::Pending, std::memory_order_release);
        if (!m_stopping.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push({0, path});
        }
        return;
    }

    // ── Zero sentinel values ──────────────────────────────────────────────────
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < totalBuckets; ++i)
            if (pd->minPk[c][i] > pd->maxPk[c][i])
                pd->minPk[c][i] = pd->maxPk[c][i] = 0.0f;
        for (int i = 0; i < kPeakLODSize; ++i)
            if (pd->lodMin[c][i] > pd->lodMax[c][i])
                pd->lodMin[c][i] = pd->lodMax[c][i] = 0.0f;
    }

    pd->nFilledLOD.store(kPeakLODSize, std::memory_order_release);
    pd->status.store(ScanStatus::Complete, std::memory_order_release);
    notifySubscribers(path);
}

void PeakRegistry::notifySubscribers(const std::string& path) {
    std::vector<std::pair<QObject*, std::function<void()>>> toNotify;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (auto& [token, sub] : m_subs)
            if (sub.path == path)
                toNotify.emplace_back(sub.context, sub.callback);
    }
    for (auto& [ctx, cb] : toNotify)
        QMetaObject::invokeMethod(ctx, cb, Qt::QueuedConnection);
}
