#include "MCImport.h"

#include <QXmlStreamReader>
#include <QFile>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Common helpers

struct RawPoint {
    int    bar{1};
    int    beat{1};
    double bpm{120.0};
    bool   isRamp{false};
    bool   hasTimeSig{false};
    int    timeSigNum{4};
    int    timeSigDen{4};
};

// Merge two raw-point collections (tempo + time-sig) into final points sorted by (bar,beat).
// When a tempo event and time-sig event coincide at the same (bar,beat), they are merged.
static std::vector<RawPoint> mergeAndSort(std::vector<RawPoint>& pts) {
    // Sort by (bar, beat)
    std::stable_sort(pts.begin(), pts.end(), [](const RawPoint& a, const RawPoint& b) {
        if (a.bar != b.bar) return a.bar < b.bar;
        return a.beat < b.beat;
    });

    // Merge consecutive entries at the same (bar, beat)
    std::vector<RawPoint> merged;
    for (auto& p : pts) {
        if (!merged.empty() && merged.back().bar == p.bar && merged.back().beat == p.beat) {
            // Merge: favour whichever fields are set
            auto& m = merged.back();
            if (p.hasTimeSig) {
                m.hasTimeSig = true;
                m.timeSigNum = p.timeSigNum;
                m.timeSigDen = p.timeSigDen;
            }
            // For bpm / isRamp: keep non-default if one is set
            if (p.bpm != 120.0 || !m.hasTimeSig) m.bpm = p.bpm;
            if (p.isRamp) m.isRamp = p.isRamp;
        } else {
            merged.push_back(p);
        }
    }
    return merged;
}

// Ensure merged points are valid for MusicContext:
// - First point must have isRamp=false and hasTimeSig=true
// - If bar1/beat1 is missing, prepend a default 4/4 with first BPM
static void finalisePoints(std::vector<RawPoint>& pts) {
    if (pts.empty()) return;

    // Ensure first point has timeSig
    if (!pts[0].hasTimeSig) {
        pts[0].hasTimeSig = true;
        pts[0].timeSigNum = 4;
        pts[0].timeSigDen = 4;
    }
    // First point must not be a ramp
    pts[0].isRamp = false;

    // If first point is not at bar 1 beat 1, prepend one
    if (pts[0].bar != 1 || pts[0].beat != 1) {
        RawPoint first;
        first.bar        = 1;
        first.beat       = 1;
        first.bpm        = pts[0].bpm;
        first.isRamp     = false;
        first.hasTimeSig = true;
        first.timeSigNum = 4;
        first.timeSigDen = 4;
        pts.insert(pts.begin(), first);
    }
}

// Convert raw points into MusicContext points and install them.
static void installPoints(const std::vector<RawPoint>& rpts, mcp::MusicContext& mc) {
    mc.points.clear();
    for (const auto& rp : rpts) {
        mcp::MusicContext::Point p;
        p.bar        = rp.bar;
        p.beat       = rp.beat;
        p.bpm        = rp.bpm;
        p.isRamp     = rp.isRamp;
        p.hasTimeSig = rp.hasTimeSig;
        p.timeSigNum = rp.timeSigNum;
        p.timeSigDen = rp.timeSigDen;
        mc.points.push_back(p);
    }
    mc.markDirty();
}

// ---------------------------------------------------------------------------
// Tick → (bar, beat) conversion helpers

struct TimeSigZone {
    int64_t tick{0};    // absolute tick position where this zone starts
    int    barOffset{0}; // 1-indexed bar number at the start of this zone
    int    num{4};
    int    den{4};       // actual denominator (not power-of-2)
    int    tpq{480};     // ticks per quarter note
};

// From a list of TimeSigZones (sorted by tick) and a tick position, compute (bar, beat).
// Returns 1-indexed bar and beat.
static std::pair<int,int> tickToBarBeat(int64_t tick,
                                         const std::vector<TimeSigZone>& zones) {
    if (zones.empty()) return {1, 1};

    // Find the zone that covers this tick
    int zi = (int)zones.size() - 1;
    for (int i = 0; i < (int)zones.size(); ++i) {
        if (i + 1 < (int)zones.size() && tick < zones[i + 1].tick) {
            zi = i; break;
        }
        if (i + 1 == (int)zones.size()) { zi = i; break; }
    }

    const auto& z = zones[zi];
    // ticks_per_beat = tpq * 4 / den
    const int64_t tpb = (int64_t)z.tpq * 4 / z.den;
    const int64_t tpbar = (int64_t)z.num * tpb;

    if (tpbar <= 0 || tpb <= 0) return {z.barOffset, 1};

    const int64_t delta = tick - z.tick;
    const int bar   = z.barOffset + (int)(delta / tpbar);   // 1-indexed
    const int beat  = 1 + (int)((delta % tpbar) / tpb);    // 1-indexed

    return {bar, beat};
}

// ---------------------------------------------------------------------------
// SMT parser

std::string parseSmt(const std::string& path, mcp::MusicContext& mc) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return "Cannot open file: " + path;

    QXmlStreamReader xml(&file);

    static const int kSMT_TPQ = 480;

    // Collect raw time-sig events (position in ticks, bar 0-indexed)
    struct SigEvent {
        int64_t tick{0};
        int     bar0{0};  // 0-indexed bar
        int     num{4};
        int     den{4};
    };
    std::vector<SigEvent> sigEvents;

    // Collect raw tempo events
    struct TempoEvent {
        int64_t tick{0};
        double  bpm{120.0};
        bool    isRamp{false};  // Func=1 means ramp
    };
    std::vector<TempoEvent> tempoEvents;

    // Parse XML
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString elemName = xml.name().toString();

            if (elemName == "MTimeSignatureEvent") {
                // Read child float/int elements
                int    bar0 = 0, num = 4, den = 4;
                int64_t pos = 0;
                // Read until end of this element
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isEndElement() && xml.name().toString() == "MTimeSignatureEvent")
                        break;
                    if (xml.isStartElement()) {
                        const QString n = xml.name().toString();
                        const QString nameAttr = xml.attributes().value("name").toString();
                        const QString valAttr  = xml.attributes().value("value").toString();
                        if (n == "int") {
                            if (nameAttr == "Bar")         bar0 = valAttr.toInt();
                            else if (nameAttr == "Numerator")   num  = valAttr.toInt();
                            else if (nameAttr == "Denominator") den  = valAttr.toInt();
                            else if (nameAttr == "Position")    pos  = valAttr.toLongLong();
                        }
                    }
                }
                SigEvent se;
                se.tick = pos;
                se.bar0 = bar0;
                se.num  = num;
                se.den  = den;
                sigEvents.push_back(se);

            } else if (elemName == "MTempoEvent") {
                double  bpm   = 120.0;
                int64_t ppq   = 0;
                bool    isRamp = false;
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isEndElement() && xml.name().toString() == "MTempoEvent")
                        break;
                    if (xml.isStartElement()) {
                        const QString n        = xml.name().toString();
                        const QString nameAttr = xml.attributes().value("name").toString();
                        const QString valAttr  = xml.attributes().value("value").toString();
                        if (n == "float") {
                            if (nameAttr == "BPM") bpm = valAttr.toDouble();
                            else if (nameAttr == "PPQ") ppq = (int64_t)valAttr.toDouble();
                        } else if (n == "int") {
                            if (nameAttr == "Func" && valAttr.toInt() == 1)
                                isRamp = true;
                        }
                    }
                }
                TempoEvent te;
                te.tick   = ppq;
                te.bpm    = bpm;
                te.isRamp = isRamp;
                tempoEvents.push_back(te);
            }
        }
    }

    if (xml.hasError())
        return "XML parse error: " + xml.errorString().toStdString();

    if (tempoEvents.empty() && sigEvents.empty())
        return "No tempo or time-sig events found in SMT file";

    // Sort sig events by tick
    std::stable_sort(sigEvents.begin(), sigEvents.end(),
                     [](const SigEvent& a, const SigEvent& b){ return a.tick < b.tick; });

    // Build TimeSigZones from sigEvents
    std::vector<TimeSigZone> zones;
    for (const auto& se : sigEvents) {
        TimeSigZone z;
        z.tick      = se.tick;
        z.barOffset = se.bar0 + 1;  // convert to 1-indexed
        z.num       = se.num;
        z.den       = se.den;
        z.tpq       = kSMT_TPQ;
        zones.push_back(z);
    }
    // If no sig events, add a default 4/4 zone at tick 0
    if (zones.empty()) {
        TimeSigZone z;
        z.tick      = 0;
        z.barOffset = 1;
        z.num       = 4;
        z.den       = 4;
        z.tpq       = kSMT_TPQ;
        zones.push_back(z);
    }

    // Collect all raw points
    std::vector<RawPoint> pts;

    // Time-sig points (bar 1-indexed, beat=1)
    for (const auto& se : sigEvents) {
        RawPoint rp;
        rp.bar        = se.bar0 + 1;
        rp.beat       = 1;
        rp.hasTimeSig = true;
        rp.timeSigNum = se.num;
        rp.timeSigDen = se.den;
        // BPM will be filled in from tempo events if they coincide, or from
        // the previous tempo event (handled during merge / finalisation)
        // For now leave at default; we'll override it in merge step
        rp.bpm        = 120.0;
        pts.push_back(rp);
    }

    // Tempo points
    for (const auto& te : tempoEvents) {
        auto [bar, beat] = tickToBarBeat(te.tick, zones);
        RawPoint rp;
        rp.bar    = bar;
        rp.beat   = beat;
        rp.bpm    = te.bpm;
        rp.isRamp = te.isRamp;
        rp.hasTimeSig = false;
        pts.push_back(rp);
    }

    auto merged = mergeAndSort(pts);

    // Fill in missing BPM for time-sig-only points by propagating forward
    double lastBpm = 120.0;
    for (auto& p : merged) {
        if (!p.hasTimeSig || p.bpm != 120.0) {
            lastBpm = p.bpm;
        } else {
            // time-sig point with no real BPM info — use last known
            p.bpm = lastBpm;
        }
    }

    finalisePoints(merged);
    installPoints(merged, mc);
    return {};
}

// ---------------------------------------------------------------------------
// MIDI parser

static uint32_t readVLQ(const uint8_t* data, size_t size, size_t& pos) {
    uint32_t val = 0;
    while (pos < size) {
        const uint8_t b = data[pos++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static uint32_t readBE32(const uint8_t* data, size_t pos) {
    return ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
           ((uint32_t)data[pos+2] << 8) | data[pos+3];
}

static uint16_t readBE16(const uint8_t* data, size_t pos) {
    return (uint16_t)(((uint16_t)data[pos] << 8) | data[pos+1]);
}

struct MidiTempoEvent {
    int64_t tick{0};
    double  bpm{120.0};
};
struct MidiSigEvent {
    int64_t tick{0};
    int     num{4};
    int     den{4};  // actual denominator (1<<dd)
};

static std::string parseMidiTrack(const uint8_t* data, size_t size, size_t pos, size_t end,
                                   std::vector<MidiTempoEvent>& tempos,
                                   std::vector<MidiSigEvent>& sigs) {
    int64_t tick = 0;
    uint8_t running = 0;

    while (pos < end) {
        // Delta time
        const uint32_t delta = readVLQ(data, size, pos);
        tick += delta;

        if (pos >= end) break;
        uint8_t b = data[pos];

        if (b == 0xFF) {
            // Meta event
            if (pos + 1 >= end) break;
            ++pos;
            const uint8_t metaType = data[pos++];
            const uint32_t metaLen = readVLQ(data, size, pos);
            if (pos + metaLen > end) break;

            if (metaType == 0x51 && metaLen == 3) {
                // Tempo
                const uint32_t usPerQN = ((uint32_t)data[pos] << 16) |
                                          ((uint32_t)data[pos+1] << 8) |
                                          data[pos+2];
                if (usPerQN > 0) {
                    MidiTempoEvent te;
                    te.tick = tick;
                    te.bpm  = 60000000.0 / usPerQN;
                    tempos.push_back(te);
                }
            } else if (metaType == 0x58 && metaLen == 4) {
                // Time signature
                const int nn = data[pos];
                const int dd = data[pos+1];
                MidiSigEvent se;
                se.tick = tick;
                se.num  = nn;
                se.den  = 1 << dd;
                sigs.push_back(se);
            } else if (metaType == 0x2F) {
                // End of track
                pos += metaLen;
                break;
            }
            pos += metaLen;
            running = 0;
        } else if (b == 0xF0 || b == 0xF7) {
            // SysEx — skip
            ++pos;
            const uint32_t len = readVLQ(data, size, pos);
            pos += len;
            running = 0;
        } else {
            // Channel event
            if (b & 0x80) {
                running = b;
                ++pos;
            }
            if (running == 0) { ++pos; continue; }
            const uint8_t status = running & 0xF0;
            // Skip event bytes (consume the right number of data bytes)
            int dataBytes = 0;
            if      (status == 0x80 || status == 0x90) dataBytes = 2;
            else if (status == 0xA0)                   dataBytes = 2;
            else if (status == 0xB0)                   dataBytes = 2;
            else if (status == 0xC0)                   dataBytes = 1;
            else if (status == 0xD0)                   dataBytes = 1;
            else if (status == 0xE0)                   dataBytes = 2;
            else { ++pos; continue; }
            pos += static_cast<size_t>(dataBytes);
        }
    }
    return {};
}

std::string parseMidi(const std::string& path, mcp::MusicContext& mc) {
    // Read entire file
    std::ifstream f(path, std::ios::binary);
    if (!f) return "Cannot open file: " + path;
    f.seekg(0, std::ios::end);
    const size_t fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(fileSize);
    if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize)))
        return "Failed to read file: " + path;

    if (fileSize < 14) return "File too small to be a MIDI file";
    if (data[0]!='M'||data[1]!='T'||data[2]!='h'||data[3]!='d')
        return "Not a MIDI file (missing MThd header)";

    const uint32_t hdrLen = readBE32(data.data(), 4);
    if (hdrLen < 6) return "Invalid MIDI header length";

    const uint16_t format   = readBE16(data.data(), 8);
    const uint16_t nTracks  = readBE16(data.data(), 10);
    const uint16_t divWord  = readBE16(data.data(), 12);

    if (divWord & 0x8000)
        return "SMPTE timecode MIDI files are not supported";

    const int tpq = divWord;  // ticks per quarter note

    std::vector<MidiTempoEvent> tempos;
    std::vector<MidiSigEvent>   sigs;

    // For type 0: parse single track; for type 1: parse track 0 only (tempo map)
    size_t pos = 8 + hdrLen;
    const int tracksToRead = (format == 0) ? 1 : 1;  // always track 0
    (void)tracksToRead;

    for (int t = 0; t < (int)nTracks; ++t) {
        if (pos + 8 > fileSize) break;
        if (data[pos]!='M'||data[pos+1]!='T'||data[pos+2]!='r'||data[pos+3]!='k')
            break;
        const uint32_t trkLen = readBE32(data.data(), pos + 4);
        const size_t trkStart = pos + 8;
        const size_t trkEnd   = trkStart + trkLen;
        if (trkEnd > fileSize) break;

        if (t == 0 || format == 0) {
            parseMidiTrack(data.data(), fileSize, trkStart, trkEnd, tempos, sigs);
        }

        pos = trkEnd;
        if (format == 0) break;  // only one track in type 0
        if (format == 1 && t == 0) break;  // only tempo track for type 1
    }

    if (tempos.empty() && sigs.empty())
        return "No tempo or time-sig meta events found in MIDI file";

    // Sort both by tick
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const MidiTempoEvent& a, const MidiTempoEvent& b){ return a.tick < b.tick; });
    std::stable_sort(sigs.begin(), sigs.end(),
                     [](const MidiSigEvent& a, const MidiSigEvent& b){ return a.tick < b.tick; });

    // Build TimeSigZones for tick→(bar,beat) conversion
    // Need to track bar offsets cumulatively
    std::vector<TimeSigZone> zones;
    {
        // Start with 4/4 default if no sig events at tick 0
        int curNum = 4, curDen = 4;
        int64_t curZoneTick  = 0;
        int     curBarOffset = 1;

        // Insert each sig event as a zone boundary
        // We process them in order and compute barOffset by accumulation
        auto computeZone = [&](int64_t fromTick, int64_t toTick,
                                int num, int den) -> int {
            // Returns number of complete bars in [fromTick, toTick)
            const int64_t tpb   = (int64_t)tpq * 4 / den;
            const int64_t tpbar = (int64_t)num * tpb;
            if (tpbar <= 0) return 0;
            return (int)((toTick - fromTick) / tpbar);
        };

        for (const auto& se : sigs) {
            if (se.tick > curZoneTick) {
                TimeSigZone z;
                z.tick      = curZoneTick;
                z.barOffset = curBarOffset;
                z.num       = curNum;
                z.den       = curDen;
                z.tpq       = tpq;
                zones.push_back(z);
                // Accumulate bars in this zone
                const int bars = computeZone(curZoneTick, se.tick, curNum, curDen);
                curBarOffset += bars;
                curZoneTick   = se.tick;
            }
            curNum = se.num;
            curDen = se.den;
        }
        // Final zone
        TimeSigZone z;
        z.tick      = curZoneTick;
        z.barOffset = curBarOffset;
        z.num       = curNum;
        z.den       = curDen;
        z.tpq       = tpq;
        zones.push_back(z);
    }

    // Collect raw points
    std::vector<RawPoint> pts;

    // Time-sig points
    for (const auto& se : sigs) {
        auto [bar, beat] = tickToBarBeat(se.tick, zones);
        RawPoint rp;
        rp.bar        = bar;
        rp.beat       = beat;
        rp.hasTimeSig = true;
        rp.timeSigNum = se.num;
        rp.timeSigDen = se.den;
        rp.bpm        = 120.0;  // will be merged with tempo event if coincident
        rp.isRamp     = false;
        pts.push_back(rp);
    }

    // Tempo points (all jumps in MIDI)
    for (const auto& te : tempos) {
        auto [bar, beat] = tickToBarBeat(te.tick, zones);
        RawPoint rp;
        rp.bar        = bar;
        rp.beat       = beat;
        rp.bpm        = te.bpm;
        rp.isRamp     = false;  // MIDI has no ramps
        rp.hasTimeSig = false;
        pts.push_back(rp);
    }

    auto merged = mergeAndSort(pts);

    // Propagate BPM into time-sig-only points
    double lastBpm = 120.0;
    if (!tempos.empty()) lastBpm = tempos[0].bpm;
    for (auto& p : merged) {
        if (!p.hasTimeSig) {
            lastBpm = p.bpm;
        } else {
            if (p.bpm == 120.0) p.bpm = lastBpm;
            else lastBpm = p.bpm;
        }
    }

    finalisePoints(merged);
    installPoints(merged, mc);
    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------

namespace MCImport {

std::string fromMidi(const std::string& path, mcp::MusicContext& mc) {
    return parseMidi(path, mc);
}

std::string fromSmt(const std::string& path, mcp::MusicContext& mc) {
    return parseSmt(path, mc);
}

} // namespace MCImport
