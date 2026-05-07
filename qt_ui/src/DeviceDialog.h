#pragma once

#include <QDialog>
#include <vector>
#include <string>

class AppModel;

class QListWidget;
class QLabel;
class QSpinBox;

// Modal dialog for audio device selection.
// Shows all available PortAudio output devices.
// On Accept, reinitialises the engine with the selected device.
class DeviceDialog : public QDialog {
    Q_OBJECT
public:
    explicit DeviceDialog(AppModel* model, QWidget* parent = nullptr);

private slots:
    void apply();

private:
    AppModel*    m_model{nullptr};
    QListWidget* m_list{nullptr};
    QSpinBox*    m_spinRate{nullptr};
    QSpinBox*    m_spinCh{nullptr};
    QLabel*      m_statusLabel{nullptr};

    struct Entry {
        int         index;
        std::string name;
        int         maxCh;
    };
    std::vector<Entry> m_devices;
};
