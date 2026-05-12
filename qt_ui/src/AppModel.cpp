#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/Cue.h"
#include "engine/MusicContext.h"
#include "engine/plugin/ExternalPluginReference.h"
#include "engine/plugin/MissingPluginProcessor.h"
#include "engine/plugin/PluginState.h"
#ifdef __APPLE__
#  include "engine/plugin/AUPluginAdapter.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QVariant>

AppModel::AppModel(QObject* parent)
    : QObject(parent)
    , scheduler(engine)
    , midiIn(this)
    , oscServer(this)
    , scriptlet(std::make_unique<ScriptletEngine>())
{
    snapshots.init(sf);
    // Create one engine CueList (more are added in syncListCount() after load).
    m_cueLists.push_back(std::make_unique<mcp::CueList>(engine, scheduler));
    connect(&midiIn,   &MidiInputManager::midiReceived,
            this, &AppModel::routeMidi);
    connect(&oscServer, &OscServer::messageReceived,
            this, &AppModel::routeOsc);

    scriptlet->setGoCallback([this]() {
        cues().go();
        emit selectionChanged(cues().selectedIndex());
        emit playbackStateChanged();
    });
    scriptlet->setSelectCallback([this](const std::string& num) -> bool {
        const int idx = cues().findByCueNumber(num);
        if (idx >= 0) { cues().setSelectedIndex(idx); emit selectionChanged(idx); return true; }
        return false;
    });
    scriptlet->setAlertCallback([](const std::string& msg) {
        QMessageBox::information(nullptr, "Script", QString::fromStdString(msg));
    });
    scriptlet->setConfirmCallback([](const std::string& msg) -> bool {
        return QMessageBox::question(nullptr, "Script", QString::fromStdString(msg))
               == QMessageBox::Yes;
    });
    scriptlet->setOutputCallback([this](const std::string& text) {
        emit scriptletOutput(QString::fromStdString(text));
    });
    scriptlet->setPanicCallback([this]() {
        panicAll();
    });
    scriptlet->setFileCallback([](const std::string& title, const std::string& mode, const std::string& filter)
            -> std::optional<std::string> {
        const QString qtTitle  = QString::fromStdString(title);
        const QString qtFilter = filter.empty()
            ? QString("All Files (*)")
            : QString::fromStdString(filter);
        QString path;
        if (mode == "save") {
            path = QFileDialog::getSaveFileName(nullptr, qtTitle, {}, qtFilter);
        } else if (mode == "dir") {
            path = QFileDialog::getExistingDirectory(nullptr, qtTitle);
        } else {
            path = QFileDialog::getOpenFileName(nullptr, qtTitle, {}, qtFilter);
        }
        if (path.isEmpty()) return std::nullopt;
        return path.toStdString();
    });
    scriptlet->setInputCallback([](const std::string& prompt, const std::string& defaultVal, const std::string& title)
            -> std::optional<std::string> {
        bool ok = false;
        const QString result = QInputDialog::getText(
            nullptr,
            QString::fromStdString(title),
            QString::fromStdString(prompt),
            QLineEdit::Normal,
            QString::fromStdString(defaultVal),
            &ok);
        if (!ok) return std::nullopt;
        return result.toStdString();
    });

    // Helper: find list index by numeric ID (-1 if not found).
    // Used by the list-ID-aware scriptlet callbacks below.
    auto findLi = [this](int listId) -> int {
        for (int li = 0; li < (int)sf.cueLists.size(); ++li)
            if (sf.cueLists[li].numericId == listId) return li;
        return -1;
    };

    // Helper: build ScriptletCueInfo for any list/index combination.
    auto buildCueInfo = [this](mcp::CueList& cl, int idx) -> ScriptletCueInfo {
        ScriptletCueInfo info;
        const mcp::Cue* c = cl.cueAt(idx);
        if (!c) return info;
        info.number       = c->cueNumber;
        info.name         = c->name;
        info.preWait      = c->preWaitSeconds;
        info.autoContinue = c->autoContinue;
        info.autoFollow   = c->autoFollow;
        info.isPlaying    = cl.isCuePlaying(idx);
        info.isPending    = cl.isCuePending(idx);
        info.isArmed      = cl.isArmed(idx);
        switch (c->type) {
            case mcp::CueType::Audio:        info.type = "audio";         break;
            case mcp::CueType::Start:        info.type = "start";         break;
            case mcp::CueType::Stop:         info.type = "stop";          break;
            case mcp::CueType::Arm:          info.type = "arm";           break;
            case mcp::CueType::Fade:         info.type = "fade";          break;
            case mcp::CueType::Devamp:       info.type = "devamp";        break;
            case mcp::CueType::Group:        info.type = "group";         break;
            case mcp::CueType::MusicContext: info.type = "music_context"; break;
            case mcp::CueType::Marker:       info.type = "marker";        break;
            case mcp::CueType::Network:      info.type = "network";       break;
            case mcp::CueType::Midi:         info.type = "midi";          break;
            case mcp::CueType::Timecode:     info.type = "timecode";      break;
            case mcp::CueType::Goto:         info.type = "goto";          break;
            case mcp::CueType::Memo:         info.type = "memo";          break;
            case mcp::CueType::Scriptlet:    info.type = "scriptlet";     break;
        }
        if (c->type == mcp::CueType::Audio) {
            info.path      = c->path;
            info.level     = c->level;
            info.trim      = c->trim;
            info.startTime = c->startTime;
            info.duration  = c->duration;
            info.playhead  = cl.cuePlayheadFileSeconds(idx);
        }
        if (c->targetIndex >= 0) {
            info.targetIndex = c->targetIndex;
            const mcp::Cue* t = cl.cueAt(c->targetIndex);
            if (t) info.targetNumber = t->cueNumber;
        }
        if (c->type == mcp::CueType::Scriptlet)
            info.code = c->scriptletCode;
        return info;
    };

    // --- mcp.cue action callbacks (always operate on the active list) ---

    scriptlet->setCueInfoCallback([this, buildCueInfo](int idx) -> ScriptletCueInfo {
        return buildCueInfo(cues(), idx);
    });
    scriptlet->setCueSelectCallback([this](int idx) {
        if (idx >= 0 && idx < cues().cueCount()) {
            cues().setSelectedIndex(idx);
            emit selectionChanged(idx);
        }
    });
    scriptlet->setCueGoCallback([this](int idx) {
        if (idx >= 0 && idx < cues().cueCount()) {
            cues().setSelectedIndex(idx);
            cues().go();
            emit selectionChanged(cues().selectedIndex());
            emit playbackStateChanged();
        }
    });
    scriptlet->setCueStartCallback([this](int idx) {
        if (idx < 0 || idx >= cues().cueCount()) return;
        cues().start(idx);
        emit playbackStateChanged();
    });
    scriptlet->setCueArmCallback([this](int idx, double startOverride) {
        if (idx >= 0 && idx < cues().cueCount())
            cues().arm(idx, startOverride);
    });
    scriptlet->setCueStopCallback([this](int idx) {
        if (idx >= 0 && idx < cues().cueCount()) {
            cues().stop(idx);
            emit playbackStateChanged();
        }
    });
    scriptlet->setCueDisarmCallback([this](int idx) {
        if (idx >= 0 && idx < cues().cueCount())
            cues().disarm(idx);
    });
    scriptlet->setCueSetNameCallback([this](int idx, const std::string& name) {
        if (idx >= 0 && idx < cues().cueCount()) {
            cues().setCueName(idx, name);
            ShowHelpers::syncSfFromCues(*this);
            emit cueListChanged();
        }
    });

    // --- mcp.cue_list.CueList method callbacks (list-ID-aware) ---

    scriptlet->setCueListCountCallback([this, findLi](int listId) -> int {
        const int li = findLi(listId);
        return (li >= 0) ? m_cueLists[li]->cueCount() : -1;  // -1 = list not found
    });

    scriptlet->setCueListInfoCallback([this, findLi, buildCueInfo](int listId, int idx) -> ScriptletCueInfo {
        const int li = findLi(listId);
        if (li < 0) return {};
        return buildCueInfo(*m_cueLists[li], idx);
    });

    scriptlet->setCueListInsertCallback([this, findLi](int listId,
                                                        const std::string& type,
                                                        const std::string& number,
                                                        const std::string& name) -> int {
        const int li = findLi(listId);
        if (li < 0) return -1;
        pushUndo();
        mcp::ShowFile::CueData cd;
        cd.type      = type;
        cd.cueNumber = number.empty() ? ShowHelpers::nextCueNumber(sf) : number;
        cd.name      = name;
        std::string err;
        ShowHelpers::insertEngineCue(*this, li, -1 /*append*/, std::move(cd), err);
        dirty = true;
        emit cueListChanged();
        const int newIdx = m_cueLists[static_cast<size_t>(li)]->cueCount() - 1;
        scriptlet->fireCueInsertedEvent(newIdx);
        return newIdx;
    });

    scriptlet->setCueListInsertAtCallback([this, findLi](int listId, int refIdx,
                                                          const std::string& type,
                                                          const std::string& number,
                                                          const std::string& name) -> int {
        const int li = findLi(listId);
        if (li < 0 || refIdx < 0 || refIdx >= m_cueLists[static_cast<size_t>(li)]->cueCount()) return -1;
        pushUndo();
        mcp::ShowFile::CueData cd;
        cd.type      = type;
        cd.cueNumber = number.empty() ? ShowHelpers::nextCueNumber(sf) : number;
        cd.name      = name;
        std::string err;
        ShowHelpers::insertEngineCue(*this, li, refIdx + 1, std::move(cd), err);
        dirty = true;
        emit cueListChanged();
        scriptlet->fireCueInsertedEvent(refIdx + 1);
        return refIdx + 1;
    });

    scriptlet->setCueListMoveCallback([this, findLi](int listId, int refIdx, int cueIdx, bool toGroup) -> bool {
        const int li = findLi(listId);
        if (li < 0) return false;
        auto& cl = *m_cueLists[static_cast<size_t>(li)];
        const int cnt = cl.cueCount();
        if (cueIdx < 0 || cueIdx >= cnt || refIdx < 0 || refIdx >= cnt || refIdx == cueIdx) return false;
        pushUndo();
        const auto* ec = cl.cueAt(cueIdx);
        const int blockSize = ec ? 1 + ec->childCount : 1;
        mcp::ShowFile::CueData cd = ShowHelpers::sfRemoveAt(sf, li, cueIdx);
        const int adjustedRef = (refIdx > cueIdx) ? refIdx - 1 : refIdx;
        if (toGroup) {
            ShowHelpers::sfAppendToGroup(sf, li, adjustedRef, std::move(cd));
            std::string err;
            ShowHelpers::rebuildCueList(*this, li, err);
        } else {
            ShowHelpers::sfInsertBefore(sf, li, adjustedRef + 1, std::move(cd));
            const int dstRow = refIdx + 1;
            ShowHelpers::sfFixTargetsForReorder(sf, li, cueIdx, blockSize, dstRow);
            cl.moveCueTo(cueIdx, dstRow);
        }
        dirty = true;
        emit cueListChanged();
        return true;
    });

    scriptlet->setCueListDeleteCallback([this, findLi](int listId, int idx) -> bool {
        const int li = findLi(listId);
        if (li < 0 || idx < 0 || idx >= m_cueLists[static_cast<size_t>(li)]->cueCount()) return false;
        pushUndo();
        ShowHelpers::removeEngineCue(*this, li, idx);
        dirty = true;
        emit cueListChanged();
        return true;
    });

    // --- mcp.cue_list module-level CRUD ---

    scriptlet->setInsertCueListCallback([this](const std::string& name) -> int {
        pushUndo();
        mcp::ShowFile::CueListData cld;
        cld.name      = name;
        cld.numericId = sf.nextListId();
        const int newId = cld.numericId;
        sf.cueLists.push_back(std::move(cld));
        insertEngineList(static_cast<int>(sf.cueLists.size()) - 1);
        dirty = true;
        emit dirtyChanged(true);
        emit cueListsChanged();
        return newId;
    });

    scriptlet->setInsertCueListAtCallback([this, findLi](int refListId, const std::string& name) -> int {
        const int refLi = findLi(refListId);
        if (refLi < 0) return -1;
        pushUndo();
        mcp::ShowFile::CueListData cld;
        cld.name      = name;
        cld.numericId = sf.nextListId();
        const int newId = cld.numericId;
        sf.cueLists.insert(sf.cueLists.begin() + refLi + 1, std::move(cld));
        insertEngineList(refLi + 1);
        dirty = true;
        emit dirtyChanged(true);
        emit cueListsChanged();
        return newId;
    });

    scriptlet->setDeleteCueListCallback([this, findLi](int listId) -> bool {
        const int li = findLi(listId);
        if (li < 0) return false;  // not found or already deleted
        if (sf.cueLists.size() <= 1) return false;  // must keep at least one list
        pushUndo();
        sf.cueLists.erase(sf.cueLists.begin() + li);
        removeEngineList(li);
        const int newActive = std::min(m_activeListIdx, (int)sf.cueLists.size() - 1);
        setActiveList(newActive);
        dirty = true;
        emit dirtyChanged(true);
        emit cueListsChanged();
        return true;
    });

    // --- mcp.time callbacks ---

    scriptlet->setGetSampleRateCallback([this]() -> int {
        return engineOk ? engine.sampleRate() : 44100;
    });

    scriptlet->setMusicalToSecondsCallback([this](int cueIdx, int bar, int beat) -> double {
        if (cueIdx < 0 || cueIdx >= cues().cueCount()) return -1.0;
        const mcp::MusicContext* mc = cues().musicContextOf(cueIdx);
        if (!mc) return -1.0;
        return mc->musicalToSeconds(bar, beat);
    });

    // --- mcp.get_mc() callback ---

    scriptlet->setGetMCCallback([this]() -> ScriptletMCInfo {
        ScriptletMCInfo info;
        int mcIdx = -1;
        for (int i = 0; i < cues().cueCount(); ++i) {
            const mcp::Cue* c = cues().cueAt(i);
            if (!c || !cues().hasMusicContext(i) || !cues().isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues().hasMusicContext(c->parentIndex)) {
                mcIdx = i; break;
            }
        }
        if (mcIdx < 0) return info;
        const mcp::MusicContext* mc = cues().musicContextOf(mcIdx);
        if (!mc) return info;
        const double elapsed = cues().cueElapsedSeconds(mcIdx);
        const auto musPos = mc->secondsToMusical(elapsed);
        const auto ts     = mc->timeSigAt(musPos.bar, musPos.beat);
        info.valid      = true;
        info.bpm        = mc->bpmAt(musPos.bar, musPos.beat, musPos.fraction);
        info.timeSigNum = ts.num;
        info.timeSigDen = ts.den;
        info.bar        = musPos.bar;
        info.beat       = musPos.beat;
        info.fraction   = musPos.fraction;
        return info;
    });

    // --- mcp.get_state() callback ---

    scriptlet->setGetStateCallback([this]() -> ScriptletStateInfo {
        ScriptletStateInfo state;
        state.selectedCue = cues().selectedIndex();
        for (int i = 0; i < cues().cueCount(); ++i)
            if (cues().isCuePlaying(i)) state.runningCues.push_back(i);
        for (int i = 0; i < cues().cueCount(); ++i) {
            const mcp::Cue* c = cues().cueAt(i);
            if (!c || !cues().hasMusicContext(i) || !cues().isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues().hasMusicContext(c->parentIndex)) {
                state.mcMaster = i; break;
            }
        }
        return state;
    });

    // --- mcp.cue_list list-info callbacks ---

    scriptlet->setListInfoCallback([this]() {
        std::vector<std::pair<int,std::string>> result;
        for (const auto& cl : sf.cueLists)
            result.push_back({cl.numericId, cl.name});
        return result;
    });
    scriptlet->setActiveListIdCallback([this]() {
        return sf.cueLists.empty() ? -1 : sf.cueLists[m_activeListIdx].numericId;
    });

    // --- Show Information state ---

    connect(this, &AppModel::cueFired, this, [this](int idx) {
        const mcp::Cue* c = cues().cueAt(idx);
        if (!c || c->parentIndex >= 0) return;  // ignore group children
        m_currentCueIdx = idx;
        if (c->type == mcp::CueType::Memo)
            m_currentMemo = c->name;
        emit showInfoChanged();
    });
    // selectionChanged already fires; showInfoChanged reuses it for Next Cue updates.
    connect(this, &AppModel::selectionChanged, this, [this](int) {
        emit showInfoChanged();
    });

    // --- Route incoming events to scriptlet event system ---

    connect(this, &AppModel::selectionChanged, this, [this](int idx) {
        scriptlet->fireCueSelectedEvent(idx);
    });
    connect(this, &AppModel::midiInputReceived, this,
        [this](mcp::MidiMsgType type, int ch, int d1, int d2) {
            scriptlet->fireMidiEvent(static_cast<int>(type), ch, d1, d2);
        });
    connect(this, &AppModel::oscInputReceived, this,
        [this](const QString& path, const QVariantList&) {
            scriptlet->fireOscEvent(path.toStdString());
        });
}

AppModel::~AppModel() {
    for (auto& cl : m_cueLists) cl->panic();
}

// ── Multi-list engine management ───────────────────────────────────────────

mcp::CueList& AppModel::cues() {
    return *m_cueLists[static_cast<size_t>(m_activeListIdx)];
}

void AppModel::syncListCount() {
    const int want = static_cast<int>(sf.cueLists.size());
    while (static_cast<int>(m_cueLists.size()) < want)
        m_cueLists.push_back(std::make_unique<mcp::CueList>(engine, scheduler));
    while (static_cast<int>(m_cueLists.size()) > want) {
        m_cueLists.back()->panic();
        m_cueLists.pop_back();
    }
    if (m_activeListIdx >= static_cast<int>(m_cueLists.size()))
        m_activeListIdx = std::max(0, static_cast<int>(m_cueLists.size()) - 1);
}

void AppModel::insertEngineList(int atIdx) {
    const int n = static_cast<int>(m_cueLists.size());
    if (atIdx < 0) atIdx = 0;
    if (atIdx > n) atIdx = n;
    m_cueLists.insert(m_cueLists.begin() + atIdx,
                      std::make_unique<mcp::CueList>(engine, scheduler));
    if (m_activeListIdx >= static_cast<int>(m_cueLists.size()))
        m_activeListIdx = std::max(0, static_cast<int>(m_cueLists.size()) - 1);
}

void AppModel::removeEngineList(int atIdx) {
    if (atIdx < 0 || atIdx >= static_cast<int>(m_cueLists.size())) return;
    m_cueLists[static_cast<size_t>(atIdx)]->panic();
    m_cueLists.erase(m_cueLists.begin() + atIdx);
    if (m_activeListIdx >= static_cast<int>(m_cueLists.size()))
        m_activeListIdx = std::max(0, static_cast<int>(m_cueLists.size()) - 1);
}

void AppModel::setActiveList(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_cueLists.size())) return;
    if (idx == m_activeListIdx) return;
    m_activeListIdx = idx;
    emit activeListChanged(idx);
    emit cueListChanged();
    emit selectionChanged(cues().selectedIndex());
}

void AppModel::panicAll() {
    for (auto& cl : m_cueLists) cl->panic();
    emit playbackStateChanged();
}

// ──────────────────────────────────────────────────────────────────────────

void AppModel::go() {
    // Clearing memo must happen before cueFired so that a Memo cue firing
    // on this same go() will set the memo back via the cueFired handler.
    m_currentMemo.clear();
    emit manualGo();
    cues().go();
    emit playbackStateChanged();
}

void AppModel::pushUndo() {
    if (sf.cueLists.empty()) return;
    undoStack.push_back(sf.cueLists);
    if ((int)undoStack.size() > kMaxUndo)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void AppModel::tick() {
    const bool wasActive = engine.activeVoiceCount() > 0;

    // Update all lists so background lists keep playing.
    for (auto& cl : m_cueLists) cl->update();

    const bool isActive = engine.activeVoiceCount() > 0;
    if (wasActive != isActive)
        emit playbackStateChanged();

    // Drain snapshot cues from all lists and recall.
    bool mixChanged = false;
    for (auto& cl : m_cueLists) {
        for (const auto& [cueIdx, snapId] : cl->drainPendingSnapshots()) {
            if (snapId >= 0 && snapshots.recallById(snapId)) {
                applyMixing();
                dirty = true;
                mixChanged = true;
            }
        }
    }

    // Sync sf.audioSetup from automation updates produced by scheduler callbacks.
    // Audio is already updated at scheduler precision; we only update sf for UI display.
    bool sendChanged = false;
    for (auto& cl : m_cueLists) {
        for (const auto& [path, value] : cl->drainAutomationUpdates()) {
            auto& as = sf.audioSetup;
            int ch = -1;
            if (path.size() > 7 && path.compare(0, 7, "/mixer/") == 0) {
                size_t slash = path.find('/', 7);
                try { ch = std::stoi(path.substr(7, slash == std::string::npos
                                                      ? std::string::npos : slash - 7)); }
                catch (...) { ch = -1; }
            }
            if (ch < 0 || ch >= (int)as.channels.size()) continue;
            auto& chan = as.channels[static_cast<size_t>(ch)];
            // Send slot params must be checked first — their paths also contain /mute etc.
            if (path.find("/send/") != std::string::npos) {
                const std::string sendMid = "/send/";
                const auto it2 = path.find(sendMid);
                if (it2 != std::string::npos) {
                    const std::string rest = path.substr(it2 + sendMid.size());
                    const auto sl = rest.find('/');
                    if (sl != std::string::npos) {
                        int slot = -1;
                        try { slot = std::stoi(rest.substr(0, sl)); } catch (...) {}
                        const std::string param = rest.substr(sl + 1);
                        if (slot >= 0 && slot < (int)chan.sends.size()) {
                            auto& s = chan.sends[static_cast<size_t>(slot)];
                            if      (param == "level") s.levelDb = static_cast<float>(value);
                            else if (param == "mute")  s.muted   = (value >= 0.5);
                            else if (param == "panL")  s.panL    = static_cast<float>(value);
                            else if (param == "panR")  s.panR    = static_cast<float>(value);
                            sendChanged = true;
                        }
                    }
                }
            } else if (path.find("/fader") != std::string::npos) {
                chan.masterGainDb = static_cast<float>(value);
            } else if (path.find("/mute") != std::string::npos) {
                chan.mute = (value >= 0.5);
            } else if (path.find("/polarity") != std::string::npos) {
                chan.phaseInvert = (value >= 0.5);
            } else if (path.find("/crosspoint/") != std::string::npos) {
                const std::string mid = "/crosspoint/";
                const auto it2 = path.find(mid);
                int out = -1;
                try { out = std::stoi(path.substr(it2 + mid.size())); } catch (...) {}
                if (out >= 0) {
                    bool found = false;
                    for (auto& xe : as.xpEntries) {
                        if (xe.ch == ch && xe.out == out) {
                            xe.db = static_cast<float>(value); found = true; break;
                        }
                    }
                    if (!found)
                        as.xpEntries.push_back({ch, out, static_cast<float>(value)});
                }
            }
            mixChanged = true;
            dirty = true;
        }
    }
    if (sendChanged) rebuildSendGains();
    if (mixChanged) {
        // Do NOT call applyMixing() here — audio was already updated by the scheduler.
        emit mixStateChanged();
    }

    // Drain scriptlets from ALL lists into a local buffer BEFORE running any of them.
    // Running a scriptlet can invoke insert/delete-cue-list callbacks which call
    // rebuildAllCueLists() → syncListCount(), mutating m_cueLists and invalidating
    // any range-for iterator over it.
    std::vector<std::pair<int,std::string>> pendingScriptlets;
    for (auto& cl : m_cueLists)
        for (auto& entry : cl->drainScriptlets())
            pendingScriptlets.push_back(std::move(entry));

    bool needListRefresh = false;
    for (const auto& [cueIdx, code] : pendingScriptlets) {
        if (m_scriptletRunningCues.count(cueIdx)) continue;   // already running this cue
        m_scriptletRunningCues.insert(cueIdx);
        const std::string err = scriptlet->run(code);
        m_scriptletRunningCues.erase(cueIdx);
        const bool hadError = scriptletErrorCues.count(cueIdx) > 0;
        if (!err.empty()) {
            scriptletErrorCues.insert(cueIdx);
            scriptletErrors[cueIdx] = err;
        } else {
            scriptletErrorCues.erase(cueIdx);
            scriptletErrors.erase(cueIdx);
        }
        if ((err.empty()) == hadError) needListRefresh = true;
    }
    if (needListRefresh) emit cueListChanged();

    // Fire cueFired for active list only (drives Show Information + scriptlet events).
    for (int idx : cues().drainFiredCues()) {
        emit cueFired(idx);
        scriptlet->fireCueFiredEvent(idx);
    }
    // Drain fired cues from background lists (discard — they drove their own triggers).
    for (int li = 0; li < static_cast<int>(m_cueLists.size()); ++li) {
        if (li != m_activeListIdx) m_cueLists[static_cast<size_t>(li)]->drainFiredCues();
    }

    // Music event detection on active list only.
    {
        int mcIdx = -1;
        for (int i = 0; i < cues().cueCount(); ++i) {
            const mcp::Cue* c = cues().cueAt(i);
            if (!c || !cues().hasMusicContext(i) || !cues().isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues().hasMusicContext(c->parentIndex)) {
                mcIdx = i; break;
            }
        }

        if (mcIdx != m_mcCueIdx) {
            m_mcCueIdx = mcIdx;
            m_lastMusicBoundary.clear();
        }

        if (mcIdx >= 0) {
            const mcp::MusicContext* mc = cues().musicContextOf(mcIdx);
            if (mc) {
                const double elapsed = cues().cueElapsedSeconds(mcIdx);
                static const int kSubs[] = {1, 2, 4, 8, 16};
                for (int sub : kSubs) {
                    auto it = m_lastMusicBoundary.find(sub);
                    if (it == m_lastMusicBoundary.end()) {
                        m_lastMusicBoundary[sub] = elapsed;
                        continue;
                    }
                    const double nextB = mc->nextQuantizationBoundary(it->second, sub);
                    if (elapsed >= nextB) {
                        scriptlet->fireMusicEvent(sub);
                        it->second = nextB;
                    }
                }
            }
        }
    }
}

void AppModel::storeSnapshot()    { syncPluginStatesToShowFile(); snapshots.store();    dirty = true; }
void AppModel::storeSnapshotAll() { syncPluginStatesToShowFile(); snapshots.storeAll(); dirty = true; }

void AppModel::recallSnapshot()
{
    if (!snapshots.recall()) return;
    ShowHelpers::applyChannelMap(*this);
    applyOutputDsp();
    applyPluginParamStates();
    dirty = true;
    emit mixStateChanged();
}

void AppModel::recallSnapshotById(int id)
{
    if (!snapshots.recallById(id)) return;
    ShowHelpers::applyChannelMap(*this);
    applyOutputDsp();
    applyPluginParamStates();
    dirty = true;
    emit mixStateChanged();
}

void AppModel::applyPluginParamStates()
{
    const auto& as = sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());
    for (int ch = 0; ch < nCh; ++ch) {
        if (static_cast<size_t>(ch) >= m_channelPlugins.size()) continue;
        const auto& chPlugins = m_channelPlugins[static_cast<size_t>(ch)];
        const auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
        for (int s = 0; s < static_cast<int>(pSlots.size()); ++s) {
            if (static_cast<size_t>(s) >= chPlugins.size()) continue;
            const auto& wrapper = chPlugins[static_cast<size_t>(s)];
            if (!wrapper) continue;
            const auto& sl = pSlots[static_cast<size_t>(s)];
            wrapper->setBypassed(sl.bypassed);
#ifdef __APPLE__
            if (auto* au = dynamic_cast<mcp::plugin::AUPluginAdapter*>(wrapper->getProcessor()))
                au->setNativeBypass(sl.bypassed);
#endif
            auto* proc = wrapper->getProcessor();
            if (!proc) continue;
            mcp::plugin::PluginState state;
            state.pluginId = sl.pluginId;
            if (sl.isExternal()) {
                state.backend  = sl.extBackend == "vst3"
                                     ? mcp::plugin::PluginBackend::VST3
                                     : mcp::plugin::PluginBackend::AU;
                state.stateData = sl.extStateBlob;
                for (const auto& [k, v] : sl.extParamSnapshot)
                    state.parameters[k] = v;
            } else {
                state.backend = mcp::plugin::PluginBackend::Internal;
                for (const auto& [k, v] : sl.parameters)
                    state.parameters[k] = v;
            }
            if (!state.stateData.empty() || !state.parameters.empty())
                proc->setState(state);
        }
    }
}

void AppModel::applyOscSettings() {
    oscServer.applySettings(sf.oscServer);
}

void AppModel::applyOutputDsp() {
    if (!engineOk) return;
    const int sr = engine.sampleRate();

    const int nCh = static_cast<int>(sf.audioSetup.channels.size());
    int nPhys = 0;
    if (sf.audioSetup.devices.empty()) {
        nPhys = engineOk ? engine.channels() : 2;
    } else {
        for (const auto& d : sf.audioSetup.devices) nPhys += d.channelCount;
    }
    nPhys = std::max(nPhys, static_cast<int>(sf.audioSetup.physOutDsp.size()));

    // Channel DSP: phase inversion and delay applied per logical channel, pre-fold.
    {
        std::vector<mcp::AudioEngine::ChannelDsp> chanConfig(static_cast<size_t>(nCh));
        for (int ch = 0; ch < nCh; ++ch) {
            const auto& c = sf.audioSetup.channels[static_cast<size_t>(ch)];
            chanConfig[static_cast<size_t>(ch)].phaseInvert  = c.phaseInvert;
            chanConfig[static_cast<size_t>(ch)].delaySamples = c.delayInSamples
                ? c.delaySamples
                : static_cast<int>(c.delayMs * sr / 1000.0 + 0.5);
        }
        engine.setChannelDsp(std::move(chanConfig));
    }

    // Physical output DSP: phase inversion and delay applied per output, post-fold.
    {
        std::vector<mcp::AudioEngine::OutputDsp> config(static_cast<size_t>(nPhys));
        const int nPhysDsp = static_cast<int>(sf.audioSetup.physOutDsp.size());
        for (int p = 0; p < nPhys && p < nPhysDsp; ++p) {
            const auto& pd = sf.audioSetup.physOutDsp[static_cast<size_t>(p)];
            config[static_cast<size_t>(p)].phaseInvert  = pd.phaseInvert;
            config[static_cast<size_t>(p)].delaySamples = pd.delayInSamples
                ? pd.delaySamples
                : static_cast<int>(pd.delayMs * sr / 1000.0 + 0.5);
        }
        engine.setOutputDsp(std::move(config));
    }
}

void AppModel::applyMixing() {
    ShowHelpers::applyChannelMap(*this);
    applyOutputDsp();
    rebuildSendGains();
}

void AppModel::rebuildSendTopology() {
    sendRouter.rebuildTopology(sf.audioSetup);
    rebuildPDC();
}

void AppModel::rebuildSendGains() {
    sendRouter.rebuildGains(sf.audioSetup);
    rebuildPDC();
}

void AppModel::rebuildPDC() {
    const auto& as = sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());

    const auto& cm = sendRouter.compiled();
    // Guard: topology not yet built — don't overwrite the engine's current valid state.
    if (static_cast<int>(cm.channelOrder.size()) != nCh) return;

    // 1. Compute per-channel plugin latency (sum across all non-bypassed slots).
    //    For linked stereo pairs the plugin chain lives on the master (ch) and processes
    //    both channels together, so the slave (ch+1) has the same latency as the master.
    std::vector<int> pluginDelay(static_cast<size_t>(nCh), 0);
    for (int ch = 0; ch < nCh; ++ch) {
        if (ch < static_cast<int>(m_channelPlugins.size())) {
            int total = 0;
            for (const auto& wp : m_channelPlugins[static_cast<size_t>(ch)]) {
                if (wp) total += wp->getLatencySamples();
            }
            pluginDelay[static_cast<size_t>(ch)] = total;
        }
    }
    // Propagate master delay to slave for linked stereo pairs.
    for (int ch = 0; ch + 1 < nCh; ++ch) {
        if (as.channels[static_cast<size_t>(ch)].linkedStereo)
            pluginDelay[static_cast<size_t>(ch + 1)] = pluginDelay[static_cast<size_t>(ch)];
    }

    // 2. Compute totalDelay and voice fold delay in topo order.
    //    Each channel's input summing node aligns all incoming sends+voices to
    //    maxIncoming = max(0, totalDelay[srcCh] for all srcCh that send to this ch).
    std::vector<int> totalDelay(static_cast<size_t>(nCh), 0);
    std::vector<int> voiceDelay(static_cast<size_t>(nCh), 0);  // = maxIncoming[ch]

    for (const int ch : cm.channelOrder) {
        if (ch < 0 || ch >= nCh) continue;
        int maxIn = 0;
        for (const auto& edge : cm.edges) {
            if (edge.dstCh == ch && edge.srcCh >= 0 && edge.srcCh < nCh)
                maxIn = std::max(maxIn, totalDelay[static_cast<size_t>(edge.srcCh)]);
        }
        voiceDelay[static_cast<size_t>(ch)] = maxIn;
        // PDC-isolated channels don't propagate their own plugin latency downstream.
        const bool isolated = ch < static_cast<int>(as.channels.size())
                              && as.channels[static_cast<size_t>(ch)].pdcIsolated;
        totalDelay[static_cast<size_t>(ch)] = maxIn + (isolated ? 0 : pluginDelay[static_cast<size_t>(ch)]);
    }

    // 3. Compute output alignment delays.
    int maxTotal = 0;
    for (int d : totalDelay) maxTotal = std::max(maxTotal, d);
    std::vector<int> outputDelay(static_cast<size_t>(nCh), 0);
    for (int ch = 0; ch < nCh; ++ch)
        outputDelay[static_cast<size_t>(ch)] = maxTotal - totalDelay[static_cast<size_t>(ch)];

    // 4. Build new CompiledMixState with PDC edge delays and per-channel PDC delays.
    mcp::CompiledMixState newState = cm;
    newState.pdcVoiceDelay  = voiceDelay;
    newState.pdcOutputDelay = outputDelay;
    for (auto& edge : newState.edges) {
        const int src = edge.srcCh, dst = edge.dstCh;
        if (src >= 0 && src < nCh && dst >= 0 && dst < nCh)
            edge.delaySamples = std::max(0, voiceDelay[static_cast<size_t>(dst)]
                                            - totalDelay[static_cast<size_t>(src)]);
        else
            edge.delaySamples = 0;
    }

    // 5. Flush PDC rings when any delay values changed.
    //    Must also check per-edge delays: adding a new send with a non-zero edge delay
    //    grows the edge list but may not change voiceDelay/outputDelay — without a
    //    flush the new edge's ring buffer would never be allocated.
    std::vector<int> edgeDelays(newState.edges.size());
    for (size_t e = 0; e < newState.edges.size(); ++e)
        edgeDelays[e] = newState.edges[e].delaySamples;

    const bool delaysChanged = (voiceDelay  != m_pdcVoiceDelay)  ||
                               (outputDelay != m_pdcOutputDelay) ||
                               (edgeDelays  != m_pdcEdgeDelays);
    if (delaysChanged) {
        m_pdcVoiceDelay  = voiceDelay;
        m_pdcOutputDelay = outputDelay;
        m_pdcEdgeDelays  = edgeDelays;
        engine.flushPDCRings(nCh, static_cast<int>(newState.edges.size()));
    }
    engine.setCompiledMixState(newState);
}

bool AppModel::sendWouldCreateCycle(int srcCh, int dstCh) const {
    return sendRouter.wouldCreateCycle(srcCh, dstCh, sf.audioSetup);
}

void AppModel::buildChannelPluginChains() {
    auto&     as  = sf.audioSetup;   // mutable: safe-load updates loadFailCount/disabled
    const int nCh = static_cast<int>(as.channels.size());
    const int sr  = engine.sampleRate() > 0 ? engine.sampleRate() : 48000;

    m_channelPlugins.assign(static_cast<size_t>(nCh), {});

    std::vector<std::shared_ptr<mcp::plugin::ChannelPluginChain>> chains(
        static_cast<size_t>(nCh), nullptr);

    for (int ch = 0; ch < nCh; ++ch) {
        // Slave channels of stereo pairs share the master's chain.
        if (ch > 0 && as.channels[static_cast<size_t>(ch - 1)].linkedStereo) continue;

        const bool isStereo = as.channels[static_cast<size_t>(ch)].linkedStereo
                              && (ch + 1 < nCh);
        const int numCh = isStereo ? 2 : 1;

        auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
        if (pSlots.empty()) continue;

        auto chain = std::make_shared<mcp::plugin::ChannelPluginChain>(numCh);

        m_channelPlugins[static_cast<size_t>(ch)].resize(
            static_cast<size_t>(mcp::plugin::ChannelPluginChain::kMaxSlots));

        for (int s = 0; s < static_cast<int>(pSlots.size())
                        && s < mcp::plugin::ChannelPluginChain::kMaxSlots; ++s) {
            auto& sl = pSlots[static_cast<size_t>(s)];

            std::unique_ptr<mcp::plugin::AudioProcessor> proc;
            if (sl.isExternal()) {
                mcp::plugin::ExternalPluginReference ref;
                ref.backend        = sl.extBackend;
                ref.pluginId       = sl.pluginId;
                ref.path           = sl.extPath;
                ref.name           = sl.extName;
                ref.vendor         = sl.extVendor;
                ref.version        = sl.extVersion;
                ref.numChannels    = sl.extNumChannels > 0 ? sl.extNumChannels : numCh;
                ref.stateBlob      = sl.extStateBlob;
                ref.paramSnapshot  = sl.extParamSnapshot;

                if (sl.disabled) {
                    // Safe-load: slot explicitly disabled; preserve state, skip instantiation.
                    proc = std::make_unique<mcp::plugin::MissingPluginProcessor>(
                        ref, mcp::plugin::PluginRuntimeStatus::Disabled,
                        "Plugin slot disabled (safe-load)");
                } else {
                    proc = m_nativeBackend.load(ref);
                    // Track consecutive failures; auto-disable to prevent crash loops.
                    const auto* mp =
                        dynamic_cast<const mcp::plugin::MissingPluginProcessor*>(proc.get());
                    if (mp && (mp->runtimeStatus() == mcp::plugin::PluginRuntimeStatus::Failed ||
                               mp->runtimeStatus() == mcp::plugin::PluginRuntimeStatus::Missing)) {
                        sl.loadFailCount++;
                        if (sl.loadFailCount >= 3)
                            sl.disabled = true;
                    } else {
                        sl.loadFailCount = 0; // reset on successful load
                    }
                }
            } else {
                if (sl.pluginId.empty()) continue;
                proc = pluginFactory.create(sl.pluginId);
                if (!proc) continue;
                mcp::plugin::PluginState state;
                state.pluginId = sl.pluginId;
                state.backend  = mcp::plugin::PluginBackend::Internal;
                state.version  = 1;
                for (const auto& [k, v] : sl.parameters)
                    state.parameters[k] = v;
                proc->setState(state);
            }

            auto wrapper = std::make_shared<mcp::plugin::PluginWrapper>(std::move(proc));
            wrapper->setBypassed(sl.bypassed);  // restore persisted bypass state
#ifdef __APPLE__
            if (auto* au = dynamic_cast<mcp::plugin::AUPluginAdapter*>(wrapper->getProcessor())) {
                au->setNativeBypass(sl.bypassed);
                au->startWatchingLatency([this]() { rebuildPDC(); });
            }
#endif
            m_channelPlugins[static_cast<size_t>(ch)][static_cast<size_t>(s)] = wrapper;
            chain->setSlot(s, wrapper);
        }

        chain->prepare(static_cast<double>(sr), 8192);
        chains[static_cast<size_t>(ch)] = std::move(chain);
    }

    engine.setChannelPluginChains(std::move(chains));

    // Recompute PDC now that plugin latencies are known.
    rebuildPDC();

    // Inject plugin accessor into all engine CueLists.
    mcp::CueList::PluginAccessor acc = [this](int ch, int slot)
        -> std::shared_ptr<mcp::plugin::AudioProcessor>
    {
        if (ch < 0 || ch >= static_cast<int>(m_channelPlugins.size())) return nullptr;
        const auto& sv = m_channelPlugins[static_cast<size_t>(ch)];
        if (slot < 0 || slot >= static_cast<int>(sv.size())) return nullptr;
        const auto& w = sv[static_cast<size_t>(slot)];
        return w ? w->getProcessorShared() : nullptr;
    };
    for (int i = 0; i < static_cast<int>(m_cueLists.size()); ++i)
        m_cueLists[static_cast<size_t>(i)]->setPluginAccessor(acc);
}

std::shared_ptr<mcp::plugin::PluginWrapper> AppModel::channelPlugin(int ch, int slot) const {
    if (ch < 0 || ch >= static_cast<int>(m_channelPlugins.size())) return nullptr;
    const auto& sv = m_channelPlugins[static_cast<size_t>(ch)];
    if (slot < 0 || slot >= static_cast<int>(sv.size())) return nullptr;
    return sv[static_cast<size_t>(slot)];
}

mcp::plugin::PluginRuntimeStatus AppModel::channelPluginStatus(int ch, int slot) const {
    const auto& w = channelPlugin(ch, slot);
    if (!w) return mcp::plugin::PluginRuntimeStatus::Ok;
    return mcp::plugin::NativePluginBackend::statusOf(*w->getProcessor());
}

void AppModel::syncPluginStatesToShowFile() {
    auto& as = sf.audioSetup;
    for (int ch = 0; ch < static_cast<int>(as.channels.size()); ++ch) {
        auto& pslots = as.channels[static_cast<size_t>(ch)].plugins;
        for (int s = 0; s < static_cast<int>(pslots.size()); ++s) {
            auto& sl = pslots[static_cast<size_t>(s)];
            const auto& w = channelPlugin(ch, s);
            if (!w || !w->getProcessor()) continue;
            if (sl.isExternal()) {
                const auto state = w->getProcessor()->getState();
                sl.extStateBlob     = state.stateData;
                sl.extParamSnapshot.clear();
                for (const auto& [id, val] : state.parameters)
                    sl.extParamSnapshot[id] = val;
            } else {
                // Sync live processor values → sl.parameters so snapshots capture
                // the current state even if the editor was never opened.
                for (const auto& p : w->getProcessor()->getParameters())
                    sl.parameters[p.id] = w->getProcessor()->getParameterValue(p.id);
            }
        }
    }
}


void AppModel::applyMidiInput() {
    midiIn.openAll();
}

void AppModel::applyScriptletLibrary() {
    // Start with built-in modules; user entries with the same name take precedence.
    std::vector<std::pair<std::string,std::string>> mods;
    mods.reserve(m_builtinScriptlets.size() + sf.scriptletLibrary.entries.size());
    for (const auto& e : m_builtinScriptlets)
        mods.emplace_back(e.name, e.code);
    for (const auto& e : sf.scriptletLibrary.entries) {
        auto it = std::find_if(mods.begin(), mods.end(),
            [&](const auto& p){ return p.first == e.name; });
        if (it != mods.end()) it->second = e.code;   // user overrides built-in
        else                  mods.emplace_back(e.name, e.code);
    }
    scriptlet->setLibrary(mods);
}

void AppModel::loadBuiltinScriptlets(const QString& dir) {
    m_builtinScriptlets.clear();
    if (!dir.isEmpty()) {
        QDir d(dir);
        const QStringList files = d.entryList({"*.py"}, QDir::Files, QDir::Name);
        for (const QString& fn : files) {
            QFile f(d.filePath(fn));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            mcp::ShowFile::ScriptletLibrary::Entry entry;
            entry.name = QFileInfo(fn).completeBaseName().toStdString();
            entry.code = QString::fromUtf8(f.readAll()).toStdString();
            m_builtinScriptlets.push_back(std::move(entry));
        }
    }
    applyScriptletLibrary();
}

// ---------------------------------------------------------------------------
// MIDI matching helper
static bool midiMatches(const mcp::MidiTrigger& t,
                        mcp::MidiMsgType type, int ch, int d1, int d2) {
    if (!t.enabled) return false;
    if (t.type != type) return false;
    if (t.channel != 0 && t.channel != ch) return false;
    if (t.data1 != d1) return false;
    if (t.data2 >= 0 && t.data2 != d2) return false;
    return true;
}
static bool midiMatchesCtrl(const mcp::ControlMidiBinding& t,
                             mcp::MidiMsgType type, int ch, int d1, int d2) {
    if (!t.enabled) return false;
    if (t.type != type) return false;
    if (t.channel != 0 && t.channel != ch) return false;
    if (t.data1 != d1) return false;
    if (t.data2 >= 0 && t.data2 != d2) return false;
    return true;
}

// DFS pre-order traversal of the SF cue tree (matches rebuildCueList insertion order).
// Calls visitor(flatIdx, CueData) for every cue, including group children.
static void visitAllCues(const std::vector<mcp::ShowFile::CueData>& cues,
                          int& counter,
                          const std::function<void(int, const mcp::ShowFile::CueData&)>& visitor)
{
    for (const auto& cd : cues) {
        const int myIdx = counter++;
        visitor(myIdx, cd);
        if (!cd.children.empty())
            visitAllCues(cd.children, counter, visitor);
    }
}

void AppModel::routeMidi(mcp::MidiMsgType type, int channel, int data1, int data2) {
    emit midiInputReceived(type, channel, data1, data2);
    // Per-cue triggers — check every list (background lists fire normally).
    for (int li = 0; li < (int)sf.cueLists.size() && li < (int)m_cueLists.size(); ++li) {
        auto& engList = *m_cueLists[static_cast<size_t>(li)];
        int counter = 0;
        visitAllCues(sf.cueLists[static_cast<size_t>(li)].cues, counter,
            [&](int flatIdx, const mcp::ShowFile::CueData& cd) {
                if (midiMatches(cd.triggers.midi, type, channel, data1, data2)) {
                    engList.start(flatIdx);
                    if (li == m_activeListIdx) emit externalTriggerFired(flatIdx);
                    emit playbackStateChanged();
                }
            });
    }
    // System actions
    for (const auto& e : sf.systemControls.entries) {
        if (!midiMatchesCtrl(e.midi, type, channel, data1, data2)) continue;
        switch (e.action) {
            case mcp::ControlAction::Go:
                cues().go(); emit playbackStateChanged(); break;
            case mcp::ControlAction::Arm:
                if (!multiSel.empty()) {
                    cues().toggleArm(*multiSel.begin());
                    emit cueListChanged();
                }
                break;
            case mcp::ControlAction::PanicSelected:
                for (int idx : multiSel) cues().stop(idx);
                emit playbackStateChanged(); break;
            case mcp::ControlAction::SelectionUp:
                if (!multiSel.empty()) {
                    int nxt = std::max(0, *multiSel.begin() - 1);
                    multiSel = {nxt};
                    emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::SelectionDown:
                if (!multiSel.empty()) {
                    int nxt = std::min(cues().cueCount() - 1, *multiSel.rbegin() + 1);
                    multiSel = {nxt};
                    emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::PanicAll:
                panicAll(); break;
        }
    }
}

void AppModel::routeOsc(const QString& path, const QVariantList& args) {
    emit oscInputReceived(path, args);
    const std::string p = path.toStdString();

    // Per-cue OSC triggers — check every list.
    for (int li = 0; li < (int)sf.cueLists.size() && li < (int)m_cueLists.size(); ++li) {
        auto& engList = *m_cueLists[static_cast<size_t>(li)];
        int counter = 0;
        visitAllCues(sf.cueLists[static_cast<size_t>(li)].cues, counter,
            [&](int flatIdx, const mcp::ShowFile::CueData& cd) {
                const auto& ot = cd.triggers.osc;
                if (ot.enabled && !ot.path.empty() && ot.path == p) {
                    engList.start(flatIdx);
                    if (li == m_activeListIdx) emit externalTriggerFired(flatIdx);
                    emit playbackStateChanged();
                }
            });
    }

    // System vocabulary
    if (p == "/go") {
        cues().go(); emit playbackStateChanged();
    } else if (p == "/panic") {
        panicAll();
    } else if (p == "/prev") {
        cues().prev(); emit playbackStateChanged();
    } else if (p == "/next") {
        cues().go();  emit playbackStateChanged();  // same as go for now
    } else if (p == "/start" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues().findByCueNumber(num.toStdString());
        if (idx >= 0) { cues().start(idx); emit playbackStateChanged(); }
    } else if (p == "/stop" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues().findByCueNumber(num.toStdString());
        if (idx >= 0) { cues().stop(idx); emit playbackStateChanged(); }
    } else if (p == "/goto" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues().findByCueNumber(num.toStdString());
        if (idx >= 0) {
            multiSel = {idx};
            emit selectionChanged(idx);
        }
    }

    // System control OSC bindings
    for (const auto& e : sf.systemControls.entries) {
        if (!e.osc.enabled || e.osc.path != p) continue;
        switch (e.action) {
            case mcp::ControlAction::Go:
                cues().go(); emit playbackStateChanged(); break;
            case mcp::ControlAction::Arm:
                if (!multiSel.empty()) { cues().toggleArm(*multiSel.begin()); emit cueListChanged(); }
                break;
            case mcp::ControlAction::PanicSelected:
                for (int idx : multiSel) cues().stop(idx);
                emit playbackStateChanged(); break;
            case mcp::ControlAction::SelectionUp:
                if (!multiSel.empty()) {
                    int nxt = std::max(0, *multiSel.begin() - 1);
                    multiSel = {nxt}; emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::SelectionDown:
                if (!multiSel.empty()) {
                    int nxt = std::min(cues().cueCount() - 1, *multiSel.rbegin() + 1);
                    multiSel = {nxt}; emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::PanicAll:
                panicAll(); break;
        }
    }
}
