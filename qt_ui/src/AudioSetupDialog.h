#pragma once

#include "engine/AudioEngine.h"
#include "engine/ShowFile.h"

#include <QDialog>
#include <vector>

class AppModel;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QWidget;

// Three-tab modal dialog for audio channel and device setup.
//
// Tab 1 — Devices (multi-device mode only; shows hint when empty)
//   Add/remove PortAudio output devices.
//   Each row: device name, output channel count, buffer size.
//   A "Master clock device" combo selects which device drives enginePlayhead.
//
// Tab 2 — Channels
//   Lists logical channels: name, device (multi-device mode), stereo-link, gain, mute.
//   Add/Remove buttons change the channel count.
//
// Tab 3 — Crosspoint
//   Grid of [channel × physOut] cells showing dB routing gains.
//   In multi-device mode headers show Dv#.Out# labels.
//
// A warning bar at the bottom flags drift risk and LTC-on-non-master issues.
//
// On Accept the caller reads result() to update ShowFile::audioSetup.
class AudioSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit AudioSetupDialog(AppModel* model, QWidget* parent = nullptr);

    // Returns the edited AudioSetup (valid after QDialog::accept()).
    mcp::ShowFile::AudioSetup result() const { return m_setup; }

private slots:
    void onAddDevice();
    void onRemoveDevice();
    void onAddChannel();
    void onRemoveChannel();
    void onChannelCountChanged(int n);

private:
    // Build each tab once during construction
    void buildDevicesTab();
    void buildChannelsTab();
    void buildXpTab();

    // Refresh widgets from m_setup
    void rebuildDevicesTable();
    void rebuildMasterCombo();
    void rebuildChannelsTable();
    void rebuildXpGrid();

    // Commit widget state back to m_setup
    void syncDevicesFromTable();
    void syncChannelsFromTable();
    void syncXpFromGrid();

    // Update the warning label from the current m_setup state
    void updateWarnings();

    // Total physical outputs across all devices (or m_numPhys in legacy mode)
    int totalPhysOutputs() const;

    AppModel* m_model{nullptr};
    int       m_numPhys{2};  // physical output count from engine (legacy mode)
    mcp::ShowFile::AudioSetup m_setup;  // working copy
    std::vector<mcp::DeviceInfo> m_paDevices;  // cached PA device list

    // Tab container
    QTabWidget* m_tabs{nullptr};

    // Tab 1 — Devices widgets
    QTableWidget* m_devTable{nullptr};
    QPushButton*  m_btnAddDev{nullptr};
    QPushButton*  m_btnRemoveDev{nullptr};
    QComboBox*    m_masterCombo{nullptr};

    // Tab 2 — Channels widgets
    QTableWidget* m_chanTable{nullptr};
    QPushButton*  m_btnAdd{nullptr};
    QPushButton*  m_btnRemove{nullptr};

    // Tab 3 — Crosspoint widgets
    QScrollArea*  m_xpScroll{nullptr};
    QWidget*      m_xpContent{nullptr};
    QGridLayout*  m_xpGrid{nullptr};
    std::vector<std::vector<QLineEdit*>> m_xpCells;  // [ch][physOut]

    // Warning bar (bottom of dialog)
    QLabel* m_warnLabel{nullptr};
};
