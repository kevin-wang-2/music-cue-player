#include "AudioSetupDialog.h"
#include "AppModel.h"
#include "FaderWidget.h"

#include <cmath>
#include <QCheckBox>
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
    "QDoubleSpinBox{background:#252525;color:#ddd;border:1px solid #444;"
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
    setMinimumSize(500, 400);
    setStyleSheet(kDarkStyle);

    m_numPhys = model->engineOk ? model->engine.channels() : 2;
    m_setup   = model->sf.audioSetup;

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
    vlay->setSpacing(10);

    m_tabs = new QTabWidget(this);
    vlay->addWidget(m_tabs, 1);

    buildChannelsTab();
    buildXpTab();

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText("Apply");
    btns->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        syncChannelsFromTable();
        syncXpFromGrid();
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    vlay->addWidget(btns);
}

// ── Tab 1 — Channels ───────────────────────────────────────────────────────

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

    // Table: Name | Linked Stereo | Master Gain (dB) | Mute
    m_chanTable = new QTableWidget(0, 4, page);
    m_chanTable->setHorizontalHeaderLabels({"Name", "Stereo Link", "Master (dB)", "Mute"});
    m_chanTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_chanTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_chanTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
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
    m_chanTable->setRowCount(0);
    const int n = static_cast<int>(m_setup.channels.size());

    // First pass: insert all rows
    for (int i = 0; i < n; ++i)
        m_chanTable->insertRow(i);

    // Second pass: populate rows
    for (int i = 0; i < n; ++i) {
        const auto& ch = m_setup.channels[static_cast<size_t>(i)];

        // Determine if this row is a slave (previous channel linked to it)
        const bool isSlave = (i > 0 && m_setup.channels[static_cast<size_t>(i - 1)].linkedStereo);
        // Determine if this row is a master that links to the next
        const bool isMaster = ch.linkedStereo && (i + 1 < n);

        m_chanTable->setVerticalHeaderItem(
            i, new QTableWidgetItem(isSlave ? QString("   └ %1").arg(i + 1)
                                            : QString::number(i + 1)));

        // Name
        auto* nameEdit = new QLineEdit(QString::fromStdString(ch.name));
        nameEdit->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 4px;");
        m_chanTable->setCellWidget(i, 0, nameEdit);

        // Stereo Link checkbox (disabled for slave rows)
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
            m_chanTable->setCellWidget(i, 1, stereoCell);
            connect(stereoChk, &QCheckBox::toggled, this, [this](bool) {
                syncChannelsFromTable();
                rebuildChannelsTable();
                rebuildXpGrid();
            });
        }
        // slave row: leave cell 1 empty (cosmetically part of the master row)

        // Master Gain and Mute: only on non-slave rows
        if (!isSlave) {
            auto* gainSpin = new QDoubleSpinBox;
            gainSpin->setRange(-60.0, 12.0);
            gainSpin->setDecimals(1);
            gainSpin->setSuffix(" dB");
            gainSpin->setSingleStep(0.5);
            gainSpin->setValue(static_cast<double>(ch.masterGainDb));
            gainSpin->setStyleSheet("background:#252525;color:#ddd;border:none;padding:1px 2px;");
            m_chanTable->setCellWidget(i, 2, gainSpin);

            auto* muteChk = new QCheckBox;
            muteChk->setChecked(ch.mute);
            muteChk->setStyleSheet("QCheckBox::indicator:checked{background:#c04040;border-color:#c04040;}");
            auto* muteCell = new QWidget;
            auto* muteLay = new QHBoxLayout(muteCell);
            muteLay->setAlignment(Qt::AlignCenter);
            muteLay->setContentsMargins(0, 0, 0, 0);
            muteLay->addWidget(muteChk);
            m_chanTable->setCellWidget(i, 3, muteCell);

            // If master, span gain and mute cells over two rows
            if (isMaster) {
                m_chanTable->setSpan(i, 2, 2, 1);
                m_chanTable->setSpan(i, 3, 2, 1);
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
    // Remove xp entries referencing the last channel
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
    const int rows = m_chanTable->rowCount();
    m_setup.channels.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        auto& c = m_setup.channels[static_cast<size_t>(i)];
        const bool isSlave = (i > 0 && m_setup.channels[static_cast<size_t>(i - 1)].linkedStereo);

        if (auto* w = qobject_cast<QLineEdit*>(m_chanTable->cellWidget(i, 0)))
            c.name = w->text().toStdString();

        if (!isSlave) {
            if (auto* cell = m_chanTable->cellWidget(i, 1))
                if (auto* chk = cell->findChild<QCheckBox*>())
                    c.linkedStereo = chk->isChecked();
            if (auto* spin = qobject_cast<QDoubleSpinBox*>(m_chanTable->cellWidget(i, 2)))
                c.masterGainDb = static_cast<float>(spin->value());
            if (auto* cell = m_chanTable->cellWidget(i, 3))
                if (auto* chk = cell->findChild<QCheckBox*>())
                    c.mute = chk->isChecked();
        } else {
            // Slave inherits gain and mute from master
            const auto& master = m_setup.channels[static_cast<size_t>(i - 1)];
            c.linkedStereo = false;
            c.masterGainDb = master.masterGainDb;
            c.mute         = master.mute;
        }
    }
}

// ── Tab 2 — Crosspoint ────────────────────────────────────────────────────

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
    // Delete old content
    delete m_xpContent;
    m_xpCells.clear();

    const int numCh   = static_cast<int>(m_setup.channels.size());
    const int numPhys = m_numPhys;

    m_xpContent = new QWidget;
    auto* outer = new QVBoxLayout(m_xpContent);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);

    if (numCh == 0 || numPhys == 0) {
        outer->addWidget(new QLabel("No channels or outputs configured."));
        m_xpScroll->setWidget(m_xpContent);
        return;
    }

    // Build lookup: (ch, out) → db from m_setup.xpEntries
    auto findXp = [&](int ch, int out) -> std::optional<float> {
        for (const auto& xe : m_setup.xpEntries)
            if (xe.ch == ch && xe.out == out)
                return xe.db;
        return std::nullopt;
    };

    auto* xpGroup = new QGroupBox("Channel → Physical Output");
    auto* grid = new QGridLayout(xpGroup);
    grid->setSpacing(3);
    grid->setContentsMargins(8, 14, 8, 8);

    constexpr int kCellW = 50;
    constexpr int kLblW  = 70;
    constexpr int kSp    = 3;
    xpGroup->setFixedWidth(kLblW + numPhys * (kCellW + kSp) + 20);

    // Column headers: physical output indices
    for (int p = 0; p < numPhys; ++p) {
        auto* lbl = new QLabel(QString("Out %1").arg(p + 1), xpGroup);
        lbl->setFixedWidth(kCellW);
        lbl->setAlignment(Qt::AlignHCenter);
        lbl->setStyleSheet("color:#888;font-size:10px;");
        grid->addWidget(lbl, 0, p + 1);
    }

    m_xpCells.resize(static_cast<size_t>(numCh),
                     std::vector<QLineEdit*>(static_cast<size_t>(numPhys), nullptr));

    for (int ch = 0; ch < numCh; ++ch) {
        // Row label: channel name
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
            cell->setStyleSheet(
                "QLineEdit{background:#1a1a1a;color:#ddd;border:1px solid #383838;"
                "  border-radius:2px;padding:1px 3px;font-size:11px;}"
                "QLineEdit:focus{border-color:#2a6ab8;}");

            // Determine display: explicit entry or diagonal default
            auto xpVal = findXp(ch, p);
            if (xpVal.has_value()) {
                cell->setText(fmtDb(*xpVal));
            } else if (ch == p) {
                // Default diagonal = 0 dB, shown as placeholder
                cell->setPlaceholderText("0.0");
            }
            // Off-diagonal default = off, shown as placeholder
            if (ch != p && !xpVal.has_value())
                cell->setPlaceholderText("-inf");

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
            if (txt.isEmpty()) continue;  // use default (diagonal=0, else off)
            float db = parseDb(txt, ch == p ? 0.0f : kInfDb);
            // Only store non-default values
            bool isDefault = (ch == p) ? (std::abs(db) < 0.001f) : (db <= kInfDb);
            if (!isDefault)
                m_setup.xpEntries.push_back({ch, p, db});
        }
    }
}
