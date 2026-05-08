#pragma once

#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include "engine/ShowFile.h"
#include "MidiInputManager.h"
#include "OscServer.h"

#include <QObject>
#include <QString>
#include <set>
#include <string>
#include <vector>

// Central application model.  Owns the audio engine stack and show data.
// All UI widgets hold a pointer to this object.
class AppModel : public QObject {
    Q_OBJECT
public:
    explicit AppModel(QObject* parent = nullptr);
    ~AppModel() override;

    // Engine stack — declared in init order (each depends on the previous).
    mcp::AudioEngine engine;
    mcp::Scheduler   scheduler;
    mcp::CueList     cues;

    // External trigger infrastructure
    MidiInputManager midiIn;
    OscServer        oscServer;

    // Show data
    mcp::ShowFile sf;
    std::string   showPath;   // empty = unsaved
    std::string   baseDir;
    bool          dirty{false};
    bool          engineOk{false};
    std::string   engineError;

    // UI state shared between widgets
    std::set<int>                         multiSel;
    std::vector<mcp::ShowFile::CueData>   clipboard;

    // Undo / redo history — snapshots of cueLists[0].cues.
    static constexpr int kMaxUndo = 100;
    std::vector<std::vector<mcp::ShowFile::CueData>> undoStack;
    std::vector<std::vector<mcp::ShowFile::CueData>> redoStack;

    // Call BEFORE modifying the cue list.  Saves a snapshot and clears redoStack.
    void pushUndo();
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    // Called every frame (~16ms) from MainWindow timer:
    // reclaims finished StreamReaders and refreshes playback state.
    void tick();

    // Number of logical channels defined in the current show.
    // Falls back to engine.channels() if audioSetup is empty.
    int channelCount() const {
        if (!sf.audioSetup.channels.empty())
            return static_cast<int>(sf.audioSetup.channels.size());
        return engineOk ? engine.channels() : 2;
    }

    // Human-readable name for channel index ch (e.g. "L", "R").
    // Returns "Ch N" if the setup has no name for that index.
    QString channelName(int ch) const {
        if (ch >= 0 && ch < static_cast<int>(sf.audioSetup.channels.size())) {
            const auto& n = sf.audioSetup.channels[static_cast<size_t>(ch)].name;
            if (!n.empty()) return QString::fromStdString(n);
        }
        return QString("Ch %1").arg(ch + 1);
    }

    // Apply OscServerSettings from sf and (re)start the OSC server.
    void applyOscSettings();
    // Apply MIDI input: open all ports.
    void applyMidiInput();

    // Route an incoming MIDI message to matching cue triggers + system controls.
    // Called internally; also exposed for testing.
    void routeMidi(mcp::MidiMsgType type, int channel, int data1, int data2);
    // Route an OSC message from OscServer to cue triggers + system controls.
    void routeOsc(const QString& path, const QVariantList& args);

signals:
    void cueListChanged();
    void selectionChanged(int index);
    void playbackStateChanged();    // voices started/stopped
    void dirtyChanged(bool dirty);
    void engineStatusChanged();
    // Emitted when an external trigger fires a cue (for UI highlight feedback)
    void externalTriggerFired(int cueIndex);
};
