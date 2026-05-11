#pragma once

#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include "engine/ShowFile.h"
#include "engine/SnapshotManager.h"
#include "MidiInputManager.h"
#include "OscServer.h"
#include "ScriptletEngine.h"
#include <memory>

#include <QObject>
#include <QString>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <array>

// Central application model.  Owns the audio engine stack and show data.
// All UI widgets hold a pointer to this object.
class AppModel : public QObject {
    Q_OBJECT
public:
    explicit AppModel(QObject* parent = nullptr);
    ~AppModel() override;

    // Engine stack — declared in init order (each depends on the previous).
    mcp::AudioEngine     engine;
    mcp::Scheduler       scheduler;
    mcp::SnapshotManager snapshots;
    // NOTE: CueList instances live in m_cueLists. Use cues() for the active list.

    // External trigger infrastructure
    MidiInputManager midiIn;
    OscServer        oscServer;

    // Python scriptlet engine (owns the embedded interpreter).
    std::unique_ptr<ScriptletEngine> scriptlet;

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

    // Cue indices where the last scriptlet execution failed.
    // Cleared on successful re-run or when the cue's code is edited.
    std::set<int>                         scriptletErrorCues;
    // Full error/traceback string per cue index (parallel to scriptletErrorCues).
    std::map<int, std::string>            scriptletErrors;

    // Show Information state — updated via cueFired / manualGo.
    int         m_currentCueIdx{-1};   // flat index of last fired top-level cue
    std::string m_currentMemo;         // text of last fired Memo cue; cleared on manual go

    // Music-event tracking — updated in tick().
    int    m_mcCueIdx{-1};         // flat index of active MC cue, -1 if none
    std::map<int, double> m_lastMusicBoundary;  // subdivision → last fired boundary (elapsed secs)

    // Undo / redo history — snapshots of all sf.cueLists.
    static constexpr int kMaxUndo = 100;
    std::vector<std::vector<mcp::ShowFile::CueListData>> undoStack;
    std::vector<std::vector<mcp::ShowFile::CueListData>> redoStack;

    // Call BEFORE modifying the cue list.  Saves a snapshot and clears redoStack.
    void pushUndo();
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    // Active-list access — the list the operator is currently viewing / controlling.
    mcp::CueList& cues();
    int  activeListIdx() const { return m_activeListIdx; }
    int  listCount()     const { return static_cast<int>(m_cueLists.size()); }
    void setActiveList(int idx);

    // Ensure the engine CueList vector has the same count as sf.cueLists.
    // Creates or removes engine CueLists as needed (new ones start empty).
    void syncListCount();

    // Insert an empty engine CueList at position atIdx (mirrors sf.cueLists insert).
    void insertEngineList(int atIdx);
    // Panic and remove the engine CueList at position atIdx (mirrors sf.cueLists erase).
    void removeEngineList(int atIdx);

    // Direct access to a specific engine CueList (for ShowHelpers::rebuildCueList).
    mcp::CueList& cueListAt(int idx) { return *m_cueLists[static_cast<size_t>(idx)]; }

    // Stop all cues in every list and emit playbackStateChanged.
    void panicAll();

    // Convenience: sf.cueLists[activeListIdx()]. Callers must ensure list is non-empty.
    mcp::ShowFile::CueListData& sfActiveList() { return sf.cueLists[static_cast<size_t>(m_activeListIdx)]; }

    // Called every frame (~16ms) from MainWindow timer:
    // reclaims finished StreamReaders and refreshes playback state.
    void tick();

    // Number of logical channels defined in the current show.
    int channelCount() const {
        return static_cast<int>(sf.audioSetup.channels.size());
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

    // Perform a manual Go (fires manualGo signal before advancing the cue list).
    // Use this instead of cues.go() for operator-initiated go actions.
    void go();

    // Snapshot helpers — delegate to snapshots.*. recall* also call applyMixing().
    void storeSnapshot();
    void storeSnapshotAll();
    void recallSnapshot();
    void recallSnapshotById(int id);

    // Apply OscServerSettings from sf and (re)start the OSC server.
    void applyOscSettings();
    // Push channel + physout DSP settings from sf to the engine.
    void applyOutputDsp();
    // Update channel map + output DSP without rebuilding or stopping cues.
    // Use this for all mixing-only parameter changes in MixConsole.
    void applyMixing();
    // Apply MIDI input: open all ports.
    void applyMidiInput();
    // Sync scriptlet library from sf into the ScriptletEngine.
    // Merges built-in entries (from scriptlets/ dir) with user entries in sf.
    void applyScriptletLibrary();

    // Returns true if cue[row] in the active list is currently executing as a scriptlet.
    // Use alongside cues().isCuePlaying() to show running state for scriptlet cues.
    bool isScriptletCuePlaying(int row) const { return m_scriptletRunningCues.count(row) > 0; }

    // Scan dir for *.py files and load them as built-in library modules.
    // Built-in modules are always available but never saved to the show file.
    // User entries in sf with the same name take precedence.
    void loadBuiltinScriptlets(const QString& dir);

    // Access the currently loaded built-in modules (read-only).
    const std::vector<mcp::ShowFile::ScriptletLibrary::Entry>& builtinScriptlets() const
        { return m_builtinScriptlets; }

    // Route an incoming MIDI message to matching cue triggers + system controls.
    // Called internally; also exposed for testing.
    void routeMidi(mcp::MidiMsgType type, int channel, int data1, int data2);
    // Route an OSC message from OscServer to cue triggers + system controls.
    void routeOsc(const QString& path, const QVariantList& args);

signals:
    void cueListChanged();
    // Emitted when operator manually presses Go (not auto-continue / auto-follow).
    void manualGo();
    // Emitted when Show Information state (currentCue, nextCue, memo) changes.
    void showInfoChanged();
    // Emitted before routing so monitors (e.g. ProjectStatusDialog) can log.
    void midiInputReceived(mcp::MidiMsgType type, int ch, int d1, int d2);
    void oscInputReceived(const QString& path, const QVariantList& args);
    // Scriptlet stdout/stderr output
    void scriptletOutput(const QString& text);
    void selectionChanged(int index);
    void playbackStateChanged();    // voices started/stopped
    void mixStateChanged();         // sf.audioSetup changed (snapshot recall, automation, etc.)
    void dirtyChanged(bool dirty);
    void engineStatusChanged();
    // Emitted when an external trigger fires a cue (for UI highlight feedback)
    void externalTriggerFired(int cueIndex);
    // Emitted when any cue is triggered (regardless of pre-wait).
    // Used by the scriptlet event system; also fired for internally-triggered cues.
    void cueFired(int cueIndex);
    // Emitted when the operator switches the active cue list.
    void activeListChanged(int listIdx);
    // Emitted when lists are added, removed, or renamed (sidebar refresh).
    void cueListsChanged();

private:
    std::vector<std::unique_ptr<mcp::CueList>> m_cueLists;
    int  m_activeListIdx{0};
    std::set<int> m_scriptletRunningCues;   // cue indices currently executing (reentrance guard)
    std::vector<mcp::ShowFile::ScriptletLibrary::Entry> m_builtinScriptlets;
};
