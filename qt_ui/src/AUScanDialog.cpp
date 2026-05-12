#include "AUScanDialog.h"
#include "AppModel.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#ifdef __APPLE__
#  include "engine/plugin/NativePluginBackend.h"
#  include "engine/ShowFile.h"
#endif

// ── column indices ────────────────────────────────────────────────────────────

static constexpr int kColName    = 0;
static constexpr int kColVendor  = 1;
static constexpr int kColType    = 2;
static constexpr int kColVersion = 3;
static constexpr int kColStatus  = 4;
static constexpr int kNumCols    = 5;

// Entry index stored per row in Qt::UserRole on the name item.
static constexpr int kEntryIndexRole = Qt::UserRole;

// ── construction ──────────────────────────────────────────────────────────────

AUScanDialog::AUScanDialog(QWidget* parent)
    : QDialog(parent)
{
    buildUi();
    setWindowTitle("AU Plugin Browser");
    resize(760, 500);
    onRefresh();
}

AUScanDialog::AUScanDialog(AppModel* model, int ch, int slot, QWidget* parent)
    : QDialog(parent), m_model(model), m_ch(ch), m_slot(slot)
{
    buildUi();
    setWindowTitle(QString("AU Plugins — Ch %1 / Slot %2").arg(ch + 1).arg(slot + 1));
    resize(760, 500);
    onRefresh();
}

AUScanDialog::~AUScanDialog() = default;

void AUScanDialog::buildUi() {
    setAttribute(Qt::WA_DeleteOnClose);

    auto* vl = new QVBoxLayout(this);
    vl->setSpacing(6);
    vl->setContentsMargins(8, 8, 8, 8);

    // Filter row
    auto* filterRow = new QHBoxLayout;
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText("Filter by name or vendor…");
    m_filterEdit->setClearButtonEnabled(true);
    m_refreshBtn = new QPushButton("Refresh List");
    filterRow->addWidget(m_filterEdit, 1);
    filterRow->addWidget(m_refreshBtn);
    vl->addLayout(filterRow);

    // Table
    m_table = new QTableWidget(0, kNumCols);
    m_table->setHorizontalHeaderLabels({"Name", "Vendor", "Type", "Version", "Test"});
    m_table->horizontalHeader()->setSectionResizeMode(kColName,    QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColVendor,  QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColType,    QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColVersion, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColStatus,  QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->setShowGrid(false);
    vl->addWidget(m_table, 1);

    // Progress bar (hidden while idle)
    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setMaximumHeight(8);
    m_progressBar->setTextVisible(false);
    m_progressBar->setVisible(false);
    vl->addWidget(m_progressBar);

    // Status label
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color:#888;font-size:11px;");
    vl->addWidget(m_statusLabel);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_testSelBtn   = new QPushButton("Test Selected");
    m_batchTestBtn = new QPushButton("Batch Test All");
    m_exportBtn    = new QPushButton("Export JSON…");
    m_loadBtn      = new QPushButton("Load into Slot");

    m_testSelBtn->setEnabled(false);
    m_exportBtn->setEnabled(false);
    m_loadBtn->setVisible(m_ch >= 0);
    m_loadBtn->setEnabled(false);

    btnRow->addWidget(m_testSelBtn);
    btnRow->addWidget(m_batchTestBtn);
    btnRow->addWidget(m_exportBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_loadBtn);
    vl->addLayout(btnRow);

    // Connections
    connect(m_refreshBtn,   &QPushButton::clicked,
            this, &AUScanDialog::onRefresh);
    connect(m_filterEdit,   &QLineEdit::textChanged,
            this, &AUScanDialog::onFilterChanged);
    connect(m_table,        &QTableWidget::itemSelectionChanged,
            this, &AUScanDialog::onSelectionChanged);
    connect(m_testSelBtn,   &QPushButton::clicked,
            this, &AUScanDialog::onTestSelected);
    connect(m_batchTestBtn, &QPushButton::clicked,
            this, &AUScanDialog::onBatchTest);
    connect(m_exportBtn,    &QPushButton::clicked,
            this, &AUScanDialog::onExportResults);
    connect(m_loadBtn,      &QPushButton::clicked,
            this, &AUScanDialog::onLoadIntoSlot);

    setStyleSheet(
        "QDialog{background:#1e1e1e;color:#ccc;}"
        "QTableWidget{background:#161616;color:#ccc;gridline-color:#1e1e1e;"
        "  alternate-background-color:#1a1a1a;selection-background-color:#2a6ab8;}"
        "QHeaderView::section{background:#252525;color:#999;padding:4px;"
        "  border:none;border-bottom:1px solid #333;}"
        "QLineEdit{background:#252525;color:#ccc;border:1px solid #333;padding:4px;}"
        "QPushButton{background:#2a2a2a;color:#ccc;border:1px solid #444;"
        "  padding:4px 12px;border-radius:3px;}"
        "QPushButton:hover{background:#333;}"
        "QPushButton:disabled{color:#555;border-color:#333;}"
        "QProgressBar{border:1px solid #333;border-radius:2px;background:#222;}"
        "QProgressBar::chunk{background:#2a6ab8;border-radius:2px;}"
    );
}

// ── close guard ───────────────────────────────────────────────────────────────

void AUScanDialog::closeEvent(QCloseEvent* e) {
    if (m_testingInProgress) {
        e->ignore();
        return;
    }
    QDialog::closeEvent(e);
}

// ── refresh / populate ────────────────────────────────────────────────────────

void AUScanDialog::onRefresh() {
#ifdef __APPLE__
    m_statusLabel->setText("Enumerating AU components…");
    QApplication::processEvents();

    m_entries = mcp::plugin::AUComponentEnumerator::enumerate();
    m_results.assign(m_entries.size(), {});

    populateTable();
    applyFilter();
    m_exportBtn->setEnabled(false);
    m_statusLabel->setText(
        QString("Found %1 AU plugins (Effect + Music Effect).")
            .arg(static_cast<int>(m_entries.size())));
#else
    m_statusLabel->setText("AU plugin browsing requires macOS.");
#endif
}

void AUScanDialog::populateTable() {
#ifdef __APPLE__
    m_table->setRowCount(0);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto* nameItem = new QTableWidgetItem(QString::fromStdString(e.name));
        nameItem->setData(kEntryIndexRole, static_cast<int>(i));
        m_table->setItem(row, kColName,    nameItem);
        m_table->setItem(row, kColVendor,  new QTableWidgetItem(QString::fromStdString(e.manufacturerName)));
        m_table->setItem(row, kColType,    new QTableWidgetItem(QString::fromStdString(e.typeLabel)));
        m_table->setItem(row, kColVersion, new QTableWidgetItem(QString::fromStdString(e.version)));

        auto* statusItem = new QTableWidgetItem("—");
        statusItem->setForeground(QColor(0x55, 0x55, 0x55));
        m_table->setItem(row, kColStatus, statusItem);
    }
#endif
}

void AUScanDialog::applyFilter() {
    const QString f = m_filterEdit->text().trimmed().toLower();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        bool show = true;
        if (!f.isEmpty()) {
            const QString name   = m_table->item(row, kColName)  ->text().toLower();
            const QString vendor = m_table->item(row, kColVendor)->text().toLower();
            show = name.contains(f) || vendor.contains(f);
        }
        m_table->setRowHidden(row, !show);
    }
}

void AUScanDialog::onFilterChanged(const QString&) {
    applyFilter();
}

void AUScanDialog::onSelectionChanged() {
    const bool sel  = !m_table->selectedItems().isEmpty();
    const bool idle = !m_testingInProgress;
    m_testSelBtn->setEnabled(sel && idle);
    if (m_ch >= 0)
        m_loadBtn->setEnabled(sel && idle);
}

// ── test selected ──────────────────────────────────────────────────────────────

void AUScanDialog::onTestSelected() {
#ifdef __APPLE__
    const auto sel = m_table->selectedItems();
    if (sel.isEmpty()) return;
    const int row = m_table->row(sel.first());
    const int idx = m_table->item(row, kColName)->data(kEntryIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_entries.size())) return;

    m_statusLabel->setText(QString("Testing %1…")
        .arg(QString::fromStdString(m_entries[static_cast<size_t>(idx)].name)));
    QApplication::processEvents();

    const auto res = mcp::plugin::AUCompatibilityTester::testIsolated(
        m_entries[static_cast<size_t>(idx)]);
    m_results[static_cast<size_t>(idx)] = res;

    // Auto-seed dangerous list so the next editor open uses polling instead.
    if (res.status == mcp::plugin::AUTestResult::Status::Crashed && m_model)
        m_model->dangerousList.add(m_entries[static_cast<size_t>(idx)].manufacturerName);

    updateRowStatus(row, idx);
    m_exportBtn->setEnabled(true);

    using S = mcp::plugin::AUTestResult::Status;
    if (res.status == S::Ok) {
        m_statusLabel->setText(
            QString("✓  %1 — OK  (%2 params, latency %3 samp, tail %4 samp)")
                .arg(QString::fromStdString(res.pluginName))
                .arg(res.parameterCount)
                .arg(res.latencySamples)
                .arg(res.tailSamples));
    } else {
        m_statusLabel->setText(
            QString("✗  %1 — %2: %3")
                .arg(QString::fromStdString(res.pluginName))
                .arg(mcp::plugin::AUTestResult::statusLabel(res.status))
                .arg(QString::fromStdString(res.errorMessage)));
    }
#endif
}

// ── update one row ────────────────────────────────────────────────────────────

#ifdef __APPLE__
void AUScanDialog::updateRowStatus(int row, int idx) {
    const auto& res = m_results[static_cast<size_t>(idx)];
    if (res.pluginId.empty()) return;  // not yet tested

    using S = mcp::plugin::AUTestResult::Status;
    auto* si = m_table->item(row, kColStatus);
    if (!si) { si = new QTableWidgetItem; m_table->setItem(row, kColStatus, si); }
    si->setText(mcp::plugin::AUTestResult::statusLabel(res.status));
    si->setForeground(res.status == S::Ok ? QColor(0x55, 0xAA, 0x55) : QColor(0xCC, 0x55, 0x55));

    if (auto* ni = m_table->item(row, kColName)) {
        if (res.status == S::Ok)
            ni->setToolTip(QString("%1 params, latency %2 samp, tail %3 samp%4")
                .arg(res.parameterCount).arg(res.latencySamples).arg(res.tailSamples)
                .arg(res.warnings.empty() ? "" :
                     QString("\nWarnings: %1").arg(
                         QString::fromStdString(res.warnings[0]))));
        else
            ni->setToolTip(QString::fromStdString(res.errorMessage));
    }
}
#endif

// ── batch test ────────────────────────────────────────────────────────────────
//
// All AU operations run on the main thread — one plugin per event-loop tick.
// This is required because many AU plugins (e.g. UAD) post callbacks back to
// the main thread via their internal JUCE message queue; instantiating and
// destroying them on a worker thread creates a race between the worker's
// AudioComponentInstanceDispose call and those pending main-thread callbacks,
// resulting in a null-pointer crash inside the plugin's cleanup code.

void AUScanDialog::onBatchTest() {
#ifdef __APPLE__
    if (m_testingInProgress || m_entries.empty()) return;

    m_testingInProgress = true;
    m_batchIndex = 0;
    m_batchTestBtn->setEnabled(false);
    m_testSelBtn->setEnabled(false);
    m_loadBtn->setEnabled(false);
    m_refreshBtn->setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(true);

    m_results.assign(m_entries.size(), {});
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto* si = m_table->item(row, kColStatus)) {
            si->setText("—");
            si->setForeground(QColor(0x55, 0x55, 0x55));
        }
    }

    QMetaObject::invokeMethod(this, &AUScanDialog::onBatchStep, Qt::QueuedConnection);
#else
    m_statusLabel->setText("AU plugins require macOS.");
#endif
}

void AUScanDialog::onBatchStep() {
#ifdef __APPLE__
    if (!m_testingInProgress) return;

    const int total = static_cast<int>(m_entries.size());
    if (m_batchIndex >= total) {
        onBatchComplete();
        return;
    }

    // Run test in an isolated subprocess so a crashing/hanging plugin cannot
    // take down the host.  The subprocess is the same binary with --au-test-plugin.
    const auto res = mcp::plugin::AUCompatibilityTester::testIsolated(
        m_entries[static_cast<size_t>(m_batchIndex)]);
    m_results[static_cast<size_t>(m_batchIndex)] = res;

    // Auto-seed dangerous list for vendors whose plugins crash under testing.
    if (res.status == mcp::plugin::AUTestResult::Status::Crashed && m_model)
        m_model->dangerousList.add(m_entries[static_cast<size_t>(m_batchIndex)].manufacturerName);

    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto* ni = m_table->item(row, kColName);
            ni && ni->data(kEntryIndexRole).toInt() == m_batchIndex) {
            updateRowStatus(row, m_batchIndex);
            break;
        }
    }

    m_progressBar->setValue((m_batchIndex + 1) * 100 / total);
    m_statusLabel->setText(
        QString("Testing %1/%2: %3…")
            .arg(m_batchIndex + 1).arg(total)
            .arg(QString::fromStdString(res.pluginName)));

    ++m_batchIndex;

    // Yield to the event loop before the next test so UI updates are painted
    // and any pending main-thread callbacks from the previous plugin are drained.
    QMetaObject::invokeMethod(this, &AUScanDialog::onBatchStep, Qt::QueuedConnection);
#endif
}

void AUScanDialog::onBatchComplete() {
#ifdef __APPLE__
    m_testingInProgress = false;
    m_progressBar->setVisible(false);
    m_batchTestBtn->setEnabled(true);
    m_refreshBtn->setEnabled(true);
    m_exportBtn->setEnabled(true);
    onSelectionChanged();  // re-evaluate test/load buttons

    int okCount = 0;
    for (const auto& r : m_results)
        if (!r.pluginId.empty() &&
            r.status == mcp::plugin::AUTestResult::Status::Ok) ++okCount;

    m_statusLabel->setText(
        QString("Batch test complete: %1 / %2 passed.")
            .arg(okCount)
            .arg(static_cast<int>(m_entries.size())));
#endif
}

// ── export ────────────────────────────────────────────────────────────────────

void AUScanDialog::onExportResults() {
#ifdef __APPLE__
    std::vector<mcp::plugin::AUTestResult> tested;
    for (const auto& r : m_results)
        if (!r.pluginId.empty()) tested.push_back(r);

    if (tested.empty()) {
        QMessageBox::information(this, "Export Results",
            "No test results to export yet.\nRun \"Test Selected\" or \"Batch Test All\" first.");
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, "Export AU Compatibility Results",
        "au_compatibility.json", "JSON (*.json)");
    if (path.isEmpty()) return;

    const std::string json = mcp::plugin::AUCompatibilityTester::toJson(tested);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text) ||
        f.write(json.c_str(), static_cast<qint64>(json.size())) < 0)
        QMessageBox::warning(this, "Export Failed",
            "Could not write: " + path);
#endif
}

// ── load into slot ────────────────────────────────────────────────────────────

void AUScanDialog::onLoadIntoSlot() {
#ifdef __APPLE__
    if (!m_model || m_ch < 0 || m_slot < 0) return;

    const auto sel = m_table->selectedItems();
    if (sel.isEmpty()) return;
    const int row = m_table->row(sel.first());
    const int idx = m_table->item(row, kColName)->data(kEntryIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_entries.size())) return;

    const auto& entry = m_entries[static_cast<size_t>(idx)];
    auto& as = m_model->sf.audioSetup;
    if (m_ch >= static_cast<int>(as.channels.size())) return;

    const bool isStereo = as.channels[static_cast<size_t>(m_ch)].linkedStereo
                          && (m_ch + 1 < static_cast<int>(as.channels.size()));
    const int nCh = isStereo ? 2 : 1;

    // Build ExternalPluginReference (probes the component for display name)
    const auto ref = mcp::plugin::NativePluginBackend::makeAUReference(
        entry.type, entry.subtype, entry.manufacturer, nCh);

    auto& pSlots = as.channels[static_cast<size_t>(m_ch)].plugins;
    while (static_cast<int>(pSlots.size()) <= m_slot)
        pSlots.emplace_back();

    auto& sl = pSlots[static_cast<size_t>(m_slot)];
    sl.pluginId       = ref.pluginId;
    sl.extBackend     = ref.backend;
    sl.extName        = ref.name;
    sl.extVendor      = ref.vendor;
    sl.extVersion     = ref.version;
    sl.extNumChannels = nCh;
    sl.extStateBlob   = {};
    sl.extParamSnapshot = {};
    sl.parameters     = {};
    sl.bypassed       = false;
    sl.disabled       = false;
    sl.loadFailCount  = 0;

    m_model->markDirty();
    m_model->buildChannelPluginChains();
    m_model->applyMixing();
    accept();
#endif
}
