#pragma once

#include "engine/ShowFile.h"

#include <QDialog>
#include <vector>

class AppModel;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QWidget;

// Multi-section settings dialog replacing AudioSetupDialog.
//
// Left sidebar (QListWidget) switches between sections:
//   Audio   — Channels tab + Crosspoint tab (same as former AudioSetupDialog)
//   Network — Network Output tab (list of OSC/text patches)
//
// On Accept the caller reads audioResult() / networkResult() to update ShowFile.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(AppModel* model, QWidget* parent = nullptr);

    mcp::ShowFile::AudioSetup   audioResult()   const { return m_audioSetup; }
    mcp::ShowFile::NetworkSetup networkResult() const { return m_networkSetup; }
    mcp::ShowFile::MidiSetup    midiResult()    const { return m_midiSetup; }

private slots:
    // Audio section
    void onAddChannel();
    void onRemoveChannel();
    // Network section
    void onAddPatch();
    void onRemovePatch();
    // MIDI section
    void onAddMidiPatch();
    void onRemoveMidiPatch();

private:
    // ── Audio tab helpers ──────────────────────────────────────────────────
    void buildAudioPage();
    void buildChannelsTab();
    void buildXpTab();
    void rebuildChannelsTable();
    void rebuildXpGrid();
    void syncChannelsFromTable();
    void syncXpFromGrid();

    // ── Network tab helpers ────────────────────────────────────────────────
    void buildNetworkPage();
    void buildNetworkOutputTab();
    void syncNetworkFromTable();

    // ── MIDI tab helpers ───────────────────────────────────────────────────
    void buildMidiPage();
    void buildMidiOutputTab();
    void syncMidiFromTable();
    void populateMidiDestCombo(QComboBox* cb, const QString& current);

    AppModel* m_model{nullptr};
    int       m_numPhys{2};

    mcp::ShowFile::AudioSetup   m_audioSetup;
    mcp::ShowFile::NetworkSetup m_networkSetup;
    mcp::ShowFile::MidiSetup    m_midiSetup;

    // Sidebar + stacked pages
    QListWidget*    m_sidebar{nullptr};
    QStackedWidget* m_stack{nullptr};

    // Audio section
    QTabWidget*   m_audioTabs{nullptr};
    QTableWidget* m_chanTable{nullptr};
    QPushButton*  m_btnAddChan{nullptr};
    QPushButton*  m_btnRemoveChan{nullptr};
    QScrollArea*  m_xpScroll{nullptr};
    QWidget*      m_xpContent{nullptr};
    QGridLayout*  m_xpGrid{nullptr};
    std::vector<std::vector<QLineEdit*>> m_xpCells;

    // Network section
    QTabWidget*   m_networkTabs{nullptr};
    QTableWidget* m_patchTable{nullptr};
    QPushButton*  m_btnAddPatch{nullptr};
    QPushButton*  m_btnRemovePatch{nullptr};

    // MIDI section
    QTabWidget*   m_midiTabs{nullptr};
    QTableWidget* m_midiPatchTable{nullptr};
    QPushButton*  m_btnAddMidiPatch{nullptr};
    QPushButton*  m_btnRemoveMidiPatch{nullptr};
};
