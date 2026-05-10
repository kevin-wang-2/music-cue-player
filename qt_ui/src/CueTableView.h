#pragma once

#include "engine/Cue.h"

#include <QTableWidget>
#include <QTimer>
#include <set>

class AppModel;

// Cue list table.
// Columns: Status | Type | # | Name | Target | Pre-Wait | Duration | Follow
// Features:
//   - Single-click selects; Shift/Ctrl extends multiSel.
//   - Double-click edits #, Name, or Duration inline.
//   - Drag rows to reorder.
//   - Drop audio files from the OS (creates new audio cues or replaces).
//   - Context menu: Go / Stop / Panic / arm / Copy / Paste / Duplicate /
//                   Delete / Add Cue ► (Audio / Start / Stop / Fade / Arm / Devamp)
//   - Status column refreshed every ~100ms: animated playing/pending/armed/idle.
class CueTableView : public QTableWidget {
    Q_OBJECT
public:
    explicit CueTableView(AppModel* model, QWidget* parent = nullptr);

    // Call after the cue list changes (reload all rows).
    void refresh();
    // Call on each timer tick to repaint the Status column only.
    void refreshStatus();

    // Currently selected row (-1 = none).
    int selectedRow() const { return m_selRow; }

    // Called from MainWindow::onTick() to sync UI when the engine advances selection.
    void syncEngineSelection(int engineIdx);

    // Create a group wrapping the given sorted flat-index selection.
    void createGroupFromSelection(const std::vector<int>& rows);

signals:
    void rowSelected(int index);     // -1 = none
    void cueListModified();          // structural change — refresh + save

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private slots:
    void onCellChanged(int row, int col);

private:
    enum Col { ColStatus=0, ColType, ColNum, ColName, ColTarget, ColPreWait, ColDuration, ColFollow, ColCount };

    void   populateRow(int row);
    void   setRowStatus(int row);
    QString typeLabel(mcp::CueType t) const;
    QString targetLabel(int cueIdx) const;
    QString durationLabel(int cueIdx) const;
    void   insertAudioCueForPath(const QString& path, int beforeRow);
    void   replaceAudioForRow(int row, const QString& absPath);
    void   addCueOfType(const QString& type, int beforeRow, int autoTarget = -1);
    void   deleteRows(const std::vector<int>& rows);

    // Compute the nesting depth (0 = top-level) for the cue at flat index `row`.
    int cueDepth(int row) const;

    struct RowProgress {
        double preWaitFrac{-1.0};   // -1 = not pending
        double sliceFrac{-1.0};     // -1 = not playing
        bool   sliceIsLoop{false};
    };

    AppModel* m_model{nullptr};
    int       m_selRow{-1};
    bool      m_refreshing{false};  // guard against onCellChanged re-entry

    std::vector<RowProgress> m_rowProgress;

    // Flat indices of collapsed group cues.  Their descendants are hidden in the table.
    std::set<int> m_collapsed;

    // Drop indicator state: -1 = inactive.
    // m_dropTargetRow: row whose Target cell to outline (target-setting drop).
    // m_dropInsertRow: row before which to draw insertion line (row reorder).
    //                  equals rowCount() = after last row.
    // m_dropInsideGroup: true = indicator is inside m_dropGroupRow; false = normal insert.
    // m_dropReplaceRow: row to outline when a URL drag would replace its audio (not insert).
    int  m_dropTargetRow{-1};
    int  m_dropInsertRow{-1};
    bool m_dropInsideGroup{false};
    int  m_dropGroupRow{-1};
    int  m_dropReplaceRow{-1};
};
