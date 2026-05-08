#include "AppModel.h"

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
    });
    scriptlet->setSelectCallback([this](const std::string& num) {
        const int idx = cues.findByCueNumber(num);
        if (idx >= 0) {
            cues.setSelectedIndex(idx);
            emit selectionChanged(idx);
        }
    });
    scriptlet->setAlertCallback([](const std::string& msg) {
        QMessageBox::information(nullptr, "Script", QString::fromStdString(msg));
    });
    scriptlet->setOutputCallback([this](const std::string& text) {
        emit scriptletOutput(QString::fromStdString(text));
    });
}

AppModel::~AppModel() {
    cues.panic();
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
    for (const auto& code : cues.drainScriptlets()) {
        const std::string err = scriptlet->run(code);
        if (!err.empty())
            emit scriptletError(QString::fromStdString(err));
    }
}

void AppModel::applyOscSettings() {
    oscServer.applySettings(sf.oscServer);
}

void AppModel::applyMidiInput() {
    midiIn.openAll();
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
