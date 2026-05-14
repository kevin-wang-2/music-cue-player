#include "PeakDiskCache.h"
#include "PeakRegistry.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// ─── internal helpers ─────────────────────────────────────────────────────────

namespace {

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static const std::string& peakCacheDir() {
    static const std::string dir = []() {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QString d    = base + "/peaks";
        QDir().mkpath(d);
        return d.toStdString();
    }();
    return dir;
}

static std::string cacheFilePath(const std::string& audioPath) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx.mpkc",
             static_cast<unsigned long long>(fnv1a64(audioPath)));
    return peakCacheDir() + "/" + buf;
}

static std::string fileBasename(const std::string& path) {
    const auto pos = path.rfind('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static bool writeImpl(FILE* f, int64_t mtime,
                      const std::string& name, const PeakData& pd) {
    const int dispCh  = std::min(pd.fileCh, 2);
    const int nFull   = pd.totalBuckets;
    const int nLod    = kPeakLODSize;
    const uint32_t nl = static_cast<uint32_t>(name.size());

    auto wb = [f](const void* p, size_t n) { return fwrite(p, 1, n, f) == n; };
    auto wu16 = [&](uint16_t v) { return wb(&v, 2); };
    auto wu32 = [&](uint32_t v) { return wb(&v, 4); };
    auto wi32 = [&](int32_t  v) { return wb(&v, 4); };
    auto wi64 = [&](int64_t  v) { return wb(&v, 8); };
    auto wd64 = [&](double   v) { return wb(&v, 8); };

    return wb("MPKC", 4)
        && wu16(1)          // version
        && wu16(0)          // flags
        && wi64(mtime)
        && wu32(nl)
        && wb(name.data(), nl)
        && wd64(pd.fileDur)
        && wi32(pd.fileCh)
        && wi32(nFull)
        && wi32(nLod)
        && wi32(dispCh)     // nChStored
        && [&]() {
               for (int c = 0; c < dispCh; ++c) {
                   if (!wi32(c)    || !wi32(nFull)
                   || !wb(pd.minPk[c].data(),  nFull * sizeof(float))
                   || !wb(pd.maxPk[c].data(),  nFull * sizeof(float))
                   || !wi32(nLod)
                   || !wb(pd.lodMin[c].data(), nLod  * sizeof(float))
                   || !wb(pd.lodMax[c].data(), nLod  * sizeof(float)))
                       return false;
               }
               return true;
           }();
}

} // namespace

// ─── public API ───────────────────────────────────────────────────────────────

namespace PeakDiskCache {

int64_t fileMtime(const std::string& audioPath) {
    struct stat st;
    if (stat(audioPath.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_mtime);
}

void write(const std::string& audioPath, int64_t mtime, const PeakData& pd) {
    if (pd.status.load(std::memory_order_acquire) != ScanStatus::Complete) return;

    const std::string cpath   = cacheFilePath(audioPath);
    const std::string tmpPath = cpath + ".tmp";

    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return;

    const bool ok = writeImpl(f, mtime, fileBasename(audioPath), pd);
    fclose(f);

    if (ok)
        std::rename(tmpPath.c_str(), cpath.c_str());
    else
        std::remove(tmpPath.c_str());
}

bool read(const std::string& audioPath, int64_t mtime, PeakData& pd) {
    const std::string cpath = cacheFilePath(audioPath);
    FILE* f = fopen(cpath.c_str(), "rb");
    if (!f) return false;

    auto fail = [&]() -> bool {
        fclose(f);
        std::remove(cpath.c_str());  // delete stale / corrupt cache
        return false;
    };

    auto rb  = [&](void* p, size_t n) { return fread(p, 1, n, f) == n; };
    auto ru16 = [&](uint16_t& v) { return rb(&v, 2); };
    auto ru32 = [&](uint32_t& v) { return rb(&v, 4); };
    auto ri32 = [&](int32_t&  v) { return rb(&v, 4); };
    auto ri64 = [&](int64_t&  v) { return rb(&v, 8); };
    auto rd64 = [&](double&   v) { return rb(&v, 8); };

    // Magic + version
    char magic[4];
    uint16_t version, flags;
    if (!rb(magic, 4) || memcmp(magic, "MPKC", 4) != 0) return fail();
    if (!ru16(version) || version != 1) return fail();
    if (!ru16(flags)) return fail();

    // mtime check
    int64_t storedMtime;
    if (!ri64(storedMtime) || storedMtime != mtime) return fail();

    // Filename check (basename)
    uint32_t nameLen;
    if (!ru32(nameLen) || nameLen > 4096) return fail();
    std::string storedName(nameLen, '\0');
    if (!rb(storedName.data(), nameLen)) return fail();
    if (storedName != fileBasename(audioPath)) return fail();

    // Metadata
    double  fileDur;
    int32_t fileCh, totalBuckets, lodSize, nChStored;
    if (!rd64(fileDur))                              return fail();
    if (!ri32(fileCh))                               return fail();
    if (!ri32(totalBuckets) || totalBuckets <= 0)    return fail();
    if (!ri32(lodSize) || lodSize != kPeakLODSize)   return fail();
    if (!ri32(nChStored) || nChStored < 1 || nChStored > 2) return fail();

    pd.fileDur      = fileDur;
    pd.fileCh       = fileCh;
    pd.totalBuckets = totalBuckets;
    pd.lodFactor    = std::max(1, totalBuckets / kPeakLODSize);

    // Allocate all channels zeroed; channel blocks below fill in stored ones.
    for (int c = 0; c < 2; ++c) {
        pd.minPk[c].assign(totalBuckets, 0.0f);
        pd.maxPk[c].assign(totalBuckets, 0.0f);
        pd.lodMin[c].assign(kPeakLODSize, 0.0f);
        pd.lodMax[c].assign(kPeakLODSize, 0.0f);
    }

    // Per-channel blocks
    for (int i = 0; i < nChStored; ++i) {
        int32_t chIdx, nFull, nLod;
        if (!ri32(chIdx) || chIdx < 0 || chIdx > 1) return fail();
        if (!ri32(nFull) || nFull != totalBuckets)   return fail();
        if (!rb(pd.minPk[chIdx].data(), nFull * sizeof(float))) return fail();
        if (!rb(pd.maxPk[chIdx].data(), nFull * sizeof(float))) return fail();
        if (!ri32(nLod) || nLod != kPeakLODSize)     return fail();
        if (!rb(pd.lodMin[chIdx].data(), nLod * sizeof(float))) return fail();
        if (!rb(pd.lodMax[chIdx].data(), nLod * sizeof(float))) return fail();
    }

    fclose(f);

    // Publish atomics in acquire/release order expected by renderers.
    pd.metadataReady.store(true,         std::memory_order_release);
    pd.nFilledLOD.store(kPeakLODSize,    std::memory_order_release);
    pd.nFilled.store(totalBuckets,       std::memory_order_release);
    return true;
}

} // namespace PeakDiskCache
