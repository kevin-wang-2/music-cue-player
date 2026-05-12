#include "engine/SnapshotManager.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

namespace mcp {

using SnapshotList = ShowFile::SnapshotList;

// ---------------------------------------------------------------------------
// Path helpers

// Returns true if `valuePath` is covered by `scopeEntry`.
// Prefix match: "/mixer/0" covers "/mixer/0/fader", "/mixer/0/crosspoint/1", etc.
static bool pathMatches(const std::string& valuePath, const std::string& scopeEntry)
{
    if (valuePath == scopeEntry) return true;
    if (valuePath.size() > scopeEntry.size() &&
        valuePath[scopeEntry.size()] == '/' &&
        valuePath.compare(0, scopeEntry.size(), scopeEntry) == 0)
        return true;
    return false;
}

static bool inScope(const std::string& path, const std::vector<std::string>& scope)
{
    for (const auto& s : scope)
        if (pathMatches(path, s)) return true;
    return false;
}

// Current XP value for (ch, out) — 0 dB if diagonal and absent, -inf otherwise.
static float getXpDb(const ShowFile::AudioSetup& as, int ch, int out)
{
    for (const auto& xe : as.xpEntries)
        if (xe.ch == ch && xe.out == out) return xe.db;
    return (ch == out) ? 0.0f : -144.0f;
}

// Parse "/mixer/{ch}/..." → ch, or -1 on failure.
static int parseMixerCh(const std::string& path)
{
    if (path.size() <= 7 || path.compare(0, 7, "/mixer/") != 0) return -1;
    size_t pos = 7;
    size_t slash = path.find('/', pos);
    try {
        return std::stoi(path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
    } catch (...) { return -1; }
}

// Parse "/mixer/{ch}/plugin/{slot}" → slot, or -1 on failure.
static int parseMixerPluginSlot(const std::string& path)
{
    const std::string mid = "/plugin/";
    auto it = path.find(mid);
    if (it == std::string::npos) return -1;
    try { return std::stoi(path.substr(it + mid.size())); }
    catch (...) { return -1; }
}

// Parse "/mixer/{ch}/crosspoint/{out}" → out, or -1 on failure.
static int parseCrosspointOut(const std::string& path)
{
    const std::string mid = "/crosspoint/";
    auto it = path.find(mid);
    if (it == std::string::npos) return -1;
    try { return std::stoi(path.substr(it + mid.size())); }
    catch (...) { return -1; }
}

// Parse "/mixer/{ch}/send/{slot}/..." → slot, or -1 on failure.
static int parseSendSlot(const std::string& path)
{
    const std::string mid = "/send/";
    auto it = path.find(mid);
    if (it == std::string::npos) return -1;
    try {
        const size_t start = it + mid.size();
        const size_t slash = path.find('/', start);
        return std::stoi(path.substr(start,
            slash == std::string::npos ? std::string::npos : slash - start));
    }
    catch (...) { return -1; }
}

// ---------------------------------------------------------------------------

void SnapshotManager::init(ShowFile& sf) { m_sf = &sf; }

void SnapshotManager::markDirty(const std::string& path)
{
    auto& paths = m_sf->snapshotList.pendingDirtyPaths;
    if (std::find(paths.begin(), paths.end(), path) == paths.end())
        paths.push_back(path);
}

// ---------------------------------------------------------------------------

static SnapshotList::Snapshot buildSnapShell(const SnapshotList& sl, int idx)
{
    const bool isNew = (idx >= static_cast<int>(sl.snapshots.size()) ||
                        !sl.snapshots[static_cast<size_t>(idx)]);
    SnapshotList::Snapshot snap;
    snap.id   = isNew ? sl.nextSnapshotId()
                      : sl.snapshots[static_cast<size_t>(idx)]->id;
    snap.name = isNew ? ("Snapshot " + std::to_string(idx + 1))
                      : sl.snapshots[static_cast<size_t>(idx)]->name;
    return snap;
}

static void placeSnapshot(SnapshotList& sl, int idx, SnapshotList::Snapshot snap)
{
    if (idx >= static_cast<int>(sl.snapshots.size()))
        sl.snapshots.resize(static_cast<size_t>(idx + 1), std::nullopt);
    sl.snapshots[static_cast<size_t>(idx)] = std::move(snap);
}

// ---------------------------------------------------------------------------

void SnapshotManager::store()
{
    auto& sl       = m_sf->snapshotList;
    const auto& as = m_sf->audioSetup;
    const int   idx = sl.currentIndex;

    auto snap  = buildSnapShell(sl, idx);
    snap.scope = sl.pendingDirtyPaths;

    // Collect channels mentioned in dirty paths
    std::set<int> dirtyChs;
    for (const auto& p : sl.pendingDirtyPaths) {
        int ch = parseMixerCh(p);
        if (ch >= 0) dirtyChs.insert(ch);
    }

    for (int ch : dirtyChs) {
        if (ch >= static_cast<int>(as.channels.size())) continue;
        const auto& c = as.channels[static_cast<size_t>(ch)];
        SnapshotList::Snapshot::ChannelState cs;
        cs.ch = ch;

        const std::string base = "/mixer/" + std::to_string(ch);
        if (inScope(base + "/delay", sl.pendingDirtyPaths)) {
            cs.delayMs        = c.delayMs;
            cs.delayInSamples = c.delayInSamples;
            cs.delaySamples   = c.delaySamples;
        }
        if (inScope(base + "/polarity", sl.pendingDirtyPaths))
            cs.polarity = c.phaseInvert;
        if (inScope(base + "/mute", sl.pendingDirtyPaths))
            cs.mute = c.mute;
        if (inScope(base + "/fader", sl.pendingDirtyPaths))
            cs.faderDb = c.masterGainDb;

        // Collect per-crosspoint paths for this channel
        for (const auto& p : sl.pendingDirtyPaths) {
            if (parseMixerCh(p) != ch) continue;
            int out = parseCrosspointOut(p);
            if (out < 0) continue;
            cs.xpSends.push_back({out, getXpDb(as, ch, out)});
        }

        // Collect dirty plugin slot states for this channel.
        // Use a set to deduplicate: multiple per-param paths for the same slot
        // (e.g. "/mixer/0/plugin/0/gain" + "/mixer/0/plugin/0/reverb") must
        // produce only one PluginParamState entry, not one per dirty path.
        std::set<int> dirtySlots;
        for (const auto& p : sl.pendingDirtyPaths) {
            if (parseMixerCh(p) != ch) continue;
            int slotIdx = parseMixerPluginSlot(p);
            if (slotIdx >= 0) dirtySlots.insert(slotIdx);
        }
        for (int slotIdx : dirtySlots) {
            if (slotIdx >= static_cast<int>(c.plugins.size())) continue;
            const auto& psl = c.plugins[static_cast<size_t>(slotIdx)];
            SnapshotList::Snapshot::ChannelState::PluginParamState ps;
            ps.slot             = slotIdx;
            ps.bypassed         = psl.bypassed;
            ps.parameters       = psl.parameters;
            ps.extStateBlob     = psl.extStateBlob;
            ps.extParamSnapshot = psl.extParamSnapshot;
            cs.pluginStates.push_back(ps);
        }

        // Collect dirty send slot states for this channel.
        std::set<int> dirtySendSlots;
        for (const auto& p : sl.pendingDirtyPaths) {
            if (parseMixerCh(p) != ch) continue;
            int slotIdx = parseSendSlot(p);
            if (slotIdx >= 0) dirtySendSlots.insert(slotIdx);
        }
        for (int slotIdx : dirtySendSlots) {
            if (slotIdx >= static_cast<int>(c.sends.size())) continue;
            const auto& ss = c.sends[static_cast<size_t>(slotIdx)];
            if (!ss.isActive()) continue;
            const std::string sp = base + "/send/" + std::to_string(slotIdx);
            SnapshotList::Snapshot::ChannelState::SendState sendSt;
            sendSt.slot = slotIdx;
            if (inScope(sp + "/mute",  sl.pendingDirtyPaths)) sendSt.muted   = ss.muted;
            if (inScope(sp + "/level", sl.pendingDirtyPaths)) sendSt.levelDb = ss.levelDb;
            if (inScope(sp + "/panL",  sl.pendingDirtyPaths)) sendSt.panL    = ss.panL;
            if (inScope(sp + "/panR",  sl.pendingDirtyPaths)) sendSt.panR    = ss.panR;
            cs.sendStates.push_back(sendSt);
        }

        snap.channels.push_back(cs);
    }

    placeSnapshot(sl, idx, std::move(snap));
    sl.pendingDirtyPaths.clear();
}

void SnapshotManager::storeAll()
{
    auto& sl       = m_sf->snapshotList;
    const auto& as = m_sf->audioSetup;
    const int   idx = sl.currentIndex;

    auto snap = buildSnapShell(sl, idx);

    // Build scope: all mixer paths
    for (int ch = 0; ch < static_cast<int>(as.channels.size()); ++ch) {
        const std::string base = "/mixer/" + std::to_string(ch);
        snap.scope.push_back(base + "/delay");
        snap.scope.push_back(base + "/polarity");
        snap.scope.push_back(base + "/mute");
        snap.scope.push_back(base + "/fader");
        // Crosspoints: add explicit entries + diagonal
        std::set<int> outs;
        for (const auto& xe : as.xpEntries)
            if (xe.ch == ch) outs.insert(xe.out);
        outs.insert(ch);  // diagonal
        for (int out : outs)
            snap.scope.push_back(base + "/crosspoint/" + std::to_string(out));

        // Plugin slot paths
        for (int s = 0; s < static_cast<int>(as.channels[static_cast<size_t>(ch)].plugins.size()); ++s)
            if (!as.channels[static_cast<size_t>(ch)].plugins[static_cast<size_t>(s)].pluginId.empty())
                snap.scope.push_back(base + "/plugin/" + std::to_string(s));

        // Send slot paths
        for (int s = 0; s < static_cast<int>(as.channels[static_cast<size_t>(ch)].sends.size()); ++s) {
            if (as.channels[static_cast<size_t>(ch)].sends[static_cast<size_t>(s)].isActive()) {
                const std::string sp = base + "/send/" + std::to_string(s);
                snap.scope.push_back(sp + "/mute");
                snap.scope.push_back(sp + "/level");
                snap.scope.push_back(sp + "/panL");
                snap.scope.push_back(sp + "/panR");
            }
        }
    }

    for (int ch = 0; ch < static_cast<int>(as.channels.size()); ++ch) {
        const auto& c = as.channels[static_cast<size_t>(ch)];
        SnapshotList::Snapshot::ChannelState cs;
        cs.ch             = ch;
        cs.delayMs        = c.delayMs;
        cs.delayInSamples = c.delayInSamples;
        cs.delaySamples   = c.delaySamples;
        cs.polarity       = c.phaseInvert;
        cs.mute           = c.mute;
        cs.faderDb        = c.masterGainDb;

        std::set<int> outs;
        for (const auto& xe : as.xpEntries)
            if (xe.ch == ch) outs.insert(xe.out);
        outs.insert(ch);
        for (int out : outs)
            cs.xpSends.push_back({out, getXpDb(as, ch, out)});

        // Capture all plugin slot states
        for (int s = 0; s < static_cast<int>(c.plugins.size()); ++s) {
            const auto& psl = c.plugins[static_cast<size_t>(s)];
            if (psl.pluginId.empty()) continue;
            SnapshotList::Snapshot::ChannelState::PluginParamState ps;
            ps.slot             = s;
            ps.bypassed         = psl.bypassed;
            ps.parameters       = psl.parameters;
            ps.extStateBlob     = psl.extStateBlob;
            ps.extParamSnapshot = psl.extParamSnapshot;
            cs.pluginStates.push_back(ps);
        }

        // Capture all active send slot states
        for (int s = 0; s < static_cast<int>(c.sends.size()); ++s) {
            const auto& ss = c.sends[static_cast<size_t>(s)];
            if (!ss.isActive()) continue;
            SnapshotList::Snapshot::ChannelState::SendState sendSt;
            sendSt.slot   = s;
            sendSt.muted  = ss.muted;
            sendSt.levelDb = ss.levelDb;
            sendSt.panL   = ss.panL;
            sendSt.panR   = ss.panR;
            cs.sendStates.push_back(sendSt);
        }

        snap.channels.push_back(cs);
    }

    placeSnapshot(sl, idx, std::move(snap));
    sl.pendingDirtyPaths.clear();
}

bool SnapshotManager::recall()
{
    const auto& sl = m_sf->snapshotList;
    const int idx = sl.currentIndex;
    if (idx < 0 || idx >= static_cast<int>(sl.snapshots.size())) return false;
    if (!sl.snapshots[static_cast<size_t>(idx)]) return false;
    return recallById(sl.snapshots[static_cast<size_t>(idx)]->id);
}

bool SnapshotManager::recallById(int id)
{
    const auto* snap = m_sf->snapshotList.findById(id);
    if (!snap) return false;
    auto& as = m_sf->audioSetup;

    for (const auto& cs : snap->channels) {
        if (cs.ch < 0 || cs.ch >= static_cast<int>(as.channels.size())) continue;
        auto& c = as.channels[static_cast<size_t>(cs.ch)];
        const std::string base = "/mixer/" + std::to_string(cs.ch);

        if (cs.delayMs && inScope(base + "/delay", snap->scope)) {
            c.delayMs        = *cs.delayMs;
            c.delayInSamples = cs.delayInSamples.value_or(false);
            c.delaySamples   = cs.delaySamples.value_or(0);
        }
        if (cs.polarity && inScope(base + "/polarity", snap->scope))
            c.phaseInvert = *cs.polarity;
        if (cs.mute && inScope(base + "/mute", snap->scope))
            c.mute = *cs.mute;
        if (cs.faderDb && inScope(base + "/fader", snap->scope))
            c.masterGainDb = *cs.faderDb;

        // Apply only the crosspoints that are in scope and present in xpSends
        for (const auto& xs : cs.xpSends) {
            const std::string xpP = base + "/crosspoint/" + std::to_string(xs.out);
            if (!inScope(xpP, snap->scope)) continue;
            auto& xp = as.xpEntries;
            xp.erase(std::remove_if(xp.begin(), xp.end(),
                [&cs, &xs](const ShowFile::AudioSetup::XpEntry& xe){
                    return xe.ch == cs.ch && xe.out == xs.out; }), xp.end());
            xp.push_back({cs.ch, xs.out, xs.db});
        }

        // Restore plugin slot states that are in scope (per-param or per-slot).
        for (const auto& ps : cs.pluginStates) {
            if (ps.slot >= static_cast<int>(c.plugins.size())) continue;
            const std::string slotPath   = base + "/plugin/" + std::to_string(ps.slot);
            const std::string bypassPath = slotPath + "/bypass";
            const bool fullSlot = inScope(slotPath, snap->scope);

            auto& psl = c.plugins[static_cast<size_t>(ps.slot)];

            // Bypass: slot-level scope or explicit /bypass path
            if (ps.bypassed && (fullSlot || inScope(bypassPath, snap->scope)))
                psl.bypassed = *ps.bypassed;

            // Blob: full-slot only — partial restore from binary blob is not possible
            if (!ps.extStateBlob.empty() && fullSlot)
                psl.extStateBlob = ps.extStateBlob;

            // Internal parameters: slot-level or per-param path
            for (const auto& [paramId, val] : ps.parameters) {
                if (fullSlot || inScope(slotPath + "/" + paramId, snap->scope))
                    psl.parameters[paramId] = val;
            }
            // External (AU) parameter snapshot: same granularity
            for (const auto& [paramId, val] : ps.extParamSnapshot) {
                if (fullSlot || inScope(slotPath + "/" + paramId, snap->scope))
                    psl.extParamSnapshot[paramId] = val;
            }
        }

        // Restore send slot states that are in scope.
        for (const auto& ss : cs.sendStates) {
            if (ss.slot >= static_cast<int>(c.sends.size())) continue;
            auto& s = c.sends[static_cast<size_t>(ss.slot)];
            const std::string sp = base + "/send/" + std::to_string(ss.slot);
            if (ss.muted   && inScope(sp + "/mute",  snap->scope)) s.muted   = *ss.muted;
            if (ss.levelDb && inScope(sp + "/level", snap->scope)) s.levelDb = *ss.levelDb;
            if (ss.panL    && inScope(sp + "/panL",  snap->scope)) s.panL    = *ss.panL;
            if (ss.panR    && inScope(sp + "/panR",  snap->scope)) s.panR    = *ss.panR;
        }
    }
    return true;
}

void SnapshotManager::navigatePrev()
{
    auto& sl = m_sf->snapshotList;
    if (sl.currentIndex > 0) --sl.currentIndex;
}

void SnapshotManager::navigateNext()
{
    auto& sl = m_sf->snapshotList;
    if (sl.currentIndex < static_cast<int>(sl.snapshots.size()))
        ++sl.currentIndex;
}

void SnapshotManager::setCurrentIndex(int idx)
{
    m_sf->snapshotList.currentIndex = idx;
}

int  SnapshotManager::currentIndex()  const { return m_sf->snapshotList.currentIndex; }
int  SnapshotManager::snapshotCount() const { return static_cast<int>(m_sf->snapshotList.snapshots.size()); }

bool SnapshotManager::isEmptySlot() const
{
    const auto& sl = m_sf->snapshotList;
    const int idx = sl.currentIndex;
    if (idx >= static_cast<int>(sl.snapshots.size())) return true;
    return !sl.snapshots[static_cast<size_t>(idx)].has_value();
}

} // namespace mcp
