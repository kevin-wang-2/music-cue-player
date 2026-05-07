#include "InspectorWidget.h"
#include "AppModel.h"
#include "FaderWidget.h"
#include "MusicContextView.h"
#include "ShowHelpers.h"
#include "SyncGroupView.h"
#include "TimelineGroupView.h"
#include "WaveformView.h"

#include "engine/CueList.h"
#include "engine/MusicContext.h"

#include "engine/Cue.h"
#include "engine/FadeData.h"

#include <algorithm>
#include <optional>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
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
#include <QVBoxLayout>

// ── style helpers ──────────────────────────────────────────────────────────

static const char* kXpCellStyle =
    "QLineEdit { background:#1a1a1a; color:#dddddd; border:1px solid #383838;"
    "  border-radius:2px; padding:1px 3px; font-size:11px; }"
    "QLineEdit:focus { border-color:#2a6ab8; }"
    "QLineEdit[disabled='true'] { background:#111; color:#444; }";

static QLineEdit* makeXpCell() {
    auto* e = new QLineEdit;
    e->setFixedSize(46, 22);
    e->setAlignment(Qt::AlignHCenter);
    e->setStyleSheet(kXpCellStyle);
    return e;
}

// Format a dB value for display in a crosspoint cell.
// Values at or below kFaderMin (-60) are stored as kFaderInf (-144) and shown as "-inf".
static QString fmtXpDb(float dB) {
    if (dB <= FaderWidget::kFaderMin + 0.05f) return QStringLiteral("-inf");
    return QString::number(static_cast<double>(dB), 'f', 1);
}

// ── ctor ───────────────────────────────────────────────────────────────────

InspectorWidget::InspectorWidget(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget(this);
    lay->addWidget(m_tabs);

    buildBasicTab();
    buildMCTab();
    buildLevelsTab();
    buildTrimTab();
    buildCurveTab();
    buildModeTab();
    buildTimeTab();
    buildTimelineTab();
}

// ── tab builders ───────────────────────────────────────────────────────────

void InspectorWidget::buildBasicTab() {
    m_basicPage = new QWidget;
    auto* form = new QFormLayout(m_basicPage);
    form->setContentsMargins(8, 8, 8, 8);

    m_editNum  = new QLineEdit;
    m_editName = new QLineEdit;
    m_spinPreWait = new QDoubleSpinBox;
    m_spinPreWait->setRange(0.0, 9999.0);
    m_spinPreWait->setDecimals(3);
    m_spinPreWait->setSuffix(" s");
    m_comboGoQuantize = new QComboBox;
    m_comboGoQuantize->addItems({"None", "Next bar", "Next beat"});

    m_chkAutoCont   = new QCheckBox("Auto-continue");
    m_chkAutoFollow = new QCheckBox("Auto-follow");

    m_spinDurationBasic = new QDoubleSpinBox;
    m_spinDurationBasic->setRange(0.0, 99999.0);
    m_spinDurationBasic->setDecimals(3);
    m_spinDurationBasic->setSuffix(" s");
    m_spinDurationBasic->setSpecialValueText("(to end)");

    form->addRow("Cue #:",    m_editNum);
    form->addRow("Name:",     m_editName);
    form->addRow("Pre-wait:", m_spinPreWait);
    form->addRow("Duration:", m_spinDurationBasic);
    form->addRow("Quantize:", m_comboGoQuantize);

    auto* flagRow = new QHBoxLayout;
    flagRow->addWidget(m_chkAutoCont);
    flagRow->addWidget(m_chkAutoFollow);
    flagRow->addStretch();
    form->addRow("", flagRow);

    // Devamp section
    m_devampGroup = new QGroupBox("Devamp options");
    auto* dvLay = new QFormLayout(m_devampGroup);
    m_comboDevampMode = new QComboBox;
    m_comboDevampMode->addItems({"Next slice", "Next cue (stop current)", "Next cue (keep current)"});
    m_chkDevampPreVamp = new QCheckBox("Skip pre-vamp loops");
    dvLay->addRow("Mode:", m_comboDevampMode);
    dvLay->addRow("", m_chkDevampPreVamp);
    form->addRow(m_devampGroup);

    // Arm section
    m_armGroup = new QGroupBox("Arm options");
    auto* armLay = new QFormLayout(m_armGroup);
    m_spinArmStart = new QDoubleSpinBox;
    m_spinArmStart->setRange(0.0, 99999.0);
    m_spinArmStart->setDecimals(3);
    m_spinArmStart->setSuffix(" s");
    armLay->addRow("Pre-load from:", m_spinArmStart);
    form->addRow(m_armGroup);

    m_tabs->addTab(m_basicPage, "Basic");

    connect(m_editNum,  &QLineEdit::editingFinished, this, &InspectorWidget::onBasicChanged);
    connect(m_editName, &QLineEdit::editingFinished, this, &InspectorWidget::onBasicChanged);
    connect(m_spinPreWait, &QDoubleSpinBox::editingFinished,
            this, &InspectorWidget::onBasicChanged);
    connect(m_comboGoQuantize, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &InspectorWidget::onBasicChanged);
    connect(m_chkAutoCont,   &QCheckBox::toggled, this, &InspectorWidget::onBasicChanged);
    connect(m_chkAutoFollow, &QCheckBox::toggled, this, &InspectorWidget::onBasicChanged);
    connect(m_comboDevampMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &InspectorWidget::onBasicChanged);
    connect(m_chkDevampPreVamp, &QCheckBox::toggled,
            this, &InspectorWidget::onBasicChanged);
    connect(m_spinArmStart, &QDoubleSpinBox::editingFinished,
            this, &InspectorWidget::onBasicChanged);
    connect(m_spinDurationBasic, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueDuration(m_cueIdx, m_spinDurationBasic->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildLevelsTab() {
    m_levelsPage = new QWidget;
    auto* outerLay = new QVBoxLayout(m_levelsPage);
    outerLay->setContentsMargins(0, 0, 0, 0);

    m_levelsScroll = new QScrollArea;
    m_levelsScroll->setWidgetResizable(true);
    m_levelsScroll->setFrameShape(QFrame::NoFrame);
    outerLay->addWidget(m_levelsScroll);

    m_levelsContent = new QWidget;
    m_levelsScroll->setWidget(m_levelsContent);

    m_tabs->addTab(m_levelsPage, "Levels");
}

void InspectorWidget::buildTrimTab() {
    m_trimPage = new QWidget;
    auto* lay = new QHBoxLayout(m_trimPage);
    lay->setContentsMargins(8, 8, 8, 8);

    m_trimFader = new FaderWidget("Trim", m_trimPage);
    lay->addWidget(m_trimFader);
    lay->addStretch();

    connect(m_trimFader, &FaderWidget::dragStarted, this, [this]() {
        if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
    });
    connect(m_trimFader, &FaderWidget::valueChanged,
            this, &InspectorWidget::onTrimFaderChanged);

    m_tabs->addTab(m_trimPage, "Trim");
}

void InspectorWidget::buildTimeTab() {
    m_timePage = new QWidget;
    auto* pageLay = new QVBoxLayout(m_timePage);
    pageLay->setContentsMargins(0, 0, 0, 0);
    pageLay->setSpacing(0);

    // Wrap everything in a scroll area so short panels scroll instead of squishing
    auto* scroll = new QScrollArea(m_timePage);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageLay->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    // ── Audio-only section (start/duration/waveform) ────────────────────────
    m_audioTimeSection = new QWidget(content);
    auto* audioLay = new QVBoxLayout(m_audioTimeSection);
    audioLay->setContentsMargins(0, 0, 0, 0);
    audioLay->setSpacing(6);

    auto* form = new QFormLayout;
    form->setSpacing(4);

    m_spinStart = new QDoubleSpinBox;
    m_spinStart->setRange(0.0, 99999.0);
    m_spinStart->setDecimals(3);
    m_spinStart->setSuffix(" s");

    m_spinDuration = new QDoubleSpinBox;
    m_spinDuration->setRange(0.0, 99999.0);
    m_spinDuration->setDecimals(3);
    m_spinDuration->setSuffix(" s");
    m_spinDuration->setSpecialValueText("(to end)");

    form->addRow("Start:", m_spinStart);
    form->addRow("Duration:", m_spinDuration);
    audioLay->addLayout(form);

    m_waveform = new WaveformView(m_model, m_audioTimeSection);
    audioLay->addWidget(m_waveform, 1);

    lay->addWidget(m_audioTimeSection, 1);

    // ── SyncGroup visual editor ──────────────────────────────────────────────
    m_syncGroupView = new SyncGroupView(m_model, content);
    m_syncGroupView->hide();
    lay->addWidget(m_syncGroupView, 1);

    // ── Marker editor panel (shared by audio and SyncGroup) ──────────────────
    m_markerPanel = new QGroupBox("Marker", content);
    auto* mform = new QFormLayout(m_markerPanel);
    mform->setSpacing(4);
    m_markerLabel = new QLabel(m_markerPanel);
    m_markerLabel->setStyleSheet("color:#aaa; font-size:11px;");

    m_markerTimeSpin = new QDoubleSpinBox(m_markerPanel);
    m_markerTimeSpin->setRange(0.0, 99999.0);
    m_markerTimeSpin->setDecimals(3);
    m_markerTimeSpin->setSuffix(" s");

    m_markerNameEdit = new QLineEdit(m_markerPanel);
    m_markerNameEdit->setPlaceholderText("(no name)");

    mform->addRow(m_markerLabel);
    mform->addRow("Time:", m_markerTimeSpin);
    mform->addRow("Name:", m_markerNameEdit);
    m_markerPanel->hide();
    lay->addWidget(m_markerPanel);

    // SyncGroupView signals
    connect(m_syncGroupView, &SyncGroupView::markerSelected, this, [this](int mi) {
        if (mi < 0 || m_cueIdx < 0) {
            m_selMarker = -1;
            m_markerPanel->hide();
            return;
        }
        const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
        if (!c || mi >= (int)c->markers.size()) { m_markerPanel->hide(); return; }
        m_selMarker = mi;
        m_loading = true;
        m_markerLabel->setText(QString("Marker %1").arg(mi + 1));
        m_markerTimeSpin->setValue(c->markers[mi].time);
        m_markerNameEdit->setText(QString::fromStdString(c->markers[mi].name));
        m_loading = false;
        m_markerPanel->show();
    });
    connect(m_syncGroupView, &SyncGroupView::cueModified, this, [this]() {
        emit cueEdited();
    });
    connect(m_syncGroupView, &SyncGroupView::rulerClicked, this, [this](double timeSec) {
        if (m_cueIdx < 0) return;
        m_model->cues.setCueTimelineArmSec(m_cueIdx, timeSec);
        emit cueEdited();
    });

    m_tabs->addTab(m_timePage, "Time & Loop");

    // Start / duration edits
    connect(m_spinStart, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueStartTime(m_cueIdx, m_spinStart->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_spinDuration, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueDuration(m_cueIdx, m_spinDuration->value());
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    // Arm click on waveform body: just visual cursor, does NOT change startTime
    // (armPositionChanged is intentionally NOT connected to setCueStartTime)

    // Marker selection from waveform
    connect(m_waveform, &WaveformView::markerSelectionChanged, this, [this](int mi) {
        m_selMarker = mi;
        if (mi < 0 || m_cueIdx < 0) {
            m_markerPanel->hide();
            return;
        }
        const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
        if (!c || mi >= (int)c->markers.size()) { m_markerPanel->hide(); return; }

        m_loading = true;
        m_markerLabel->setText(QString("Marker %1").arg(mi + 1));
        m_markerTimeSpin->setValue(c->markers[mi].time);
        m_markerNameEdit->setText(QString::fromStdString(c->markers[mi].name));
        m_loading = false;
        m_markerPanel->show();
    });

    // Marker time edit
    connect(m_markerTimeSpin, &QDoubleSpinBox::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0 || m_selMarker < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueMarkerTime(m_cueIdx, m_selMarker, m_markerTimeSpin->value());
        ShowHelpers::syncSfFromCues(*m_model);
        if (m_syncGroupView->isVisible()) m_syncGroupView->update();
        emit cueEdited();
    });

    // Marker name edit
    connect(m_markerNameEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_loading || m_cueIdx < 0 || m_selMarker < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueMarkerName(m_cueIdx, m_selMarker,
                                        m_markerNameEdit->text().toStdString());
        ShowHelpers::syncSfFromCues(*m_model);
        if (m_syncGroupView->isVisible()) m_syncGroupView->update();
        emit cueEdited();
    });
}

void InspectorWidget::buildCurveTab() {
    m_curvePage = new QWidget;
    auto* form = new QFormLayout(m_curvePage);
    form->setContentsMargins(8, 8, 8, 8);

    m_comboCurve = new QComboBox;
    m_comboCurve->addItems({"Linear", "Equal power"});

    m_chkStopWhenDone = new QCheckBox("Stop target when done");

    form->addRow("Curve:", m_comboCurve);
    form->addRow("", m_chkStopWhenDone);

    m_tabs->addTab(m_curvePage, "Curve");

    connect(m_comboCurve, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueFadeCurve(m_cueIdx,
            idx == 1 ? mcp::FadeData::Curve::EqualPower : mcp::FadeData::Curve::Linear);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
    connect(m_chkStopWhenDone, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueFadeStopWhenDone(m_cueIdx, v);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildModeTab() {
    m_modePage = new QWidget;
    auto* form = new QFormLayout(m_modePage);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_comboGroupMode = new QComboBox;
    m_comboGroupMode->addItem("Timeline",           static_cast<int>(mcp::GroupData::Mode::Timeline));
    m_comboGroupMode->addItem("Playlist",           static_cast<int>(mcp::GroupData::Mode::Playlist));
    m_comboGroupMode->addItem("Start First & Enter",static_cast<int>(mcp::GroupData::Mode::StartFirst));
    m_comboGroupMode->addItem("Synchronization",    static_cast<int>(mcp::GroupData::Mode::Sync));
    form->addRow("Mode:", m_comboGroupMode);

    m_chkGroupRandom = new QCheckBox("Random order (Playlist only)");
    form->addRow("", m_chkGroupRandom);

    m_tabs->addTab(m_modePage, "Mode");

    connect(m_comboGroupMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_loading || m_cueIdx < 0) return;
        const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
        if (!c || !c->groupData) return;
        m_model->pushUndo();
        auto mode = static_cast<mcp::GroupData::Mode>(
            m_comboGroupMode->currentData().toInt());
        m_model->cues.setCueGroupMode(m_cueIdx, mode);
        ShowHelpers::syncSfFromCues(*m_model);
        // Show/hide Time and Timeline tabs based on new mode
        const bool isTimeline = (mode == mcp::GroupData::Mode::Timeline);
        const bool isSyncMode = (mode == mcp::GroupData::Mode::Sync);
        m_tabs->setTabVisible(m_tabs->indexOf(m_timePage),     isSyncMode);
        m_tabs->setTabVisible(m_tabs->indexOf(m_timelinePage), isTimeline);
        m_chkGroupRandom->setEnabled(mode == mcp::GroupData::Mode::Playlist);
        if (isSyncMode) loadTime();
        emit cueEdited();
    });

    connect(m_chkGroupRandom, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        m_model->cues.setCueGroupRandom(m_cueIdx, v);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });
}

void InspectorWidget::buildMCTab() {
    m_mcPage = new QWidget;
    auto* outerLay = new QVBoxLayout(m_mcPage);
    outerLay->setContentsMargins(8, 6, 8, 6);
    outerLay->setSpacing(4);

    m_chkAttachMC = new QCheckBox("Attach Music Context");
    outerLay->addWidget(m_chkAttachMC);

    m_mcContent = new QWidget;
    auto* contentLay = new QVBoxLayout(m_mcContent);
    contentLay->setContentsMargins(0, 4, 0, 0);
    contentLay->setSpacing(4);

    m_chkApplyBefore = new QCheckBox("Apply before cue start (extrapolate first point)");
    contentLay->addWidget(m_chkApplyBefore);

    m_mcView = new MusicContextView(m_model, m_mcContent);
    contentLay->addWidget(m_mcView);

    // Property panel for selected point
    m_mcPropGroup = new QWidget;
    auto* propLay = new QFormLayout(m_mcPropGroup);
    propLay->setContentsMargins(0, 4, 0, 0);
    propLay->setSpacing(3);

    m_comboPtType = new QComboBox; m_comboPtType->addItems({"Jump", "Ramp"});
    m_spinPtBpm   = new QDoubleSpinBox;
    m_spinPtBpm->setRange(10.0, 999.0); m_spinPtBpm->setDecimals(2); m_spinPtBpm->setSuffix(" BPM");

    auto* tsRow = new QHBoxLayout;
    m_spinTSNum = new QSpinBox; m_spinTSNum->setRange(1, 32);
    m_spinTSDen = new QSpinBox; m_spinTSDen->setRange(1, 32);
    m_chkTSInherit = new QCheckBox("inherit");
    tsRow->addWidget(m_spinTSNum);
    tsRow->addWidget(new QLabel("/"));
    tsRow->addWidget(m_spinTSDen);
    tsRow->addWidget(m_chkTSInherit);
    tsRow->addStretch();

    m_lblPtPos = new QLabel("—");

    propLay->addRow("Type:",     m_comboPtType);
    propLay->addRow("BPM:",      m_spinPtBpm);
    propLay->addRow("Time Sig:", tsRow);
    propLay->addRow("Position:", m_lblPtPos);
    m_mcPropGroup->hide();
    contentLay->addWidget(m_mcPropGroup);
    contentLay->addStretch();

    m_mcContent->hide();
    outerLay->addWidget(m_mcContent);
    outerLay->addStretch();

    m_tabs->addTab(m_mcPage, "Music");

    // ── signals ────────────────────────────────────────────────────────────
    connect(m_chkAttachMC, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading || m_cueIdx < 0) return;
        m_model->pushUndo();
        if (on) {
            // Create default MC: 4/4, 120 BPM, bar 1 beat 1
            auto mc = std::make_unique<mcp::MusicContext>();
            mcp::MusicContext::Point p;
            p.bar = 1; p.beat = 1; p.bpm = 120.0;
            p.isRamp = false; p.hasTimeSig = true; p.timeSigNum = 4; p.timeSigDen = 4;
            mc->points.push_back(p);
            m_model->cues.setCueMusicContext(m_cueIdx, std::move(mc));
        } else {
            m_model->cues.setCueMusicContext(m_cueIdx, nullptr);
        }
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
        // Reload the tab without losing the current tab selection
        const int cIdx = m_cueIdx;
        m_cueIdx = -1;
        setCueIndex(cIdx);
    });

    connect(m_chkApplyBefore, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading || m_cueIdx < 0) return;
        auto* mc = m_model->cues.musicContextOf(m_cueIdx);
        if (!mc) return;
        m_model->pushUndo();
        mc->applyBeforeStart = on;
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_mcView, &MusicContextView::pointSelected, this, [this](int pt) {
        m_selMCPt = pt;
        loadMCPropPanel();
    });

    connect(m_mcView, &MusicContextView::mcChanged, this, [this] {
        if (m_cueIdx < 0) return;
        ShowHelpers::syncSfFromCues(*m_model);
        loadMCPropPanel();  // refresh position label after drag
        emit cueEdited();
    });

    // Property panel changes
    auto onPropChanged = [this] {
        if (m_loading || m_cueIdx < 0 || m_selMCPt < 0) return;
        auto* mc = m_model->cues.musicContextOf(m_cueIdx);
        if (!mc || m_selMCPt >= (int)mc->points.size()) return;
        m_model->pushUndo();
        auto& pt = mc->points[m_selMCPt];
        pt.isRamp    = (m_comboPtType->currentIndex() == 1) && (m_selMCPt > 0);
        pt.bpm       = m_spinPtBpm->value();
        const bool inherit = m_chkTSInherit->isChecked() && (m_selMCPt > 0);
        pt.hasTimeSig = !inherit;
        if (pt.hasTimeSig) { pt.timeSigNum = m_spinTSNum->value(); pt.timeSigDen = m_spinTSDen->value(); }
        mc->markDirty();
        m_mcView->update();
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    };
    connect(m_comboPtType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onPropChanged);
    connect(m_spinPtBpm,   &QDoubleSpinBox::editingFinished, this, onPropChanged);
    connect(m_spinTSNum,   &QSpinBox::editingFinished,       this, onPropChanged);
    connect(m_spinTSDen,   &QSpinBox::editingFinished,       this, onPropChanged);
    connect(m_chkTSInherit, &QCheckBox::toggled, this, [this, onPropChanged](bool) {
        const bool inh = m_chkTSInherit->isChecked();
        m_spinTSNum->setEnabled(!inh);
        m_spinTSDen->setEnabled(!inh);
        onPropChanged();
    });
}

void InspectorWidget::loadMCPropPanel() {
    if (m_cueIdx < 0) { m_mcPropGroup->hide(); return; }
    const auto* c = m_model->cues.cueAt(m_cueIdx);
    if (!c || !c->musicContext || m_selMCPt < 0 ||
        m_selMCPt >= (int)c->musicContext->points.size()) {
        m_mcPropGroup->hide();
        return;
    }
    m_loading = true;
    const auto& pt = c->musicContext->points[m_selMCPt];
    m_comboPtType->setCurrentIndex(pt.isRamp ? 1 : 0);
    m_comboPtType->setEnabled(m_selMCPt > 0);
    m_spinPtBpm->setValue(pt.bpm);
    const bool inherit = !pt.hasTimeSig && (m_selMCPt > 0);
    m_chkTSInherit->setChecked(inherit);
    m_chkTSInherit->setEnabled(m_selMCPt > 0);
    m_spinTSNum->setValue(pt.hasTimeSig ? pt.timeSigNum : 4);
    m_spinTSDen->setValue(pt.hasTimeSig ? pt.timeSigDen : 4);
    m_spinTSNum->setEnabled(!inherit);
    m_spinTSDen->setEnabled(!inherit);
    m_lblPtPos->setText(QString("%1 | %2").arg(pt.bar).arg(pt.beat));
    m_mcPropGroup->show();
    m_loading = false;
}

void InspectorWidget::buildTimelineTab() {
    m_timelinePage = new QWidget;
    auto* lay = new QVBoxLayout(m_timelinePage);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_timelineView = new TimelineGroupView(m_model, m_timelinePage);
    lay->addWidget(m_timelineView);
    lay->addStretch(1);

    m_tabs->addTab(m_timelinePage, "Timeline");

    connect(m_timelineView, &TimelineGroupView::childOffsetChanged,
            this, [this](int childFlatIdx, double newOffsetSec) {
        m_model->pushUndo();
        m_model->cues.setCueTimelineOffset(childFlatIdx, newOffsetSec);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_timelineView, &TimelineGroupView::childTrimChanged,
            this, [this](int childFlatIdx, double newOffsetSec,
                         double newStartTimeSec, double newDurationSec) {
        m_model->pushUndo();
        m_model->cues.setCueTimelineOffset(childFlatIdx, newOffsetSec);
        m_model->cues.setCueStartTime(childFlatIdx, newStartTimeSec);
        m_model->cues.setCueDuration(childFlatIdx, newDurationSec);
        ShowHelpers::syncSfFromCues(*m_model);
        emit cueEdited();
    });

    connect(m_timelineView, &TimelineGroupView::rulerClicked,
            this, [this](double timeSec) {
        if (m_cueIdx < 0) return;
        m_model->cues.setCueTimelineArmSec(m_cueIdx, timeSec);
        emit cueEdited();   // refresh table so "armed" state appears
    });
}

// ── public API ─────────────────────────────────────────────────────────────

void InspectorWidget::setCueIndex(int idx) {
    m_cueIdx    = idx;
    m_selMarker = -1;
    m_loading   = true;

    const mcp::Cue* c = (idx >= 0) ? m_model->cues.cueAt(idx) : nullptr;

    const bool isAudio    = c && c->type == mcp::CueType::Audio;
    const bool isFade     = c && c->type == mcp::CueType::Fade;
    const bool isDevamp   = c && c->type == mcp::CueType::Devamp;
    const bool isArm      = c && c->type == mcp::CueType::Arm;
    const bool isGroup    = c && c->type == mcp::CueType::Group;
    const bool isSyncGroup = isGroup && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Sync;
    const bool isTimeline  = isGroup && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Timeline;

    const bool hasMC = isAudio || (isGroup && (isSyncGroup || isTimeline));

    m_tabs->setTabVisible(m_tabs->indexOf(m_mcPage),       hasMC);
    m_tabs->setTabVisible(m_tabs->indexOf(m_levelsPage),   isAudio || isFade);
    m_tabs->setTabVisible(m_tabs->indexOf(m_trimPage),     isAudio);
    m_tabs->setTabVisible(m_tabs->indexOf(m_timePage),     isAudio || isSyncGroup);
    m_tabs->setTabVisible(m_tabs->indexOf(m_curvePage),    isFade);
    m_tabs->setTabVisible(m_tabs->indexOf(m_modePage),     isGroup);
    m_tabs->setTabVisible(m_tabs->indexOf(m_timelinePage), isTimeline);

    m_spinDurationBasic->setVisible(true);
    m_devampGroup->setVisible(isDevamp);
    m_armGroup->setVisible(isArm);

    // Music Context tab
    if (hasMC) {
        const bool mcAttached = c && c->musicContext != nullptr;
        m_chkAttachMC->setChecked(mcAttached);
        m_mcContent->setVisible(mcAttached);
        m_selMCPt = -1;
        if (mcAttached) {
            m_chkApplyBefore->setChecked(c->musicContext->applyBeforeStart);
            m_mcView->setCueIndex(idx);
            m_mcPropGroup->hide();
        }
    }

    loadBasic();
    if (isAudio || isFade) rebuildLevelsForCue();
    if (isAudio) { loadTrim(); loadTime(); }
    if (isSyncGroup) loadTime();
    if (isFade)  loadCurve();
    if (isGroup) {
        loadMode();
        if (isTimeline) m_timelineView->setGroupCueIndex(idx);
    }

    // Pass MC to timeline views for bar/beat ruler
    {
        const mcp::MusicContext* mc = (c && c->musicContext) ? c->musicContext.get() : nullptr;
        const double startTime = (c && c->type == mcp::CueType::Audio) ? c->startTime : 0.0;
        if (m_waveform)      m_waveform->setMusicContext(mc, startTime);
        if (m_timelineView)  m_timelineView->setMusicContext(mc);
        if (m_syncGroupView) m_syncGroupView->setMusicContext(mc);
    }

    m_markerPanel->hide();
    m_loading = false;
}

void InspectorWidget::updatePlayhead() {
    if (m_waveform) m_waveform->updatePlayhead();
}

void InspectorWidget::clearTimelineArm() {
    if (m_timelineView)
        m_timelineView->clearArmCursor();
    if (m_syncGroupView)
        m_syncGroupView->clearArmCursor();
}

int InspectorWidget::currentTabIndex() const {
    return m_tabs->currentIndex();
}

void InspectorWidget::restoreTabIndex(int idx) {
    if (idx >= 0 && idx < m_tabs->count() && m_tabs->isTabVisible(idx))
        m_tabs->setCurrentIndex(idx);
}

// ── load helpers ───────────────────────────────────────────────────────────

void InspectorWidget::loadBasic() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    const bool en = c != nullptr;

    m_editNum->setEnabled(en);
    m_editName->setEnabled(en);
    m_spinPreWait->setEnabled(en);
    m_comboGoQuantize->setEnabled(en);
    m_chkAutoCont->setEnabled(en);
    m_chkAutoFollow->setEnabled(en);

    if (!c) {
        m_editNum->clear(); m_editName->clear();
        m_spinPreWait->setValue(0.0);
        m_comboGoQuantize->setCurrentIndex(0);
        m_chkAutoCont->setChecked(false);
        m_chkAutoFollow->setChecked(false);
        return;
    }
    m_editNum->setText(QString::fromStdString(c->cueNumber));
    m_editName->setText(QString::fromStdString(c->name));
    m_spinPreWait->setValue(c->preWaitSeconds);
    m_comboGoQuantize->setCurrentIndex(std::clamp(c->goQuantize, 0, 2));
    // Fade cues treat stored duration=0 as "use 3 s default" — show the real value.
    const double effectiveDur = (c->type == mcp::CueType::Fade && c->duration == 0.0)
                                ? 3.0 : c->duration;
    m_spinDurationBasic->setValue(effectiveDur);
    m_chkAutoCont->setChecked(c->autoContinue);
    m_chkAutoFollow->setChecked(c->autoFollow);
    if (c->type == mcp::CueType::Devamp) {
        m_comboDevampMode->setCurrentIndex(c->devampMode);
        m_chkDevampPreVamp->setChecked(c->devampPreVamp);
    }
    if (c->type == mcp::CueType::Arm)
        m_spinArmStart->setValue(c->armStartTime);
}

void InspectorWidget::loadTrim() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    m_trimFader->setValue(c ? static_cast<float>(c->trim) : 0.0f);
}

void InspectorWidget::loadTime() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;

    const bool isSyncGroup = c && c->type == mcp::CueType::Group && c->groupData &&
                             c->groupData->mode == mcp::GroupData::Mode::Sync;

    if (isSyncGroup) {
        m_audioTimeSection->hide();
        m_syncGroupView->show();
        m_markerPanel->hide();
        loadSyncSection();
        return;
    }

    m_audioTimeSection->show();
    m_syncGroupView->hide();

    if (!c) {
        m_spinStart->setValue(0.0);
        m_spinDuration->setValue(0.0);
        m_waveform->setCueIndex(-1);
        return;
    }
    m_spinStart->setValue(c->startTime);
    m_spinDuration->setValue(c->duration);
    m_waveform->setCueIndex(m_cueIdx);
}

void InspectorWidget::loadSyncSection() {
    m_syncGroupView->setGroupCueIndex(m_cueIdx);
}

void InspectorWidget::loadCurve() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c || !c->fadeData) {
        m_comboCurve->setCurrentIndex(0);
        m_chkStopWhenDone->setChecked(false);
        return;
    }
    m_comboCurve->setCurrentIndex(
        c->fadeData->curve == mcp::FadeData::Curve::EqualPower ? 1 : 0);
    m_chkStopWhenDone->setChecked(c->fadeData->stopWhenDone);
}

void InspectorWidget::loadMode() {
    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c || !c->groupData) return;

    const auto mode = c->groupData->mode;
    for (int i = 0; i < m_comboGroupMode->count(); ++i) {
        if (m_comboGroupMode->itemData(i).toInt() == static_cast<int>(mode)) {
            m_comboGroupMode->setCurrentIndex(i);
            break;
        }
    }
    m_chkGroupRandom->setChecked(c->groupData->random);
    m_chkGroupRandom->setEnabled(mode == mcp::GroupData::Mode::Playlist);
}

// ── rebuildLevelsForCue ────────────────────────────────────────────────────
//
// Layout (Audio):
//   Fader row:  [Master]  [Out1] [Out2] [Out3] ...
//   Crosspoint:
//       headers:         Out1    Out2   ...
//       row "Src 1":    [0.0]   [—]    ...
//       row "Src 2":    [—]     [0.0]  ...
//
// Layout (Fade): master-target fader + per-output target faders (with en checkboxes)

void InspectorWidget::rebuildLevelsForCue() {
    delete m_levelsContent;
    m_outFaders.clear();
    m_fadeOutFaders.clear();
    m_xpCells.clear();
    m_fadeXpCells.clear();
    m_masterFader     = nullptr;
    m_fadeMasterFader = nullptr;

    m_levelsContent = new QWidget;
    auto* lay = new QVBoxLayout(m_levelsContent);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);
    m_levelsScroll->setWidget(m_levelsContent);

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    if (c->type == mcp::CueType::Audio) {
        const int outCh = m_model->engineOk ? m_model->engine.channels() : 2;
        const int srcCh = c->audioFile.isLoaded() ? c->audioFile.metadata().channels : 1;

        // ── Fader row ──────────────────────────────────────────────────
        auto* fadersRow = new QHBoxLayout;
        fadersRow->setSpacing(4);

        // Master fader (labelled "main")
        m_masterFader = new FaderWidget("main", m_levelsContent);
        m_masterFader->setValue(static_cast<float>(c->level));
        fadersRow->addWidget(m_masterFader);

        // Separator
        auto* sep = new QFrame(m_levelsContent);
        sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("color:#333;");
        fadersRow->addWidget(sep);

        // Per-output faders
        for (int o = 0; o < outCh; ++o) {
            float dB = 0.0f;
            if (o < (int)c->routing.outLevelDb.size())
                dB = c->routing.outLevelDb[o];
            auto* fw = new FaderWidget(QString("Out %1").arg(o + 1), m_levelsContent);
            fw->setValue(dB);
            m_outFaders.push_back(fw);
            fadersRow->addWidget(fw);
            connect(fw, &FaderWidget::dragStarted, this, [this]() {
                if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
            });
            connect(fw, &FaderWidget::valueChanged, this, [this, o](float dB) {
                onLevelFaderChanged(o, dB);
            });
        }
        fadersRow->addStretch();
        connect(m_masterFader, &FaderWidget::dragStarted, this, [this]() {
            if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
        });
        connect(m_masterFader, &FaderWidget::valueChanged,
                this, &InspectorWidget::onMasterFaderChanged);
        lay->addLayout(fadersRow);

        // ── Crosspoint grid ────────────────────────────────────────────
        if (srcCh > 0 && outCh > 0) {
            auto* xpGroup = new QGroupBox("Crosspoint", m_levelsContent);
            xpGroup->setStyleSheet(
                "QGroupBox{color:#888;border:1px solid #2a2a2a;"
                "margin-top:8px;padding-top:6px;border-radius:4px;}"
                "QGroupBox::title{subcontrol-origin:margin;left:8px;}");
            auto* grid = new QGridLayout(xpGroup);
            grid->setSpacing(3);
            grid->setContentsMargins(6, 12, 6, 6);

            constexpr int kCellW = 46;
            constexpr int kLblW  = 40;
            constexpr int kSp    = 3;
            // Compact fixed width: label col + outCh data cols + outCh gaps + margins + border
            xpGroup->setFixedWidth(kLblW + outCh * (kCellW + kSp) + 14);
            // Absorb any extra horizontal space in an empty stretch column
            grid->setColumnStretch(outCh + 1, 1);

            for (int o = 0; o < outCh; ++o) {
                auto* lbl = new QLabel(QString("Out %1").arg(o + 1), xpGroup);
                lbl->setFixedWidth(kCellW);
                lbl->setAlignment(Qt::AlignHCenter);
                lbl->setStyleSheet("color:#888;font-size:10px;");
                grid->addWidget(lbl, 0, o + 1);
            }

            m_xpCells.resize(srcCh, std::vector<QLineEdit*>(outCh, nullptr));
            for (int s = 0; s < srcCh; ++s) {
                auto* rowLbl = new QLabel(QString("Src %1").arg(s + 1), xpGroup);
                rowLbl->setFixedWidth(kLblW);
                rowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                rowLbl->setStyleSheet("color:#888;font-size:11px;");
                grid->addWidget(rowLbl, s + 1, 0);

                for (int o = 0; o < outCh; ++o) {
                    std::optional<float> xp;
                    if (s < (int)c->routing.xpoint.size()
                        && o < (int)c->routing.xpoint[s].size())
                        xp = c->routing.xpoint[s][o];
                    else if (s == o)
                        xp = 0.0f;  // engine default: unspecified diagonal = 0 dB

                    auto* cell = makeXpCell();
                    if (xp.has_value())
                        cell->setText(fmtXpDb(*xp));
                    else
                        cell->clear();

                    m_xpCells[s][o] = cell;
                    grid->addWidget(cell, s + 1, o + 1);

                    connect(cell, &QLineEdit::editingFinished, this, [this, s, o]() {
                        if (m_loading || m_cueIdx < 0) return;
                        if (s >= (int)m_xpCells.size()) return;
                        if (o >= (int)m_xpCells[s].size()) return;
                        auto* ce = m_xpCells[s][o];
                        if (!ce) return;
                        m_model->pushUndo();
                        const QString txt = ce->text().trimmed();
                        if (txt.isEmpty()) {
                            m_model->cues.setCueXpoint(m_cueIdx, s, o, std::nullopt);
                        } else {
                            bool ok = false;
                            float dB = txt.toFloat(&ok);
                            // Non-numeric or below-floor → -inf (true silence, 0 linear)
                            if (!ok || dB < FaderWidget::kFaderMin)
                                dB = FaderWidget::kFaderInf;
                            else
                                dB = std::min(dB, FaderWidget::kFaderMax);
                            m_model->cues.setCueXpoint(m_cueIdx, s, o, dB);
                            ce->setText(dB <= FaderWidget::kFaderMin + 0.05f
                                ? QStringLiteral("-inf")
                                : QString::number(static_cast<double>(dB), 'f', 1));
                        }
                        ShowHelpers::syncSfFromCues(*m_model);
                        emit cueEdited();
                    });
                }
            }

            lay->addWidget(xpGroup, 0, Qt::AlignLeft);
        }

    } else if (c->type == mcp::CueType::Fade && c->fadeData) {
        const auto& fd = *c->fadeData;
        const int outCh = m_model->engineOk ? m_model->engine.channels() : 2;

        // Helper: create a toggleable fader for a fade target.
        // Clicking the label toggles the "enabled" state; dragging sets target dB.
        auto makeFadeFader = [&](const QString& lbl, bool enabled, float targetDb)
                -> FaderWidget* {
            auto* fw = new FaderWidget(lbl, m_levelsContent);
            fw->setToggleable(true);
            fw->setValue(targetDb);
            fw->setActivated(enabled);
            return fw;
        };

        // ── Fader row: [Master] | [Out1] [Out2] … ─────────────────────────
        auto* fadersRow = new QHBoxLayout;
        fadersRow->setSpacing(4);

        m_fadeMasterFader = makeFadeFader("Master",
            fd.masterLevel.enabled, fd.masterLevel.targetDb);
        fadersRow->addWidget(m_fadeMasterFader);

        auto* sep = new QFrame(m_levelsContent);
        sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("color:#333;");
        fadersRow->addWidget(sep);

        m_model->cues.setCueFadeOutTargetCount(m_cueIdx, outCh);
        for (int o = 0; o < outCh; ++o) {
            bool  enabled = (o < (int)fd.outLevels.size()) ? fd.outLevels[o].enabled  : false;
            float target  = (o < (int)fd.outLevels.size()) ? fd.outLevels[o].targetDb : 0.0f;
            auto* fw = makeFadeFader(QString("Out %1").arg(o + 1), enabled, target);
            m_fadeOutFaders.push_back(fw);
            fadersRow->addWidget(fw);
            connect(fw, &FaderWidget::dragStarted, this, [this]() {
                if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
            });
            connect(fw, &FaderWidget::toggled, this, [this, o](bool en) {
                if (m_loading || m_cueIdx < 0) return;
                const mcp::Cue* c2 = m_model->cues.cueAt(m_cueIdx);
                if (!c2 || !c2->fadeData) return;
                float tgt = (o < (int)c2->fadeData->outLevels.size())
                    ? c2->fadeData->outLevels[o].targetDb : 0.0f;
                m_model->cues.setCueFadeOutTarget(m_cueIdx, o, en, tgt);
                ShowHelpers::syncSfFromCues(*m_model);
                emit cueEdited();
            });
            connect(fw, &FaderWidget::valueChanged, this, [this, o](float dB) {
                onFadeOutTargetChanged(o, dB);
            });
        }
        fadersRow->addStretch();

        connect(m_fadeMasterFader, &FaderWidget::dragStarted, this, [this]() {
            if (!m_loading && m_cueIdx >= 0) m_model->pushUndo();
        });
        connect(m_fadeMasterFader, &FaderWidget::toggled, this, [this](bool en) {
            if (m_loading || m_cueIdx < 0) return;
            const mcp::Cue* c2 = m_model->cues.cueAt(m_cueIdx);
            if (!c2 || !c2->fadeData) return;
            m_model->cues.setCueFadeMasterTarget(m_cueIdx, en,
                c2->fadeData->masterLevel.targetDb);
            ShowHelpers::syncSfFromCues(*m_model);
            emit cueEdited();
        });
        connect(m_fadeMasterFader, &FaderWidget::valueChanged,
                this, &InspectorWidget::onFadeMasterTargetChanged);
        lay->addLayout(fadersRow);

        // ── Crosspoint fade targets ────────────────────────────────────────
        // Determine grid dimensions from existing xpTargets or target cue.
        int xpSrcCh = 0;
        int xpOutCh = outCh;
        if (!fd.xpTargets.empty()) {
            xpSrcCh = (int)fd.xpTargets.size();
            if (!fd.xpTargets[0].empty()) xpOutCh = (int)fd.xpTargets[0].size();
        } else {
            const int tIdx = fd.resolvedTargetIdx;
            const mcp::Cue* tgt = (tIdx >= 0) ? m_model->cues.cueAt(tIdx) : nullptr;
            if (tgt && tgt->audioFile.isLoaded())
                xpSrcCh = tgt->audioFile.metadata().channels;
        }

        if (xpSrcCh > 0 && xpOutCh > 0) {
            // Ensure matrix is sized (no-op if already correct).
            m_model->cues.setCueFadeXpSize(m_cueIdx, xpSrcCh, xpOutCh);
            // Re-read fd pointer (setCueFadeXpSize may reallocate vectors).
            const auto& fd2 = *m_model->cues.cueAt(m_cueIdx)->fadeData;

            auto* xpGroup = new QGroupBox("Crosspoint fade", m_levelsContent);
            xpGroup->setStyleSheet(
                "QGroupBox{color:#888;border:1px solid #2a2a2a;"
                "margin-top:8px;padding-top:6px;border-radius:4px;}"
                "QGroupBox::title{subcontrol-origin:margin;left:8px;}");
            auto* grid = new QGridLayout(xpGroup);
            grid->setSpacing(3);
            grid->setContentsMargins(6, 12, 6, 6);

            constexpr int kCellW = 46;
            constexpr int kLblW  = 40;
            constexpr int kSp    = 3;
            xpGroup->setFixedWidth(kLblW + xpOutCh * (kCellW + kSp) + 14);
            grid->setColumnStretch(xpOutCh + 1, 1);

            for (int o = 0; o < xpOutCh; ++o) {
                auto* lbl = new QLabel(QString("Out %1").arg(o + 1), xpGroup);
                lbl->setFixedWidth(kCellW);
                lbl->setAlignment(Qt::AlignHCenter);
                lbl->setStyleSheet("color:#888;font-size:10px;");
                grid->addWidget(lbl, 0, o + 1);
            }

            m_fadeXpCells.resize(xpSrcCh, std::vector<QLineEdit*>(xpOutCh, nullptr));
            for (int s = 0; s < xpSrcCh; ++s) {
                auto* rowLbl = new QLabel(QString("Src %1").arg(s + 1), xpGroup);
                rowLbl->setFixedWidth(kLblW);
                rowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                rowLbl->setStyleSheet("color:#888;font-size:11px;");
                grid->addWidget(rowLbl, s + 1, 0);

                for (int o = 0; o < xpOutCh; ++o) {
                    bool  en  = false;
                    float tdb = 0.0f;
                    if (s < (int)fd2.xpTargets.size() &&
                        o < (int)fd2.xpTargets[s].size()) {
                        en  = fd2.xpTargets[s][o].enabled;
                        tdb = fd2.xpTargets[s][o].targetDb;
                    }

                    auto* cell = makeXpCell();
                    if (en)
                        cell->setText(fmtXpDb(tdb));
                    else
                        cell->clear();
                    cell->setPlaceholderText("—");

                    m_fadeXpCells[s][o] = cell;
                    grid->addWidget(cell, s + 1, o + 1);

                    connect(cell, &QLineEdit::editingFinished, this, [this, s, o]() {
                        if (m_loading || m_cueIdx < 0) return;
                        if (s >= (int)m_fadeXpCells.size()) return;
                        if (o >= (int)m_fadeXpCells[s].size()) return;
                        auto* ce = m_fadeXpCells[s][o];
                        if (!ce) return;
                        m_model->pushUndo();
                        const QString txt = ce->text().trimmed();
                        if (txt.isEmpty()) {
                            // Empty = deactivate (distinct from -inf which activates at silence)
                            m_model->cues.setCueFadeXpTarget(m_cueIdx, s, o, false, 0.0f);
                        } else {
                            bool ok = false;
                            float dB = txt.toFloat(&ok);
                            // Non-numeric or below-floor → -inf (activate at true silence)
                            if (!ok || dB < FaderWidget::kFaderMin)
                                dB = FaderWidget::kFaderInf;
                            else
                                dB = std::min(dB, FaderWidget::kFaderMax);
                            m_model->cues.setCueFadeXpTarget(m_cueIdx, s, o, true, dB);
                            ce->setText(dB <= FaderWidget::kFaderMin + 0.05f
                                ? QStringLiteral("-inf")
                                : QString::number(static_cast<double>(dB), 'f', 1));
                        }
                        ShowHelpers::syncSfFromCues(*m_model);
                        emit cueEdited();
                    });
                }
            }
            lay->addWidget(xpGroup, 0, Qt::AlignLeft);
        }
    }

    lay->addStretch();
}

// ── slot implementations ───────────────────────────────────────────────────

void InspectorWidget::onBasicChanged() {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
    if (!c) return;
    m_model->pushUndo();

    ShowHelpers::setCueNumberChecked(*m_model, m_cueIdx,
                                     m_editNum->text().toStdString());
    m_model->cues.setCueName(m_cueIdx, m_editName->text().toStdString());
    m_model->cues.setCuePreWait(m_cueIdx, m_spinPreWait->value());
    m_model->cues.setCueGoQuantize(m_cueIdx, m_comboGoQuantize->currentIndex());
    m_model->cues.setCueAutoContinue(m_cueIdx, m_chkAutoCont->isChecked());
    m_model->cues.setCueAutoFollow(m_cueIdx, m_chkAutoFollow->isChecked());
    if (c->type == mcp::CueType::Devamp) {
        m_model->cues.setCueDevampMode(m_cueIdx, m_comboDevampMode->currentIndex());
        m_model->cues.setCueDevampPreVamp(m_cueIdx, m_chkDevampPreVamp->isChecked());
    }
    if (c->type == mcp::CueType::Arm)
        m_model->cues.setCueArmStartTime(m_cueIdx, m_spinArmStart->value());

    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onMasterFaderChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues.setCueLevel(m_cueIdx, static_cast<double>(dB));
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onLevelFaderChanged(int outCh, float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues.setCueOutLevel(m_cueIdx, outCh, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onTrimFaderChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    m_model->cues.setCueTrim(m_cueIdx, static_cast<double>(dB));
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onFadeMasterTargetChanged(float dB) {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
    if (!c || !c->fadeData) return;
    m_model->cues.setCueFadeMasterTarget(m_cueIdx, c->fadeData->masterLevel.enabled, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}

void InspectorWidget::onFadeOutTargetChanged(int outCh, float dB) {
    if (m_loading || m_cueIdx < 0) return;
    const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
    if (!c || !c->fadeData) return;
    bool en = (outCh < (int)c->fadeData->outLevels.size())
        ? c->fadeData->outLevels[outCh].enabled : false;
    m_model->cues.setCueFadeOutTarget(m_cueIdx, outCh, en, dB);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueEdited();
}
