#pragma once
#include <QDialog>

class AppModel;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

#ifdef __APPLE__
#  include "engine/plugin/AUCompatibilityTester.h"
#  include "engine/plugin/AUComponentEnumerator.h"
#  include <vector>
#endif

// Dialog for browsing, testing, and loading AU effect plugins.
//
// Standalone mode  (AUScanDialog(parent)):
//   Opened from the Show menu.  No ch/slot context.  Used for compatibility
//   testing and JSON export only.  "Load" button is hidden.
//
// Slot-picker mode (AUScanDialog(model, ch, slot, parent)):
//   Opened from the MixConsole plugin slot picker.  "Load into Slot" button
//   is visible; on accept the slot is populated and the dialog closes.
class AUScanDialog : public QDialog {
    Q_OBJECT
public:
    explicit AUScanDialog(QWidget* parent = nullptr);
    AUScanDialog(AppModel* model, int ch, int slot, QWidget* parent = nullptr);
    ~AUScanDialog() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onRefresh();
    void onFilterChanged(const QString& text);
    void onSelectionChanged();
    void onTestSelected();
    void onBatchTest();
    void onBatchStep();    // one test per event-loop tick, runs on main thread
    void onBatchComplete();
    void onExportResults();
    void onLoadIntoSlot();

private:
    void buildUi();
    void populateTable();
    void applyFilter();

#ifdef __APPLE__
    void updateRowStatus(int row, int entryIndex);

    std::vector<mcp::plugin::AUComponentEntry> m_entries;
    std::vector<mcp::plugin::AUTestResult>     m_results;  // parallel, empty = not tested
#endif

    AppModel*    m_model{nullptr};
    int          m_ch{-1};
    int          m_slot{-1};
    bool         m_testingInProgress{false};
    int          m_batchIndex{0};

    QLineEdit*    m_filterEdit{nullptr};
    QPushButton*  m_refreshBtn{nullptr};
    QTableWidget* m_table{nullptr};
    QPushButton*  m_testSelBtn{nullptr};
    QPushButton*  m_batchTestBtn{nullptr};
    QPushButton*  m_exportBtn{nullptr};
    QPushButton*  m_loadBtn{nullptr};
    QLabel*       m_statusLabel{nullptr};
    QProgressBar* m_progressBar{nullptr};
};
