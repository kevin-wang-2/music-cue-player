#include "SettingsDialog.h"
#include "AppModel.h"
#include "MidiInputManager.h"
#include "engine/AudioEngine.h"
#include "engine/MidiOut.h"

#include <functional>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <iphlpapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#  include <ifaddrs.h>
#  include <net/if.h>
#  if defined(__APPLE__)
#    include <CoreFoundation/CoreFoundation.h>
#    include <SystemConfiguration/SCNetworkConfiguration.h>
#  endif
#endif

#include <cmath>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

static const char* kDialogStyle =
    "QDialog{background:#1a1a1a;color:#ddd;}"
    "QListWidget{background:#141414;color:#bbb;border:none;border-right:1px solid #2a2a2a;"
    "  outline:none;}"
    "QListWidget::item{padding:8px 12px;border-bottom:1px solid #222;}"
    "QListWidget::item:selected{background:#2a4a70;color:#fff;}"
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
    "QDoubleSpinBox{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QLineEdit{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QLineEdit:focus{border-color:#2a6ab8;}"
    "QComboBox{background:#252525;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:2px 5px;}"
    "QLabel{color:#aaa;}"
    "QPushButton{background:#2a2a2a;color:#ddd;border:1px solid #444;"
    "  border-radius:3px;padding:4px 12px;}"
    "QPushButton:hover{background:#383838;}"
    "QPushButton:default{background:#1a4a88;border-color:#2a6ab8;}"
    "QScrollArea{background:#1a1a1a;border:none;}"
    "QGroupBox{color:#888;border:1px solid #2a2a2a;margin-top:8px;"
    "  padding-top:6px;border-radius:4px;}"
    "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}";

// Populate a QComboBox with network interfaces that are UP and have an IP address.
// Item text: "en0 (Wi-Fi)" on macOS, plain BSD name elsewhere.
// Item data: BSD name string (used when saving).
// 'current' is the BSD name to pre-select; if not found it is appended as-is.
static void populateIfaceCombo(QComboBox* cb, const QString& current) {
    cb->addItem("any", QString("any"));

#if defined(_WIN32)
    // Windows: use GetAdaptersAddresses — FriendlyName is the user-visible adapter name,
    // Description is the hardware model string.  We save FriendlyName as the key.
    {
        ULONG bufLen = 15000;
        std::vector<BYTE> buf(bufLen);
        DWORD rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
            &bufLen);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(bufLen);
            rc = GetAdaptersAddresses(AF_UNSPEC,
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST,
                nullptr,
                reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                &bufLen);
        }
        if (rc == NO_ERROR) {
            for (auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
                 aa; aa = aa->Next) {
                if (aa->OperStatus != IfOperStatusUp) continue;
                const QString friendly = QString::fromWCharArray(aa->FriendlyName);
                const QString descr    = QString::fromWCharArray(aa->Description);
                const QString label    = QString("%1 (%2)").arg(friendly, descr);
                cb->addItem(label, friendly);
            }
        }
    }

#elif defined(__unix__) || defined(__APPLE__)
    // POSIX: collect UP interfaces with an IPv4/IPv6 address, deduped.
    QStringList bsdNames;
    {
        struct ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) == 0 && ifap) {
            for (const struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_name || !ifa->ifa_addr) continue;
                if (!(ifa->ifa_flags & IFF_UP))           continue;
                if (ifa->ifa_flags & IFF_POINTOPOINT)     continue;  // utun*, gif*, stf*, vpn tunnels
                if (ifa->ifa_addr->sa_family != AF_INET)  continue;  // IPv4 only — drops awdl0, llw0, etc.
                const QString n = QString::fromLatin1(ifa->ifa_name);
                if (!bsdNames.contains(n)) bsdNames << n;
            }
            freeifaddrs(ifap);
        }
    }
#  if defined(__APPLE__)
    // macOS: enrich with human-readable names via SystemConfiguration
    QMap<QString, QString> dispMap;
    {
        CFArrayRef ifaces = SCNetworkInterfaceCopyAll();
        if (ifaces) {
            const CFIndex n = CFArrayGetCount(ifaces);
            for (CFIndex i = 0; i < n; ++i) {
                auto* iface = static_cast<SCNetworkInterfaceRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(ifaces, i)));
                CFStringRef bsd  = SCNetworkInterfaceGetBSDName(iface);
                CFStringRef disp = SCNetworkInterfaceGetLocalizedDisplayName(iface);
                if (bsd && disp)
                    dispMap[QString::fromCFString(bsd)] = QString::fromCFString(disp);
            }
            CFRelease(ifaces);
        }
    }
    for (const QString& bsd : bsdNames) {
        const QString disp  = dispMap.value(bsd);
        const QString label = disp.isEmpty() ? bsd : QString("%1 (%2)").arg(bsd, disp);
        cb->addItem(label, bsd);
    }
#  else
    for (const QString& bsd : bsdNames)
        cb->addItem(bsd, bsd);
#  endif
#endif

    // Pre-select; if not found (e.g. adapter unplugged), append as a fallback
    int idx = cb->findData(current);
    if (idx < 0) {
        cb->addItem(current, current);
        idx = cb->count() - 1;
    }
    cb->setCurrentIndex(idx);
}

// ── ctor ───────────────────────────────────────────────────────────────────

SettingsDialog::SettingsDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Settings");
    setMinimumSize(900, 520);
    setStyleSheet(kDialogStyle);

    m_numPhys            = model->engineOk ? model->engine.channels() : 2;
    m_audioSetupSnapshot = model->sf.audioSetup;   // saved for cancel/restore
    m_paDevices          = mcp::AudioEngine::listOutputDevices();
    m_networkSetup   = model->sf.networkSetup;
    m_midiSetup      = model->sf.midiSetup;
    m_oscSettings    = model->sf.oscServer;
    m_systemControls = model->sf.systemControls;

    // Bootstrap a default device when the show has no device config yet.
    // Uses the engine's current device name and channel count so the user
    // sees a meaningful starting point rather than an empty list.
    if (m_model->sf.audioSetup.devices.empty()) {
        mcp::ShowFile::AudioSetup::Device d;
        d.name         = model->sf.engine.deviceName;  // "" = system default
        d.channelCount = m_numPhys;
        d.bufferSize   = 512;
        d.masterClock  = true;
        m_model->sf.audioSetup.devices.push_back(d);
    }

    if (m_model->sf.audioSetup.channels.empty()) {
        for (int i = 0; i < m_numPhys; ++i) {
            mcp::ShowFile::AudioSetup::Channel c;
            c.name = "Ch " + std::to_string(i + 1);
            m_model->sf.audioSetup.channels.push_back(c);
        }
    }

    // Explicitly initialize the diagonal crosspoint (0 dB) for any missing entries.
    // This ensures every cell is explicit so user edits are never silently discarded.
    {
        const int nCh   = static_cast<int>(m_model->sf.audioSetup.channels.size());
        const int nPhys = totalPhysOutputs();
        for (int i = 0; i < std::min(nCh, nPhys); ++i) {
            bool found = false;
            for (const auto& xe : m_model->sf.audioSetup.xpEntries)
                if (xe.ch == i && xe.out == i) { found = true; break; }
            if (!found)
                m_model->sf.audioSetup.xpEntries.push_back({i, i, 0.0f});
        }
    }

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 12);
    outer->setSpacing(0);

    // Body: sidebar + stack
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_sidebar = new QListWidget;
    m_sidebar->setFixedWidth(130);
    m_sidebar->addItem("Audio");
    m_sidebar->addItem("Network");
    m_sidebar->addItem("MIDI");
    m_sidebar->addItem("Controls");
    m_sidebar->setCurrentRow(0);
    body->addWidget(m_sidebar);

    m_stack = new QStackedWidget;
    m_stack->setContentsMargins(0, 0, 0, 0);
    body->addWidget(m_stack, 1);

    outer->addLayout(body, 1);

    // OK / Cancel buttons
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setText("Apply");
    btnBox->button(QDialogButtonBox::Ok)->setDefault(true);
    btnBox->setContentsMargins(12, 0, 12, 0);
    outer->addWidget(btnBox);

    buildAudioPage();
    buildNetworkPage();
    buildMidiPage();
    buildControlsPage();

    connect(m_sidebar, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    connect(btnBox, &QDialogButtonBox::accepted, this, [this]() {
        syncDevicesFromTable();
        syncChannelsFromTable();
        syncOutputFromTable();
        syncNetworkFromTable();
        syncMidiFromTable();
        syncOscFromUI();
        saveControls();
        m_model->sf.audioSetup.normalizeMaster();
        accept();
    });
    connect(btnBox, &QDialogButtonBox::rejected, this, [this]() {
        // Restore audio setup to state before this dialog was opened.
        m_model->sf.audioSetup = m_audioSetupSnapshot;
        reject();
    });
}

// ── Audio section ──────────────────────────────────────────────────────────

void SettingsDialog::buildAudioPage() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(6);

    // Warning bar — shown only when device drift/LTC warnings apply
    m_audioWarnLabel = new QLabel(page);
    m_audioWarnLabel->setWordWrap(true);
    m_audioWarnLabel->setStyleSheet(
        "color:#e8a020;background:#2a2010;border:1px solid #5a4010;"
        "border-radius:3px;padding:4px 8px;font-size:11px;");
    m_audioWarnLabel->setVisible(false);
    vlay->addWidget(m_audioWarnLabel);

    m_audioTabs = new QTabWidget(page);
    vlay->addWidget(m_audioTabs, 1);

    buildDevicesTab();
    buildChannelsTab();
    buildOutputTab();

    m_stack->addWidget(page);
}

// ── Devices tab ────────────────────────────────────────────────────────────

int SettingsDialog::totalPhysOutputs() const {
    if (m_model->sf.audioSetup.devices.empty()) return m_numPhys;
    int total = 0;
    for (const auto& d : m_model->sf.audioSetup.devices) total += d.channelCount;
    return total;
}

void SettingsDialog::buildDevicesTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    // Master clock selector
    auto* masterRow = new QHBoxLayout;
    auto* masterLbl = new QLabel("Master clock device:");
    masterLbl->setStyleSheet("color:#aaa;");
    masterRow->addWidget(masterLbl);
    m_masterCombo = new QComboBox;
    m_masterCombo->setMinimumWidth(120);
    connect(m_masterCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::updateAudioWarnings);
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
    hint->setStyleSheet("color:#555;font-size:10px;");
    hint->setWordWrap(true);
    vlay->addWidget(hint);

    rebuildDevicesTable();

    connect(m_btnAddDev,    &QPushButton::clicked, this, &SettingsDialog::onAddDevice);
    connect(m_btnRemoveDev, &QPushButton::clicked, this, &SettingsDialog::onRemoveDevice);

    m_audioTabs->addTab(page, "Devices");
}

void SettingsDialog::rebuildDevicesTable() {
    m_devTable->setRowCount(0);
    for (int i = 0; i < (int)m_model->sf.audioSetup.devices.size(); ++i) {
        m_devTable->insertRow(i);
        const auto& dev = m_model->sf.audioSetup.devices[static_cast<size_t>(i)];

        auto* nameCb = new QComboBox;
        nameCb->addItem("(system default)", QString(""));
        for (const auto& pa : m_paDevices)
            nameCb->addItem(
                QString::fromStdString(pa.name) + QString(" (%1 ch)").arg(pa.maxOutputChannels),
                QString::fromStdString(pa.name));
        const int nameIdx = nameCb->findData(QString::fromStdString(dev.name));
        if (nameIdx >= 0) nameCb->setCurrentIndex(nameIdx);
        m_devTable->setCellWidget(i, 0, nameCb);

        auto* chSpin = new QSpinBox;
        chSpin->setRange(1, 64);
        chSpin->setValue(dev.channelCount);

        // When user picks a different device, auto-fill channel count from PA info
        connect(nameCb, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this, nameCb, chSpin]() {
            const QString devName = nameCb->currentData().toString();
            for (const auto& pa : m_paDevices) {
                if (QString::fromStdString(pa.name) == devName) {
                    chSpin->setValue(pa.maxOutputChannels);
                    break;
                }
            }
            updateAudioWarnings();
        });
        connect(chSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
            syncDevicesFromTable();
            rebuildMasterCombo();
            updateAudioWarnings();
        });
        m_devTable->setCellWidget(i, 1, chSpin);

        auto* bufCb = new QComboBox;
        for (int bs : {128, 256, 512, 1024, 2048})
            bufCb->addItem(QString::number(bs), bs);
        const int bufIdx = bufCb->findData(dev.bufferSize);
        bufCb->setCurrentIndex(bufIdx >= 0 ? bufIdx : 2);
        m_devTable->setCellWidget(i, 2, bufCb);
    }
    rebuildMasterCombo();
}

void SettingsDialog::rebuildMasterCombo() {
    if (!m_masterCombo) return;
    m_masterCombo->blockSignals(true);
    const int prevSel = m_masterCombo->currentIndex();
    m_masterCombo->clear();
    for (int i = 0; i < (int)m_model->sf.audioSetup.devices.size(); ++i)
        m_masterCombo->addItem(QString("Device %1").arg(i + 1));
    int masterIdx = 0;
    for (int i = 0; i < (int)m_model->sf.audioSetup.devices.size(); ++i)
        if (m_model->sf.audioSetup.devices[static_cast<size_t>(i)].masterClock) { masterIdx = i; break; }
    const int toSelect = (masterIdx < m_masterCombo->count())  ? masterIdx
                       : (prevSel  < m_masterCombo->count())   ? prevSel : 0;
    m_masterCombo->setCurrentIndex(toSelect);
    m_masterCombo->setEnabled(m_masterCombo->count() > 0);
    m_masterCombo->blockSignals(false);
    updateAudioWarnings();
}

void SettingsDialog::syncDevicesFromTable() {
    if (!m_devTable) return;
    const int masterIdx = m_masterCombo ? m_masterCombo->currentIndex() : 0;
    const int rows = m_devTable->rowCount();
    m_model->sf.audioSetup.devices.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& dev = m_model->sf.audioSetup.devices[static_cast<size_t>(i)];
        if (auto* cb = qobject_cast<QComboBox*>(m_devTable->cellWidget(i, 0)))
            dev.name = cb->currentData().toString().toStdString();
        if (auto* sp = qobject_cast<QSpinBox*>(m_devTable->cellWidget(i, 1)))
            dev.channelCount = sp->value();
        if (auto* cb = qobject_cast<QComboBox*>(m_devTable->cellWidget(i, 2)))
            dev.bufferSize = cb->currentData().toInt();
        dev.masterClock = (i == masterIdx);
    }
}

void SettingsDialog::onAddDevice() {
    syncDevicesFromTable();
    mcp::ShowFile::AudioSetup::Device d;
    // Default channel count from the first available PA device, else 2
    d.channelCount = m_paDevices.empty() ? 2 : m_paDevices[0].maxOutputChannels;
    d.bufferSize   = 512;
    d.masterClock  = m_model->sf.audioSetup.devices.empty();
    m_model->sf.audioSetup.devices.push_back(d);
    rebuildDevicesTable();
    rebuildChannelsTable();
    updateAudioWarnings();
}

void SettingsDialog::onRemoveDevice() {
    const int row = m_devTable->currentRow();
    if (row < 0 || row >= (int)m_model->sf.audioSetup.devices.size()) return;
    syncDevicesFromTable();
    const int removedDev = row;
    auto& chans = m_model->sf.audioSetup.channels;
    chans.erase(
        std::remove_if(chans.begin(), chans.end(),
            [removedDev](const mcp::ShowFile::AudioSetup::Channel& c) {
                return c.deviceIndex == removedDev;
            }),
        chans.end());
    for (auto& c : chans)
        if (c.deviceIndex > removedDev) --c.deviceIndex;
    m_model->sf.audioSetup.devices.erase(m_model->sf.audioSetup.devices.begin() + removedDev);
    m_model->sf.audioSetup.normalizeMaster();
    rebuildDevicesTable();
    rebuildChannelsTable();
    updateAudioWarnings();
}

void SettingsDialog::updateAudioWarnings() {
    if (!m_audioWarnLabel) return;
    QStringList warns;
    const int numDevices = static_cast<int>(m_model->sf.audioSetup.devices.size());
    if (numDevices >= 2) {
        warns << "Multiple independent devices may drift over long shows.";
        const int masterDev = m_masterCombo ? m_masterCombo->currentIndex() : 0;
        std::vector<int> physDev;
        for (int d = 0; d < numDevices; ++d)
            for (int lp = 0; lp < m_model->sf.audioSetup.devices[static_cast<size_t>(d)].channelCount; ++lp)
                physDev.push_back(d);
        bool ltcOnNonMaster = false;
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> scanCues;
        scanCues = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& c : cues) {
                if (c.type == "timecode" && c.tcType == "ltc") {
                    const int ch = c.tcLtcChannel;
                    if (ch >= 0 && ch < (int)physDev.size() && physDev[ch] != masterDev)
                        ltcOnNonMaster = true;
                }
                if (!c.children.empty()) scanCues(c.children);
            }
        };
        if (m_model)
            for (const auto& cl : m_model->sf.cueLists) scanCues(cl.cues);
        if (ltcOnNonMaster)
            warns << "LTC cue is routed to a non-master device — timecode may drift.";
    }
    m_audioWarnLabel->setVisible(!warns.isEmpty());
    if (!warns.isEmpty())
        m_audioWarnLabel->setText("⚠  " + warns.join("\n⚠  "));
}

void SettingsDialog::buildChannelsTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    auto* toolbar = new QHBoxLayout;
    m_btnAddChan    = new QPushButton("+  Add Channel");
    m_btnRemoveChan = new QPushButton("−  Remove Last");
    toolbar->addWidget(m_btnAddChan);
    toolbar->addWidget(m_btnRemoveChan);
    toolbar->addStretch();
    vlay->addLayout(toolbar);

    // Col 0: Name | Col 1: Device | Col 2: Stereo Link
    m_chanTable = new QTableWidget(0, 3, page);
    m_chanTable->setHorizontalHeaderLabels({"Name", "Device", "Stereo Link"});
    m_chanTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_chanTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_chanTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_chanTable->verticalHeader()->setDefaultSectionSize(26);
    m_chanTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_chanTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    vlay->addWidget(m_chanTable, 1);

    rebuildChannelsTable();

    connect(m_btnAddChan,    &QPushButton::clicked, this, &SettingsDialog::onAddChannel);
    connect(m_btnRemoveChan, &QPushButton::clicked, this, &SettingsDialog::onRemoveChannel);

    m_audioTabs->addTab(page, "Channels");
}

void SettingsDialog::rebuildChannelsTable() {
    if (!m_chanTable) return;
    m_chanTable->setRowCount(0);
    const int n = static_cast<int>(m_model->sf.audioSetup.channels.size());
    const bool multiDev = !m_model->sf.audioSetup.devices.empty();
    m_chanTable->setColumnHidden(1, !multiDev);

    for (int i = 0; i < n; ++i) m_chanTable->insertRow(i);

    for (int i = 0; i < n; ++i) {
        const auto& ch = m_model->sf.audioSetup.channels[static_cast<size_t>(i)];
        const bool isSlave = (i > 0 && m_model->sf.audioSetup.channels[static_cast<size_t>(i - 1)].linkedStereo);

        m_chanTable->setVerticalHeaderItem(
            i, new QTableWidgetItem(isSlave ? QString("   └ %1").arg(i + 1)
                                            : QString::number(i + 1)));

        // Col 0: Name
        auto* nameEdit = new QLineEdit(QString::fromStdString(ch.name));
        nameEdit->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
        m_chanTable->setCellWidget(i, 0, nameEdit);

        // Col 1: Device (multi-device mode only)
        if (multiDev) {
            auto* devCb = new QComboBox;
            for (int d = 0; d < (int)m_model->sf.audioSetup.devices.size(); ++d)
                devCb->addItem(QString("Device %1").arg(d + 1));
            if (ch.deviceIndex >= 0 && ch.deviceIndex < (int)m_model->sf.audioSetup.devices.size())
                devCb->setCurrentIndex(ch.deviceIndex);
            m_chanTable->setCellWidget(i, 1, devCb);
        }

        // Col 2: Stereo Link (non-slave rows only)
        if (!isSlave) {
            auto* stereoChk = new QCheckBox;
            stereoChk->setChecked(ch.linkedStereo);
            stereoChk->setToolTip("Link this and the next channel as a stereo pair.");
            auto* stereoCell = new QWidget;
            auto* stLay = new QHBoxLayout(stereoCell);
            stLay->setAlignment(Qt::AlignCenter);
            stLay->setContentsMargins(0, 0, 0, 0);
            stLay->addWidget(stereoChk);
            m_chanTable->setCellWidget(i, 2, stereoCell);
            connect(stereoChk, &QCheckBox::toggled, this, [this](bool) {
                syncChannelsFromTable();
                rebuildChannelsTable();
            });
        }
    }
}

void SettingsDialog::onAddChannel() {
    syncChannelsFromTable();
    mcp::ShowFile::AudioSetup::Channel c;
    c.name = "Ch " + std::to_string(m_model->sf.audioSetup.channels.size() + 1);
    m_model->sf.audioSetup.channels.push_back(c);
    rebuildChannelsTable();
}

void SettingsDialog::onRemoveChannel() {
    if (m_model->sf.audioSetup.channels.size() <= 1) return;
    syncChannelsFromTable();
    const int lastCh = static_cast<int>(m_model->sf.audioSetup.channels.size()) - 1;
    m_model->sf.audioSetup.xpEntries.erase(
        std::remove_if(m_model->sf.audioSetup.xpEntries.begin(), m_model->sf.audioSetup.xpEntries.end(),
            [lastCh](const mcp::ShowFile::AudioSetup::XpEntry& e){ return e.ch == lastCh; }),
        m_model->sf.audioSetup.xpEntries.end());
    m_model->sf.audioSetup.channels.pop_back();
    rebuildChannelsTable();
}

void SettingsDialog::syncChannelsFromTable() {
    if (!m_chanTable) return;
    const int rows = m_chanTable->rowCount();
    const bool multiDev = !m_model->sf.audioSetup.devices.empty();
    m_model->sf.audioSetup.channels.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& c = m_model->sf.audioSetup.channels[static_cast<size_t>(i)];
        const bool isSlave = (i > 0 && m_model->sf.audioSetup.channels[static_cast<size_t>(i - 1)].linkedStereo);

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
        } else {
            c.linkedStereo = false;
        }
    }
}

// ── Output DSP tab ─────────────────────────────────────────────────────────

void SettingsDialog::buildOutputTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    auto* hint = new QLabel(
        "Per-physical-output DSP applied after channel processing. "
        "Phase and delay combine with channel DSP (phase XOR'd, delays summed).");
    hint->setStyleSheet("color:#777;font-size:11px;");
    hint->setWordWrap(true);
    vlay->addWidget(hint);

    m_outTable = new QTableWidget(0, 4, page);
    m_outTable->setHorizontalHeaderLabels({"Output", "Phase ⌀", "Delay", "Unit"});
    m_outTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_outTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_outTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_outTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_outTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_outTable->verticalHeader()->setDefaultSectionSize(26);
    m_outTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_outTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vlay->addWidget(m_outTable, 1);

    rebuildOutputTable();

    m_audioTabs->addTab(page, "Output");
}

void SettingsDialog::rebuildOutputTable() {
    if (!m_outTable) return;
    m_outTable->setRowCount(0);

    const int numPhys = totalPhysOutputs();
    const bool multiDev = !m_model->sf.audioSetup.devices.empty();

    m_model->sf.audioSetup.physOutDsp.resize(static_cast<size_t>(numPhys));

    int globalIdx = 0;
    for (int d = 0; d < (multiDev ? (int)m_model->sf.audioSetup.devices.size() : 1); ++d) {
        const int chCount = multiDev ? m_model->sf.audioSetup.devices[static_cast<size_t>(d)].channelCount
                                     : numPhys;
        for (int lp = 0; lp < chCount; ++lp, ++globalIdx) {
            const int row = globalIdx;
            m_outTable->insertRow(row);

            const auto& dsp = m_model->sf.audioSetup.physOutDsp[static_cast<size_t>(globalIdx)];

            QString label = multiDev ? QString("D%1.%2").arg(d + 1).arg(lp + 1)
                                     : QString("Out %1").arg(lp + 1);
            auto* lbl = new QLabel(label);
            lbl->setStyleSheet("color:#ccc;padding:1px 6px;");
            m_outTable->setCellWidget(row, 0, lbl);

            auto* phaseChk = new QCheckBox;
            phaseChk->setChecked(dsp.phaseInvert);
            phaseChk->setToolTip("Invert polarity of this physical output");
            auto* phaseCell = new QWidget;
            auto* phaseLay = new QHBoxLayout(phaseCell);
            phaseLay->setAlignment(Qt::AlignCenter);
            phaseLay->setContentsMargins(0, 0, 0, 0);
            phaseLay->addWidget(phaseChk);
            m_outTable->setCellWidget(row, 1, phaseCell);

            const bool inSamp = dsp.delayInSamples;
            auto* delaySpin = new QDoubleSpinBox;
            delaySpin->setDecimals(inSamp ? 0 : 2);
            delaySpin->setSingleStep(inSamp ? 1.0 : 0.1);
            delaySpin->setRange(0.0, inSamp ? (mcp::AudioEngine::kMaxDelaySamples - 1) : 1000.0);
            delaySpin->setValue(inSamp ? dsp.delaySamples : dsp.delayMs);
            delaySpin->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 2px;");
            m_outTable->setCellWidget(row, 2, delaySpin);

            auto* unitCb = new QComboBox;
            unitCb->addItem("ms");
            unitCb->addItem("samp");
            unitCb->setCurrentIndex(inSamp ? 1 : 0);
            connect(unitCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [delaySpin](int idx) {
                const bool toSamp = (idx == 1);
                delaySpin->setDecimals(toSamp ? 0 : 2);
                delaySpin->setSingleStep(toSamp ? 1.0 : 0.1);
                delaySpin->setRange(0.0, toSamp ? (mcp::AudioEngine::kMaxDelaySamples - 1) : 1000.0);
            });
            m_outTable->setCellWidget(row, 3, unitCb);
        }
    }
}

void SettingsDialog::syncOutputFromTable() {
    if (!m_outTable) return;
    const int rows = m_outTable->rowCount();
    m_model->sf.audioSetup.physOutDsp.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& dsp = m_model->sf.audioSetup.physOutDsp[static_cast<size_t>(i)];

        if (auto* cell = m_outTable->cellWidget(i, 1))
            if (auto* chk = cell->findChild<QCheckBox*>())
                dsp.phaseInvert = chk->isChecked();

        const auto* unitCb  = qobject_cast<QComboBox*>(m_outTable->cellWidget(i, 3));
        const auto* delaySp = qobject_cast<QDoubleSpinBox*>(m_outTable->cellWidget(i, 2));
        if (unitCb && delaySp) {
            dsp.delayInSamples = (unitCb->currentIndex() == 1);
            if (dsp.delayInSamples) {
                dsp.delaySamples = static_cast<int>(delaySp->value());
                dsp.delayMs      = 0.0;
            } else {
                dsp.delayMs      = delaySp->value();
                dsp.delaySamples = 0;
            }
        }
    }
}

// ── Network section ────────────────────────────────────────────────────────

void SettingsDialog::buildNetworkPage() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(0);

    m_networkTabs = new QTabWidget(page);
    vlay->addWidget(m_networkTabs, 1);

    buildNetworkOutputTab();
    buildOscServerTab();

    m_stack->addWidget(page);
}

void SettingsDialog::buildNetworkOutputTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    // Toolbar
    auto* toolbar = new QHBoxLayout;
    m_btnAddPatch    = new QPushButton("+  Add Patch");
    m_btnRemovePatch = new QPushButton("−  Remove Selected");
    toolbar->addWidget(m_btnAddPatch);
    toolbar->addWidget(m_btnRemovePatch);
    toolbar->addStretch();
    vlay->addLayout(toolbar);

    // Patch table: Name | Type | Protocol | Interface | Destination | Password
    m_patchTable = new QTableWidget(0, 6, page);
    m_patchTable->setHorizontalHeaderLabels(
        {"Name", "Type", "Protocol", "Interface", "Destination", "Password"});
    m_patchTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_patchTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_patchTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_patchTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_patchTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_patchTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_patchTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_patchTable->verticalHeader()->setDefaultSectionSize(26);
    m_patchTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    vlay->addWidget(m_patchTable, 1);

    // Populate from m_networkSetup
    for (const auto& patch : m_networkSetup.patches) {
        const int row = m_patchTable->rowCount();
        m_patchTable->insertRow(row);

        auto addEdit = [&](int col, const QString& val) {
            auto* e = new QLineEdit(val);
            e->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
            m_patchTable->setCellWidget(row, col, e);
        };
        auto addCombo = [&](int col, const QStringList& items, const QString& cur) {
            auto* cb = new QComboBox;
            cb->addItems(items);
            cb->setCurrentText(cur);
            m_patchTable->setCellWidget(row, col, cb);
        };

        addEdit(0, QString::fromStdString(patch.name));
        addCombo(1, {"osc", "plaintext"}, QString::fromStdString(patch.type));
        addCombo(2, {"udp", "tcp"},       QString::fromStdString(patch.protocol));
        { auto* cb = new QComboBox;
          populateIfaceCombo(cb, QString::fromStdString(patch.iface.empty() ? "any" : patch.iface));
          m_patchTable->setCellWidget(row, 3, cb); }
        addEdit(4, QString::fromStdString(patch.destination));
        addEdit(5, QString::fromStdString(patch.password));
    }

    connect(m_btnAddPatch,    &QPushButton::clicked, this, &SettingsDialog::onAddPatch);
    connect(m_btnRemovePatch, &QPushButton::clicked, this, &SettingsDialog::onRemovePatch);

    m_networkTabs->addTab(page, "Network Output");
}

void SettingsDialog::onAddPatch() {
    const int row = m_patchTable->rowCount();
    m_patchTable->insertRow(row);

    auto addEdit = [&](int col, const QString& val) {
        auto* e = new QLineEdit(val);
        e->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
        m_patchTable->setCellWidget(row, col, e);
    };
    auto addCombo = [&](int col, const QStringList& items, const QString& cur) {
        auto* cb = new QComboBox;
        cb->addItems(items);
        cb->setCurrentText(cur);
        m_patchTable->setCellWidget(row, col, cb);
    };

    addEdit(0, QString("Patch %1").arg(row + 1));
    addCombo(1, {"osc", "plaintext"}, "osc");
    addCombo(2, {"udp", "tcp"},       "udp");
    { auto* cb = new QComboBox;
      populateIfaceCombo(cb, "any");
      m_patchTable->setCellWidget(row, 3, cb); }
    addEdit(4, "127.0.0.1:8000");
    addEdit(5, "");
    m_patchTable->scrollToBottom();
    m_patchTable->selectRow(row);
}

void SettingsDialog::onRemovePatch() {
    const int row = m_patchTable->currentRow();
    if (row >= 0) m_patchTable->removeRow(row);
}

void SettingsDialog::syncNetworkFromTable() {
    m_networkSetup.patches.clear();
    const int rows = m_patchTable->rowCount();
    for (int i = 0; i < rows; ++i) {
        mcp::ShowFile::NetworkSetup::Patch p;

        auto getText = [&](int col) -> std::string {
            if (auto* w = qobject_cast<QLineEdit*>(m_patchTable->cellWidget(i, col)))
                return w->text().toStdString();
            return {};
        };
        auto getCombo = [&](int col) -> std::string {
            if (auto* w = qobject_cast<QComboBox*>(m_patchTable->cellWidget(i, col)))
                return w->currentText().toStdString();
            return {};
        };

        auto getIfaceData = [&]() -> std::string {
            if (auto* w = qobject_cast<QComboBox*>(m_patchTable->cellWidget(i, 3)))
                return w->currentData().toString().toStdString();
            return "any";
        };
        p.name        = getText(0);
        p.type        = getCombo(1);
        p.protocol    = getCombo(2);
        p.iface       = getIfaceData();
        p.destination = getText(4);
        p.password    = getText(5);
        m_networkSetup.patches.push_back(p);
    }
}

// ── MIDI section ───────────────────────────────────────────────────────────

void SettingsDialog::populateMidiDestCombo(QComboBox* cb, const QString& current) {
    cb->clear();
    const auto ports = mcp::midiOutputPorts();
    bool found = false;
    for (const auto& p : ports) {
        const QString qs = QString::fromStdString(p);
        cb->addItem(qs, qs);
        if (qs == current) found = true;
    }
    if (!found && !current.isEmpty()) {
        cb->addItem(current, current);
        cb->setCurrentIndex(cb->count() - 1);
    } else if (found) {
        cb->setCurrentText(current);
    }
}

void SettingsDialog::buildMidiPage() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    m_midiTabs = new QTabWidget;
    vlay->addWidget(m_midiTabs);

    buildMidiOutputTab();
    m_stack->addWidget(page);
}

void SettingsDialog::buildMidiOutputTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(8);

    // Toolbar (matches Network section style)
    auto* toolbar = new QHBoxLayout;
    m_btnAddMidiPatch    = new QPushButton("+  Add Patch");
    m_btnRemoveMidiPatch = new QPushButton("−  Remove Selected");
    toolbar->addWidget(m_btnAddMidiPatch);
    toolbar->addWidget(m_btnRemoveMidiPatch);
    toolbar->addStretch();
    vlay->addLayout(toolbar);

    // Table: Name | Destination
    m_midiPatchTable = new QTableWidget(0, 2, page);
    m_midiPatchTable->setHorizontalHeaderLabels({"Name", "Destination"});
    m_midiPatchTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_midiPatchTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_midiPatchTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_midiPatchTable->verticalHeader()->setDefaultSectionSize(26);
    m_midiPatchTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    vlay->addWidget(m_midiPatchTable, 1);

    // Populate from saved data
    for (const auto& p : m_midiSetup.patches) {
        const int row = m_midiPatchTable->rowCount();
        m_midiPatchTable->insertRow(row);
        auto* nameEdit = new QLineEdit(QString::fromStdString(p.name));
        nameEdit->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
        m_midiPatchTable->setCellWidget(row, 0, nameEdit);
        auto* destCb = new QComboBox;
        populateMidiDestCombo(destCb, QString::fromStdString(p.destination));
        m_midiPatchTable->setCellWidget(row, 1, destCb);
    }

    connect(m_btnAddMidiPatch,    &QPushButton::clicked, this, &SettingsDialog::onAddMidiPatch);
    connect(m_btnRemoveMidiPatch, &QPushButton::clicked, this, &SettingsDialog::onRemoveMidiPatch);

    m_midiTabs->addTab(page, "MIDI Output");
}

void SettingsDialog::onAddMidiPatch() {
    const int row = m_midiPatchTable->rowCount();
    m_midiPatchTable->insertRow(row);

    auto* nameEdit = new QLineEdit(QString("Patch %1").arg(row + 1));
    nameEdit->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
    m_midiPatchTable->setCellWidget(row, 0, nameEdit);

    auto* destCb = new QComboBox;
    populateMidiDestCombo(destCb, "");
    m_midiPatchTable->setCellWidget(row, 1, destCb);

    m_midiPatchTable->scrollToBottom();
    m_midiPatchTable->selectRow(row);
}

void SettingsDialog::onRemoveMidiPatch() {
    const int row = m_midiPatchTable->currentRow();
    if (row >= 0) m_midiPatchTable->removeRow(row);
}

void SettingsDialog::syncMidiFromTable() {
    m_midiSetup.patches.clear();
    const int rows = m_midiPatchTable->rowCount();
    for (int i = 0; i < rows; ++i) {
        mcp::ShowFile::MidiSetup::Patch p;
        if (auto* w = qobject_cast<QLineEdit*>(m_midiPatchTable->cellWidget(i, 0)))
            p.name = w->text().toStdString();
        if (auto* w = qobject_cast<QComboBox*>(m_midiPatchTable->cellWidget(i, 1)))
            p.destination = w->currentData().toString().toStdString();
        m_midiSetup.patches.push_back(p);
    }
}

// ── OSC Server tab (inside Network section) ────────────────────────────────

void SettingsDialog::buildOscServerTab() {
    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(12);

    // Enable + port row
    auto* topRow = new QHBoxLayout;
    m_chkOscEnabled = new QCheckBox("Enable OSC Server");
    m_chkOscEnabled->setChecked(m_oscSettings.enabled);
    topRow->addWidget(m_chkOscEnabled);
    topRow->addSpacing(20);
    topRow->addWidget(new QLabel("Listen Port:"));
    m_spinOscPort = new QSpinBox;
    m_spinOscPort->setRange(1024, 65535);
    m_spinOscPort->setValue(m_oscSettings.listenPort);
    m_spinOscPort->setFixedWidth(80);
    topRow->addWidget(m_spinOscPort);
    topRow->addStretch();
    vlay->addLayout(topRow);

    // Access control group
    auto* grp = new QGroupBox("Access Control");
    auto* grpLay = new QVBoxLayout(grp);
    grpLay->setSpacing(6);

    auto* infoLbl = new QLabel(
        "Add an \"Open\" entry to allow unauthenticated access.\n"
        "Add a \"Password\" entry to require a matching string as the first OSC argument.");
    infoLbl->setStyleSheet("color:#777;font-size:11px;");
    infoLbl->setWordWrap(true);
    grpLay->addWidget(infoLbl);

    m_oscAccessList = new QListWidget;
    m_oscAccessList->setFixedHeight(160);
    grpLay->addWidget(m_oscAccessList);

    auto* btnRow = new QHBoxLayout;
    m_btnOscAddOpen = new QPushButton("+ Add Open");
    m_btnOscAddPw   = new QPushButton("+ Add Password…");
    m_btnOscRemove  = new QPushButton("− Remove");
    btnRow->addWidget(m_btnOscAddOpen);
    btnRow->addWidget(m_btnOscAddPw);
    btnRow->addWidget(m_btnOscRemove);
    btnRow->addStretch();
    grpLay->addLayout(btnRow);

    vlay->addWidget(grp);
    vlay->addStretch();

    rebuildAccessList();

    connect(m_btnOscAddOpen, &QPushButton::clicked, this, [this]() {
        m_oscSettings.accessList.push_back({""});
        rebuildAccessList();
    });
    connect(m_btnOscAddPw, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString pw = QInputDialog::getText(this, "Add Password Entry",
            "Password:", QLineEdit::Normal, "", &ok);
        if (!ok || pw.isEmpty()) return;
        m_oscSettings.accessList.push_back({pw.toStdString()});
        rebuildAccessList();
    });
    connect(m_btnOscRemove, &QPushButton::clicked, this, [this]() {
        const int row = m_oscAccessList->currentRow();
        if (row < 0 || row >= (int)m_oscSettings.accessList.size()) return;
        m_oscSettings.accessList.erase(m_oscSettings.accessList.begin() + row);
        rebuildAccessList();
    });

    m_networkTabs->addTab(page, "OSC Server");
}

void SettingsDialog::rebuildAccessList() {
    m_oscAccessList->clear();
    for (const auto& entry : m_oscSettings.accessList) {
        if (entry.password.empty())
            m_oscAccessList->addItem("Open Access (no password required)");
        else
            m_oscAccessList->addItem(
                QString("Password: %1").arg(QString::fromStdString(entry.password)));
    }
}

void SettingsDialog::syncOscFromUI() {
    m_oscSettings.enabled    = m_chkOscEnabled->isChecked();
    m_oscSettings.listenPort = m_spinOscPort->value();
    // accessList already up to date — modified in-place via Add/Remove buttons
}

// ── Controls section ───────────────────────────────────────────────────────

static const struct { mcp::ControlAction action; const char* label; } kCtrlActions[] = {
    { mcp::ControlAction::Go,            "GO"               },
    { mcp::ControlAction::Arm,           "Arm"              },
    { mcp::ControlAction::PanicSelected, "Panic Selected"   },
    { mcp::ControlAction::SelectionUp,   "Selection Up"     },
    { mcp::ControlAction::SelectionDown, "Selection Down"   },
    { mcp::ControlAction::PanicAll,      "Panic All"        },
};

void SettingsDialog::buildControlsPage() {
    // Build one ActionRow per action (shared between both tabs)
    for (const auto& a : kCtrlActions) {
        ActionRow r;
        r.action = a.action;
        r.label  = a.label;
        m_actionRows.push_back(r);
    }

    auto* page = new QWidget;
    auto* vlay = new QVBoxLayout(page);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(0);

    m_controlsTabs = new QTabWidget(page);
    vlay->addWidget(m_controlsTabs, 1);

    buildControlsMidiTab();
    buildControlsOscTab();
    loadControls();

    m_stack->addWidget(page);
}

void SettingsDialog::buildControlsMidiTab() {
    auto* page = new QWidget;
    auto* lay  = new QVBoxLayout(page);
    lay->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    grid->addWidget(new QLabel("Action"), 0, 0);
    grid->addWidget(new QLabel("Enable"), 0, 1);
    grid->addWidget(new QLabel("Type"),   0, 2);
    grid->addWidget(new QLabel("Ch"),     0, 3);
    grid->addWidget(new QLabel("D1"),     0, 4);
    grid->addWidget(new QLabel("D2"),     0, 5);
    grid->addWidget(new QLabel(""),       0, 6);

    for (int i = 0; i < (int)m_actionRows.size(); ++i) {
        auto& r = m_actionRows[i];
        const int row = i + 1;
        grid->addWidget(new QLabel(r.label), row, 0);

        r.midiEnable = new QCheckBox;
        r.midiType   = new QComboBox;
        for (const char* s : {"Note On","Note Off","Control Change","Program Change","Pitch Bend"})
            r.midiType->addItem(s);
        r.midiCh = new QSpinBox; r.midiCh->setRange(0, 16); r.midiCh->setSpecialValueText("Any");
        r.midiD1 = new QSpinBox; r.midiD1->setRange(0, 127);
        r.midiD2 = new QSpinBox; r.midiD2->setRange(-1, 127); r.midiD2->setSpecialValueText("Any");
        r.midiCapture = new QPushButton("Capture");
        r.midiCapture->setFixedWidth(70);

        grid->addWidget(r.midiEnable,  row, 1, Qt::AlignHCenter);
        grid->addWidget(r.midiType,    row, 2);
        grid->addWidget(r.midiCh,      row, 3);
        grid->addWidget(r.midiD1,      row, 4);
        grid->addWidget(r.midiD2,      row, 5);
        grid->addWidget(r.midiCapture, row, 6);

        auto* capture = r.midiCapture;
        auto& rowRef  = r;
        connect(capture, &QPushButton::clicked, this, [this, capture, &rowRef]() {
            capture->setText("…");
            capture->setEnabled(false);
            m_model->midiIn.armCapture([capture, &rowRef](
                    mcp::MidiMsgType t, int ch, int d1, int d2) {
                rowRef.midiEnable->setChecked(true);
                rowRef.midiType->setCurrentIndex(static_cast<int>(t));
                rowRef.midiCh->setValue(ch);
                rowRef.midiD1->setValue(d1);
                const int d2save = (t == mcp::MidiMsgType::NoteOn || t == mcp::MidiMsgType::NoteOff)
                                   ? -1 : d2;
                rowRef.midiD2->setValue(d2save);
                capture->setText("Capture");
                capture->setEnabled(true);
            });
        });
    }

    lay->addLayout(grid);
    lay->addStretch();
    m_controlsTabs->addTab(page, "MIDI Learn");
}

void SettingsDialog::buildControlsOscTab() {
    auto* page = new QWidget;
    auto* lay  = new QVBoxLayout(page);
    lay->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    grid->addWidget(new QLabel("Action"),   0, 0);
    grid->addWidget(new QLabel("Enable"),   0, 1);
    grid->addWidget(new QLabel("OSC Path"), 0, 2);

    for (int i = 0; i < (int)m_actionRows.size(); ++i) {
        auto& r = m_actionRows[i];
        const int row = i + 1;
        grid->addWidget(new QLabel(r.label), row, 0);
        r.oscEnable = new QCheckBox;
        r.oscPath   = new QLineEdit;
        r.oscPath->setPlaceholderText("/my/go");
        grid->addWidget(r.oscEnable, row, 1, Qt::AlignHCenter);
        grid->addWidget(r.oscPath,   row, 2);
    }

    lay->addLayout(grid);
    lay->addWidget(new QLabel(
        "Note: these paths may conflict with the system vocabulary (/go, /start, /stop, …)\n"
        "but will still work — system vocabulary always takes priority."));
    lay->addStretch();
    m_controlsTabs->addTab(page, "OSC");
}

void SettingsDialog::loadControls() {
    for (auto& r : m_actionRows) {
        const auto* e = m_systemControls.find(r.action);
        if (e) {
            r.midiEnable->setChecked(e->midi.enabled);
            r.midiType->setCurrentIndex(static_cast<int>(e->midi.type));
            r.midiCh->setValue(e->midi.channel);
            r.midiD1->setValue(e->midi.data1);
            r.midiD2->setValue(e->midi.data2);
            r.oscEnable->setChecked(e->osc.enabled);
            r.oscPath->setText(QString::fromStdString(e->osc.path));
        }
    }
}

void SettingsDialog::saveControls() {
    for (const auto& r : m_actionRows) {
        auto& entry = m_systemControls.get(r.action);
        entry.midi.enabled = r.midiEnable->isChecked();
        entry.midi.type    = static_cast<mcp::MidiMsgType>(r.midiType->currentIndex());
        entry.midi.channel = r.midiCh->value();
        entry.midi.data1   = r.midiD1->value();
        entry.midi.data2   = r.midiD2->value();
        entry.osc.enabled  = r.oscEnable->isChecked();
        entry.osc.path     = r.oscPath->text().toStdString();
    }
}
