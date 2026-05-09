#include "AudioSetupDialog.h"
#include "AppModel.h"
#include "FaderWidget.h"

#include <cmath>
#include <functional>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

static constexpr float kInfDb = -144.0f;

static const char* kDarkStyle =
    "QDialog{background:#1a1a1a;color:#ddd;}"
    "QTabWidget::pane{background:#1a1a1a;border:1px solid #333;}"
    "QTabBar::tab{background:#222;color:#aaa;padding:5px 12px;border:1px solid #333;"
    "  border-bottom:none;border-top-left-radius:4px;border-top-right-radius:4px;}"
    "QTabBar::tab:selected{background:#1a1a1a;color:#ddd;}"
    "QTableWidget{background:#1e1e1e;color:#ddd;gridline-color:#333;"
    "  selection-background-color:#2a4a70;border:1px solid #333;}"
    "QTableWidget QHeaderView::section{background:#252525;color:#aaa;"
    "  border:1px solid #333;padding:3px 6px;}"
    "QCheckBox{color:#ddd;spacing:5px;}"
    "QCheckBox::indicator{width:14px;height:14px;border:1px solid #555;"
    "  border-radius:2px;background:#222;}"
    "QCheckBox::indicator:checked{background:#2a6ab8;border-color:#2a6ab8;}"
    "QComboBox{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QComboBox:focus{border-color:#2a6ab8;}"
    "QComboBox QAbstractItemView{background:#252525;color:#ddd;selection-background-color:#2a4a70;}"
    "QDoubleSpinBox{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QSpinBox{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QLineEdit{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QLineEdit:focus{border-color:#2a6ab8;}"
    "QLabel{color:#aaa;}"
    "QPushButton{background:#2a2a2a;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:4px 12px;}"
    "QPushButton:hover{background:#383838;}"
    "QPushButton:default{background:#1a4a88;border-color:#2a6ab8;}"
    "QScrollArea{background:#1a1a1a;border:none;}"
    "QGroupBox{color:#888;border:1px solid #2a2a2a;margin-top:8px;"
    "  padding-top:6px;border-radius:4px;}"
    "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}";

static QString fmtDb(float dB) {
    if (dB <= FaderWidget::kFaderMin + 0.05f) return QStringLiteral("-inf");
    return QString::number(static_cast<double>(dB), 'f', 1);
}

static float parseDb(const QString& txt, float defaultVal) {
    const QString t = txt.trimmed();
    if (t.isEmpty() || t.compare("-inf", Qt::CaseInsensitive) == 0)
        return kInfDb;
    bool ok = false;
    float v = t.toFloat(&ok);
    return ok ? std::max(kInfDb, std::min(FaderWidget::kFaderMax, v)) : defaultVal;
}

// ── ctor ───────────────────────────────────────────────────────────────────

AudioSetupDialog::AudioSetupDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Audio Setup");
    setMinimumSize(540, 420);
    setStyleSheet(kDarkStyle);

    m_numPhys = model->engineOk ? model->engine.channels() : 2;
    m_setup   = model->sf.audioSetup;
    m_paDevices = mcp::AudioEngine::listOutputDevices();

    // Ensure at least one channel
    if (m_setup.channels.empty()) {
        for (int i = 0; i < m_numPhys; ++i) {
            mcp::ShowFile::AudioSetup::Channel c;
            c.name = "Ch " + std::to_string(i + 1);
            m_setup.channels.push_back(c);
        }
    }

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(12, 12, 12, 12);
    vlay->setSpacing(8);

    m_tabs = new QTabWidget(this);
    vlay->addWidget(m_tabs, 1);

    buildDevicesTab();
    buildChannelsTab();
    buildXpTab();

    // Warning bar — shown only when warnings exist
    m_warnLabel = new QLabel(this);
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setStyleSheet(
        "color:#e8a020;background:#2a2010;border:1px solid #5a4010;"
        "border-radius:3px;padding:4px 8px;font-size:11px;");
    m_warnLabel->setVisible(false);
    vlay->addWidget(m_warnLabel);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText("Apply");
    btns->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        syncDevicesFromTable();
        syncChannelsFromTable();
        syncXpFromGrid();
        m_setup.normalizeMaster();
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    vlay->addWidget(btns);

    updateWarnings();
}

// ── Helpers ────────────────────────────────────────────────────────────────

int AudioSetupDialog::totalPhysOutputs() const {
    if (m_setup.devices.empty())
        return m_numPhys;
    int total = 0;
    for (const auto& d : m_setup.devices)
        total += d.channelCount;
    return total;
}

// ── Tab 1 — Devices ────────────────────────────────────────────────────────

void AudioSetupDialog::buildDevicesTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    // Master clock selector row
    auto* masterRow = new QHBoxLayout;
    auto* masterLbl = new QLabel("Master clock device:");
    masterLbl->setStyleSheet("color:#aaa;");
    masterRow->addWidget(masterLbl);
    m_masterCombo = new QComboBox;
    m_masterCombo->setMinimumWidth(120);
    connect(m_masterCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &AudioSetupDialog::updateWarnings);
    masterRow->addWidget(m_masterCombo);
    masterRow->addStretch();
    vlay->addLayout(masterRow);

    // Device table
    m_devTable = new QTableWidget(0, 3, page);
    m_devTable->setHorizontalHeaderLabels({"Device Name", "Channels", "Buffer Size"});
    m_devTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_devTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_devTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_devTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_devTable->verticalHeader()->setDefaultSectionSize(28);
    m_devTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_devTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vlay->addWidget(m_devTable, 1);

    // Add / Remove buttons
    auto* btnRow = new QHBoxLayout;
    m_btnAddDev    = new QPushButton("+ Add Device");
    m_btnRemoveDev = new QPushButton("− Remove Selected");
    btnRow->addWidget(m_btnAddDev);
    btnRow->addWidget(m_btnRemoveDev);
    btnRow->addStretch();
    vlay->addLayout(btnRow);

    auto* hint = new QLabel(
        "Add devices to enable multi-device audio output. "
        "All devices share the same sample rate. "
        "The master clock device drives the internal timing reference.");
    hint->setStyleSheet("color:#666;font-size:10px;");
    hint->setWordWrap(true);
    vlay->addWidget(hint);

    rebuildDevicesTable();

    connect(m_btnAddDev,    &QPushButton::clicked, this, &AudioSetupDialog::onAddDevice);
    connect(m_btnRemoveDev, &QPushButton::clicked, this, &AudioSetupDialog::onRemoveDevice);

    m_tabs->addTab(page, "Devices");
}

void AudioSetupDialog::rebuildDevicesTable() {
    m_devTable->setRowCount(0);
    for (int i = 0; i < (int)m_setup.devices.size(); ++i) {
        m_devTable->insertRow(i);
        const auto& dev = m_setup.devices[static_cast<size_t>(i)];

        // Column 0: device name combo
        auto* nameCb = new QComboBox;
        nameCb->addItem("(system default)", QString(""));
        for (const auto& pa : m_paDevices)
            nameCb->addItem(
                QString::fromStdString(pa.name) +
                QString(" (%1 ch)").arg(pa.maxOutputChannels),
                QString::fromStdString(pa.name));
        const int nameIdx = nameCb->findData(QString::fromStdString(dev.name));
        if (nameIdx >= 0) nameCb->setCurrentIndex(nameIdx);
        connect(nameCb, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &AudioSetupDialog::updateWarnings);
        m_devTable->setCellWidget(i, 0, nameCb);

        // Column 1: channel count
        auto* chSpin = new QSpinBox;
        chSpin->setRange(1, 64);
        chSpin->setValue(dev.channelCount);
        connect(chSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
            syncDevicesFromTable();
            rebuildMasterCombo();
            rebuildXpGrid();
            updateWarnings();
        });
        m_devTable->setCellWidget(i, 1, chSpin);

        // Column 2: buffer size combo
        auto* bufCb = new QComboBox;
        for (int bs : {128, 256, 512, 1024, 2048})
            bufCb->addItem(QString::number(bs), bs);
        const int bufIdx = bufCb->findData(dev.bufferSize);
        bufCb->setCurrentIndex(bufIdx >= 0 ? bufIdx : 2);  // default 512
        m_devTable->setCellWidget(i, 2, bufCb);
    }
    rebuildMasterCombo();
}

void AudioSetupDialog::rebuildMasterCombo() {
    if (!m_masterCombo) return;
    const int prevSel = m_masterCombo->currentIndex();
    m_masterCombo->blockSignals(true);
    m_masterCombo->clear();
    for (int i = 0; i < (int)m_setup.devices.size(); ++i)
        m_masterCombo->addItem(QString("Device %1").arg(i + 1));
    // Pre-select the device that has masterClock=true
    int masterIdx = 0;
    for (int i = 0; i < (int)m_setup.devices.size(); ++i)
        if (m_setup.devices[static_cast<size_t>(i)].masterClock) { masterIdx = i; break; }
    const int toSelect = (masterIdx < m_masterCombo->count()) ? masterIdx
                       : (prevSel < m_masterCombo->count())  ? prevSel : 0;
    m_masterCombo->setCurrentIndex(toSelect);
    m_masterCombo->blockSignals(false);
    m_masterCombo->setEnabled(m_masterCombo->count() > 0);
}

void AudioSetupDialog::syncDevicesFromTable() {
    const int masterIdx = m_masterCombo ? m_masterCombo->currentIndex() : 0;
    const int rows = m_devTable->rowCount();
    m_setup.devices.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& dev = m_setup.devices[static_cast<size_t>(i)];
        if (auto* cb = qobject_cast<QComboBox*>(m_devTable->cellWidget(i, 0)))
            dev.name = cb->currentData().toString().toStdString();
        if (auto* sp = qobject_cast<QSpinBox*>(m_devTable->cellWidget(i, 1)))
            dev.channelCount = sp->value();
        if (auto* cb = qobject_cast<QComboBox*>(m_devTable->cellWidget(i, 2)))
            dev.bufferSize = cb->currentData().toInt();
        dev.masterClock = (i == masterIdx);
    }
}

void AudioSetupDialog::onAddDevice() {
    syncDevicesFromTable();
    mcp::ShowFile::AudioSetup::Device d;
    d.name         = "";    // system default
    d.channelCount = 2;
    d.bufferSize   = 512;
    d.masterClock  = m_setup.devices.empty();  // first device is master by default
    m_setup.devices.push_back(d);
    rebuildDevicesTable();
    rebuildChannelsTable();
    rebuildXpGrid();
    updateWarnings();
}

void AudioSetupDialog::onRemoveDevice() {
    const int row = m_devTable->currentRow();
    if (row < 0 || row >= (int)m_setup.devices.size()) return;
    syncDevicesFromTable();
    // Remove channels that reference this device; remap higher indices
    const int removedDev = row;
    auto& chans = m_setup.channels;
    chans.erase(
        std::remove_if(chans.begin(), chans.end(),
            [removedDev](const mcp::ShowFile::AudioSetup::Channel& c) {
                return c.deviceIndex == removedDev;
            }),
        chans.end());
    for (auto& c : chans)
        if (c.deviceIndex > removedDev) --c.deviceIndex;
    m_setup.devices.erase(m_setup.devices.begin() + removedDev);
    m_setup.normalizeMaster();
    rebuildDevicesTable();
    rebuildChannelsTable();
    rebuildXpGrid();
    updateWarnings();
}

// ── Tab 2 — Channels ───────────────────────────────────────────────────────

void AudioSetupDialog::buildChannelsTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    // Toolbar: Add / Remove
    auto* toolbar = new QHBoxLayout;
    m_btnAdd    = new QPushButton("+  Add Channel");
    m_btnRemove = new QPushButton("−  Remove Last");
    toolbar->addWidget(m_btnAdd);
    toolbar->addWidget(m_btnRemove);
    toolbar->addStretch();
    vlay->addLayout(toolbar);

    // Table columns: Name | [Device] | Stereo Link | Master (dB) | Mute
    // Device column is inserted only in multi-device mode (see rebuildChannelsTable)
    m_chanTable = new QTableWidget(0, 5, page);
    m_chanTable->setHorizontalHeaderLabels({"Name", "Device", "Stereo Link", "Master (dB)", "Mute"});
    m_chanTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_chanTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_chanTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_chanTable->verticalHeader()->setDefaultSectionSize(26);
    m_chanTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_chanTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    vlay->addWidget(m_chanTable, 1);

    rebuildChannelsTable();

    connect(m_btnAdd,    &QPushButton::clicked, this, &AudioSetupDialog::onAddChannel);
    connect(m_btnRemove, &QPushButton::clicked, this, &AudioSetupDialog::onRemoveChannel);

    m_tabs->addTab(page, "Channels");
}

void AudioSetupDialog::rebuildChannelsTable() {
    if (!m_chanTable) return;
    m_chanTable->setRowCount(0);
    const int n = static_cast<int>(m_setup.channels.size());
    const bool multiDev = !m_setup.devices.empty();

    // Show/hide Device column
    m_chanTable->setColumnHidden(1, !multiDev);

    for (int i = 0; i < n; ++i)
        m_chanTable->insertRow(i);

    for (int i = 0; i < n; ++i) {
        const auto& ch = m_setup.channels[static_cast<size_t>(i)];
        const bool isSlave  = (i > 0 && m_setup.channels[static_cast<size_t>(i - 1)].linkedStereo);
        const bool isMaster = ch.linkedStereo && (i + 1 < n);

        m_chanTable->setVerticalHeaderItem(
            i, new QTableWidgetItem(isSlave ? QString("   └ %1").arg(i + 1)
                                            : QString::number(i + 1)));

        // Col 0: Name
        auto* nameEdit = new QLineEdit(QString::fromStdString(ch.name));
        nameEdit->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
        m_chanTable->setCellWidget(i, 0, nameEdit);

        // Col 1: Device (multi-device only)
        if (multiDev) {
            auto* devCb = new QComboBox;
            for (int d = 0; d < (int)m_setup.devices.size(); ++d)
                devCb->addItem(QString("Device %1").arg(d + 1));
            if (ch.deviceIndex >= 0 && ch.deviceIndex < (int)m_setup.devices.size())
                devCb->setCurrentIndex(ch.deviceIndex);
            m_chanTable->setCellWidget(i, 1, devCb);
        }

        // Col 2: Stereo Link (skip for slave rows)
        if (!isSlave) {
            auto* stereoChk = new QCheckBox;
            stereoChk->setChecked(ch.linkedStereo);
            stereoChk->setToolTip("Link this and the next channel as a stereo pair\n"
                                  "They share one master fader and one mute.");
            auto* stereoCell = new QWidget;
            auto* stLay = new QHBoxLayout(stereoCell);
            stLay->setAlignment(Qt::AlignCenter);
            stLay->setContentsMargins(0, 0, 0, 0);
            stLay->addWidget(stereoChk);
            m_chanTable->setCellWidget(i, 2, stereoCell);
            connect(stereoChk, &QCheckBox::toggled, this, [this](bool) {
                syncChannelsFromTable();
                rebuildChannelsTable();
                rebuildXpGrid();
            });
        }

        // Cols 3 & 4: Master Gain and Mute (non-slave only)
        if (!isSlave) {
            auto* gainSpin = new QDoubleSpinBox;
            gainSpin->setRange(-60.0, 12.0);
            gainSpin->setDecimals(1);
            gainSpin->setSuffix(" dB");
            gainSpin->setSingleStep(0.5);
            gainSpin->setValue(static_cast<double>(ch.masterGainDb));
            gainSpin->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 2px;");
            m_chanTable->setCellWidget(i, 3, gainSpin);

            auto* muteChk = new QCheckBox;
            muteChk->setChecked(ch.mute);
            muteChk->setStyleSheet("QCheckBox::indicator:checked{background:#c04040;border-color:#c04040;}");
            auto* muteCell = new QWidget;
            auto* muteLay = new QHBoxLayout(muteCell);
            muteLay->setAlignment(Qt::AlignCenter);
            muteLay->setContentsMargins(0, 0, 0, 0);
            muteLay->addWidget(muteChk);
            m_chanTable->setCellWidget(i, 4, muteCell);

            if (isMaster) {
                m_chanTable->setSpan(i, 3, 2, 1);
                m_chanTable->setSpan(i, 4, 2, 1);
            }
        }
    }
}

void AudioSetupDialog::onAddChannel() {
    syncChannelsFromTable();
    mcp::ShowFile::AudioSetup::Channel c;
    c.name = "Ch " + std::to_string(m_setup.channels.size() + 1);
    m_setup.channels.push_back(c);
    rebuildChannelsTable();
    rebuildXpGrid();
}

void AudioSetupDialog::onRemoveChannel() {
    if (m_setup.channels.size() <= 1) return;
    syncChannelsFromTable();
    const int lastCh = static_cast<int>(m_setup.channels.size()) - 1;
    m_setup.xpEntries.erase(
        std::remove_if(m_setup.xpEntries.begin(), m_setup.xpEntries.end(),
            [lastCh](const mcp::ShowFile::AudioSetup::XpEntry& e){ return e.ch == lastCh; }),
        m_setup.xpEntries.end());
    m_setup.channels.pop_back();
    rebuildChannelsTable();
    rebuildXpGrid();
}

void AudioSetupDialog::onChannelCountChanged(int /*n*/) {}

void AudioSetupDialog::syncChannelsFromTable() {
    if (!m_chanTable) return;
    const int rows = m_chanTable->rowCount();
    const bool multiDev = !m_setup.devices.empty();
    m_setup.channels.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& c = m_setup.channels[static_cast<size_t>(i)];
        const bool isSlave = (i > 0 && m_setup.channels[static_cast<size_t>(i - 1)].linkedStereo);

        if (auto* w = qobject_cast<QLineEdit*>(m_chanTable->cellWidget(i, 0)))
            c.name = w->text().toStdString();

        if (multiDev) {
            if (auto* cb = qobject_cast<QComboBox*>(m_chanTable->cellWidget(i, 1)))
                c.deviceIndex = cb->currentIndex();
        }

        if (!isSlave) {
            if (auto* cell = m_chanTable->cellWidget(i, 2))
                if (auto* chk = cell->findChild<QCheckBox*>())
                    c.linkedStereo = chk->isChecked();
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(m_chanTable->cellWidget(i, 3)))
                c.masterGainDb = static_cast<float>(spin->value());
            if (auto* cell = m_chanTable->cellWidget(i, 4))
                if (auto* chk = cell->findChild<QCheckBox*>())
                    c.mute = chk->isChecked();
        } else {
            const auto& master = m_setup.channels[static_cast<size_t>(i - 1)];
            c.linkedStereo = false;
            c.masterGainDb = master.masterGainDb;
            c.mute         = master.mute;
        }
    }
}

// ── Tab 3 — Crosspoint ────────────────────────────────────────────────────

void AudioSetupDialog::buildXpTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);

    auto* infoLbl = new QLabel(
        "Route each channel to a physical output.  "
        "Default diagonal = 0 dB.  Blank = off.");
    infoLbl->setStyleSheet("color:#777;font-size:11px;");
    infoLbl->setWordWrap(true);
    vlay->addWidget(infoLbl);

    m_xpScroll = new QScrollArea(page);
    m_xpScroll->setWidgetResizable(true);
    m_xpScroll->setStyleSheet("QScrollArea{background:#1a1a1a;border:1px solid #2a2a2a;}");
    vlay->addWidget(m_xpScroll, 1);

    m_tabs->addTab(page, "Crosspoint");
    rebuildXpGrid();
}

void AudioSetupDialog::rebuildXpGrid() {
    delete m_xpContent;
    m_xpCells.clear();

    const int numCh   = static_cast<int>(m_setup.channels.size());
    const int numPhys = totalPhysOutputs();
    const bool multiDev = !m_setup.devices.empty();

    m_xpContent = new QWidget;
    auto* outer = new QVBoxLayout(m_xpContent);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);

    if (numCh == 0 || numPhys == 0) {
        outer->addWidget(new QLabel("No channels or outputs configured."));
        m_xpScroll->setWidget(m_xpContent);
        return;
    }

    // Build lookup: (ch, out) → db
    auto findXp = [&](int ch, int out) -> std::optional<float> {
        for (const auto& xe : m_setup.xpEntries)
            if (xe.ch == ch && xe.out == out)
                return xe.db;
        return std::nullopt;
    };

    // Build physOut labels and device-break indices
    // In multi-device mode: "D1.1", "D1.2", "D2.1", etc.
    std::vector<QString> physLabels;
    std::vector<int>     physDevIdx;  // which device each physOut belongs to
    if (multiDev) {
        int gp = 0;
        for (int d = 0; d < (int)m_setup.devices.size(); ++d) {
            for (int lp = 0; lp < m_setup.devices[static_cast<size_t>(d)].channelCount; ++lp, ++gp) {
                physLabels.push_back(QString("D%1.%2").arg(d + 1).arg(lp + 1));
                physDevIdx.push_back(d);
            }
        }
    } else {
        for (int p = 0; p < numPhys; ++p) {
            physLabels.push_back(QString("Out %1").arg(p + 1));
            physDevIdx.push_back(0);
        }
    }

    auto* xpGroup = new QGroupBox("Channel → Physical Output");
    auto* grid = new QGridLayout(xpGroup);
    grid->setSpacing(3);
    grid->setContentsMargins(8, 14, 8, 8);

    constexpr int kCellW = 52;
    constexpr int kLblW  = 70;
    constexpr int kSp    = 3;
    xpGroup->setFixedWidth(kLblW + numPhys * (kCellW + kSp) + 20);

    // Column headers
    for (int p = 0; p < numPhys; ++p) {
        auto* lbl = new QLabel(physLabels[static_cast<size_t>(p)], xpGroup);
        lbl->setFixedWidth(kCellW);
        lbl->setAlignment(Qt::AlignHCenter);
        lbl->setStyleSheet("color:#888;font-size:10px;");
        grid->addWidget(lbl, 0, p + 1);
    }

    m_xpCells.resize(static_cast<size_t>(numCh),
                     std::vector<QLineEdit*>(static_cast<size_t>(numPhys), nullptr));

    for (int ch = 0; ch < numCh; ++ch) {
        const QString chName = (ch < (int)m_setup.channels.size())
            ? QString::fromStdString(m_setup.channels[static_cast<size_t>(ch)].name)
            : QString("Ch %1").arg(ch + 1);
        auto* rowLbl = new QLabel(chName, xpGroup);
        rowLbl->setFixedWidth(kLblW);
        rowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLbl->setStyleSheet("color:#aaa;font-size:11px;");
        grid->addWidget(rowLbl, ch + 1, 0);

        for (int p = 0; p < numPhys; ++p) {
            auto* cell = new QLineEdit(xpGroup);
            cell->setFixedSize(kCellW, 22);
            cell->setAlignment(Qt::AlignHCenter);

            // Dim cells that belong to a different device than this channel's device
            const bool channelOnSameDev =
                !multiDev ||
                (ch < (int)m_setup.channels.size() &&
                 m_setup.channels[static_cast<size_t>(ch)].deviceIndex == physDevIdx[static_cast<size_t>(p)]);
            cell->setStyleSheet(channelOnSameDev
                ? "QLineEdit{background:#1a1a1a;color:#ddd;border:1px solid #383838;"
                  "  border-radius:2px;padding:1px 3px;font-size:11px;}"
                  "QLineEdit:focus{border-color:#2a6ab8;}"
                : "QLineEdit{background:#141414;color:#555;border:1px solid #262626;"
                  "  border-radius:2px;padding:1px 3px;font-size:11px;}");

            auto xpVal = findXp(ch, p);
            if (xpVal.has_value()) {
                cell->setText(fmtDb(*xpVal));
            } else if (ch == p) {
                cell->setPlaceholderText("0.0");
            } else {
                cell->setPlaceholderText("-inf");
            }

            m_xpCells[static_cast<size_t>(ch)][static_cast<size_t>(p)] = cell;
            grid->addWidget(cell, ch + 1, p + 1);
        }
    }

    outer->addWidget(xpGroup, 0, Qt::AlignLeft | Qt::AlignTop);
    outer->addStretch();
    m_xpScroll->setWidget(m_xpContent);
}

void AudioSetupDialog::syncXpFromGrid() {
    m_setup.xpEntries.clear();
    const int numCh   = static_cast<int>(m_xpCells.size());
    const int numPhys = (numCh > 0) ? static_cast<int>(m_xpCells[0].size()) : 0;

    for (int ch = 0; ch < numCh; ++ch) {
        for (int p = 0; p < numPhys; ++p) {
            auto* cell = m_xpCells[static_cast<size_t>(ch)][static_cast<size_t>(p)];
            if (!cell) continue;
            const QString txt = cell->text().trimmed();
            if (txt.isEmpty()) continue;
            float db = parseDb(txt, ch == p ? 0.0f : kInfDb);
            bool isDefault = (ch == p) ? (std::abs(db) < 0.001f) : (db <= kInfDb);
            if (!isDefault)
                m_setup.xpEntries.push_back({ch, p, db});
        }
    }
}

// ── Warnings ───────────────────────────────────────────────────────────────

void AudioSetupDialog::updateWarnings() {
    if (!m_warnLabel) return;
    QStringList warns;

    const int numDevices = static_cast<int>(m_setup.devices.size());
    if (numDevices >= 2) {
        warns << "Multiple independent devices may drift over long shows.";

        // Check for LTC cues routed to a non-master device
        const int masterDev = m_masterCombo ? m_masterCombo->currentIndex() : 0;

        // Build physOut → device map from current device list
        std::vector<int> physDev;
        for (int d = 0; d < numDevices; ++d) {
            const int cnt = m_setup.devices[static_cast<size_t>(d)].channelCount;
            for (int lp = 0; lp < cnt; ++lp)
                physDev.push_back(d);
        }

        // Recursive scan of show cues for LTC cues on non-master device
        bool ltcOnNonMaster = false;
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> scanCues;
        scanCues = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& c : cues) {
                if (c.type == "timecode" && c.tcType == "ltc") {
                    const int ch = c.tcLtcChannel;
                    if (ch >= 0 && ch < (int)physDev.size() && physDev[ch] != masterDev)
                        ltcOnNonMaster = true;
                }
                if (!c.children.empty())
                    scanCues(c.children);
            }
        };
        if (m_model) {
            for (const auto& cl : m_model->sf.cueLists)
                scanCues(cl.cues);
        }
        if (ltcOnNonMaster)
            warns << "LTC cue is routed to a non-master device — timecode may drift.";
    }

    m_warnLabel->setVisible(!warns.isEmpty());
    if (!warns.isEmpty())
        m_warnLabel->setText("⚠  " + warns.join("\n⚠  "));
}
