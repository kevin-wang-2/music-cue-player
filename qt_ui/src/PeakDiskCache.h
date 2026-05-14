#pragma once
#include <cstdint>
#include <string>

struct PeakData;  // defined in PeakRegistry.h

// ─── PeakDiskCache ────────────────────────────────────────────────────────────
// Serialise / deserialise peak data to/from ~/Library/Caches/.../peaks/.
//
// Binary format  "MPKC" v1  (little-endian, assumes x86/ARM host):
//   [0]  4  magic "MPKC"
//   [4]  2  version (uint16) = 1
//   [6]  2  flags   (uint16) = 0  (reserved)
//   [8]  8  source mtime  (int64, seconds since epoch)
//   [16] 4  nameLen (uint32)
//   [20] N  source filename UTF-8  (basename, not full path)
//   ...  8  fileDur  (double)
//   ...  4  fileCh   (int32)  — original channel count of the audio file
//   ...  4  totalBuckets (int32)
//   ...  4  lodSize  (int32)  — kPeakLODSize constant used when writing
//   ...  4  nChStored (int32) — number of channel blocks that follow
//   per-channel block (repeated nChStored times):
//     4  chIdx  (int32)
//     4  nFull  (int32)   — totalBuckets
//     nFull*4  minPk  (float32[])
//     nFull*4  maxPk  (float32[])
//     4  nLod   (int32)   — lodSize
//     nLod*4   lodMin (float32[])
//     nLod*4   lodMax (float32[])
//
// The per-channel layout is open-ended: future versions can store more than
// 2 channels by increasing nChStored.  Readers validate nChStored ≤ their
// own channel limit and skip unrecognised channel indices.

namespace PeakDiskCache {
    // mtime of the audio file in seconds since epoch.  Returns -1 on error.
    int64_t fileMtime(const std::string& audioPath);

    // Serialise a Complete PeakData to disk.  No-op if pd is not Complete.
    // Writes atomically via a .tmp rename so a crash mid-write leaves no
    // corrupt cache.
    void write(const std::string& audioPath, int64_t mtime, const PeakData& pd);

    // Deserialise and populate pd from disk cache.  Returns false on miss,
    // mtime/filename mismatch, or format error (stale file is deleted).
    // On success pd is fully populated and all atomics are published.
    bool read(const std::string& audioPath, int64_t mtime, PeakData& pd);
}
