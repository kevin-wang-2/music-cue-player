#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/Cue.h"
#include "engine/MusicContext.h"

#include <QMessageBox>
#include <QVariant>

AppModel::AppModel(QObject* parent)
    : QObject(parent)
    , scheduler(engine)
    , cues(engine, scheduler)
    , midiIn(this)
    , oscServer(this)
    , scriptlet(std::make_unique<ScriptletEngine>())
{
    connect(&midiIn,   &MidiInputManager::midiReceived,
            this, &AppModel::routeMidi);
    connect(&oscServer, &OscServer::messageReceived,
            this, &AppModel::routeOsc);

    scriptlet->setGoCallback([this]() {
        cues.go();
        emit selectionChanged(cues.selectedIndex());
        emit playbackStateChanged();
    });
    scriptlet->setSelectCallback([this](const std::string& num) {
        const int idx = cues.findByCueNumber(num);
        if (idx >= 0) { cues.setSelectedIndex(idx); emit selectionChanged(idx); }
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
        cues.panic();
        emit playbackStateChanged();
    });

    // --- mcp.cue callbacks ---
    scriptlet->setCueCountCallback([this]() { return cues.cueCount(); });

    scriptlet->setCueInfoCallback([this](int idx) -> ScriptletCueInfo {
        ScriptletCueInfo info;
        const mcp::Cue* c = cues.cueAt(idx);
        if (!c) return info;
        info.number       = c->cueNumber;
        info.name         = c->name;
        info.preWait      = c->preWaitSeconds;
        info.autoContinue = c->autoContinue;
        info.autoFollow   = c->autoFollow;
        info.isPlaying    = cues.isCuePlaying(idx);
        info.isPending    = cues.isCuePending(idx);
        info.isArmed      = cues.isArmed(idx);
        // type string
        switch (c->type) {
            case mcp::CueType::Audio:        info.type = "audio";        break;
            case mcp::CueType::Start:        info.type = "start";        break;
            case mcp::CueType::Stop:         info.type = "stop";         break;
            case mcp::CueType::Arm:          info.type = "arm";          break;
            case mcp::CueType::Fade:         info.type = "fade";         break;
            case mcp::CueType::Devamp:       info.type = "devamp";       break;
            case mcp::CueType::Group:        info.type = "group";        break;
            case mcp::CueType::MusicContext: info.type = "music_context";break;
            case mcp::CueType::Marker:       info.type = "marker";       break;
            case mcp::CueType::Network:      info.type = "network";      break;
            case mcp::CueType::Midi:         info.type = "midi";         break;
            case mcp::CueType::Timecode:     info.type = "timecode";     break;
            case mcp::CueType::Goto:         info.type = "goto";         break;
            case mcp::CueType::Memo:         info.type = "memo";         break;
            case mcp::CueType::Scriptlet:    info.type = "scriptlet";    break;
        }
        // Audio fields
        if (c->type == mcp::CueType::Audio) {
            info.path      = c->path;
            info.level     = c->level;
            info.trim      = c->trim;
            info.startTime = c->startTime;
            info.duration  = c->duration;
            info.playhead  = cues.cuePlayheadFileSeconds(idx);
        }
        // Control cue target
        if (c->targetIndex >= 0) {
            info.targetIndex = c->targetIndex;
            const mcp::Cue* t = cues.cueAt(c->targetIndex);
            if (t) info.targetNumber = t->cueNumber;
        }
        // Scriptlet code
        if (c->type == mcp::CueType::Scriptlet)
            info.code = c->scriptletCode;
        return info;
    });

    scriptlet->setCueSelectCallback([this](int idx) {
        if (idx >= 0 && idx < cues.cueCount()) {
            cues.setSelectedIndex(idx);
            emit selectionChanged(idx);
        }
    });
    scriptlet->setCueGoCallback([this](int idx) {
        if (idx >= 0 && idx < cues.cueCount()) {
            cues.setSelectedIndex(idx);
            cues.go();
            emit selectionChanged(cues.selectedIndex());
            emit playbackStateChanged();
        }
    });
    scriptlet->setCueArmCallback([this](int idx, double startOverride) {
        if (idx >= 0 && idx < cues.cueCount())
            cues.arm(idx, startOverride);
    });
    scriptlet->setCueStopCallback([this](int idx) {
        if (idx >= 0 && idx < cues.cueCount()) {
            cues.stop(idx);
            emit playbackStateChanged();
        }
    });
    scriptlet->setCueDisarmCallback([this](int idx) {
        if (idx >= 0 && idx < cues.cueCount())
            cues.disarm(idx);
    });
    scriptlet->setCueSetNameCallback([this](int idx, const std::string& name) {
        if (idx >= 0 && idx < cues.cueCount()) {
            cues.setCueName(idx, name);
            ShowHelpers::syncSfFromCues(*this);
            emit cueListChanged();
        }
    });

    // --- mcp.cue mutation callbacks ---

    scriptlet->setCueInsertCallback([this](const std::string& type,
                                           const std::string& number,
                                           const std::string& name) -> int {
        if (sf.cueLists.empty()) return -1;
        pushUndo();
        mcp::ShowFile::CueData cd;
        cd.type      = type;
        cd.cueNumber = number.empty() ? ShowHelpers::nextCueNumber(sf) : number;
        cd.name      = name;
        const int beforeIdx = cues.cueCount();  // past-end → appends
        ShowHelpers::sfInsertBefore(sf, beforeIdx, std::move(cd));
        std::string err;
        ShowHelpers::rebuildCueList(*this, err);
        dirty = true;
        emit cueListChanged();
        const int newIdx = cues.cueCount() - 1;
        scriptlet->fireCueInsertedEvent(newIdx);
        return newIdx;
    });

    scriptlet->setCueInsertAtCallback([this](int refIdx,
                                              const std::string& type,
                                              const std::string& number,
                                              const std::string& name) -> int {
        if (sf.cueLists.empty() || refIdx < 0 || refIdx >= cues.cueCount()) return -1;
        pushUndo();
        mcp::ShowFile::CueData cd;
        cd.type      = type;
        cd.cueNumber = number.empty() ? ShowHelpers::nextCueNumber(sf) : number;
        cd.name      = name;
        ShowHelpers::sfInsertBefore(sf, refIdx + 1, std::move(cd));
        std::string err;
        ShowHelpers::rebuildCueList(*this, err);
        dirty = true;
        emit cueListChanged();
        scriptlet->fireCueInsertedEvent(refIdx + 1);
        return refIdx + 1;
    });

    scriptlet->setCueMoveCallback([this](int refIdx, int cueIdx, bool toGroup) {
        if (sf.cueLists.empty()) return;
        if (cueIdx < 0 || cueIdx >= cues.cueCount()) return;
        if (refIdx < 0 || refIdx >= cues.cueCount()) return;
        if (refIdx == cueIdx) return;
        pushUndo();
        mcp::ShowFile::CueData cd = ShowHelpers::sfRemoveAt(sf, cueIdx);
        // After removal, indices at > cueIdx shift down by 1.
        int adjustedRef = (refIdx > cueIdx) ? refIdx - 1 : refIdx;
        if (toGroup) {
            ShowHelpers::sfAppendToGroup(sf, adjustedRef, std::move(cd));
        } else {
            ShowHelpers::sfInsertBefore(sf, adjustedRef + 1, std::move(cd));
        }
        std::string err;
        ShowHelpers::rebuildCueList(*this, err);
        dirty = true;
        emit cueListChanged();
    });

    scriptlet->setCueDeleteCallback([this](int idx) {
        if (sf.cueLists.empty() || idx < 0 || idx >= cues.cueCount()) return;
        pushUndo();
        ShowHelpers::sfRemoveAt(sf, idx);
        ShowHelpers::sfFixTargetsAfterRemoval(sf, idx);
        std::string err;
        ShowHelpers::rebuildCueList(*this, err);
        dirty = true;
        emit cueListChanged();
    });

    // --- mcp.cue.start callback ---

    scriptlet->setCueStartCallback([this](int idx) {
        if (idx < 0 || idx >= cues.cueCount()) return;
        cues.start(idx);
        emit playbackStateChanged();
    });

    // --- mcp.time callbacks ---

    scriptlet->setGetSampleRateCallback([this]() -> int {
        return engineOk ? engine.sampleRate() : 44100;
    });

    scriptlet->setMusicalToSecondsCallback([this](int cueIdx, int bar, int beat) -> double {
        if (cueIdx < 0 || cueIdx >= cues.cueCount()) return -1.0;
        const mcp::MusicContext* mc = cues.musicContextOf(cueIdx);
        if (!mc) return -1.0;
        return mc->musicalToSeconds(bar, beat);
    });

    // --- mcp.get_mc() callback ---

    scriptlet->setGetMCCallback([this]() -> ScriptletMCInfo {
        ScriptletMCInfo info;
        // Find the outermost playing cue that has an MC and no MC-having parent.
        int mcIdx = -1;
        for (int i = 0; i < cues.cueCount(); ++i) {
            const mcp::Cue* c = cues.cueAt(i);
            if (!c || !cues.hasMusicContext(i) || !cues.isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues.hasMusicContext(c->parentIndex)) {
                mcIdx = i; break;
            }
        }
        if (mcIdx < 0) return info;
        const mcp::MusicContext* mc = cues.musicContextOf(mcIdx);
        if (!mc) return info;
        const double elapsed = cues.cueElapsedSeconds(mcIdx);
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
        // selected cue
        state.selectedCue = cues.selectedIndex();
        // running cues
        for (int i = 0; i < cues.cueCount(); ++i)
            if (cues.isCuePlaying(i)) state.runningCues.push_back(i);
        // mc master — outermost playing cue with MC and no MC-having parent
        for (int i = 0; i < cues.cueCount(); ++i) {
            const mcp::Cue* c = cues.cueAt(i);
            if (!c || !cues.hasMusicContext(i) || !cues.isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues.hasMusicContext(c->parentIndex)) {
                state.mcMaster = i; break;
            }
        }
        return state;
    });

    // --- Show Information state ---

    connect(this, &AppModel::cueFired, this, [this](int idx) {
        const mcp::Cue* c = cues.cueAt(idx);
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
    cues.panic();
}

void AppModel::go() {
    // Clearing memo must happen before cueFired so that a Memo cue firing
    // on this same go() will set the memo back via the cueFired handler.
    m_currentMemo.clear();
    emit manualGo();
    cues.go();
    emit playbackStateChanged();
}

void AppModel::pushUndo() {
    if (sf.cueLists.empty()) return;
    undoStack.push_back(sf.cueLists[0].cues);
    if ((int)undoStack.size() > kMaxUndo)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void AppModel::tick() {
    const bool wasActive = engine.activeVoiceCount() > 0;
    cues.update();
    const bool isActive  = engine.activeVoiceCount() > 0;
    if (wasActive != isActive)
        emit playbackStateChanged();

    // Run any scriptlets queued since last tick (on main thread for Qt safety).
    for (const auto& [cueIdx, code] : cues.drainScriptlets()) {
        const std::string err = scriptlet->run(code);
        const bool hadError = scriptletErrorCues.count(cueIdx) > 0;
        if (!err.empty()) {
            scriptletErrorCues.insert(cueIdx);
            scriptletErrors[cueIdx] = err;
        } else {
            scriptletErrorCues.erase(cueIdx);
            scriptletErrors.erase(cueIdx);
        }
        // Refresh the status dot if error state changed.
        if ((err.empty()) == hadError)
            emit cueListChanged();
    }

    // Fire on_cue_fired events for any cues triggered since last tick.
    for (int idx : cues.drainFiredCues()) {
        emit cueFired(idx);
        scriptlet->fireCueFiredEvent(idx);
    }

    // Music event detection: find active MC and fire on_music_event callbacks.
    {
        int mcIdx = -1;
        for (int i = 0; i < cues.cueCount(); ++i) {
            const mcp::Cue* c = cues.cueAt(i);
            if (!c || !cues.hasMusicContext(i) || !cues.isCuePlaying(i)) continue;
            if (c->parentIndex < 0 || !cues.hasMusicContext(c->parentIndex)) {
                mcIdx = i; break;
            }
        }

        if (mcIdx != m_mcCueIdx) {
            m_mcCueIdx = mcIdx;
            m_lastMusicBoundary.clear();
        }

        if (mcIdx >= 0) {
            const mcp::MusicContext* mc = cues.musicContextOf(mcIdx);
            if (mc) {
                const double elapsed = cues.cueElapsedSeconds(mcIdx);
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

void AppModel::applyOscSettings() {
    oscServer.applySettings(sf.oscServer);
}

void AppModel::applyMidiInput() {
    midiIn.openAll();
}

void AppModel::applyScriptletLibrary() {
    std::vector<std::pair<std::string,std::string>> mods;
    mods.reserve(sf.scriptletLibrary.entries.size());
    for (const auto& e : sf.scriptletLibrary.entries)
        mods.emplace_back(e.name, e.code);
    scriptlet->setLibrary(mods);
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

void AppModel::routeMidi(mcp::MidiMsgType type, int channel, int data1, int data2) {
    emit midiInputReceived(type, channel, data1, data2);
    // Per-cue triggers
    if (!sf.cueLists.empty()) {
        const auto& cueDatas = sf.cueLists[0].cues;
        for (int i = 0; i < (int)cueDatas.size(); ++i) {
            if (midiMatches(cueDatas[i].triggers.midi, type, channel, data1, data2)) {
                cues.start(i);
                emit externalTriggerFired(i);
                emit playbackStateChanged();
            }
        }
    }
    // System actions
    for (const auto& e : sf.systemControls.entries) {
        if (!midiMatchesCtrl(e.midi, type, channel, data1, data2)) continue;
        switch (e.action) {
            case mcp::ControlAction::Go:
                cues.go(); emit playbackStateChanged(); break;
            case mcp::ControlAction::Arm:
                if (!multiSel.empty()) {
                    cues.toggleArm(*multiSel.begin());
                    emit cueListChanged();
                }
                break;
            case mcp::ControlAction::PanicSelected:
                for (int idx : multiSel) cues.stop(idx);
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
                    int nxt = std::min(cues.cueCount() - 1, *multiSel.rbegin() + 1);
                    multiSel = {nxt};
                    emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::PanicAll:
                cues.panic(); emit playbackStateChanged(); break;
        }
    }
}

void AppModel::routeOsc(const QString& path, const QVariantList& args) {
    emit oscInputReceived(path, args);
    const std::string p = path.toStdString();

    // Per-cue OSC triggers
    if (!sf.cueLists.empty()) {
        const auto& cueDatas = sf.cueLists[0].cues;
        for (int i = 0; i < (int)cueDatas.size(); ++i) {
            const auto& ot = cueDatas[i].triggers.osc;
            if (ot.enabled && !ot.path.empty() && ot.path == p) {
                cues.start(i);
                emit externalTriggerFired(i);
                emit playbackStateChanged();
            }
        }
    }

    // System vocabulary
    if (p == "/go") {
        cues.go(); emit playbackStateChanged();
    } else if (p == "/panic") {
        cues.panic(); emit playbackStateChanged();
    } else if (p == "/prev") {
        cues.prev(); emit playbackStateChanged();
    } else if (p == "/next") {
        cues.go();  emit playbackStateChanged();  // same as go for now
    } else if (p == "/start" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues.findByCueNumber(num.toStdString());
        if (idx >= 0) { cues.start(idx); emit playbackStateChanged(); }
    } else if (p == "/stop" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues.findByCueNumber(num.toStdString());
        if (idx >= 0) { cues.stop(idx); emit playbackStateChanged(); }
    } else if (p == "/goto" && !args.isEmpty()) {
        const QString num = args[0].toString();
        const int idx = cues.findByCueNumber(num.toStdString());
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
                cues.go(); emit playbackStateChanged(); break;
            case mcp::ControlAction::Arm:
                if (!multiSel.empty()) { cues.toggleArm(*multiSel.begin()); emit cueListChanged(); }
                break;
            case mcp::ControlAction::PanicSelected:
                for (int idx : multiSel) cues.stop(idx);
                emit playbackStateChanged(); break;
            case mcp::ControlAction::SelectionUp:
                if (!multiSel.empty()) {
                    int nxt = std::max(0, *multiSel.begin() - 1);
                    multiSel = {nxt}; emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::SelectionDown:
                if (!multiSel.empty()) {
                    int nxt = std::min(cues.cueCount() - 1, *multiSel.rbegin() + 1);
                    multiSel = {nxt}; emit selectionChanged(nxt);
                }
                break;
            case mcp::ControlAction::PanicAll:
                cues.panic(); emit playbackStateChanged(); break;
        }
    }
}
