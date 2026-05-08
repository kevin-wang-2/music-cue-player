#include "ProjectStatusDialog.h"
#include "AppModel.h"

#include "engine/Cue.h"
#include "engine/CueList.h"

#include <QCheckBox>
#include <QDateTime>
#include <QLabel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Helper: human-readable reason why a cue is not loaded

static QString breakReason(const mcp::Cue* c) {
    using T = mcp::CueType;
    switch (c->type) {
        case T::Audio:
            return c->path.empty() ? "No file path" : "File not found or failed to load";
        case T::Fade:
            if (!c->fadeData)       return "No fade data";
            if (c->targetIndex < 0) return "No target cue";
            return "Invalid";
        case T::Start:
        case T::Stop:
        case T::Arm:
        case T::Devamp:
        case T::Goto:
            return "No target cue assigned";
        case T::Marker:
            return c->targetIndex < 0 ? "No target cue" : "No marker selected";
        case T::MusicContext:
            return "No music context attached";
        case T::Network:
            return "No network patch assigned";
        case T::Midi:
            return "No MIDI patch assigned";
        case T::Timecode:
            return "Start TC ≥ End TC";
        default:
            return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Helper: MIDI message type label

static QString midiTypeLabel(mcp::MidiMsgType t) {
    switch (t) {
        case mcp::MidiMsgType::NoteOn:        return "NoteOn";
        case mcp::MidiMsgType::NoteOff:       return "NoteOff";
        case mcp::MidiMsgType::ControlChange: return "CC";
        case mcp::MidiMsgType::ProgramChange: return "PC";
        case mcp::MidiMsgType::PitchBend:     return "PitchBend";
    }
    return "?";
}

// ---------------------------------------------------------------------------

ProjectStatusDialog::ProjectStatusDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Project Status");
    resize(640, 400);
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget(this);
    m_tabs->setStyleSheet(
        "QTabWidget::pane{border:0;}"
        "QTabBar::tab{background:#1a1a1a;color:#aaa;padding:5px 14px;border-bottom:2px solid transparent;}"
        "QTabBar::tab:selected{color:#fff;border-bottom:2px solid #2a6ab8;}"
        "QTabBar::tab:hover{background:#222;}");
    lay->addWidget(m_tabs);

    buildWarningsTab();
    buildLogsTab();
    buildScriptletTab();

    // Connect model signals
    connect(m_model, &AppModel::cueListChanged, this, &ProjectStatusDialog::refreshWarnings);
    connect(m_model, &AppModel::scriptletOutput, this, &ProjectStatusDialog::onScriptletOutput);
}

// ── Warnings tab ───────────────────────────────────────────────────────────

void ProjectStatusDialog::buildWarningsTab() {
    m_warningsPage = new QWidget;
    auto* lay = new QVBoxLayout(m_warningsPage);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    auto* topRow = new QHBoxLayout;
    auto* lbl    = new QLabel("Cues that are not ready to fire:");
    lbl->setStyleSheet("color:#888; font-size:11px;");
    topRow->addWidget(lbl);
    topRow->addStretch();
    auto* btnRefresh = new QPushButton("Refresh");
    btnRefresh->setFixedWidth(70);
    topRow->addWidget(btnRefresh);
    lay->addLayout(topRow);

    m_warningsList = new QTreeWidget;
    m_warningsList->setColumnCount(3);
    m_warningsList->setHeaderLabels({"#", "Name", "Reason"});
    m_warningsList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_warningsList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_warningsList->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_warningsList->setRootIsDecorated(false);
    m_warningsList->setAlternatingRowColors(true);
    m_warningsList->setStyleSheet(
        "QTreeWidget{background:#111;color:#ddd;border:1px solid #333;}"
        "QTreeWidget::item:alternate{background:#151515;}"
        "QHeaderView::section{background:#1a1a1a;color:#aaa;border:0;padding:3px 6px;"
        "  border-bottom:1px solid #333;}");
    lay->addWidget(m_warningsList);

    connect(btnRefresh, &QPushButton::clicked, this, &ProjectStatusDialog::refreshWarnings);

    m_tabs->addTab(m_warningsPage, "Warnings");
    refreshWarnings();
}

void ProjectStatusDialog::refreshWarnings() {
    m_warningsList->clear();
    const int n = m_model->cues.cueCount();
    for (int i = 0; i < n; ++i) {
        const mcp::Cue* c = m_model->cues.cueAt(i);
        if (!c || c->isLoaded()) continue;
        auto* item = new QTreeWidgetItem(m_warningsList);
        item->setText(0, QString::fromStdString(c->cueNumber));
        item->setText(1, QString::fromStdString(c->name));
        item->setText(2, breakReason(c));
        item->setForeground(2, QColor(0xff, 0x88, 0x44));
    }
    // Update tab label with count
    const int count = m_warningsList->topLevelItemCount();
    m_tabs->setTabText(m_tabs->indexOf(m_warningsPage),
                       count > 0 ? QString("Warnings (%1)").arg(count) : "Warnings");
}

// ── Logs tab ───────────────────────────────────────────────────────────────

void ProjectStatusDialog::buildLogsTab() {
    m_logsPage = new QWidget;
    auto* lay = new QVBoxLayout(m_logsPage);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    auto* ctrlRow = new QHBoxLayout;
    m_chkMidiLog = new QCheckBox("Log MIDI input");
    m_chkOscLog  = new QCheckBox("Log OSC input");
    m_chkMidiLog->setStyleSheet("color:#ccc;");
    m_chkOscLog ->setStyleSheet("color:#ccc;");
    ctrlRow->addWidget(m_chkMidiLog);
    ctrlRow->addSpacing(12);
    ctrlRow->addWidget(m_chkOscLog);
    ctrlRow->addStretch();
    auto* btnClear = new QPushButton("Clear");
    btnClear->setFixedWidth(60);
    ctrlRow->addWidget(btnClear);
    lay->addLayout(ctrlRow);

    m_logsView = new QPlainTextEdit;
    m_logsView->setReadOnly(true);
    m_logsView->setMaximumBlockCount(2000);
    m_logsView->setStyleSheet(
        "QPlainTextEdit{background:#0d0d0d;color:#ccc;border:1px solid #333;"
        "  font-family:monospace;font-size:11px;}");
    lay->addWidget(m_logsView);

    // Connect/disconnect MIDI logging based on checkbox
    connect(m_chkMidiLog, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            connect(m_model, &AppModel::midiInputReceived,
                    this,    &ProjectStatusDialog::onMidiInput);
        else
            disconnect(m_model, &AppModel::midiInputReceived,
                       this,    &ProjectStatusDialog::onMidiInput);
    });

    // Connect/disconnect OSC logging based on checkbox
    connect(m_chkOscLog, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            connect(m_model, &AppModel::oscInputReceived,
                    this,    &ProjectStatusDialog::onOscInput);
        else
            disconnect(m_model, &AppModel::oscInputReceived,
                       this,    &ProjectStatusDialog::onOscInput);
    });

    connect(btnClear, &QPushButton::clicked, m_logsView, &QPlainTextEdit::clear);

    m_tabs->addTab(m_logsPage, "Logs");
}

void ProjectStatusDialog::onMidiInput(mcp::MidiMsgType type, int ch, int d1, int d2) {
    const QString line = QString("[MIDI]  %1  ch=%2  d1=%3  d2=%4")
        .arg(midiTypeLabel(type), -10).arg(ch, 2).arg(d1, 3).arg(d2, 3);
    m_logsView->appendPlainText(line);
}

void ProjectStatusDialog::onOscInput(const QString& path, const QVariantList& args) {
    QString line = "[OSC]   " + path;
    for (const auto& a : args)
        line += "  " + a.toString();
    m_logsView->appendPlainText(line);
}

// ── Scriptlet tab ──────────────────────────────────────────────────────────

void ProjectStatusDialog::buildScriptletTab() {
    m_scriptletPage = new QWidget;
    auto* lay = new QVBoxLayout(m_scriptletPage);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    auto* topRow = new QHBoxLayout;
    auto* lbl    = new QLabel("stdout / stderr from scriptlet cues");
    lbl->setStyleSheet("color:#888; font-size:11px;");
    topRow->addWidget(lbl);
    topRow->addStretch();
    auto* btnClear = new QPushButton("Clear");
    btnClear->setFixedWidth(60);
    topRow->addWidget(btnClear);
    lay->addLayout(topRow);

    m_scriptletView = new QPlainTextEdit;
    m_scriptletView->setReadOnly(true);
    m_scriptletView->setMaximumBlockCount(5000);
    m_scriptletView->setStyleSheet(
        "QPlainTextEdit{background:#0d1117;color:#e0e0ff;border:1px solid #333;"
        "  font-family:monospace;font-size:12px;}");
    lay->addWidget(m_scriptletView);

    connect(btnClear, &QPushButton::clicked, m_scriptletView, &QPlainTextEdit::clear);

    m_tabs->addTab(m_scriptletPage, "Scriptlet");
}

void ProjectStatusDialog::onScriptletOutput(const QString& text) {
    // Strip trailing newline to avoid blank line from appendPlainText's own newline.
    QString t = text;
    if (t.endsWith('\n')) t.chop(1);
    if (!t.isEmpty())
        m_scriptletView->appendPlainText(t);
}
