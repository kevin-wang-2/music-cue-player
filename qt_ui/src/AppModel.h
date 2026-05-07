#pragma once

#include "engine/AudioEngine.h"
#include "engine/CueList.h"
#include "engine/Scheduler.h"
#include "engine/ShowFile.h"

#include <QObject>
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

signals:
    void cueListChanged();
    void selectionChanged(int index);
    void playbackStateChanged();    // voices started/stopped
    void dirtyChanged(bool dirty);
    void engineStatusChanged();
};
