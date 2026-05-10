#pragma once
#include "ShowHelpers.h"
#include <QDialog>
#include <vector>

class QTableWidget;
class QPushButton;
class QLabel;
class AppModel;

// Shown automatically after loading a show that has missing media files,
// and accessible manually from the Show menu.
//
// Each row represents one cue whose media file cannot be found.
// Two resolution strategies:
//   • Search in Folder — scans a directory tree for files whose name matches
//     the missing filename (exact then stem-only, case-insensitive).
//   • Locate Selected  — lets the user pick any file for the selected row
//                        (manual override; name need not match).
//
// "Apply" writes the resolved paths back into the show and rebuilds the engine.
// "Skip" closes without modifying anything.
class MissingMediaDialog : public QDialog {
    Q_OBJECT
public:
    explicit MissingMediaDialog(AppModel* model, QWidget* parent = nullptr);

    // Re-scan the model for missing media and repopulate the table.
    void refresh();

signals:
    // Emitted after Apply so the main window can refresh its cue table.
    void mediaFixed();

private slots:
    void onSearchFolder();
    void onLocateSelected();
    void onApply();

private:
    void buildUi();
    void populateTable();
    void updateRow(int row);
    void updateButtons();

    AppModel*    m_model;
    QTableWidget* m_table;
    QLabel*      m_summary;
    QPushButton* m_locateBtn;
    QPushButton* m_applyBtn;

    std::vector<MissingEntry> m_entries;
};
