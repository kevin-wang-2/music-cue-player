#pragma once

#include "engine/Cue.h"

#include <QTableWidget>
#include <QTimer>
#include <set>

class AppModel;

// Cue list table.
// Columns: # | Type | Name | Target | Duration | Status
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
    enum Col { ColNum=0, ColType, ColName, ColTarget, ColDuration, ColStatus, ColCount };

    void   populateRow(int row);
    void   setRowStatus(int row);
    QString typeLabel(mcp::CueType t) const;
    QString targetLabel(int cueIdx) const;
    QString durationLabel(int cueIdx) const;
    void   insertAudioCueForPath(const QString& path, int beforeRow);
    void   addCueOfType(const QString& type, int beforeRow, int autoTarget = -1);
    void   deleteRows(const std::vector<int>& rows);

    AppModel* m_model{nullptr};
    int       m_selRow{-1};
    bool      m_refreshing{false};  // guard against onCellChanged re-entry

    // Drop indicator state: -1 = inactive.
    // m_dropTargetRow: row whose Target cell to outline (target-setting drop).
    // m_dropInsertRow: row before which to draw insertion line (row reorder).
    //                  equals rowCount() = after last row.
    int m_dropTargetRow{-1};
    int m_dropInsertRow{-1};
};
