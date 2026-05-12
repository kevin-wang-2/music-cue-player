#include "PluginManagerDialog.h"
#include "AppModel.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QCoreApplication>
#include <QProcess>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <filesystem>

#ifdef __APPLE__
#  include "engine/plugin/NativePluginBackend.h"
#  include "engine/ShowFile.h"
#endif
#ifdef MCP_HAVE_VST3
#  include "engine/plugin/VST3Scanner.h"
#endif

// ── Async VST3 scan worker ────────────────────────────────────────────────────
#ifdef MCP_HAVE_VST3
class PluginManagerDialog::VST3ScanWorker : public QObject {
    Q_OBJECT
public:
    explicit VST3ScanWorker(std::vector<std::string> bundles, QObject* parent = nullptr)
        : QObject(parent), m_bundles(std::move(bundles)) {}
    void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }
    std::vector<mcp::plugin::VST3Entry> takeResults() { return std::move(m_results); }
public slots:
    void run() {
        const int total = static_cast<int>(m_bundles.size());
        const QString appPath = QCoreApplication::applicationFilePath();
        for (int i = 0; i < total; ++i) {
            if (m_cancelled.load(std::memory_order_relaxed)) break;
            const std::string& bundlePath = m_bundles[static_cast<size_t>(i)];
            const QString name = QString::fromStdString(
                std::filesystem::path(bundlePath).stem().string());
            emit progress(i + 1, total, name);

            // Scan in an isolated child process — if the plugin's static
            // initialiser throws or crashes during dlopen, only the child
            // dies; the main app continues to the next bundle.
            QProcess proc;
            proc.start(appPath, {"--scan-vst3", QString::fromStdString(bundlePath)});
            const bool finished = proc.waitForFinished(10000); // 10-second timeout
            if (!finished) {
                proc.kill();
                proc.waitForFinished(2000);
                continue; // timed-out (e.g. license dialog) — skip
            }
            if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
                continue; // crashed or returned error — skip

            const QByteArray out = proc.readAllStandardOutput();
            for (const QByteArray& line : out.split('\n')) {
                if (line.isEmpty()) continue;
                const QList<QByteArray> fields = line.split('\t');
                if (fields.size() < 5) continue;
                mcp::plugin::VST3Entry e;
                e.name       = QString::fromUtf8(fields[0]).toStdString();
                e.vendor     = QString::fromUtf8(fields[1]).toStdString();
                e.version    = QString::fromUtf8(fields[2]).toStdString();
                e.pluginId   = QString::fromUtf8(fields[3]).toStdString();
                e.classIndex = QString::fromUtf8(fields[4]).trimmed().toInt();
                e.path       = bundlePath;
                m_results.push_back(std::move(e));
            }
        }
        emit finished();
    }
signals:
    void progress(int cur, int total, const QString& bundleName);
    void finished();
private:
    std::vector<std::string> m_bundles;
    std::vector<mcp::plugin::VST3Entry> m_results;
    std::atomic<bool> m_cancelled{false};
};
#endif // MCP_HAVE_VST3

static const QString kStyleSheet =
    "QDialog{background:#1e1e1e;color:#ccc;}"
    "QTabWidget::pane{border:1px solid #333;background:#1e1e1e;}"
    "QTabBar::tab{background:#252525;color:#999;padding:5px 14px;"
    "  border:1px solid #333;border-bottom:none;border-radius:0;}"
    "QTabBar::tab:selected{background:#1e1e1e;color:#ddd;}"
    "QTableWidget{background:#161616;color:#ccc;gridline-color:#1e1e1e;"
    "  alternate-background-color:#1a1a1a;selection-background-color:#2a6ab8;}"
    "QHeaderView::section{background:#252525;color:#999;padding:4px;"
    "  border:none;border-bottom:1px solid #333;}"
    "QLineEdit{background:#252525;color:#ccc;border:1px solid #333;padding:4px;}"
    "QListWidget{background:#1a1a1a;color:#ccc;border:1px solid #333;}"
    "QPushButton{background:#2a2a2a;color:#ccc;border:1px solid #444;"
    "  padding:4px 12px;border-radius:3px;}"
    "QPushButton:hover{background:#333;}"
    "QPushButton:disabled{color:#555;border-color:#333;}"
    "QProgressBar{border:1px solid #333;border-radius:2px;background:#222;}"
    "QProgressBar::chunk{background:#2a6ab8;border-radius:2px;}";

// ── AU column indices ─────────────────────────────────────────────────────────
static constexpr int kAUColName    = 0;
static constexpr int kAUColVendor  = 1;
static constexpr int kAUColType    = 2;
static constexpr int kAUColVersion = 3;
static constexpr int kAUColStatus  = 4;
static constexpr int kAUNumCols    = 5;

// VST3 column indices
static constexpr int kV3ColName    = 0;
static constexpr int kV3ColVendor  = 1;
static constexpr int kV3ColVersion = 2;
static constexpr int kV3ColPath    = 3;
static constexpr int kV3NumCols    = 4;

static constexpr int kEntryIndexRole = Qt::UserRole;

// ── construction ──────────────────────────────────────────────────────────────

PluginManagerDialog::PluginManagerDialog(QWidget* parent)
    : QDialog(parent)
{
    buildUi();
    setWindowTitle("Plugin Manager");
    resize(820, 560);
    onAURefresh();
}

PluginManagerDialog::PluginManagerDialog(AppModel* model, int ch, int slot,
                                          QWidget* parent)
    : QDialog(parent), m_model(model), m_ch(ch), m_slot(slot)
{
    buildUi();
    setWindowTitle(QString("Plugin Manager — Ch %1 / Slot %2")
                   .arg(ch + 1).arg(slot + 1));
    resize(820, 560);
    onAURefresh();
}

PluginManagerDialog::~PluginManagerDialog() = default;

// ── main UI ───────────────────────────────────────────────────────────────────

void PluginManagerDialog::buildUi() {
    setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet(kStyleSheet);

    auto* vl = new QVBoxLayout(this);
    vl->setSpacing(0);
    vl->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget;
    vl->addWidget(m_tabs, 1);

    buildAUTab();
    buildVST3Tab();
}

// ──────────────────────────────────────────────────────────────────────────────
// AU TAB
// ──────────────────────────────────────────────────────────────────────────────

void PluginManagerDialog::buildAUTab() {
    auto* w  = new QWidget;
    auto* vl = new QVBoxLayout(w);
    vl->setSpacing(6);
    vl->setContentsMargins(8, 8, 8, 8);

    auto* filterRow = new QHBoxLayout;
    m_auFilter = new QLineEdit;
    m_auFilter->setPlaceholderText("Filter by name or vendor…");
    m_auFilter->setClearButtonEnabled(true);
    m_auRefreshBtn = new QPushButton("Refresh List");
    filterRow->addWidget(m_auFilter, 1);
    filterRow->addWidget(m_auRefreshBtn);
    vl->addLayout(filterRow);

    m_auTable = new QTableWidget(0, kAUNumCols);
    m_auTable->setHorizontalHeaderLabels({"Name", "Vendor", "Type", "Version", "Test"});
    m_auTable->horizontalHeader()->setSectionResizeMode(kAUColName,    QHeaderView::Stretch);
    m_auTable->horizontalHeader()->setSectionResizeMode(kAUColVendor,  QHeaderView::ResizeToContents);
    m_auTable->horizontalHeader()->setSectionResizeMode(kAUColType,    QHeaderView::ResizeToContents);
    m_auTable->horizontalHeader()->setSectionResizeMode(kAUColVersion, QHeaderView::ResizeToContents);
    m_auTable->horizontalHeader()->setSectionResizeMode(kAUColStatus,  QHeaderView::ResizeToContents);
    m_auTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_auTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_auTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_auTable->setAlternatingRowColors(true);
    m_auTable->verticalHeader()->hide();
    m_auTable->setShowGrid(false);
    vl->addWidget(m_auTable, 1);

    m_auProgress = new QProgressBar;
    m_auProgress->setRange(0, 100);
    m_auProgress->setMaximumHeight(8);
    m_auProgress->setTextVisible(false);
    m_auProgress->setVisible(false);
    vl->addWidget(m_auProgress);

    m_auStatus = new QLabel;
    m_auStatus->setStyleSheet("color:#888;font-size:11px;");
    vl->addWidget(m_auStatus);

    auto* btnRow = new QHBoxLayout;
    m_auTestSelBtn   = new QPushButton("Test Selected");
    m_auBatchTestBtn = new QPushButton("Batch Test All");
    m_auExportBtn    = new QPushButton("Export JSON…");
    m_auLoadBtn      = new QPushButton("Load into Slot");

    m_auTestSelBtn->setEnabled(false);
    m_auExportBtn->setEnabled(false);
    m_auLoadBtn->setVisible(m_ch >= 0);
    m_auLoadBtn->setEnabled(false);

    btnRow->addWidget(m_auTestSelBtn);
    btnRow->addWidget(m_auBatchTestBtn);
    btnRow->addWidget(m_auExportBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_auLoadBtn);
    vl->addLayout(btnRow);

    connect(m_auRefreshBtn,   &QPushButton::clicked,
            this, &PluginManagerDialog::onAURefresh);
    connect(m_auFilter,       &QLineEdit::textChanged,
            this, &PluginManagerDialog::onAUFilterChanged);
    connect(m_auTable,        &QTableWidget::itemSelectionChanged,
            this, &PluginManagerDialog::onAUSelectionChanged);
    connect(m_auTestSelBtn,   &QPushButton::clicked,
            this, &PluginManagerDialog::onAUTestSelected);
    connect(m_auBatchTestBtn, &QPushButton::clicked,
            this, &PluginManagerDialog::onAUBatchTest);
    connect(m_auExportBtn,    &QPushButton::clicked,
            this, &PluginManagerDialog::onAUExportResults);
    connect(m_auLoadBtn,      &QPushButton::clicked,
            this, &PluginManagerDialog::onAULoadIntoSlot);

    m_tabs->addTab(w, "AU Plugins");
}

void PluginManagerDialog::closeEvent(QCloseEvent* e) {
    if (m_testingInProgress) { e->ignore(); return; }
    if (m_vst3Scanning) {
        m_pendingClose = true;
#ifdef MCP_HAVE_VST3
        if (m_vst3ScanWorker) m_vst3ScanWorker->cancel();
#endif
        m_vst3CancelBtn->setEnabled(false);
        m_vst3Status->setText("Cancelling…");
        e->ignore();
        return;
    }
    QDialog::closeEvent(e);
}

// ── AU: refresh / populate ────────────────────────────────────────────────────

void PluginManagerDialog::onAURefresh() {
#ifdef __APPLE__
    m_auStatus->setText("Enumerating AU components…");
    QApplication::processEvents();
    m_auEntries = mcp::plugin::AUComponentEnumerator::enumerate();
    m_auResults.assign(m_auEntries.size(), {});
    auPopulateTable();
    auApplyFilter();
    m_auExportBtn->setEnabled(false);
    m_auStatus->setText(
        QString("Found %1 AU plugins.")
            .arg(static_cast<int>(m_auEntries.size())));
#else
    m_auStatus->setText("AU plugin browsing requires macOS.");
#endif
}

void PluginManagerDialog::auPopulateTable() {
#ifdef __APPLE__
    m_auTable->setRowCount(0);
    for (size_t i = 0; i < m_auEntries.size(); ++i) {
        const auto& e = m_auEntries[i];
        const int row = m_auTable->rowCount();
        m_auTable->insertRow(row);
        auto* ni = new QTableWidgetItem(QString::fromStdString(e.name));
        ni->setData(kEntryIndexRole, static_cast<int>(i));
        m_auTable->setItem(row, kAUColName,    ni);
        m_auTable->setItem(row, kAUColVendor,  new QTableWidgetItem(QString::fromStdString(e.manufacturerName)));
        m_auTable->setItem(row, kAUColType,    new QTableWidgetItem(QString::fromStdString(e.typeLabel)));
        m_auTable->setItem(row, kAUColVersion, new QTableWidgetItem(QString::fromStdString(e.version)));
        auto* si = new QTableWidgetItem("—");
        si->setForeground(QColor(0x55, 0x55, 0x55));
        m_auTable->setItem(row, kAUColStatus, si);
    }
#endif
}

void PluginManagerDialog::auApplyFilter() {
    const QString f = m_auFilter->text().trimmed().toLower();
    for (int row = 0; row < m_auTable->rowCount(); ++row) {
        bool show = f.isEmpty() ||
            m_auTable->item(row, kAUColName)  ->text().toLower().contains(f) ||
            m_auTable->item(row, kAUColVendor)->text().toLower().contains(f);
        m_auTable->setRowHidden(row, !show);
    }
}

void PluginManagerDialog::onAUFilterChanged(const QString&) { auApplyFilter(); }

void PluginManagerDialog::onAUSelectionChanged() {
    const bool sel  = !m_auTable->selectedItems().isEmpty();
    const bool idle = !m_testingInProgress;
    m_auTestSelBtn->setEnabled(sel && idle);
    if (m_ch >= 0) m_auLoadBtn->setEnabled(sel && idle);
}

// ── AU: test selected ─────────────────────────────────────────────────────────

void PluginManagerDialog::onAUTestSelected() {
#ifdef __APPLE__
    const auto sel = m_auTable->selectedItems();
    if (sel.isEmpty()) return;
    const int row = m_auTable->row(sel.first());
    const int idx = m_auTable->item(row, kAUColName)->data(kEntryIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_auEntries.size())) return;

    m_auStatus->setText(
        QString("Testing %1…")
            .arg(QString::fromStdString(m_auEntries[static_cast<size_t>(idx)].name)));
    QApplication::processEvents();

    const auto res = mcp::plugin::AUCompatibilityTester::testIsolated(
        m_auEntries[static_cast<size_t>(idx)]);
    m_auResults[static_cast<size_t>(idx)] = res;

    if (res.status == mcp::plugin::AUTestResult::Status::Crashed && m_model)
        m_model->dangerousList.add(m_auEntries[static_cast<size_t>(idx)].manufacturerName);

    auUpdateRowStatus(row, idx);
    m_auExportBtn->setEnabled(true);

    using S = mcp::plugin::AUTestResult::Status;
    if (res.status == S::Ok)
        m_auStatus->setText(
            QString("✓  %1 — OK  (%2 params, latency %3 samp, tail %4 samp)")
                .arg(QString::fromStdString(res.pluginName))
                .arg(res.parameterCount).arg(res.latencySamples).arg(res.tailSamples));
    else
        m_auStatus->setText(
            QString("✗  %1 — %2: %3")
                .arg(QString::fromStdString(res.pluginName))
                .arg(mcp::plugin::AUTestResult::statusLabel(res.status))
                .arg(QString::fromStdString(res.errorMessage)));
#endif
}

#ifdef __APPLE__
void PluginManagerDialog::auUpdateRowStatus(int row, int idx) {
    const auto& res = m_auResults[static_cast<size_t>(idx)];
    if (res.pluginId.empty()) return;
    using S = mcp::plugin::AUTestResult::Status;
    auto* si = m_auTable->item(row, kAUColStatus);
    if (!si) { si = new QTableWidgetItem; m_auTable->setItem(row, kAUColStatus, si); }
    si->setText(mcp::plugin::AUTestResult::statusLabel(res.status));
    si->setForeground(res.status == S::Ok ? QColor(0x55,0xAA,0x55) : QColor(0xCC,0x55,0x55));
    if (auto* ni = m_auTable->item(row, kAUColName)) {
        if (res.status == S::Ok)
            ni->setToolTip(
                QString("%1 params, latency %2 samp, tail %3 samp%4")
                    .arg(res.parameterCount).arg(res.latencySamples).arg(res.tailSamples)
                    .arg(res.warnings.empty() ? "" :
                         QString("\nWarnings: %1").arg(
                             QString::fromStdString(res.warnings[0]))));
        else
            ni->setToolTip(QString::fromStdString(res.errorMessage));
    }
}
#endif

// ── AU: batch test ────────────────────────────────────────────────────────────

void PluginManagerDialog::onAUBatchTest() {
#ifdef __APPLE__
    if (m_testingInProgress || m_auEntries.empty()) return;
    m_testingInProgress = true;
    m_auBatchIndex = 0;
    m_auBatchTestBtn->setEnabled(false);
    m_auTestSelBtn->setEnabled(false);
    m_auLoadBtn->setEnabled(false);
    m_auRefreshBtn->setEnabled(false);
    m_auProgress->setValue(0);
    m_auProgress->setVisible(true);
    m_auResults.assign(m_auEntries.size(), {});
    for (int row = 0; row < m_auTable->rowCount(); ++row) {
        if (auto* si = m_auTable->item(row, kAUColStatus)) {
            si->setText("—");
            si->setForeground(QColor(0x55, 0x55, 0x55));
        }
    }
    QMetaObject::invokeMethod(this, &PluginManagerDialog::onAUBatchStep,
                               Qt::QueuedConnection);
#else
    m_auStatus->setText("AU plugins require macOS.");
#endif
}

void PluginManagerDialog::onAUBatchStep() {
#ifdef __APPLE__
    if (!m_testingInProgress) return;
    const int total = static_cast<int>(m_auEntries.size());
    if (m_auBatchIndex >= total) { onAUBatchComplete(); return; }

    const auto res = mcp::plugin::AUCompatibilityTester::testIsolated(
        m_auEntries[static_cast<size_t>(m_auBatchIndex)]);
    m_auResults[static_cast<size_t>(m_auBatchIndex)] = res;

    if (res.status == mcp::plugin::AUTestResult::Status::Crashed && m_model)
        m_model->dangerousList.add(
            m_auEntries[static_cast<size_t>(m_auBatchIndex)].manufacturerName);

    for (int row = 0; row < m_auTable->rowCount(); ++row) {
        if (auto* ni = m_auTable->item(row, kAUColName);
            ni && ni->data(kEntryIndexRole).toInt() == m_auBatchIndex)
        { auUpdateRowStatus(row, m_auBatchIndex); break; }
    }

    m_auProgress->setValue((m_auBatchIndex + 1) * 100 / total);
    m_auStatus->setText(
        QString("Testing %1/%2: %3…")
            .arg(m_auBatchIndex + 1).arg(total)
            .arg(QString::fromStdString(res.pluginName)));
    ++m_auBatchIndex;
    QMetaObject::invokeMethod(this, &PluginManagerDialog::onAUBatchStep,
                               Qt::QueuedConnection);
#endif
}

void PluginManagerDialog::onAUBatchComplete() {
#ifdef __APPLE__
    m_testingInProgress = false;
    m_auProgress->setVisible(false);
    m_auBatchTestBtn->setEnabled(true);
    m_auRefreshBtn->setEnabled(true);
    m_auExportBtn->setEnabled(true);
    onAUSelectionChanged();
    int ok = 0;
    for (const auto& r : m_auResults)
        if (!r.pluginId.empty() && r.status == mcp::plugin::AUTestResult::Status::Ok) ++ok;
    m_auStatus->setText(
        QString("Batch test complete: %1 / %2 passed.")
            .arg(ok).arg(static_cast<int>(m_auEntries.size())));
#endif
}

// ── AU: export ────────────────────────────────────────────────────────────────

void PluginManagerDialog::onAUExportResults() {
#ifdef __APPLE__
    std::vector<mcp::plugin::AUTestResult> tested;
    for (const auto& r : m_auResults)
        if (!r.pluginId.empty()) tested.push_back(r);
    if (tested.empty()) {
        QMessageBox::information(this, "Export Results",
            "No test results yet.\nRun Test Selected or Batch Test All first.");
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
        QMessageBox::warning(this, "Export Failed", "Could not write: " + path);
#endif
}

// ── AU: load into slot ────────────────────────────────────────────────────────

void PluginManagerDialog::onAULoadIntoSlot() {
#ifdef __APPLE__
    if (!m_model || m_ch < 0 || m_slot < 0) return;
    const auto sel = m_auTable->selectedItems();
    if (sel.isEmpty()) return;
    const int row = m_auTable->row(sel.first());
    const int idx = m_auTable->item(row, kAUColName)->data(kEntryIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_auEntries.size())) return;

    const auto& entry = m_auEntries[static_cast<size_t>(idx)];
    auto& as = m_model->sf.audioSetup;
    if (m_ch >= static_cast<int>(as.channels.size())) return;

    const bool isStereo = as.channels[static_cast<size_t>(m_ch)].linkedStereo
                          && (m_ch + 1 < static_cast<int>(as.channels.size()));
    const int nCh = isStereo ? 2 : 1;

    const auto ref = mcp::plugin::NativePluginBackend::makeAUReference(
        entry.type, entry.subtype, entry.manufacturer, nCh);

    auto& pSlots = as.channels[static_cast<size_t>(m_ch)].plugins;
    while (static_cast<int>(pSlots.size()) <= m_slot) pSlots.emplace_back();

    auto& sl = pSlots[static_cast<size_t>(m_slot)];
    sl.pluginId        = ref.pluginId;
    sl.extBackend      = ref.backend;
    sl.extName         = ref.name;
    sl.extVendor       = ref.vendor;
    sl.extVersion      = ref.version;
    sl.extNumChannels  = nCh;
    sl.extStateBlob    = {};
    sl.extParamSnapshot= {};
    sl.parameters      = {};
    sl.bypassed        = false;
    sl.disabled        = false;
    sl.loadFailCount   = 0;

    m_model->markDirty();
    m_model->buildChannelPluginChains();
    m_model->applyMixing();
    accept();
#endif
}

// ──────────────────────────────────────────────────────────────────────────────
// VST3 TAB
// ──────────────────────────────────────────────────────────────────────────────

void PluginManagerDialog::buildVST3Tab() {
    auto* w  = new QWidget;
    auto* vl = new QVBoxLayout(w);
    vl->setSpacing(6);
    vl->setContentsMargins(8, 8, 8, 8);

    // Path management row
    auto* pathLbl = new QLabel("Search Paths:");
    pathLbl->setStyleSheet("color:#999;font-size:11px;");
    vl->addWidget(pathLbl);

    auto* pathRow = new QHBoxLayout;
    m_vst3PathList = new QListWidget;
    m_vst3PathList->setMaximumHeight(90);

    // Load saved paths
    {
        QSettings s("click-in", "MusicCuePlayer");
        const QStringList paths =
            s.value("vst3/searchPaths").toStringList();
        for (const auto& p : paths)
            m_vst3PathList->addItem(p);
        // Populate default paths if nothing saved
        if (paths.isEmpty()) {
#ifdef MCP_HAVE_VST3
            for (const auto& dp : mcp::plugin::VST3Scanner::defaultPaths())
                m_vst3PathList->addItem(QString::fromStdString(dp));
#endif
        }
    }

    auto* pathBtns = new QVBoxLayout;
    m_vst3AddPathBtn    = new QPushButton("Add…");
    m_vst3RemovePathBtn = new QPushButton("Remove");
    m_vst3RemovePathBtn->setEnabled(false);
    pathBtns->addWidget(m_vst3AddPathBtn);
    pathBtns->addWidget(m_vst3RemovePathBtn);
    pathBtns->addStretch();
    pathRow->addWidget(m_vst3PathList, 1);
    pathRow->addLayout(pathBtns);
    vl->addLayout(pathRow);

    // Filter + scan row
    auto* scanRow = new QHBoxLayout;
    m_vst3Filter = new QLineEdit;
    m_vst3Filter->setPlaceholderText("Filter by name or vendor…");
    m_vst3Filter->setClearButtonEnabled(true);
    m_vst3ScanBtn   = new QPushButton("Scan Paths");
    m_vst3CancelBtn = new QPushButton("Cancel");
    m_vst3CancelBtn->setVisible(false);
    scanRow->addWidget(m_vst3Filter, 1);
    scanRow->addWidget(m_vst3ScanBtn);
    scanRow->addWidget(m_vst3CancelBtn);
    vl->addLayout(scanRow);

    // Table
    m_vst3Table = new QTableWidget(0, kV3NumCols);
    m_vst3Table->setHorizontalHeaderLabels({"Name", "Vendor", "Version", "Bundle Path"});
    m_vst3Table->horizontalHeader()->setSectionResizeMode(kV3ColName,    QHeaderView::Stretch);
    m_vst3Table->horizontalHeader()->setSectionResizeMode(kV3ColVendor,  QHeaderView::ResizeToContents);
    m_vst3Table->horizontalHeader()->setSectionResizeMode(kV3ColVersion, QHeaderView::ResizeToContents);
    m_vst3Table->horizontalHeader()->setSectionResizeMode(kV3ColPath,    QHeaderView::Stretch);
    m_vst3Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_vst3Table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_vst3Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vst3Table->setAlternatingRowColors(true);
    m_vst3Table->verticalHeader()->hide();
    m_vst3Table->setShowGrid(false);
    vl->addWidget(m_vst3Table, 1);

    m_vst3Progress = new QProgressBar;
    m_vst3Progress->setRange(0, 100);
    m_vst3Progress->setMaximumHeight(8);
    m_vst3Progress->setTextVisible(false);
    m_vst3Progress->setVisible(false);
    vl->addWidget(m_vst3Progress);

    m_vst3Status = new QLabel;
    m_vst3Status->setStyleSheet("color:#888;font-size:11px;");
    vl->addWidget(m_vst3Status);

    auto* btnRow = new QHBoxLayout;
    m_vst3LoadBtn = new QPushButton("Load into Slot");
    m_vst3LoadBtn->setVisible(m_ch >= 0);
    m_vst3LoadBtn->setEnabled(false);
    btnRow->addStretch();
    btnRow->addWidget(m_vst3LoadBtn);
    vl->addLayout(btnRow);

    connect(m_vst3AddPathBtn,    &QPushButton::clicked,
            this, &PluginManagerDialog::onVST3AddPath);
    connect(m_vst3RemovePathBtn, &QPushButton::clicked,
            this, &PluginManagerDialog::onVST3RemovePath);
    connect(m_vst3ScanBtn,       &QPushButton::clicked,
            this, &PluginManagerDialog::onVST3Scan);
    connect(m_vst3CancelBtn, &QPushButton::clicked, this, [this]() {
#ifdef MCP_HAVE_VST3
        if (m_vst3ScanWorker) m_vst3ScanWorker->cancel();
#endif
        m_vst3CancelBtn->setEnabled(false);
        m_vst3Status->setText("Cancelling…");
    });
    connect(m_vst3Filter,        &QLineEdit::textChanged,
            this, &PluginManagerDialog::onVST3FilterChanged);
    connect(m_vst3Table,         &QTableWidget::itemSelectionChanged,
            this, &PluginManagerDialog::onVST3SelectionChanged);
    connect(m_vst3LoadBtn,       &QPushButton::clicked,
            this, &PluginManagerDialog::onVST3LoadIntoSlot);
    connect(m_vst3PathList,      &QListWidget::itemSelectionChanged, this,
            [this]() { m_vst3RemovePathBtn->setEnabled(
                           !m_vst3PathList->selectedItems().isEmpty()); });

    m_tabs->addTab(w, "VST3 Plugins");

#ifndef MCP_HAVE_VST3
    m_vst3Status->setText("VST3 support not compiled in this build.");
    m_vst3ScanBtn->setEnabled(false);
    m_vst3AddPathBtn->setEnabled(false);
#endif

    vst3LoadCache();
}

void PluginManagerDialog::onVST3AddPath() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Add VST3 Search Path", QString());
    if (dir.isEmpty()) return;
    // Check not already present
    for (int i = 0; i < m_vst3PathList->count(); ++i)
        if (m_vst3PathList->item(i)->text() == dir) return;
    m_vst3PathList->addItem(dir);
    vst3SavePaths();
}

void PluginManagerDialog::onVST3RemovePath() {
    const auto sel = m_vst3PathList->selectedItems();
    for (auto* it : sel) delete it;
    vst3SavePaths();
}

void PluginManagerDialog::vst3SavePaths() {
    QStringList paths;
    for (int i = 0; i < m_vst3PathList->count(); ++i)
        paths << m_vst3PathList->item(i)->text();
    QSettings s("click-in", "MusicCuePlayer");
    s.setValue("vst3/searchPaths", paths);
}

void PluginManagerDialog::vst3LoadCache() {
#ifdef MCP_HAVE_VST3
    QSettings s("click-in", "MusicCuePlayer");
    const QVariantList list = s.value("vst3/cachedPlugins").toList();
    if (list.isEmpty()) return;

    m_vst3Entries.clear();
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        mcp::plugin::VST3Entry e;
        e.name       = m.value("name").toString().toStdString();
        e.vendor     = m.value("vendor").toString().toStdString();
        e.version    = m.value("version").toString().toStdString();
        e.path       = m.value("path").toString().toStdString();
        e.pluginId   = m.value("pluginId").toString().toStdString();
        e.classIndex = m.value("classIndex").toInt();
        m_vst3Entries.push_back(std::move(e));
    }

    vst3PopulateTable();
    vst3ApplyFilter();
    m_vst3Status->setText(
        QString("%1 VST3 plugin(s) from last scan. Click \"Scan Paths\" to refresh.")
            .arg(static_cast<int>(m_vst3Entries.size())));
#endif
}

void PluginManagerDialog::onVST3Scan() {
#ifdef MCP_HAVE_VST3
    if (m_vst3Scanning) return;

    std::vector<std::string> dirs;
    for (int i = 0; i < m_vst3PathList->count(); ++i)
        dirs.push_back(m_vst3PathList->item(i)->text().toStdString());

    const auto bundles = mcp::plugin::VST3Scanner::findBundles(dirs);
    if (bundles.empty()) {
        m_vst3Status->setText("No .vst3 bundles found in the search paths.");
        return;
    }

    m_vst3Scanning = true;
    m_vst3ScanBtn->setEnabled(false);
    m_vst3CancelBtn->setVisible(true);
    m_vst3CancelBtn->setEnabled(true);
    m_vst3Progress->setRange(0, static_cast<int>(bundles.size()));
    m_vst3Progress->setValue(0);
    m_vst3Progress->setVisible(true);
    m_vst3Status->setText(
        QString("Found %1 bundle(s). Scanning…").arg(static_cast<int>(bundles.size())));

    m_vst3ScanWorker = new VST3ScanWorker(bundles);
    m_vst3ScanThread = new QThread(this);
    m_vst3ScanWorker->moveToThread(m_vst3ScanThread);

    connect(m_vst3ScanThread, &QThread::started,
            m_vst3ScanWorker, &VST3ScanWorker::run);
    connect(m_vst3ScanWorker, &VST3ScanWorker::progress,
            this, &PluginManagerDialog::onVST3ScanProgress);
    connect(m_vst3ScanWorker, &VST3ScanWorker::finished,
            this, &PluginManagerDialog::onVST3ScanFinished);

    m_vst3ScanThread->start();
#else
    m_vst3Status->setText("VST3 support not compiled.");
#endif
}

void PluginManagerDialog::onVST3ScanProgress(int cur, int total, const QString& name) {
    m_vst3Progress->setValue(cur);
    m_vst3Status->setText(
        QString("Scanning %1 / %2: %3…").arg(cur).arg(total).arg(name));
}

static void vst3SaveCache(const std::vector<mcp::plugin::VST3Entry>& entries) {
    QVariantList list;
    for (const auto& e : entries) {
        QVariantMap m;
        m["name"]        = QString::fromStdString(e.name);
        m["vendor"]      = QString::fromStdString(e.vendor);
        m["version"]     = QString::fromStdString(e.version);
        m["path"]        = QString::fromStdString(e.path);
        m["pluginId"]    = QString::fromStdString(e.pluginId);
        m["classIndex"]  = e.classIndex;
        list.append(m);
    }
    QSettings s("click-in", "MusicCuePlayer");
    s.setValue("vst3/cachedPlugins", list);
}

void PluginManagerDialog::onVST3ScanFinished() {
#ifdef MCP_HAVE_VST3
    if (m_vst3ScanWorker) {
        m_vst3Entries = m_vst3ScanWorker->takeResults();
        delete m_vst3ScanWorker;
        m_vst3ScanWorker = nullptr;
    }
    if (m_vst3ScanThread) {
        m_vst3ScanThread->quit();
        m_vst3ScanThread->wait();
        delete m_vst3ScanThread;
        m_vst3ScanThread = nullptr;
    }

    m_vst3Scanning = false;
    m_vst3Progress->setVisible(false);
    m_vst3CancelBtn->setVisible(false);
    m_vst3ScanBtn->setEnabled(true);

    if (m_pendingClose) {
        close();
        return;
    }

    // Persist results so MixConsole picker can display them without re-scanning
    vst3SaveCache(m_vst3Entries);

    vst3PopulateTable();
    vst3ApplyFilter();
    m_vst3Status->setText(
        QString("Found %1 VST3 plugin(s).")
            .arg(static_cast<int>(m_vst3Entries.size())));
#endif
}

void PluginManagerDialog::vst3PopulateTable() {
#ifdef MCP_HAVE_VST3
    m_vst3Table->setRowCount(0);
    for (size_t i = 0; i < m_vst3Entries.size(); ++i) {
        const auto& e = m_vst3Entries[i];
        const int row = m_vst3Table->rowCount();
        m_vst3Table->insertRow(row);
        auto* ni = new QTableWidgetItem(QString::fromStdString(e.name));
        ni->setData(kEntryIndexRole, static_cast<int>(i));
        m_vst3Table->setItem(row, kV3ColName,    ni);
        m_vst3Table->setItem(row, kV3ColVendor,  new QTableWidgetItem(QString::fromStdString(e.vendor)));
        m_vst3Table->setItem(row, kV3ColVersion, new QTableWidgetItem(QString::fromStdString(e.version)));
        m_vst3Table->setItem(row, kV3ColPath,    new QTableWidgetItem(QString::fromStdString(e.path)));
    }
#endif
}

void PluginManagerDialog::vst3ApplyFilter() {
    const QString f = m_vst3Filter->text().trimmed().toLower();
    for (int row = 0; row < m_vst3Table->rowCount(); ++row) {
        bool show = f.isEmpty() ||
            m_vst3Table->item(row, kV3ColName)  ->text().toLower().contains(f) ||
            m_vst3Table->item(row, kV3ColVendor)->text().toLower().contains(f);
        m_vst3Table->setRowHidden(row, !show);
    }
}

void PluginManagerDialog::onVST3FilterChanged(const QString&) { vst3ApplyFilter(); }

void PluginManagerDialog::onVST3SelectionChanged() {
    m_vst3LoadBtn->setEnabled(
        m_ch >= 0 && !m_vst3Table->selectedItems().isEmpty());
}

void PluginManagerDialog::onVST3LoadIntoSlot() {
#ifdef MCP_HAVE_VST3
    if (!m_model || m_ch < 0 || m_slot < 0) return;
    const auto sel = m_vst3Table->selectedItems();
    if (sel.isEmpty()) return;
    const int row = m_vst3Table->row(sel.first());
    const int idx = m_vst3Table->item(row, kV3ColName)->data(kEntryIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_vst3Entries.size())) return;

    const auto& entry = m_vst3Entries[static_cast<size_t>(idx)];
    auto& as = m_model->sf.audioSetup;
    if (m_ch >= static_cast<int>(as.channels.size())) return;

    const bool isStereo = as.channels[static_cast<size_t>(m_ch)].linkedStereo
                          && (m_ch + 1 < static_cast<int>(as.channels.size()));
    const int nCh = isStereo ? 2 : 1;

    auto& pSlots = as.channels[static_cast<size_t>(m_ch)].plugins;
    while (static_cast<int>(pSlots.size()) <= m_slot) pSlots.emplace_back();

    auto& sl = pSlots[static_cast<size_t>(m_slot)];
    sl.pluginId        = entry.pluginId;
    sl.extBackend      = "vst3";
    sl.extName         = entry.name;
    sl.extVendor       = entry.vendor;
    sl.extVersion      = entry.version;
    sl.extPath         = entry.path;
    sl.extNumChannels  = nCh;
    sl.extStateBlob    = {};
    sl.extParamSnapshot= {};
    sl.parameters      = {};
    sl.bypassed        = false;
    sl.disabled        = false;
    sl.loadFailCount   = 0;

    m_model->markDirty();
    m_model->buildChannelPluginChains();
    m_model->applyMixing();
    accept();
#endif
}

// Q_OBJECT in VST3ScanWorker (defined in this translation unit) needs its moc output
#ifdef MCP_HAVE_VST3
#  include "PluginManagerDialog.moc"
#endif
