#include "MissingMediaDialog.h"
#include "AppModel.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <set>
#include <unordered_map>

// Audio + video extensions considered when searching a folder
static const std::set<std::string> kMediaExts = {
    ".wav", ".aif", ".aiff", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".caf",
    ".opus", ".wv", ".ape",
    ".mp4", ".mov", ".avi", ".mkv", ".webm", ".mxf",
};

enum Col { ColCueNum = 0, ColType, ColName, ColMissing, ColReplacement, ColStatus, ColCount };

MissingMediaDialog::MissingMediaDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Missing Media");
    setMinimumSize(800, 400);
    buildUi();
}

void MissingMediaDialog::buildUi()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(10, 10, 10, 10);
    vbox->setSpacing(8);

    m_summary = new QLabel(this);
    m_summary->setStyleSheet("color:#ccc;");
    vbox->addWidget(m_summary);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels(
        {"Cue #", "Type", "Name", "Missing File", "Replacement", "Status"});
    m_table->horizontalHeader()->setSectionResizeMode(ColCueNum,      QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColType,        QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColName,        QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColMissing,     QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColReplacement, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus,      QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->setStyleSheet(
        "QTableWidget{background:#111;color:#ccc;gridline-color:#2a2a2a;}"
        "QHeaderView::section{background:#1a1a1a;color:#aaa;border:none;"
        "  padding:4px;border-bottom:1px solid #333;}"
    );
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &MissingMediaDialog::updateButtons);
    vbox->addWidget(m_table, 1);

    // ── Button bar ──────────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    auto* searchBtn = new QPushButton("Search in Folder…", this);
    searchBtn->setToolTip("Scan a directory for files whose name matches the missing filename");
    connect(searchBtn, &QPushButton::clicked, this, &MissingMediaDialog::onSearchFolder);
    btnRow->addWidget(searchBtn);

    m_locateBtn = new QPushButton("Locate Selected…", this);
    m_locateBtn->setToolTip("Manually choose a replacement file for the selected row");
    m_locateBtn->setEnabled(false);
    connect(m_locateBtn, &QPushButton::clicked, this, &MissingMediaDialog::onLocateSelected);
    btnRow->addWidget(m_locateBtn);

    btnRow->addStretch();

    m_applyBtn = new QPushButton("Apply && Close", this);
    m_applyBtn->setDefault(true);
    m_applyBtn->setEnabled(false);
    connect(m_applyBtn, &QPushButton::clicked, this, &MissingMediaDialog::onApply);
    btnRow->addWidget(m_applyBtn);

    auto* skipBtn = new QPushButton("Skip", this);
    connect(skipBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(skipBtn);

    vbox->addLayout(btnRow);
}

// ── Public ────────────────────────────────────────────────────────────────────

void MissingMediaDialog::refresh()
{
    m_entries = ShowHelpers::findMissingMedia(*m_model);
    populateTable();
    updateButtons();
}

// ── Private helpers ───────────────────────────────────────────────────────────

void MissingMediaDialog::populateTable()
{
    m_table->setRowCount((int)m_entries.size());
    for (int r = 0; r < (int)m_entries.size(); ++r)
        updateRow(r);

    const int total    = (int)m_entries.size();
    const int resolved = (int)std::count_if(m_entries.begin(), m_entries.end(),
                                             [](const auto& e){ return !e.newPath.empty(); });
    if (total == 0)
        m_summary->setText("All media files found.");
    else
        m_summary->setText(QString("%1 missing file(s) — %2 resolved")
                               .arg(total).arg(resolved));
}

void MissingMediaDialog::updateRow(int row)
{
    const auto& e = m_entries[(size_t)row];
    const bool resolved = !e.newPath.empty();

    namespace fs = std::filesystem;
    const QString missingName  = QString::fromStdString(
        fs::path(e.resolvedPath).filename().string());
    const QString replaceName  = resolved
        ? QString::fromStdString(fs::path(e.newPath).filename().string())
        : QString();

    const QColor rowBg  = resolved ? QColor(0x10, 0x30, 0x10) : QColor(0x30, 0x10, 0x10);
    const QString stTxt = resolved ? "✓ Found"  : "● Missing";
    const QColor  stClr = resolved ? QColor("#4c4")  : QColor("#c44");

    auto setItem = [&](int col, const QString& txt, Qt::AlignmentFlag align = Qt::AlignLeft) {
        auto* it = new QTableWidgetItem(txt);
        it->setBackground(rowBg);
        it->setForeground(QColor(0xdd, 0xdd, 0xdd));
        it->setTextAlignment(align | Qt::AlignVCenter);
        m_table->setItem(row, col, it);
    };

    setItem(ColCueNum,      QString::fromStdString(e.cueNumber), Qt::AlignHCenter);
    setItem(ColType,        QString::fromStdString(e.cueType),   Qt::AlignHCenter);
    setItem(ColName,        QString::fromStdString(e.cueName));
    auto* missingItem = new QTableWidgetItem(missingName);
    missingItem->setBackground(rowBg);
    missingItem->setForeground(QColor(0xdd, 0xdd, 0xdd));
    missingItem->setToolTip(QString::fromStdString(e.resolvedPath));
    m_table->setItem(row, ColMissing, missingItem);

    auto* repItem = new QTableWidgetItem(replaceName);
    repItem->setBackground(rowBg);
    repItem->setForeground(QColor(0xdd, 0xdd, 0xdd));
    if (resolved) repItem->setToolTip(QString::fromStdString(e.newPath));
    m_table->setItem(row, ColReplacement, repItem);

    auto* stItem = new QTableWidgetItem(stTxt);
    stItem->setBackground(rowBg);
    stItem->setForeground(stClr);
    stItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_table->setItem(row, ColStatus, stItem);
}

void MissingMediaDialog::updateButtons()
{
    const bool hasSelection = !m_table->selectedItems().isEmpty();
    m_locateBtn->setEnabled(hasSelection);

    const bool anyResolved = std::any_of(m_entries.begin(), m_entries.end(),
                                          [](const auto& e){ return !e.newPath.empty(); });
    m_applyBtn->setEnabled(anyResolved);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void MissingMediaDialog::onSearchFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, "Search in Folder", QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (folder.isEmpty()) return;

    namespace fs = std::filesystem;

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    // Build index: lowercased filename → absolute path.
    // Exact filename match (with ext) takes priority over stem-only match.
    std::unordered_map<std::string, std::string> byFilename;
    std::unordered_map<std::string, std::string> byStem;

    try {
        for (auto& de : fs::recursive_directory_iterator(
                 folder.toStdString(),
                 fs::directory_options::skip_permission_denied))
        {
            if (!de.is_regular_file()) continue;
            std::string extLow = de.path().extension().string();
            std::transform(extLow.begin(), extLow.end(), extLow.begin(), ::tolower);
            if (!kMediaExts.count(extLow)) continue;

            const std::string fnameLow = lower(de.path().filename().string());
            const std::string stemLow  = lower(de.path().stem().string());
            const std::string absPath  = de.path().string();

            if (!byFilename.count(fnameLow)) byFilename[fnameLow] = absPath;
            if (!byStem.count(stemLow))      byStem[stemLow]      = absPath;
        }
    } catch (...) {
        QMessageBox::warning(this, "Search", "Could not read some directories.");
    }

    int matched = 0;
    for (int r = 0; r < (int)m_entries.size(); ++r) {
        auto& e = m_entries[(size_t)r];
        if (!e.newPath.empty()) continue;

        const fs::path missing(e.resolvedPath);
        const std::string fnameLow = lower(missing.filename().string());
        const std::string stemLow  = lower(missing.stem().string());

        std::string found;
        auto fit = byFilename.find(fnameLow);
        if (fit != byFilename.end()) {
            found = fit->second;
        } else {
            auto sit = byStem.find(stemLow);
            if (sit != byStem.end()) found = sit->second;
        }
        if (!found.empty()) {
            e.newPath = found;
            updateRow(r);
            ++matched;
        }
    }

    // refresh summary
    const int resolved = (int)std::count_if(m_entries.begin(), m_entries.end(),
                                             [](const auto& e){ return !e.newPath.empty(); });
    m_summary->setText(QString("%1 missing file(s) — %2 resolved")
                           .arg((int)m_entries.size()).arg(resolved));
    updateButtons();

    if (matched == 0)
        QMessageBox::information(this, "Search Result",
            "No matching files found in the selected folder.");
    else
        QMessageBox::information(this, "Search Result",
            QString("Matched %1 file(s). Click \"Apply & Close\" to save.").arg(matched));
}

void MissingMediaDialog::onLocateSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= (int)m_entries.size()) return;
    auto& e = m_entries[(size_t)row];

    const QString path = QFileDialog::getOpenFileName(
        this, "Choose Replacement File",
        QString::fromStdString(
            std::filesystem::path(e.resolvedPath).parent_path().string()),
        "Media Files (*.wav *.aif *.aiff *.mp3 *.flac *.ogg *.m4a *.aac *.caf "
        "*.mp4 *.mov *.avi *.mkv *.webm *.mxf);;All Files (*)");
    if (path.isEmpty()) return;

    e.newPath = path.toStdString();
    updateRow(row);

    const int resolved = (int)std::count_if(m_entries.begin(), m_entries.end(),
                                             [](const auto& e){ return !e.newPath.empty(); });
    m_summary->setText(QString("%1 missing file(s) — %2 resolved")
                           .arg((int)m_entries.size()).arg(resolved));
    updateButtons();
}

void MissingMediaDialog::onApply()
{
    ShowHelpers::applyMediaFixes(*m_model, m_entries);
    emit mediaFixed();
    // Re-check — some might still be missing (user resolved only some)
    refresh();
    if (std::none_of(m_entries.begin(), m_entries.end(),
                     [](const auto& e){ return !e.newPath.empty(); }))
        accept(); // all done, close
}
