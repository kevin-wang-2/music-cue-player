#include "MainWindow.h"
#include "AppModel.h"
#include "CollectDialog.h"
#include "MissingMediaDialog.h"
#include "CueListPanel.h"
#include "ProjectStatusDialog.h"
#include "ScriptletLibraryDialog.h"
#include "ShowInfoDialog.h"
#include "SettingsDialog.h"
#include "CueTableView.h"

#include "InspectorWidget.h"
#include "RenumberDialog.h"
#include "ShowHelpers.h"

#include "engine/CueList.h"
#include "engine/MusicContext.h"
#include "engine/ShowFile.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
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
#include <QFileOpenEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <filesystem>

// ── Built-in scriptlets directory ─────────────────────────────────────────

static QString findScriptletsDir() {
    const QString appDir = QCoreApplication::applicationDirPath();
    // Production .app bundle: Contents/MacOS → Contents/Resources/scriptlets
    {
        QDir d(appDir + "/../Resources/scriptlets");
        if (d.exists()) return d.canonicalPath();
    }
    // Dev build: build/bin/app.app/Contents/MacOS → project-root/scriptlets (5 up)
    {
        QDir d(appDir + "/../../../../../scriptlets");
        if (d.exists()) return d.canonicalPath();
    }
    // Flat layout: scriptlets/ next to binary
    {
        QDir d(appDir + "/scriptlets");
        if (d.exists()) return d.canonicalPath();
    }
    return {};
}

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

    // Central widget: horizontal — mainArea (stretches) + CueListPanel (right sidebar)
    auto* central = new QWidget(this);
    central->setObjectName("central");
    setCentralWidget(central);

    auto* hlay = new QHBoxLayout(central);
    hlay->setContentsMargins(0, 0, 0, 0);
    hlay->setSpacing(0);

    // Main area (vertical stack)
    auto* mainArea = new QWidget(central);
    auto* vlay = new QVBoxLayout(mainArea);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    buildGoBar();
    buildIconBar();
    buildSplitter();

    vlay->addWidget(findChild<QWidget*>("goBar"));
    vlay->addWidget(findChild<QWidget*>("iconBar"));
    vlay->addWidget(m_splitter, 1);

    hlay->addWidget(mainArea, 1);

    // Right sidebar — CueListPanel
    m_listPanel = new CueListPanel(model, central);
    m_listPanel->setVisible(false);  // hidden by default; toggle via View menu
    hlay->addWidget(m_listPanel, 0);

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
    connect(m_model, &AppModel::activeListChanged, m_cueTable,  &CueTableView::refresh);
    connect(m_model, &AppModel::activeListChanged, this, [this](int) {
        m_inspector->setCueIndex(-1);
        updateCueInfo();
    });
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

    // Start external trigger infrastructure
    m_model->applyMidiInput();
    m_model->applyOscSettings();
    // Load built-in scriptlets from scriptlets/ dir (merged with show's user entries).
    m_model->loadBuiltinScriptlets(findScriptletsDir());
    // applyScriptletLibrary() is called inside loadBuiltinScriptlets.

    // App-level event filter for per-cue hotkey triggers
    qApp->installEventFilter(this);
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
        m_model->go();
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
                ShowHelpers::sfInsertBefore(m_model->sf, m_model->activeListIdx(), -1, std::move(cd));
                std::string err;
                ShowHelpers::rebuildAllCueLists(*m_model, err);
                onCueListModified();
                m_cueTable->refresh();
                m_cueTable->selectRow(m_model->cues().cueCount() - 1);
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
            if (typeStr == "marker") {
                const auto* tc = m_model->cues().cueAt(selRow);
                if (tc && tc->type == mcp::CueType::Audio) {
                    cd.target      = selRow;
                    cd.markerIndex = -1;
                    cd.targetCueNumber = tc->cueNumber;
                }
            }
        }

        // Insert after the selected cue and all its children (for Group cues).
        int ins = (selRow >= 0) ? selRow + 1 : -1;
        if (selRow >= 0) {
            const auto* sc = m_model->cues().cueAt(selRow);
            if (sc && sc->type == mcp::CueType::Group)
                ins = selRow + 1 + sc->childCount;
        }
        ShowHelpers::sfInsertBefore(m_model->sf, m_model->activeListIdx(), ins, std::move(cd));
        std::string err;
        ShowHelpers::rebuildAllCueLists(*m_model, err);
        const int newRow = (ins >= 0) ? ins : m_model->cues().cueCount() - 1;
        m_model->cues().setSelectedIndex(newRow);
        onCueListModified();
        m_cueTable->refresh();
        m_cueTable->selectRow(newRow);
    };

    // Add-cue type buttons grouped by category.
    // A null entry inserts a visual separator between groups.
    struct CueBtnDef { const char* icon; const char* tip; const char* type; };
    const CueBtnDef cueBtns[] = {
        { "▤",  "Add Group cue",          "group"  },
        { nullptr, nullptr, nullptr },
        { "♫",  "Add Audio cue",          "audio"  },
        { "♩",  "Add Music Context cue",  "mc"     },
        { nullptr, nullptr, nullptr },
        { "〰", "Add Fade cue",           "fade"   },
        { nullptr, nullptr, nullptr },
        { "▷",  "Add Start cue",          "start"  },
        { "□",  "Add Stop cue",           "stop"   },
        { "→",  "Add Goto cue",           "goto"   },
        { "⊙",  "Add Arm cue",            "arm"    },
        { "⤴",  "Add Devamp cue",         "devamp"   },
        { "◈",  "Add Marker cue",         "marker"   },
        { "✎",  "Add Memo cue",           "memo"      },
        { "λ",  "Add Scriptlet cue",     "scriptlet" },
        { nullptr, nullptr, nullptr },
        { "⊹",  "Add Network cue",        "network"  },
        { "♪",  "Add MIDI cue",           "midi"     },
        { "TC", "Add Timecode cue",       "timecode" },
    };
    for (const auto& b : cueBtns) {
        if (!b.type) {
            auto* s = new QFrame(bar);
            s->setFrameShape(QFrame::VLine);
            s->setStyleSheet("color:#333; margin:4px 2px;");
            hlay->addWidget(s);
        } else {
            auto* btn = makeIconBtn(b.icon, b.tip);
            connect(btn, &QToolButton::clicked, this, [=]() { addCue(b.type); });
            hlay->addWidget(btn);
        }
    }

    // Separator before playback controls
    auto* sep = new QFrame(bar);
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet("color:#333; margin:4px 4px;");
    hlay->addWidget(sep);

    // Playback controls
    auto* btnGo = makeIconBtn("▶", "Go  [Space]",
        "QToolButton:hover{background:#1a4d1a;border-color:#2a7a2a;color:#5f5;}"
        "QToolButton:pressed{background:#0f2f0f;}");
    connect(btnGo, &QToolButton::clicked, this, [this]() {
        m_model->go();
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnGo);

    auto* btnStop = makeIconBtn("■", "Stop selected  [Esc]");
    connect(btnStop, &QToolButton::clicked, this, [this]() {
        const int sel = m_cueTable->selectedRow();
        if (sel >= 0) m_model->cues().stop(sel);
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnStop);

    auto* btnPanic = makeIconBtn("✕", "Panic — stop all  [⇧Esc]",
        "QToolButton:hover{background:#4d1a1a;border-color:#aa2222;color:#f88;}"
        "QToolButton:pressed{background:#2f0f0f;}");
    connect(btnPanic, &QToolButton::clicked, this, [this]() {
        m_model->panicAll();
        m_inspector->clearTimelineArm();
    });
    hlay->addWidget(btnPanic);

    hlay->addStretch();
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
    mkAct(fileMenu, "Collect All Files…", {},
          this, &MainWindow::onCollectAllFiles);
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

    editMenu->addSeparator();
    auto* actRenumber = editMenu->addAction("Renumber Cues…");
    actRenumber->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(actRenumber, &QAction::triggered, this, &MainWindow::onRenumberCues);

    auto* showMenu = mb->addMenu("&Show");
    showMenu->addAction("&Settings…", this, &MainWindow::onOpenSettings);
    showMenu->addAction("Show &Information…", this, [this]() {
        if (!m_showInfoDialog)
            m_showInfoDialog = new ShowInfoDialog(m_model, this);
        m_showInfoDialog->show();
        m_showInfoDialog->raise();
        m_showInfoDialog->activateWindow();
    });
    showMenu->addAction("Project &Status…", this, [this]() {
        if (!m_statusDialog)
            m_statusDialog = new ProjectStatusDialog(m_model, this);
        m_statusDialog->show();
        m_statusDialog->raise();
        m_statusDialog->activateWindow();
        m_statusDialog->refreshWarnings();
    });
    showMenu->addAction("Scriptlet &Library…", this, [this]() {
        if (!m_libraryDialog)
            m_libraryDialog = new ScriptletLibraryDialog(m_model, this);
        m_libraryDialog->show();
        m_libraryDialog->raise();
        m_libraryDialog->activateWindow();
    });
    showMenu->addAction("&Missing Media…", this, &MainWindow::onMissingMedia);
    showMenu->addSeparator();

    auto* actPanic = showMenu->addAction("Panic", this, [this]() {
        m_model->panicAll();
        m_inspector->clearTimelineArm();
    });
    actPanic->setShortcut(QKeySequence(Qt::Key_Escape));

    // View menu — panel toggles
    auto* viewMenu = mb->addMenu("&View");

    m_actInspector = viewMenu->addAction("&Inspector");
    m_actInspector->setCheckable(true);
    m_actInspector->setChecked(true);  // open by default
    m_actInspector->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    connect(m_actInspector, &QAction::toggled, this, [this](bool on) {
        m_inspector->setVisible(on);
    });

    m_actListPanel = viewMenu->addAction("&Cue Lists Sidebar");
    m_actListPanel->setCheckable(true);
    m_actListPanel->setChecked(false);
    m_actListPanel->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(m_actListPanel, &QAction::toggled, this, [this](bool on) {
        m_listPanel->setVisible(on);
    });
}

// ── slots ──────────────────────────────────────────────────────────────────

void MainWindow::onTick() {
    m_model->tick();
    m_cueTable->refreshStatus();
    if (m_listPanel->isVisible()) m_listPanel->refresh();
    m_inspector->updatePlayhead();

    // Sync UI selection to engine's selectedIndex (advances after go()).
    const int engineSel  = m_model->cues().selectedIndex();
    const int cueCount   = m_model->cues().cueCount();
    const int currentRow = m_cueTable->selectedRow();
    if (engineSel != currentRow) {
        if (engineSel >= 0 && engineSel < cueCount) {
            m_cueTable->syncEngineSelection(engineSel);
        } else if (engineSel >= cueCount && currentRow >= 0) {
            // Past end of list — deselect all.
            m_cueTable->syncEngineSelection(-1);
        }
    }

    // Global MC indicator: find playing cue with MC
    // Priority 1: dedicated MusicContext cue
    int globalMCIdx = -1;
    for (int i = 0; i < cueCount && globalMCIdx < 0; ++i) {
        const auto* c = m_model->cues().cueAt(i);
        if (!c || c->type != mcp::CueType::MusicContext || !m_model->cues().hasMusicContext(i)) continue;
        if (m_model->cues().isCuePlaying(i)) globalMCIdx = i;
    }
    // Priority 2: audio/group cue with MC (outermost)
    for (int i = 0; i < cueCount && globalMCIdx < 0; ++i) {
        const auto* c = m_model->cues().cueAt(i);
        if (!c || !m_model->cues().hasMusicContext(i)) continue;
        if (c->type == mcp::CueType::MusicContext) continue;  // already handled above
        if (!m_model->cues().isCuePlaying(i)) continue;
        // Outermost = parent has no MC (or top-level)
        if (c->parentIndex < 0) { globalMCIdx = i; break; }
        if (!m_model->cues().hasMusicContext(c->parentIndex)) { globalMCIdx = i; break; }
    }
    if (globalMCIdx >= 0) {
        const auto* mc = m_model->cues().musicContextOf(globalMCIdx);
        const double cueRelSec = m_model->cues().cueElapsedSeconds(globalMCIdx);
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
    m_model->applyScriptletLibrary();
    ShowHelpers::normalizeListRefs(m_model->sf);
    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);
    m_cueTable->refresh();
    m_inspector->setCueIndex(-1);
    updateCueInfo();
    updateTitle();
    m_actSave->setEnabled(false);
    if (m_libraryDialog) m_libraryDialog->refreshList();
}

void MainWindow::onOpenShow() {
    if (!confirmDirty()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, "Open Show", {}, "MCP Show (*.mcp);;All Files (*)");
    if (!path.isEmpty()) loadShowFile(path);
}

void MainWindow::onSaveShow() {
    if (m_model->showPath.empty()) { onSaveShowAs(); return; }
    saveUiState();
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

void MainWindow::onCollectAllFiles() {
    CollectDialog dlg(m_model, this);
    dlg.exec();
}

void MainWindow::onMissingMedia() {
    if (!m_missingMediaDialog) {
        m_missingMediaDialog = new MissingMediaDialog(m_model, this);
        connect(m_missingMediaDialog, &MissingMediaDialog::mediaFixed, this, [this]() {
            m_cueTable->refresh();
            m_inspector->setCueIndex(-1);
            updateTitle();
        });
    }
    m_missingMediaDialog->refresh();
    m_missingMediaDialog->show();
    m_missingMediaDialog->raise();
    m_missingMediaDialog->activateWindow();
}

void MainWindow::checkMissingMedia() {
    const auto missing = ShowHelpers::findMissingMedia(*m_model);
    if (missing.empty()) return;
    showToast(QString("%1 missing media file(s) — see Show → Missing Media")
                  .arg((int)missing.size()), 5000);
    onMissingMedia();
}

void MainWindow::onOpenSettings() {
    SettingsDialog dlg(m_model, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_model->sf.audioSetup      = dlg.audioResult();
    m_model->sf.networkSetup    = dlg.networkResult();
    m_model->sf.midiSetup       = dlg.midiResult();
    m_model->sf.oscServer       = dlg.oscResult();
    m_model->sf.systemControls  = dlg.controlsResult();
    m_model->applyOscSettings();
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);

    // Re-initialize engine when multi-device config is present
    const auto& setup = m_model->sf.audioSetup;
    if (!setup.devices.empty()) {
        if (m_model->engineOk) m_model->engine.shutdown();
        std::vector<mcp::AudioEngine::DeviceSpec> specs;
        for (const auto& d : setup.devices) {
            mcp::AudioEngine::DeviceSpec sp;
            sp.name         = d.name;
            sp.channelCount = d.channelCount;
            sp.bufferSize   = d.bufferSize;
            sp.masterClock  = d.masterClock;
            specs.push_back(sp);
        }
        const int sr = (setup.sampleRate > 0) ? setup.sampleRate
                     : m_model->sf.engine.sampleRate;
        m_model->engineOk = m_model->engine.initialize(sr, specs);
        emit m_model->engineStatusChanged();
    }

    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);
    m_inspector->setCueIndex(m_model->cues().selectedIndex());
    updateTitle();
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
        m_model->redoStack.push_back(m_model->sf.cueLists);
    m_model->sf.cueLists = std::move(m_model->undoStack.back());
    m_model->undoStack.pop_back();
    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);
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
        m_model->undoStack.push_back(m_model->sf.cueLists);
    m_model->sf.cueLists = std::move(m_model->redoStack.back());
    m_model->redoStack.pop_back();
    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);
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

void MainWindow::onRenumberCues() {
    if (m_model->sf.cueLists.empty()) return;

    // Collect selected rows; fall back to all rows if nothing selected.
    std::vector<int> rows;
    for (const auto& idx : m_cueTable->selectionModel()->selectedRows())
        rows.push_back(idx.row());
    if (rows.empty()) {
        const int n = m_model->cues().cueCount();
        for (int i = 0; i < n; ++i) rows.push_back(i);
    }
    std::sort(rows.begin(), rows.end());

    RenumberDialog dlg(static_cast<int>(rows.size()), this);
    if (dlg.exec() != QDialog::Accepted) return;

    const bool doRe  = dlg.renumberChecked();
    const bool doPre = dlg.prefixChecked();
    const bool doSuf = dlg.suffixChecked();
    if (!doRe && !doPre && !doSuf) return;

    const double start = dlg.startAt();
    const double inc   = dlg.incrementBy();
    const QString pfx  = doPre ? dlg.prefix()  : QString{};
    const QString sfx  = doSuf ? dlg.suffix()  : QString{};

    // Format a double as a cue number string (integer when possible)
    auto fmtNum = [](double v) -> std::string {
        if (v == std::floor(v) && std::abs(v) < 1e9)
            return std::to_string(static_cast<long long>(v));
        return std::to_string(v);  // fallback (rarely used)
    };

    m_model->pushUndo();

    int reIdx = 0;
    for (const int row : rows) {
        auto* cd = ShowHelpers::sfCueAt(m_model->sf, m_model->activeListIdx(), row);
        if (!cd) continue;

        std::string base;
        if (doRe) {
            base = fmtNum(start + reIdx * inc);
            ++reIdx;
        } else {
            // Without Renumber: only modify cues that already have a number.
            if (cd->cueNumber.empty()) continue;
            base = cd->cueNumber;
        }

        cd->cueNumber = pfx.toStdString() + base + sfx.toStdString();
    }

    std::string err;
    ShowHelpers::rebuildAllCueLists(*m_model, err);
    onCueListModified();
    m_cueTable->refresh();
}

void MainWindow::onRowSelected(int idx) {
    if (idx >= 0) m_model->cues().setSelectedIndex(idx);
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

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::FileOpen) {
        const QString path = static_cast<QFileOpenEvent*>(ev)->file();
        if (!path.isEmpty() && path.endsWith(".mcp", Qt::CaseInsensitive)) {
            if (confirmDirty()) loadShowFile(path);
            return true;
        }
    }
    if (ev->type() != QEvent::KeyPress)
        return QMainWindow::eventFilter(obj, ev);

    // Don't intercept keys typed into text-input widgets
    QWidget* fw = QApplication::focusWidget();
    if (qobject_cast<QLineEdit*>(fw)      || qobject_cast<QTextEdit*>(fw) ||
        qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QSpinBox*>(fw)  ||
        qobject_cast<QDoubleSpinBox*>(fw))
        return QMainWindow::eventFilter(obj, ev);

    auto* ke = static_cast<QKeyEvent*>(ev);
    const int k = ke->key();
    // Ignore bare modifier presses
    if (k == Qt::Key_Control || k == Qt::Key_Shift ||
        k == Qt::Key_Alt     || k == Qt::Key_Meta)
        return QMainWindow::eventFilter(obj, ev);

    const QKeySequence ks(ke->modifiers() | k);
    const std::string ksStr = ks.toString(QKeySequence::PortableText).toStdString();

    // Hotkeys fire on all lists, including background ones.
    bool hotkeyFired = false;
    for (int li = 0; li < m_model->listCount(); ++li) {
        if (li >= (int)m_model->sf.cueLists.size()) break;
        auto& engList = m_model->cueListAt(li);
        int counter = 0;
        std::function<void(const std::vector<mcp::ShowFile::CueData>&)> visit;
        visit = [&](const std::vector<mcp::ShowFile::CueData>& cues) {
            for (const auto& cd : cues) {
                const int flatIdx = counter++;
                const auto& ht = cd.triggers.hotkey;
                if (ht.enabled && !ht.keyString.empty() && ht.keyString == ksStr) {
                    engList.start(flatIdx);
                    if (li == m_model->activeListIdx()) emit m_model->externalTriggerFired(flatIdx);
                    emit m_model->playbackStateChanged();
                    hotkeyFired = true;
                    return;
                }
                if (!cd.children.empty()) visit(cd.children);
            }
        };
        visit(m_model->sf.cueLists[static_cast<size_t>(li)].cues);
    }
    if (hotkeyFired) return true;  // consume event — don't propagate to Go button etc.
    return QMainWindow::eventFilter(obj, ev);
}

// ── helpers ────────────────────────────────────────────────────────────────

bool MainWindow::confirmDirty() {
    if (!m_model->dirty) return true;
    return QMessageBox::question(
        this, "Unsaved changes",
        "The current show has unsaved changes. Discard them?",
        QMessageBox::Discard | QMessageBox::Cancel) == QMessageBox::Discard;
}

void MainWindow::saveUiState() {
    auto& h = m_model->sf.uiHints;
    h.set("inspector.visible",  m_inspector->isVisible()  ? "1" : "0");
    h.set("listPanel.visible",  m_listPanel->isVisible()  ? "1" : "0");
    h.set("showInfo.visible",   (m_showInfoDialog  && m_showInfoDialog->isVisible())  ? "1" : "0");
    h.set("projectStatus.visible", (m_statusDialog && m_statusDialog->isVisible())   ? "1" : "0");
    const QList<int> sizes = m_splitter->sizes();
    if (sizes.size() >= 2)
        h.set("inspector.height", std::to_string(sizes[1]));
}

void MainWindow::loadUiState() {
    const auto& h = m_model->sf.uiHints;

    const bool inspVis   = h.get("inspector.visible",      "1") != "0";
    const bool panelVis  = h.get("listPanel.visible",      "0") != "0";
    const bool infoVis   = h.get("showInfo.visible",       "0") != "0";
    const bool statusVis = h.get("projectStatus.visible",  "0") != "0";

    // Drive through the actions so the checkmarks stay in sync.
    m_actInspector->setChecked(inspVis);
    m_actListPanel->setChecked(panelVis);

    const std::string heightStr = h.get("inspector.height", "");
    if (!heightStr.empty() && inspVis) {
        try {
            const int h2 = std::stoi(heightStr);
            if (h2 > 0) {
                const int total = m_splitter->sizes()[0] + m_splitter->sizes()[1];
                m_splitter->setSizes({total - h2, h2});
            }
        } catch (...) {}
    }

    if (infoVis) {
        if (!m_showInfoDialog) m_showInfoDialog = new ShowInfoDialog(m_model, this);
        m_showInfoDialog->show();
    }
    if (statusVis) {
        if (!m_statusDialog) m_statusDialog = new ProjectStatusDialog(m_model, this);
        m_statusDialog->refreshWarnings();
        m_statusDialog->show();
    }
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
    m_model->applyOscSettings();
    m_model->applyScriptletLibrary();
    ShowHelpers::normalizeListRefs(m_model->sf);  // resolve any -1 list refs from older files
    ShowHelpers::rebuildAllCueLists(*m_model, err);
    m_cueTable->refresh();
    m_inspector->setCueIndex(-1);
    loadUiState();
    updateCueInfo();
    updateTitle();
    m_actSave->setEnabled(false);
    if (m_libraryDialog) m_libraryDialog->refreshList();
    showToast("Loaded: " + QString::fromStdString(
        std::filesystem::path(path.toStdString()).filename().string()));
    checkMissingMedia();
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
    const mcp::Cue* c = (idx >= 0) ? m_model->cues().cueAt(idx) : nullptr;
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
            case mcp::CueType::Audio:        detail += "Audio";    break;
            case mcp::CueType::Start:        detail += "Start";    break;
            case mcp::CueType::Stop:         detail += "Stop";     break;
            case mcp::CueType::Fade:         detail += "Fade";     break;
            case mcp::CueType::Arm:          detail += "Arm";      break;
            case mcp::CueType::Devamp:       detail += "Devamp";   break;
            case mcp::CueType::MusicContext: detail += "MC";       break;
            case mcp::CueType::Marker:       detail += "Marker";   break;
            case mcp::CueType::Network:      detail += "Network";  break;
            case mcp::CueType::Midi:         detail += "MIDI";     break;
            case mcp::CueType::Timecode:     detail += "Timecode"; break;
            case mcp::CueType::Goto:         detail += "Goto";     break;
            case mcp::CueType::Memo:         detail += "Memo";     break;
            case mcp::CueType::Scriptlet:    detail += "Script";   break;
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
