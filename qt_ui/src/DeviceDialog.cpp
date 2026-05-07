#include "DeviceDialog.h"
#include "AppModel.h"

#include "engine/AudioEngine.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QVBoxLayout>

DeviceDialog::DeviceDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Audio Device Settings");
    setMinimumWidth(420);

    auto* lay = new QVBoxLayout(this);

    // Device list
    m_list = new QListWidget(this);
    const auto devs = mcp::AudioEngine::listOutputDevices();
    // First entry = system default
    m_list->addItem("(system default)");
    m_devices.push_back({-1, "", 0});

    for (const auto& d : devs) {
        const QString label = QString("[%1] %2 (%3 ch)")
            .arg(d.index).arg(QString::fromStdString(d.name)).arg(d.maxOutputChannels);
        m_list->addItem(label);
        m_devices.push_back({d.index, d.name, d.maxOutputChannels});
    }

    // Pre-select current device if any
    m_list->setCurrentRow(0);
    lay->addWidget(new QLabel("Output device:", this));
    lay->addWidget(m_list, 1);

    // Sample rate + channels
    auto* form = new QFormLayout;
    m_spinRate = new QSpinBox(this);
    m_spinRate->setRange(22050, 192000);
    m_spinRate->setValue(m_model->engineOk ? m_model->engine.sampleRate() : 48000);
    m_spinRate->setSuffix(" Hz");

    m_spinCh = new QSpinBox(this);
    m_spinCh->setRange(0, 64);
    m_spinCh->setValue(m_model->engineOk ? m_model->engine.channels() : 0);
    m_spinCh->setSpecialValueText("auto");

    form->addRow("Sample rate:", m_spinRate);
    form->addRow("Channels:",    m_spinCh);
    lay->addLayout(form);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    lay->addWidget(m_statusLabel);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &DeviceDialog::apply);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

void DeviceDialog::apply() {
    const int row = m_list->currentRow();
    if (row < 0 || row >= (int)m_devices.size()) { accept(); return; }

    const std::string devName = m_devices[row].name;
    const int sr  = m_spinRate->value();
    const int ch  = m_spinCh->value();

    // Stop existing engine
    if (m_model->engineOk) m_model->engine.shutdown();

    std::string err;
    m_model->engineOk = m_model->engine.initialize(sr, ch, devName);
    if (!m_model->engineOk) {
        m_statusLabel->setText("Failed to open device. Try another selection.");
        return;
    }
    emit m_model->engineStatusChanged();
    accept();
}
