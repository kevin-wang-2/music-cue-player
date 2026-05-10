#pragma once

#include "engine/AudioEngine.h"
#include "engine/ShowFile.h"

#include <QDialog>
#include <vector>

class AppModel;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QWidget;

// Multi-section settings dialog.
//
// Left sidebar (QListWidget) switches between sections:
//   Audio    — Devices tab + Channels tab + Crosspoint tab + Output tab
//   Network  — Network Output tab + OSC Server tab
//   MIDI     — MIDI Output tab
//   Controls — MIDI Learn tab + OSC tab (system action bindings)
//
// On Accept the caller reads the result accessors to update ShowFile.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(AppModel* model, QWidget* parent = nullptr);

    mcp::ShowFile::AudioSetup      audioResult()    const { return m_audioSetup; }
    mcp::ShowFile::NetworkSetup    networkResult()  const { return m_networkSetup; }
    mcp::ShowFile::MidiSetup       midiResult()     const { return m_midiSetup; }
    mcp::OscServerSettings         oscResult()      const { return m_oscSettings; }
    mcp::SystemControlBindings     controlsResult() const { return m_systemControls; }

private slots:
    // Audio — device section
    void onAddDevice();
    void onRemoveDevice();
    // Audio — channel section
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
    void buildDevicesTab();
    void buildChannelsTab();
    void buildXpTab();
    void buildOutputTab();
    void rebuildDevicesTable();
    void rebuildMasterCombo();
    void rebuildChannelsTable();
    void rebuildXpGrid();
    void rebuildOutputTable();
    void syncDevicesFromTable();
    void syncChannelsFromTable();
    void syncXpFromGrid();
    void syncOutputFromTable();
    void updateAudioWarnings();
    int  totalPhysOutputs() const;

    // ── Network tab helpers ────────────────────────────────────────────────
    void buildNetworkPage();
    void buildNetworkOutputTab();
    void buildOscServerTab();
    void syncNetworkFromTable();
    void syncOscFromUI();
    void rebuildAccessList();

    // ── MIDI tab helpers ───────────────────────────────────────────────────
    void buildMidiPage();
    void buildMidiOutputTab();
    void syncMidiFromTable();
    void populateMidiDestCombo(QComboBox* cb, const QString& current);

    // ── Controls tab helpers ───────────────────────────────────────────────
    void buildControlsPage();
    void buildControlsMidiTab();
    void buildControlsOscTab();
    void loadControls();
    void saveControls();

    AppModel* m_model{nullptr};
    int       m_numPhys{2};
    std::vector<mcp::DeviceInfo> m_paDevices;  // cached PA device list

    mcp::ShowFile::AudioSetup      m_audioSetup;
    mcp::ShowFile::NetworkSetup    m_networkSetup;
    mcp::ShowFile::MidiSetup       m_midiSetup;
    mcp::OscServerSettings         m_oscSettings;
    mcp::SystemControlBindings     m_systemControls;

    // Sidebar + stacked pages
    QListWidget*    m_sidebar{nullptr};
    QStackedWidget* m_stack{nullptr};

    // Audio section — devices tab
    QTableWidget* m_devTable{nullptr};
    QPushButton*  m_btnAddDev{nullptr};
    QPushButton*  m_btnRemoveDev{nullptr};
    QComboBox*    m_masterCombo{nullptr};
    QLabel*       m_audioWarnLabel{nullptr};

    // Audio section — channels tab
    QTabWidget*   m_audioTabs{nullptr};
    QTableWidget* m_chanTable{nullptr};
    QPushButton*  m_btnAddChan{nullptr};
    QPushButton*  m_btnRemoveChan{nullptr};

    // Audio section — crosspoint tab
    QScrollArea*  m_xpScroll{nullptr};
    QWidget*      m_xpContent{nullptr};
    QGridLayout*  m_xpGrid{nullptr};
    std::vector<std::vector<QLineEdit*>> m_xpCells;

    // Audio section — output DSP tab
    QTableWidget* m_outTable{nullptr};

    // Network section
    QTabWidget*   m_networkTabs{nullptr};
    QTableWidget* m_patchTable{nullptr};
    QPushButton*  m_btnAddPatch{nullptr};
    QPushButton*  m_btnRemovePatch{nullptr};

    // OSC server (tab inside Network)
    QCheckBox*    m_chkOscEnabled{nullptr};
    QSpinBox*     m_spinOscPort{nullptr};
    QListWidget*  m_oscAccessList{nullptr};
    QPushButton*  m_btnOscAddOpen{nullptr};
    QPushButton*  m_btnOscAddPw{nullptr};
    QPushButton*  m_btnOscRemove{nullptr};

    // MIDI section
    QTabWidget*   m_midiTabs{nullptr};
    QTableWidget* m_midiPatchTable{nullptr};
    QPushButton*  m_btnAddMidiPatch{nullptr};
    QPushButton*  m_btnRemoveMidiPatch{nullptr};

    // Controls section
    struct ActionRow {
        mcp::ControlAction action;
        QString            label;
        QCheckBox*   midiEnable{nullptr};
        QComboBox*   midiType{nullptr};
        QSpinBox*    midiCh{nullptr};
        QSpinBox*    midiD1{nullptr};
        QSpinBox*    midiD2{nullptr};
        QPushButton* midiCapture{nullptr};
        QCheckBox*   oscEnable{nullptr};
        QLineEdit*   oscPath{nullptr};
    };
    std::vector<ActionRow> m_actionRows;
    QTabWidget*            m_controlsTabs{nullptr};
};
