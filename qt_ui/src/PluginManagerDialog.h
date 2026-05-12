#pragma once
#include <QDialog>

class AppModel;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QThread;

#ifdef __APPLE__
#  include "engine/plugin/AUCompatibilityTester.h"
#  include "engine/plugin/AUComponentEnumerator.h"
#  include <vector>
#endif

#ifdef MCP_HAVE_VST3
#  include "engine/plugin/VST3Scanner.h"
#endif

// Tabbed plugin manager dialog.
//
// Standalone mode (PluginManagerDialog(parent)):
//   Opened from Show menu.  No ch/slot context.  AU testing, VST3 path
//   management.  "Load into Slot" buttons are hidden.
//
// Slot-picker mode (PluginManagerDialog(model, ch, slot, parent)):
//   Opened from MixConsole.  "Load into Slot" button on the active tab.
//   On accept the slot is populated and the dialog closes.
class PluginManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginManagerDialog(QWidget* parent = nullptr);
    PluginManagerDialog(AppModel* model, int ch, int slot, QWidget* parent = nullptr);
    ~PluginManagerDialog() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    // ── shared ───────────────────────────────────────────────────────────────
    void buildUi();

    AppModel* m_model{nullptr};
    int       m_ch{-1};
    int       m_slot{-1};
    bool      m_testingInProgress{false};
    bool      m_pendingClose{false};

    QTabWidget* m_tabs{nullptr};

    // ── AU tab ────────────────────────────────────────────────────────────────
    void buildAUTab();
    void auPopulateTable();
    void auApplyFilter();
#ifdef __APPLE__
    void auUpdateRowStatus(int row, int entryIndex);
    std::vector<mcp::plugin::AUComponentEntry> m_auEntries;
    std::vector<mcp::plugin::AUTestResult>     m_auResults;
    int m_auBatchIndex{0};
#endif

    QLineEdit*    m_auFilter{nullptr};
    QPushButton*  m_auRefreshBtn{nullptr};
    QTableWidget* m_auTable{nullptr};
    QPushButton*  m_auTestSelBtn{nullptr};
    QPushButton*  m_auBatchTestBtn{nullptr};
    QPushButton*  m_auExportBtn{nullptr};
    QPushButton*  m_auLoadBtn{nullptr};
    QLabel*       m_auStatus{nullptr};
    QProgressBar* m_auProgress{nullptr};

private slots:
    void onAURefresh();
    void onAUFilterChanged(const QString& text);
    void onAUSelectionChanged();
    void onAUTestSelected();
    void onAUBatchTest();
    void onAUBatchStep();
    void onAUBatchComplete();
    void onAUExportResults();
    void onAULoadIntoSlot();

    // ── VST3 tab ──────────────────────────────────────────────────────────────
private:
    void buildVST3Tab();
    void vst3PopulateTable();
    void vst3ApplyFilter();
    void vst3SavePaths();
#ifdef MCP_HAVE_VST3
    std::vector<mcp::plugin::VST3Entry> m_vst3Entries;
#endif

    // Async scan state
    class VST3ScanWorker;          // defined in .cpp
    QThread*        m_vst3ScanThread{nullptr};
    VST3ScanWorker* m_vst3ScanWorker{nullptr};
    bool            m_vst3Scanning{false};

    QListWidget*  m_vst3PathList{nullptr};
    QPushButton*  m_vst3AddPathBtn{nullptr};
    QPushButton*  m_vst3RemovePathBtn{nullptr};
    QLineEdit*    m_vst3Filter{nullptr};
    QPushButton*  m_vst3ScanBtn{nullptr};
    QPushButton*  m_vst3CancelBtn{nullptr};
    QTableWidget* m_vst3Table{nullptr};
    QPushButton*  m_vst3LoadBtn{nullptr};
    QLabel*       m_vst3Status{nullptr};
    QProgressBar* m_vst3Progress{nullptr};

private slots:
    void onVST3AddPath();
    void onVST3RemovePath();
    void onVST3Scan();
    void onVST3ScanProgress(int cur, int total, const QString& bundleName);
    void onVST3ScanFinished();
    void onVST3FilterChanged(const QString& text);
    void onVST3SelectionChanged();
    void onVST3LoadIntoSlot();
};
