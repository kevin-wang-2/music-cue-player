#pragma once

#include "engine/ShowFile.h"

#include <QDialog>
#include <vector>

class AppModel;
class QCheckBox;
class QDoubleSpinBox;
class QGridLayout;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QWidget;

// Two-tab modal dialog for audio channel setup.
//
// Tab 1 — Channels
//   Lists logical channels: name, stereo-link flag (UI only), master gain, mute.
//   Add/Remove buttons change the channel count.
//
// Tab 2 — Crosspoint
//   Grid of [channel × physOut] cells showing dB routing gains.
//   Default diagonal is 0 dB (blank = use default); type a value to override.
//   Blank or "-inf" = off.
//
// On Accept the caller reads result() to update ShowFile::audioSetup.
class AudioSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit AudioSetupDialog(AppModel* model, QWidget* parent = nullptr);

    // Returns the edited AudioSetup (valid after QDialog::accept()).
    mcp::ShowFile::AudioSetup result() const { return m_setup; }

private slots:
    void onAddChannel();
    void onRemoveChannel();
    void onChannelCountChanged(int n);

private:
    void buildChannelsTab();
    void buildXpTab();
    void rebuildChannelsTable();  // refresh table from m_setup
    void rebuildXpGrid();         // rebuild xp grid from m_setup

    // Commit channel-table edits back to m_setup.channels
    void syncChannelsFromTable();
    // Commit xp-grid edits back to m_setup.xpEntries
    void syncXpFromGrid();

    AppModel* m_model{nullptr};
    int       m_numPhys{2};  // physical output count from engine
    mcp::ShowFile::AudioSetup m_setup;  // working copy

    // Tab 1 widgets
    QTabWidget*   m_tabs{nullptr};
    QTableWidget* m_chanTable{nullptr};
    QPushButton*  m_btnAdd{nullptr};
    QPushButton*  m_btnRemove{nullptr};

    // Tab 2 widgets
    QScrollArea*  m_xpScroll{nullptr};
    QWidget*      m_xpContent{nullptr};
    QGridLayout*  m_xpGrid{nullptr};
    std::vector<std::vector<QLineEdit*>> m_xpCells;  // [ch][physOut]
};
