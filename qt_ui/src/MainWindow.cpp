#include "MainWindow.h"
#include "AppModel.h"
#include "CueTableView.h"
#include "DeviceDialog.h"
#include "InspectorWidget.h"
#include "ShowHelpers.h"

#include "engine/CueList.h"
#include "engine/MusicContext.h"
#include "engine/ShowFile.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QResizeEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <filesystem>

// ── style constants ────────────────────────────────────────────────────────

static const char* kGoBarStyle = R"(
    QWidget#goBar {
        background-color: #111111;
        border-bottom: 1px solid #333;
    }
)";

static const char* kGoBtnStyle = R"(
    QPushButton {
        background-color: #3a3a3a;
        color: #dddddd;
        font-size: 30px;
        font-weight: bold;
        letter-spacing: 2px;
        border: 1px solid #555;
        border-radius: 6px;
    }
    QPushButton:hover  { background-color: #4a4a4a; border-color: #777; }
    QPushButton:pressed{ background-color: #252525; }
)";

static const char* kIconBarStyle = R"(
    QWidget#iconBar {
        background-color: #161616;
        border-bottom: 1px solid #2a2a2a;
    }
)";

static const char* kIconBtnBase = R"(
    QToolButton {
        background-color: transparent;
        color: #aaaaaa;
        font-size: 15px;
        border: 1px solid transparent;
        border-radius: 5px;
        padding: 2px;
    }
    QToolButton:hover {
        background-color: #2e2e2e;
        border-color: #444;
        color: #eeeeee;
    }
    QToolButton:pressed {
        background-color: #1a1a1a;
        color: #ffffff;
    }
)";

// ── ctor ───────────────────────────────────────────────────────────────────

MainWindow::MainWindow(AppModel* model, QWidget* parent)
    : QMainWindow(parent), m_model(model)
{
    setAcceptDrops(true);
    setMinimumSize(800, 500);

    buildMenuBar();

    // Central widget holds everything in a vertical stack
    auto* central = new QWidget(this);
    central->setObjectName("central");
    setCentralWidget(central);

    auto* vlay = new QVBoxLayout(central);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    buildGoBar();
    buildIconBar();
    buildSplitter();

    vlay->addWidget(findChild<QWidget*>("goBar"));
    vlay->addWidget(findChild<QWidget*>("iconBar"));
    vlay->addWidget(m_splitter, 1);

    // Toast
    m_toastLabel = new QLabel(central);
    m_toastLabel->setAlignment(Qt::AlignCenter);
    m_toastLabel->setStyleSheet(
        "background:rgba(40,40,40,210);color:white;"
        "border-radius:6px;padding:6px 16px;font-size:13px;");
    m_toastLabel->hide();
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, m_toastLabel, &QLabel::hide);

    // 16ms tick
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::onTick);
    m_timer->start();

    // Model wiring
    connect(m_model, &AppModel::cueListChanged,   m_cueTable,  &CueTableView::refresh);
    connect(m_model, &AppModel::selectionChanged,  this,        &MainWindow::onRowSelected);
    connect(m_model, &AppModel::playbackStateChanged, m_cueTable, &CueTableView::refreshStatus);
    connect(m_model, &AppModel::dirtyChanged, this, [this](bool) {
        updateTitle();
        m_actSave->setEnabled(m_model->dirty);
    });
    connect(m_model, &AppModel::engineStatusChanged, this, [this]() {
        if (!m_model->engineOk)
            showToast("Engine error: " + QString::fromStdString(m_model->engineError));
    });

    connect(m_cueTable,  &CueTableView::rowSelected,    this, &MainWindow::onRowSelected);
    connect(m_cueTable,  &CueTableView::cueListModified, this, &MainWindow::onCueListModified);
    connect(m_inspector, &InspectorWidget::cueEdited,    this, &MainWindow::onCueListModified);

    updateTitle();
    m_cueTable->refresh();
}

// ── GoBar ──────────────────────────────────────────────────────────────────

void MainWindow::buildGoBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName("goBar");
    bar->setStyleSheet(kGoBarStyle);
    bar->setFixedHeight(84);

    auto* hlay = new QHBoxLayout(bar);
    hlay->setContentsMargins(8, 6, 12, 6);
    hlay->setSpacing(12);

    // GO button
    m_goBtn = new QPushButton("GO", bar);
    m_goBtn->setStyleSheet(kGoBtnStyle);
    m_goBtn->setFixedSize(110, 72);
    m_goBtn->setShortcut(Qt::Key_Space);
    connect(m_goBtn, &QPushButton::clicked, this, [this]() {
        m_model->cues.go();
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(m_goBtn);

    // Vertical divider
    auto* div = new QFrame(bar);
    div->setFrameShape(QFrame::VLine);
    div->setStyleSheet("color:#333;");
    hlay->addWidget(div);

    // Cue info panel
    auto* infoPanel = new QWidget(bar);
    auto* ilay = new QVBoxLayout(infoPanel);
    ilay->setContentsMargins(6, 4, 0, 4);
    ilay->setSpacing(4);

    m_lblCueName = new QLabel("—", infoPanel);
    m_lblCueName->setStyleSheet(
        "color:#f0f0f0; font-size:22px; font-weight:600;");

    m_lblCueDetail = new QLabel({}, infoPanel);
    m_lblCueDetail->setStyleSheet("color:#777; font-size:12px;");

    ilay->addWidget(m_lblCueName);
    ilay->addWidget(m_lblCueDetail);
    ilay->addStretch();

    hlay->addWidget(infoPanel, 1);

    // Global Music Context indicator (right side of GoBar)
    m_lblGlobalMC = new QLabel(bar);
    m_lblGlobalMC->setStyleSheet(
        "color:#88ccff; font-size:12px; font-family:monospace;"
        "background:#1a2030; border-radius:4px; padding:3px 7px;");
    m_lblGlobalMC->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_lblGlobalMC->hide();
    hlay->addWidget(m_lblGlobalMC);
}

// ── IconBar ────────────────────────────────────────────────────────────────

void MainWindow::buildIconBar() {
    auto* bar = new QWidget(this);
    bar->setObjectName("iconBar");
    bar->setStyleSheet(kIconBarStyle);
    bar->setFixedHeight(40);

    auto* hlay = new QHBoxLayout(bar);
    hlay->setContentsMargins(6, 3, 6, 3);
    hlay->setSpacing(2);

    // Helper that inserts a cue of given type after the currently selected cue.
    auto addCue = [this](const QString& type) {
        if (m_model->sf.cueLists.empty())
            m_model->sf.cueLists.push_back({});

        const int selRow = m_cueTable->selectedRow();
        m_model->pushUndo();

        const std::string typeStr = type.toLower().toStdString();

        if (typeStr == "group") {
            // Wrap ALL selected rows.  Fall back to selRow-only if nothing multi-selected.
            std::vector<int> rows;
            for (const auto& idx : m_cueTable->selectionModel()->selectedRows())
                rows.push_back(idx.row());
            std::sort(rows.begin(), rows.end());
            if (rows.empty() && selRow >= 0) rows.push_back(selRow);

            if (!rows.empty()) {
                // createGroupFromSelection handles undo, rebuild, refresh, and selection.
                m_cueTable->createGroupFromSelection(rows);
            } else {
                // Nothing selected → empty group at end.
                mcp::ShowFile::CueData cd;
                cd.type      = "group";
                cd.groupMode = "timeline";
                cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);
                ShowHelpers::sfInsertBefore(m_model->sf, -1, std::move(cd));
                std::string err;
                ShowHelpers::rebuildCueList(*m_model, err);
                onCueListModified();
                m_cueTable->refresh();
                m_cueTable->selectRow(m_model->cues.cueCount() - 1);
            }
            return;
        }

        mcp::ShowFile::CueData cd;
        cd.type      = typeStr;
        cd.cueNumber = ShowHelpers::nextCueNumber(m_model->sf);

        // Auto-assign target for cue types that reference another cue.
        if (selRow >= 0) {
            if (typeStr == "fade" || typeStr == "start" || typeStr == "stop"
                || typeStr == "arm" || typeStr == "devamp")
                cd.target = selRow;
        }

        const int ins = (selRow >= 0) ? selRow + 1 : -1;
        ShowHelpers::sfInsertBefore(m_model->sf, ins, std::move(cd));
        std::string err;
        ShowHelpers::rebuildCueList(*m_model, err);
        onCueListModified();
        m_cueTable->refresh();
        m_cueTable->selectRow(selRow >= 0 ? selRow + 1 : m_model->cues.cueCount() - 1);
    };

    // Add-cue type buttons
    struct { const char* icon; const char* tip; const char* type; } cueBtns[] = {
        { "▤",  "Add Group cue",   "group"  },
        { "♫",  "Add Audio cue",   "audio"  },
        { "▷",  "Add Start cue",   "start"  },
        { "□",  "Add Stop cue",    "stop"   },
        { "〰", "Add Fade cue",    "fade"   },
        { "⊙",  "Add Arm cue",     "arm"    },
        { "⤴",  "Add Devamp cue",  "devamp" },
    };
    for (const auto& b : cueBtns) {
        auto* btn = makeIconBtn(b.icon, b.tip);
        connect(btn, &QToolButton::clicked, this, [=]() { addCue(b.type); });
        hlay->addWidget(btn);
    }

    // Separator
    auto* sep = new QFrame(bar);
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet("color:#333; margin:4px 4px;");
    hlay->addWidget(sep);

    // Playback controls
    auto* btnGo = makeIconBtn("▶", "Go  [Space]",
        "QToolButton:hover{background:#1a4d1a;border-color:#2a7a2a;color:#5f5;}"
        "QToolButton:pressed{background:#0f2f0f;}");
    connect(btnGo, &QToolButton::clicked, this, [this]() {
        m_model->cues.go();
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnGo);

    auto* btnStop = makeIconBtn("■", "Stop selected  [Esc]");
    connect(btnStop, &QToolButton::clicked, this, [this]() {
        const int sel = m_cueTable->selectedRow();
        if (sel >= 0) m_model->cues.stop(sel);
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnStop);

    auto* btnPanic = makeIconBtn("✕", "Panic — stop all  [⇧Esc]",
        "QToolButton:hover{background:#4d1a1a;border-color:#aa2222;color:#f88;}"
        "QToolButton:pressed{background:#2f0f0f;}");
    connect(btnPanic, &QToolButton::clicked, this, [this]() {
        m_model->cues.panic();
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnPanic);

    hlay->addStretch();

    // Device button (right-aligned)
    auto* btnDev = makeIconBtn("⚙", "Audio Device settings…");
    connect(btnDev, &QToolButton::clicked, this, &MainWindow::onOpenDeviceDialog);
    hlay->addWidget(btnDev);
}

QToolButton* MainWindow::makeIconBtn(const QString& icon, const QString& tip,
                                     const QString& extraStyle) {
    auto* btn = new QToolButton;
    btn->setText(icon);
    btn->setToolTip(tip);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setFixedSize(32, 32);
    btn->setStyleSheet(QString(kIconBtnBase) + extraStyle);
    return btn;
}

// ── Splitter (cue table + inspector) ──────────────────────────────────────

void MainWindow::buildSplitter() {
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(4);
    m_splitter->setStyleSheet("QSplitter::handle { background: #2a2a2a; }");

    m_cueTable  = new CueTableView(m_model, m_splitter);
    m_inspector = new InspectorWidget(m_model, m_splitter);
    m_inspector->setMinimumHeight(150);

    m_splitter->addWidget(m_cueTable);
    m_splitter->addWidget(m_inspector);
    // Initially cue table ~65%, inspector ~35%
    m_splitter->setSizes({400, 220});
}

// ── menu bar ───────────────────────────────────────────────────────────────

void MainWindow::buildMenuBar() {
    auto* mb = menuBar();
    menuBar()->setNativeMenuBar(false);   // embed in window so stylesheet applies
    mb->setStyleSheet("QMenuBar{background:#111;color:#ccc;border-bottom:1px solid #2a2a2a;}"
                      "QMenuBar::item{padding:4px 8px;}"
                      "QMenuBar::item:selected{background:#2a2a2a;}"
                      "QMenu{background:#1a1a1a;color:#ccc;border:1px solid #333;}"
                      "QMenu::item:selected{background:#2a6ab8;}"
                      "QMenu::separator{height:1px;background:#333;margin:3px 0;}");

    auto* fileMenu = mb->addMenu("&File");

    auto mkAct = [&](QMenu* menu, const QString& text, QKeySequence ks,
                     auto* recv, auto slot) {
        auto* a = new QAction(text, menu);
        a->setShortcut(ks);
        connect(a, &QAction::triggered, recv, slot);
        menu->addAction(a);
        return a;
    };

    mkAct(fileMenu, "&New Show",  QKeySequence::New,  this,  &MainWindow::onNewShow);
    mkAct(fileMenu, "&Open…",     QKeySequence::Open, this,  &MainWindow::onOpenShow);
    m_actSave = mkAct(fileMenu, "&Save", QKeySequence::Save, this, &MainWindow::onSaveShow);
    m_actSave->setEnabled(false);
    mkAct(fileMenu, "Save &As…",
          QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
          this, &MainWindow::onSaveShowAs);
    fileMenu->addSeparator();
    mkAct(fileMenu, "&Quit", QKeySequence::Quit, qApp, &QApplication::quit);

    auto* editMenu = mb->addMenu("&Edit");
    m_actUndo = editMenu->addAction("&Undo");
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actUndo->setEnabled(false);
    connect(m_actUndo, &QAction::triggered, this, &MainWindow::onUndo);

    m_actRedo = editMenu->addAction("&Redo");
    m_actRedo->setShortcut(QKeySequence::Redo);
    m_actRedo->setEnabled(false);
    connect(m_actRedo, &QAction::triggered, this, &MainWindow::onRedo);

    auto* showMenu = mb->addMenu("&Show");
    showMenu->addAction("Audio &Device…", this, &MainWindow::onOpenDeviceDialog);
    showMenu->addSeparator();

    auto* actPanic = showMenu->addAction("Panic", this, [this]() {
        m_model->cues.panic();
        m_inspector->clearTimelineArm();
    });
    actPanic->setShortcut(QKeySequence(Qt::Key_Escape));
}

// ── slots ──────────────────────────────────────────────────────────────────

void MainWindow::onTick() {
    m_model->tick();
    m_cueTable->refreshStatus();
    m_inspector->updatePlayhead();

    // Sync UI selection to engine's selectedIndex (advances after go()).
    const int engineSel  = m_model->cues.selectedIndex();
    const int cueCount   = m_model->cues.cueCount();
    const int currentRow = m_cueTable->selectedRow();
    if (engineSel != currentRow) {
        if (engineSel >= 0 && engineSel < cueCount) {
            m_cueTable->syncEngineSelection(engineSel);
        } else if (engineSel >= cueCount && currentRow >= 0) {
            // Past end of list — deselect all.
            m_cueTable->syncEngineSelection(-1);
        }
    }

    // Global MC indicator: find outermost playing cue with MC
    int globalMCIdx = -1;
    for (int i = 0; i < cueCount; i++) {
        const auto* c = m_model->cues.cueAt(i);
        if (!c || !c->musicContext) continue;
        if (!m_model->cues.isCuePlaying(i)) continue;
        // Outermost = parent has no MC (or top-level)
        if (c->parentIndex < 0) { globalMCIdx = i; break; }
        const auto* par = m_model->cues.cueAt(c->parentIndex);
        if (!par || !par->musicContext) { globalMCIdx = i; break; }
    }
    if (globalMCIdx >= 0) {
        const auto* c  = m_model->cues.cueAt(globalMCIdx);
        const auto* mc = c->musicContext.get();
        const double cueRelSec = m_model->cues.cueElapsedSeconds(globalMCIdx);
        const auto   pos = mc->secondsToMusical(cueRelSec);
        const double bpm = mc->bpmAt(pos.bar, pos.beat, pos.fraction);
        const auto   ts  = mc->timeSigAt(pos.bar, pos.beat);
        m_lblGlobalMC->setText(
            QString("♪ %1  %2/%3  %4|%5")
                .arg(bpm, 0, 'f', 1).arg(ts.num).arg(ts.den)
                .arg(pos.bar).arg(pos.beat));
        m_lblGlobalMC->show();
    } else {
        m_lblGlobalMC->hide();
    }
}

void MainWindow::onNewShow() {
    if (!confirmDirty()) return;
    m_model->sf = mcp::ShowFile::empty();
    m_model->showPath.clear();
    m_model->baseDir.clear();
    m_model->dirty = false;
    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_cueTable->refresh();
    m_inspector->setCueIndex(-1);
    updateCueInfo();
    updateTitle();
    m_actSave->setEnabled(false);
}

void MainWindow::onOpenShow() {
    if (!confirmDirty()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, "Open Show", {}, "MCP Show (*.mcp);;All Files (*)");
    if (!path.isEmpty()) loadShowFile(path);
}

void MainWindow::onSaveShow() {
    if (m_model->showPath.empty()) { onSaveShowAs(); return; }
    ShowHelpers::syncSfFromCues(*m_model);
    ShowHelpers::saveShow(*m_model);
    updateTitle();
    m_actSave->setEnabled(false);
}

void MainWindow::onSaveShowAs() {
    const QString path = QFileDialog::getSaveFileName(
        this, "Save Show As", {}, "MCP Show (*.mcp);;All Files (*)");
    if (path.isEmpty()) return;
    m_model->showPath = path.toStdString();
    m_model->baseDir  = std::filesystem::path(m_model->showPath).parent_path().string();
    onSaveShow();
}

void MainWindow::onOpenDeviceDialog() {
    DeviceDialog dlg(m_model, this);
    dlg.exec();
}

void MainWindow::onCueListModified() {
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    m_cueTable->refresh();
    m_actUndo->setEnabled(m_model->canUndo());
    m_actRedo->setEnabled(m_model->canRedo());
    updateTitle();
}

void MainWindow::onUndo() {
    if (!m_model->canUndo()) return;
    if (!m_model->sf.cueLists.empty())
        m_model->redoStack.push_back(m_model->sf.cueLists[0].cues);
    m_model->sf.cueLists[0].cues = std::move(m_model->undoStack.back());
    m_model->undoStack.pop_back();
    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    m_cueTable->refresh();
    // Reload inspector so faders/spinboxes/waveform reflect the restored state.
    const int row     = m_cueTable->selectedRow();
    const int savedTab = m_inspector->currentTabIndex();
    m_inspector->setCueIndex(-1);
    m_inspector->setCueIndex(row);
    m_inspector->restoreTabIndex(savedTab);
    m_actUndo->setEnabled(m_model->canUndo());
    m_actRedo->setEnabled(m_model->canRedo());
    updateTitle();
}

void MainWindow::onRedo() {
    if (!m_model->canRedo()) return;
    if (!m_model->sf.cueLists.empty())
        m_model->undoStack.push_back(m_model->sf.cueLists[0].cues);
    m_model->sf.cueLists[0].cues = std::move(m_model->redoStack.back());
    m_model->redoStack.pop_back();
    std::string err;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    m_cueTable->refresh();
    const int row      = m_cueTable->selectedRow();
    const int savedTab = m_inspector->currentTabIndex();
    m_inspector->setCueIndex(-1);
    m_inspector->setCueIndex(row);
    m_inspector->restoreTabIndex(savedTab);
    m_actUndo->setEnabled(m_model->canUndo());
    m_actRedo->setEnabled(m_model->canRedo());
    updateTitle();
}

void MainWindow::onRowSelected(int idx) {
    if (idx >= 0) m_model->cues.setSelectedIndex(idx);
    m_model->multiSel.clear();
    if (idx >= 0) m_model->multiSel.insert(idx);
    m_inspector->setCueIndex(idx);
    updateCueInfo();
}

// ── drag & drop ────────────────────────────────────────────────────────────

void MainWindow::dragEnterEvent(QDragEnterEvent* ev) {
    if (ev->mimeData()->hasUrls()) ev->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* ev) {
    for (const QUrl& u : ev->mimeData()->urls()) {
        if (!u.isLocalFile()) continue;
        const QString p = u.toLocalFile();
        if (p.endsWith(".mcp", Qt::CaseInsensitive)) {
            if (!confirmDirty()) return;
            loadShowFile(p);
            break;
        }
    }
    ev->acceptProposedAction();
}

// ── close & resize ─────────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* ev) {
    if (!confirmDirty()) { ev->ignore(); return; }
    m_timer->stop();
    m_model->engine.shutdown();
    ev->accept();
}

void MainWindow::resizeEvent(QResizeEvent* ev) {
    QMainWindow::resizeEvent(ev);
    if (m_toastLabel && m_toastLabel->isVisible()) {
        const int tw = m_toastLabel->width();
        const int th = m_toastLabel->height();
        m_toastLabel->move((width() - tw) / 2, height() - th - 50);
    }
}

// ── helpers ────────────────────────────────────────────────────────────────

bool MainWindow::confirmDirty() {
    if (!m_model->dirty) return true;
    return QMessageBox::question(
        this, "Unsaved changes",
        "The current show has unsaved changes. Discard them?",
        QMessageBox::Discard | QMessageBox::Cancel) == QMessageBox::Discard;
}

void MainWindow::loadShowFile(const QString& path) {
    std::string err;
    if (!m_model->sf.load(path.toStdString(), err)) {
        QMessageBox::critical(this, "Load error",
            QString("Could not load show:\n%1").arg(QString::fromStdString(err)));
        return;
    }
    m_model->showPath = path.toStdString();
    m_model->baseDir  = std::filesystem::path(m_model->showPath).parent_path().string();
    m_model->dirty    = false;
    ShowHelpers::rebuildCueList(*m_model, err);
    m_cueTable->refresh();
    m_inspector->setCueIndex(-1);
    updateCueInfo();
    updateTitle();
    m_actSave->setEnabled(false);
    showToast("Loaded: " + QString::fromStdString(
        std::filesystem::path(path.toStdString()).filename().string()));
}

void MainWindow::updateTitle() {
    const QString name = m_model->showPath.empty()
        ? "Untitled"
        : QString::fromStdString(
              std::filesystem::path(m_model->showPath).stem().string());
    setWindowTitle(name + (m_model->dirty ? " *" : "") + " — Music Cue Player");
}

void MainWindow::updateCueInfo() {
    const int idx = m_cueTable->selectedRow();
    const mcp::Cue* c = (idx >= 0) ? m_model->cues.cueAt(idx) : nullptr;
    if (!c) {
        m_lblCueName->setText("—");
        m_lblCueDetail->clear();
        return;
    }
    const QString name = QString::fromStdString(c->name.empty() ? c->path : c->name);
    m_lblCueName->setText(name.isEmpty() ? "—" : name);

    // Detail line: "Q3 · Audio · 00:56.000"  (Group leads: "Group · Q5 · 3 cues")
    QString detail;
    const QString cueNum = QString::fromStdString(c->cueNumber);

    if (c->type == mcp::CueType::Group) {
        detail = "Group";
        if (!cueNum.isEmpty()) detail += "  ·  " + cueNum;
        if (c->childCount > 0)
            detail += QString("  ·  %1 cue%2").arg(c->childCount)
                                               .arg(c->childCount == 1 ? "" : "s");
    } else {
        detail = cueNum;
        if (!detail.isEmpty()) detail += "  ·  ";
        switch (c->type) {
            case mcp::CueType::Audio:  detail += "Audio";  break;
            case mcp::CueType::Start:  detail += "Start";  break;
            case mcp::CueType::Stop:   detail += "Stop";   break;
            case mcp::CueType::Fade:   detail += "Fade";   break;
            case mcp::CueType::Arm:    detail += "Arm";    break;
            case mcp::CueType::Devamp: detail += "Devamp"; break;
            case mcp::CueType::Group:  break;  // handled above
        }
    }

    if (c->type == mcp::CueType::Audio && c->duration > 0.0)
        detail += "  ·  " + QString::fromStdString(ShowHelpers::fmtDuration(c->duration));

    m_lblCueDetail->setText(detail);
}

void MainWindow::showToast(const QString& msg, int ms) {
    m_toastLabel->setText("  " + msg + "  ");
    m_toastLabel->adjustSize();
    const int tw = m_toastLabel->width();
    const int th = m_toastLabel->height();
    m_toastLabel->setGeometry((width()-tw)/2, height()-th-50, tw, th);
    m_toastLabel->show();
    m_toastLabel->raise();
    m_toastTimer->start(ms);
}
