#include "MixConsoleDialog.h"
#include "AppModel.h"

#include "engine/AudioEngine.h"
#include "engine/ShowFile.h"

using ShowFile = mcp::ShowFile;

static std::string mixerParamPath(int ch, const char* sub) {
    return "/mixer/" + std::to_string(ch) + "/" + sub;
}
static std::string xpPath(int ch, int out) {
    return "/mixer/" + std::to_string(ch) + "/crosspoint/" + std::to_string(out);
}

#include <QApplication>
#include <QCheckBox>
#include <QMenu>
#include <QToolButton>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

static constexpr float kInfDb       = -144.0f;
static constexpr int   kFaderMin    =    0;
static constexpr int   kFaderMax    = 1000;   // slider units: pos * 1000
static constexpr int   kStripW      =  110;
static constexpr int   kStripLinkedW = kStripW * 2 + 3;  // 223
static constexpr int   kCellH       =   38;

// Fader taper — logarithmic mapping, pos 0..1.
// pos=0 → -inf, pos=kTaperUnityPos → 0 dB, pos=1 → kTaperMaxDb
static constexpr float kTaperMaxDb    =  6.0f;
static constexpr float kTaperUnityPos =  0.78f;
static constexpr float kTaperMinDb    = -80.0f;

static float taperK() {
    static const float K = -kTaperMaxDb / std::log10f(kTaperUnityPos);
    return K;
}
static float posToDb(float pos) {
    if (pos <= 0.0f) return kInfDb;
    const float db = kTaperMaxDb + taperK() * std::log10f(pos);
    return (db <= kTaperMinDb) ? kInfDb : db;
}
static float dbToPos(float db) {
    if (!std::isfinite(db) || db <= kTaperMinDb) return 0.0f;
    if (db >= kTaperMaxDb) return 1.0f;
    return std::clamp(std::pow(10.0f, (db - kTaperMaxDb) / taperK()), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// XpCellWidget — compact routing send cell.
// Orange bar fills horizontally proportional to log-scale dB (max +10, min -inf).
// Left/right drag adjusts value; double-click for text entry.
// ---------------------------------------------------------------------------
class XpCellWidget : public QWidget {
public:
    std::function<void(float)> onCommit;

    XpCellWidget(const QString& destLabel, float db, QWidget* parent = nullptr)
        : QWidget(parent), m_label(destLabel), m_db(db)
    {
        setFixedHeight(kCellH);
        setCursor(Qt::SizeHorCursor);
    }

    void setDb(float db) { m_db = db; update(); }
    float db() const { return m_db; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const float frac  = dbToFraction(m_db);
        const bool active = (m_db > kInfDb);

        p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));

        if (active && frac > 0.0f) {
            const int barW = std::max(1, static_cast<int>(frac * width()));
            p.fillRect(QRect(0, 0, barW, height()), QColor(0xaa, 0x55, 0x00));
        }

        p.setPen(active ? QColor(0xcc, 0x66, 0x00) : QColor(0x2a, 0x2a, 0x2a));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        const QColor fg = active ? QColor(0xff, 0xff, 0xff) : QColor(0x44, 0x44, 0x44);
        p.setPen(fg);

        QFont nameFont = font();
        nameFont.setPointSize(8);
        p.setFont(nameFont);
        QRect nameRect(4, 2, width() - 8, kCellH / 2 - 2);
        p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(nameFont).elidedText(m_label, Qt::ElideMiddle, nameRect.width()));

        QFont valFont = font();
        valFont.setPointSize(9);
        p.setFont(valFont);
        QRect valRect(4, kCellH / 2, width() - 6, kCellH / 2 - 2);
        p.drawText(valRect, Qt::AlignRight | Qt::AlignVCenter,
            m_db <= kInfDb ? QString("-∞")
                           : QString::number(static_cast<double>(m_db), 'f', 1));
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragX    = e->pos().x();
            m_dragDb   = m_db;
            m_dragging = true;
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_dragging) return;
        const int dx = e->pos().x() - m_dragX;
        constexpr float kDragRange = 70.0f;
        const float raw = m_dragDb + static_cast<float>(dx) * kDragRange / std::max(width(), 1);
        const float clamped = (raw < -58.0f) ? kInfDb : std::clamp(raw, -60.0f, 10.0f);
        if (clamped != m_db) {
            m_db = clamped;
            update();
            if (onCommit) onCommit(m_db);
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) m_dragging = false;
    }

    void mouseDoubleClickEvent(QMouseEvent*) override { startEdit(); }

private:
    static float dbToFraction(float db) {
        constexpr float kFloor = -60.0f, kTop = +10.0f;
        if (db <= kFloor) return 0.0f;
        return std::clamp((db - kFloor) / (kTop - kFloor), 0.0f, 1.0f);
    }

    void startEdit() {
        if (m_editor) return;
        m_editor = new QLineEdit(this);
        m_editor->setGeometry(rect());
        m_editor->setText(m_db <= kInfDb ? QString()
            : QString::number(static_cast<double>(m_db), 'f', 1));
        m_editor->setPlaceholderText("-inf");
        m_editor->selectAll();
        m_editor->show();
        m_editor->setFocus();
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() { finishEdit(); });
    }

    void finishEdit() {
        if (!m_editor) return;
        const QString t = m_editor->text().trimmed().toLower();
        float db;
        if (t.isEmpty() || t == "-inf" || t.startsWith("-∞")) {
            db = kInfDb;
        } else {
            bool ok = false;
            db = t.toFloat(&ok);
            if (!ok) db = m_db;
            else     db = std::clamp(db, -60.0f, 10.0f);
        }
        m_editor->deleteLater();
        m_editor = nullptr;
        m_db = db;
        update();
        if (onCommit) onCommit(m_db);
    }

    QString    m_label;
    float      m_db;
    QLineEdit* m_editor{nullptr};
    bool       m_dragging{false};
    int        m_dragX{0};
    float      m_dragDb{0.0f};
};

// ---------------------------------------------------------------------------
// FaderScaleWidget — dB tick marks drawn to the left of the fader slider.
// ---------------------------------------------------------------------------
class FaderScaleWidget : public QWidget {
public:
    FaderScaleWidget(QWidget* parent = nullptr) : QWidget(parent) { setFixedWidth(24); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));

        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);

        static constexpr int kMargin = 8;
        const int usable = height() - 2 * kMargin;

        static const struct { float db; const char* label; } kMarks[] = {
            {  6.0f, "+6" }, {  0.0f,  "0" }, { -5.0f,  "-5" },
            { -10.0f, "-10" }, { -20.0f, "-20" }, { -30.0f, "-30" },
            { -40.0f, "-40" }, { -60.0f, "-60" }
        };
        for (const auto& mk : kMarks) {
            const float t = dbToPos(mk.db);
            const int   y = kMargin + static_cast<int>((1.0f - t) * usable);
            p.setPen(QColor(0x55, 0x55, 0x55));
            p.drawLine(width() - 4, y, width() - 1, y);
            p.setPen(QColor(0x88, 0x88, 0x88));
            p.drawText(QRect(0, y - 7, width() - 5, 14),
                       Qt::AlignRight | Qt::AlignVCenter, mk.label);
        }
    }
};

// ---------------------------------------------------------------------------
// PeakMeterWidget — vertical peak meter with hold line, updated via setLevel().
// ---------------------------------------------------------------------------
class PeakMeterWidget : public QWidget {
public:
    PeakMeterWidget(QWidget* parent = nullptr) : QWidget(parent) { setFixedWidth(10); }

    void setLevel(float amplitude) {
        const float db = amplitude > 1e-8f
            ? 20.0f * std::log10f(amplitude) : kInfDb;
        m_currentDb = db;
        if (db > m_holdDb) {
            m_holdDb  = db;
            m_holdAge = 0;
        } else if (++m_holdAge > 30) {
            m_holdDb -= 1.5f;
            if (m_holdDb < kInfDb) m_holdDb = kInfDb;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x10, 0x10, 0x10));

        static constexpr int kMargin = 8;
        const int usable = height() - 2 * kMargin;
        const int bot    = kMargin + usable;

        auto dbToY = [&](float db) -> int {
            return kMargin + static_cast<int>((1.0f - dbToPos(db)) * usable);
        };

        const int barTop = dbToY(m_currentDb);
        if (bot > barTop) {
            const QColor col = (m_currentDb > 0.0f)  ? QColor(0xff, 0x33, 0x33)
                             : (m_currentDb > -6.0f) ? QColor(0xff, 0xcc, 0x00)
                                                      : QColor(0x22, 0xcc, 0x44);
            p.fillRect(QRect(1, barTop, width() - 2, bot - barTop), col);
        }

        if (m_holdDb > kInfDb) {
            const int hy = dbToY(m_holdDb);
            if (hy >= kMargin && hy <= bot) {
                p.setPen(Qt::white);
                p.drawLine(1, hy, width() - 2, hy);
            }
        }
    }

private:
    float m_currentDb{kInfDb};
    float m_holdDb{kInfDb};
    int   m_holdAge{0};
};

// ---------------------------------------------------------------------------
// Button stylesheet constants
static const char* kPhaseOff =
    "QPushButton { background:#2d2d2d; color:#777; border:1px solid #555;"
    " border-radius:3px; font-weight:bold; }"
    "QPushButton:hover { background:#383838; }";
static const char* kPhaseOn =
    "QPushButton { background:#22aa55; color:#fff; border:1px solid #33cc66;"
    " border-radius:3px; font-weight:bold; }";
static const char* kMuteOff =
    "QPushButton { background:#2d2d2d; color:#777; border:1px solid #555;"
    " border-radius:3px; font-weight:bold; }"
    "QPushButton:hover { background:#383838; }";
static const char* kMuteOn =
    "QPushButton { background:#bb2222; color:#fff; border:1px solid #ee3333;"
    " border-radius:3px; font-weight:bold; }";
static const char* kLinkOff =
    "QPushButton { background:#2d2d2d; color:#777; border:1px solid #555;"
    " border-radius:3px; font-size:9px; }"
    "QPushButton:hover { background:#383838; }";
static const char* kLinkOn =
    "QPushButton { background:#2255aa; color:#fff; border:1px solid #3377cc;"
    " border-radius:3px; font-size:9px; }";
static const char* kViewSel =
    "QPushButton { background:#3a5a7a; color:#fff; border:1px solid #5a8aaa;"
    " border-radius:2px; font-size:9px; font-weight:bold; }";

static QString routingHdrStyle() {
    return "QPushButton {"
           " background:#363636; color:#aaaaaa;"
           " border:none; border-top:1px solid #555; border-bottom:1px solid #555;"
           " text-align:left; padding-left:5px; font-size:10px; }"
           "QPushButton:hover { background:#404040; color:#cccccc; }";
}

// ---------------------------------------------------------------------------
static float findXp(const mcp::ShowFile::AudioSetup& as, int ch, int out) {
    for (const auto& xe : as.xpEntries)
        if (xe.ch == ch && xe.out == out) return xe.db;
    return (ch == out) ? 0.0f : kInfDb;
}

static QString fmtDb(float db) {
    if (db <= kInfDb) return "-inf";
    QString s = QString::number(static_cast<double>(db), 'f', 1);
    if (s == "-0.0") s = "0.0";
    return s;
}

// Stylesheet applied to the QFrame of a selected strip.
static QString kStripSelStyle() {
    return "QFrame { border: 1px solid #888; background: #2e2e2e; }";
}

// ---------------------------------------------------------------------------
MixConsoleDialog::MixConsoleDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Mix Console");
    setWindowFlags(windowFlags() | Qt::Window);
    resize(900, 560);

    auto* top = new QVBoxLayout(this);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);

    // ── Fixed toolbar (not inside scroll area) ──────────────────────────────
    auto* toolbar = new QWidget;
    toolbar->setFixedHeight(36);
    toolbar->setStyleSheet("background:#2a2a2a; border-bottom:1px solid #444;");
    auto* tbl = new QHBoxLayout(toolbar);
    tbl->setContentsMargins(8, 4, 8, 4);
    tbl->setSpacing(6);

    // ── Snapshot navigation ────────────────────────────────────────────────
    m_snapPrevBtn = new QPushButton("◀");
    m_snapPrevBtn->setFixedSize(26, 26);
    m_snapPrevBtn->setAutoDefault(false); m_snapPrevBtn->setDefault(false);
    connect(m_snapPrevBtn, &QPushButton::clicked, this, [this]() {
        m_model->snapshots.navigatePrev();
        updateSnapToolbar();
    });

    m_snapNameBtn = new QPushButton("—");
    m_snapNameBtn->setMinimumWidth(120);
    m_snapNameBtn->setFixedHeight(26);
    m_snapNameBtn->setAutoDefault(false); m_snapNameBtn->setDefault(false);
    connect(m_snapNameBtn, &QPushButton::clicked, this, &MixConsoleDialog::openSnapshotList);

    m_snapNextBtn = new QPushButton("▶");
    m_snapNextBtn->setFixedSize(26, 26);
    m_snapNextBtn->setAutoDefault(false); m_snapNextBtn->setDefault(false);
    connect(m_snapNextBtn, &QPushButton::clicked, this, [this]() {
        m_model->snapshots.navigateNext();
        updateSnapToolbar();
    });

    m_snapStoreBtn = new QToolButton;
    m_snapStoreBtn->setText("Store");
    m_snapStoreBtn->setFixedHeight(26);
    m_snapStoreBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_snapStoreBtn->setPopupMode(QToolButton::MenuButtonPopup);
    auto* storeMenu = new QMenu(m_snapStoreBtn);
    auto* storeAllAct = storeMenu->addAction(tr("Store All Channels"));
    connect(storeAllAct, &QAction::triggered, this, [this]() {
        m_model->storeSnapshotAll();
        updateSnapToolbar();
    });
    m_snapStoreBtn->setMenu(storeMenu);
    connect(m_snapStoreBtn, &QToolButton::clicked, this, &MixConsoleDialog::storeSnapshot);

    m_snapRecallBtn = new QPushButton("Recall");
    m_snapRecallBtn->setFixedHeight(26);
    m_snapRecallBtn->setAutoDefault(false); m_snapRecallBtn->setDefault(false);
    connect(m_snapRecallBtn, &QPushButton::clicked, this, &MixConsoleDialog::recallSnapshot);

    auto* snapSep = new QFrame;
    snapSep->setFrameShape(QFrame::VLine);
    snapSep->setStyleSheet("color:#555;");

    // ── Channel management ─────────────────────────────────────────────────
    auto* addBtn = new QPushButton("＋ Add Channel");
    addBtn->setFixedHeight(26);
    addBtn->setAutoDefault(false); addBtn->setDefault(false);
    connect(addBtn, &QPushButton::clicked, this, [this]() { addChannel(); });

    auto* removeBtn = new QPushButton("－ Remove");
    removeBtn->setFixedHeight(26);
    removeBtn->setAutoDefault(false); removeBtn->setDefault(false);
    connect(removeBtn, &QPushButton::clicked, this, [this]() { removeSelectedChannels(); });

    tbl->addWidget(m_snapPrevBtn);
    tbl->addWidget(m_snapNameBtn, 1);
    tbl->addWidget(m_snapNextBtn);
    tbl->addWidget(m_snapStoreBtn);
    tbl->addWidget(m_snapRecallBtn);
    tbl->addWidget(snapSep);
    tbl->addWidget(addBtn);
    tbl->addWidget(removeBtn);
    top->addWidget(toolbar);

    m_scroll = new QScrollArea;
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_scroll->setWidgetResizable(false);
    top->addWidget(m_scroll, 1);

    buildConsole();

    m_meterTimer = new QTimer(this);
    connect(m_meterTimer, &QTimer::timeout, this, [this]() {
        const auto peaks = m_model->engine.takeChannelPeaks();
        for (auto& s : m_strips) {
            if (s.peakMeter && s.ch >= 0 && static_cast<size_t>(s.ch) < peaks.size())
                s.peakMeter->setLevel(peaks[static_cast<size_t>(s.ch)]);
            if (s.peakMeter2 && s.slaveCh >= 0 && static_cast<size_t>(s.slaveCh) < peaks.size())
                s.peakMeter2->setLevel(peaks[static_cast<size_t>(s.slaveCh)]);
        }
    });
    m_meterTimer->start(50);
    updateSnapToolbar();
}

// ---------------------------------------------------------------------------
void MixConsoleDialog::buildConsole()
{
    if (m_content) {
        m_scroll->takeWidget();
        delete m_content;
        m_content = nullptr;
    }
    m_strips.clear();

    auto& as = m_model->sf.audioSetup;

    const int nCh = static_cast<int>(as.channels.size());

    int nPhys = 0;
    if (as.devices.empty()) {
        nPhys = m_model->engineOk ? m_model->engine.channels() : 2;
    } else {
        for (const auto& d : as.devices) nPhys += d.channelCount;
    }
    nPhys = std::max(nPhys, 1);

    std::vector<QString> outLabels(static_cast<size_t>(nPhys));
    if (as.devices.size() <= 1) {
        for (int p = 0; p < nPhys; ++p)
            outLabels[static_cast<size_t>(p)] = QString("Out %1").arg(p + 1);
    } else {
        int p = 0;
        for (int d = 0; d < static_cast<int>(as.devices.size()) && p < nPhys; ++d) {
            const int dc = as.devices[static_cast<size_t>(d)].channelCount;
            for (int oc = 0; oc < dc && p < nPhys; ++oc, ++p)
                outLabels[static_cast<size_t>(p)] = QString("D%1.%2").arg(d + 1).arg(oc + 1);
        }
    }

    m_content = new QWidget;
    auto* row = new QHBoxLayout(m_content);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(3);

    for (int ch = 0; ch < nCh; ++ch) {
        // Slave channels of a linked pair are rendered inside the master strip.
        if (ch > 0 && as.channels[static_cast<size_t>(ch - 1)].linkedStereo) continue;

        const bool isLinked = as.channels[static_cast<size_t>(ch)].linkedStereo
                              && (ch + 1 < nCh);
        const int stripW = isLinked ? kStripLinkedW : kStripW;
        const int innerW = stripW - 10;

        const auto& c  = as.channels[static_cast<size_t>(ch)];
        const auto* cs = isLinked ? &as.channels[static_cast<size_t>(ch + 1)] : nullptr;

        Strip s;
        s.ch      = ch;
        s.slaveCh = isLinked ? ch + 1 : -1;

        auto* strip = new QFrame;
        s.frame = strip;
        strip->setFrameShape(QFrame::StyledPanel);
        strip->setFixedWidth(stripW);
        auto* vl = new QVBoxLayout(strip);
        vl->setContentsMargins(5, 5, 5, 5);
        vl->setSpacing(4);

        // ── Phase row ─────────────────────────────────────────────────────
        {
            auto* phaseRow = new QWidget;
            auto* phl = new QHBoxLayout(phaseRow);
            phl->setContentsMargins(0, 0, 0, 0);
            phl->setSpacing(3);

            s.phaseBtn = new QPushButton(isLinked ? "ø L" : "ø Phase");
            s.phaseBtn->setCheckable(true);
            s.phaseBtn->setChecked(c.phaseInvert);
            s.phaseBtn->setFixedHeight(24);
            s.phaseBtn->setAutoDefault(false);
            s.phaseBtn->setDefault(false);
            s.phaseBtn->setStyleSheet(c.phaseInvert ? kPhaseOn : kPhaseOff);
            connect(s.phaseBtn, &QPushButton::toggled, this, [this, ch](bool on) {
                auto& channels = m_model->sf.audioSetup.channels;
                for (auto& ss : m_strips)
                    if (ss.ch == ch) { ss.phaseBtn->setStyleSheet(on ? kPhaseOn : kPhaseOff); break; }
                if (ch >= 0 && ch < static_cast<int>(channels.size()))
                    channels[static_cast<size_t>(ch)].phaseInvert = on;
                m_model->snapshots.markDirty(mixerParamPath(ch, "polarity"));
                // Option: sync phase invert to other selected strips
                if ((QApplication::keyboardModifiers() & Qt::AltModifier) && m_selectedChs.count(ch)) {
                    for (auto& ss : m_strips) {
                        if (ss.ch == ch || !m_selectedChs.count(ss.ch) || !ss.phaseBtn) continue;
                        ss.phaseBtn->blockSignals(true);
                        ss.phaseBtn->setChecked(on);
                        ss.phaseBtn->blockSignals(false);
                        ss.phaseBtn->setStyleSheet(on ? kPhaseOn : kPhaseOff);
                        if (ss.ch >= 0 && ss.ch < static_cast<int>(channels.size()))
                            channels[static_cast<size_t>(ss.ch)].phaseInvert = on;
                        m_model->snapshots.markDirty(mixerParamPath(ss.ch, "polarity"));
                    }
                }
                applyAll();
            });
            phl->addWidget(s.phaseBtn, 1);

            if (isLinked) {
                s.phaseBtn2 = new QPushButton("ø R");
                s.phaseBtn2->setCheckable(true);
                s.phaseBtn2->setChecked(cs->phaseInvert);
                s.phaseBtn2->setFixedHeight(24);
                s.phaseBtn2->setAutoDefault(false);
                s.phaseBtn2->setDefault(false);
                s.phaseBtn2->setStyleSheet(cs->phaseInvert ? kPhaseOn : kPhaseOff);
                const int sch = ch + 1;
                connect(s.phaseBtn2, &QPushButton::toggled, this, [this, ch, sch](bool on) {
                    auto& channels = m_model->sf.audioSetup.channels;
                    for (auto& ss : m_strips)
                        if (ss.ch == ch) { ss.phaseBtn2->setStyleSheet(on ? kPhaseOn : kPhaseOff); break; }
                    if (sch < static_cast<int>(channels.size()))
                        channels[static_cast<size_t>(sch)].phaseInvert = on;
                    m_model->snapshots.markDirty(mixerParamPath(sch, "polarity"));
                    // Option: sync slave phase to other selected linked strips
                    if ((QApplication::keyboardModifiers() & Qt::AltModifier) && m_selectedChs.count(ch)) {
                        for (auto& ss : m_strips) {
                            if (ss.ch == ch || !m_selectedChs.count(ss.ch) || !ss.phaseBtn2) continue;
                            ss.phaseBtn2->blockSignals(true);
                            ss.phaseBtn2->setChecked(on);
                            ss.phaseBtn2->blockSignals(false);
                            ss.phaseBtn2->setStyleSheet(on ? kPhaseOn : kPhaseOff);
                            if (ss.slaveCh >= 0 && ss.slaveCh < static_cast<int>(channels.size()))
                                channels[static_cast<size_t>(ss.slaveCh)].phaseInvert = on;
                            m_model->snapshots.markDirty(mixerParamPath(ss.slaveCh, "polarity"));
                        }
                    }
                    applyAll();
                });
                phl->addWidget(s.phaseBtn2, 1);
            }
            vl->addWidget(phaseRow);
        }

        // ── Delay ─────────────────────────────────────────────────────────
        {
            const bool inSamp = c.delayInSamples;
            s.delaySpin = new QDoubleSpinBox;
            s.delaySpin->setFixedWidth(innerW);
            s.delaySpin->setDecimals(inSamp ? 0 : 1);
            s.delaySpin->setRange(0.0, inSamp
                ? static_cast<double>(mcp::AudioEngine::kMaxDelaySamples - 1) : 1000.0);
            s.delaySpin->setValue(inSamp ? c.delaySamples : c.delayMs);
            s.delaySpin->setToolTip(isLinked ? "Delay — shared L+R" : "Delay");

            s.delayUnit = new QComboBox;
            s.delayUnit->setFixedWidth(innerW);
            s.delayUnit->addItem("ms");
            s.delayUnit->addItem("samp");
            s.delayUnit->setCurrentIndex(inSamp ? 1 : 0);

            connect(s.delayUnit, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, ch](int idx) {
                Strip* sp = nullptr;
                for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
                if (!sp) return;
                const bool isSamp = (idx == 1);
                sp->delaySpin->setDecimals(isSamp ? 0 : 1);
                sp->delaySpin->setRange(0.0, isSamp
                    ? static_cast<double>(mcp::AudioEngine::kMaxDelaySamples - 1) : 1000.0);
                m_model->snapshots.markDirty(mixerParamPath(ch, "delay"));
                applyChannelDsp(ch);
            });
            connect(s.delaySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this, ch](double) {
                m_model->snapshots.markDirty(mixerParamPath(ch, "delay"));
                applyChannelDsp(ch);
            });

            vl->addWidget(s.delaySpin);
            vl->addWidget(s.delayUnit);
        }

        // ── Routing header ────────────────────────────────────────────────
        {
            auto* hdrContainer = new QWidget;
            auto* hhl = new QHBoxLayout(hdrContainer);
            hhl->setContentsMargins(0, 0, 0, 0);
            hhl->setSpacing(2);

            s.routingHdr = new QPushButton(m_routingVisible ? "▾ Routing" : "▸ Routing");
            s.routingHdr->setFlat(false);
            s.routingHdr->setFixedHeight(20);
            s.routingHdr->setAutoDefault(false);
            s.routingHdr->setDefault(false);
            s.routingHdr->setStyleSheet(routingHdrStyle());
            connect(s.routingHdr, &QPushButton::clicked, this, [this]() {
                m_routingVisible = !m_routingVisible;
                const QString arrow = m_routingVisible ? "▾ Routing" : "▸ Routing";
                for (auto& ss : m_strips) {
                    if (ss.routingHdr) ss.routingHdr->setText(arrow);
                    if (!ss.routingBody) continue;
                    auto* stack = qobject_cast<QStackedWidget*>(ss.routingBody->parentWidget());
                    if (stack)
                        stack->setVisible(m_routingVisible);
                    else
                        ss.routingBody->setVisible(m_routingVisible);
                }
                if (m_content) m_content->adjustSize();
            });
            hhl->addWidget(s.routingHdr, 1);

            if (isLinked) {
                auto* viewBtn = new QPushButton("L");
                viewBtn->setFixedSize(22, 20);
                viewBtn->setAutoDefault(false);
                viewBtn->setDefault(false);
                viewBtn->setStyleSheet(kViewSel);
                connect(viewBtn, &QPushButton::clicked, this, [this, ch, viewBtn]() {
                    Strip* sp = nullptr;
                    for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
                    if (!sp || !sp->routingBody) return;
                    sp->routingView = (sp->routingView == 0) ? 1 : 0;
                    viewBtn->setText(sp->routingView == 0 ? "L" : "R");
                    auto* stack = qobject_cast<QStackedWidget*>(sp->routingBody->parentWidget());
                    if (stack) stack->setCurrentIndex(sp->routingView);
                });
                hhl->addWidget(viewBtn);
            }
            vl->addWidget(hdrContainer);
        }

        // ── Routing body/bodies ───────────────────────────────────────────
        if (isLinked) {
            auto* routingStack = new QStackedWidget;
            routingStack->setVisible(m_routingVisible);

            s.routingBody = new QWidget;
            {
                auto* rl = new QVBoxLayout(s.routingBody);
                rl->setContentsMargins(0, 2, 0, 2);
                rl->setSpacing(2);
                s.xpCells.resize(static_cast<size_t>(nPhys), nullptr);
                for (int p = 0; p < nPhys; ++p) {
                    const float db = findXp(as, ch, p);
                    auto* cell = new XpCellWidget(outLabels[static_cast<size_t>(p)], db);
                    cell->setFixedWidth(innerW);
                    cell->onCommit = [this, ch, p](float db) { applyXp(ch, p, db); };
                    s.xpCells[static_cast<size_t>(p)] = cell;
                    rl->addWidget(cell);
                }
            }
            routingStack->addWidget(s.routingBody);

            s.routingBody2 = new QWidget;
            {
                auto* rl2 = new QVBoxLayout(s.routingBody2);
                rl2->setContentsMargins(0, 2, 0, 2);
                rl2->setSpacing(2);
                s.xpCells2.resize(static_cast<size_t>(nPhys), nullptr);
                const int sch = ch + 1;
                for (int p = 0; p < nPhys; ++p) {
                    const float db = findXp(as, sch, p);
                    auto* cell = new XpCellWidget(outLabels[static_cast<size_t>(p)], db);
                    cell->setFixedWidth(innerW);
                    cell->onCommit = [this, sch, p](float db) { applyXp(sch, p, db); };
                    s.xpCells2[static_cast<size_t>(p)] = cell;
                    rl2->addWidget(cell);
                }
            }
            routingStack->addWidget(s.routingBody2);
            routingStack->setCurrentIndex(s.routingView);
            vl->addWidget(routingStack);
        } else {
            s.routingBody = new QWidget;
            auto* rl = new QVBoxLayout(s.routingBody);
            rl->setContentsMargins(0, 2, 0, 2);
            rl->setSpacing(2);
            s.xpCells.resize(static_cast<size_t>(nPhys), nullptr);
            for (int p = 0; p < nPhys; ++p) {
                const float db = findXp(as, ch, p);
                auto* cell = new XpCellWidget(outLabels[static_cast<size_t>(p)], db, s.routingBody);
                cell->setFixedWidth(innerW);
                cell->onCommit = [this, ch, p](float db) { applyXp(ch, p, db); };
                s.xpCells[static_cast<size_t>(p)] = cell;
                rl->addWidget(cell);
            }
            s.routingBody->setVisible(m_routingVisible);
            vl->addWidget(s.routingBody);
        }

        // ── Mute ──────────────────────────────────────────────────────────
        s.muteBtn = new QPushButton("MUTE");
        s.muteBtn->setCheckable(true);
        s.muteBtn->setChecked(c.mute);
        s.muteBtn->setFixedWidth(innerW);
        s.muteBtn->setFixedHeight(28);
        s.muteBtn->setAutoDefault(false);
        s.muteBtn->setDefault(false);
        s.muteBtn->setStyleSheet(c.mute ? kMuteOn : kMuteOff);
        connect(s.muteBtn, &QPushButton::toggled, this, [this, ch](bool on) {
            auto& channels = m_model->sf.audioSetup.channels;
            for (auto& ss : m_strips)
                if (ss.ch == ch) { ss.muteBtn->setStyleSheet(on ? kMuteOn : kMuteOff); break; }
            m_model->snapshots.markDirty(mixerParamPath(ch, "mute"));
            if (ch >= 0 && ch < static_cast<int>(channels.size())) {
                channels[static_cast<size_t>(ch)].mute = on;
                // propagate to slave if linked
                Strip* sp = nullptr;
                for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
                if (sp && sp->slaveCh >= 0 && sp->slaveCh < static_cast<int>(channels.size())) {
                    channels[static_cast<size_t>(sp->slaveCh)].mute = on;
                    m_model->snapshots.markDirty(mixerParamPath(sp->slaveCh, "mute"));
                }
            }
            // Option: sync mute to other selected strips
            if ((QApplication::keyboardModifiers() & Qt::AltModifier) && m_selectedChs.count(ch)) {
                for (auto& ss : m_strips) {
                    if (ss.ch == ch || !m_selectedChs.count(ss.ch) || !ss.muteBtn) continue;
                    ss.muteBtn->blockSignals(true);
                    ss.muteBtn->setChecked(on);
                    ss.muteBtn->blockSignals(false);
                    ss.muteBtn->setStyleSheet(on ? kMuteOn : kMuteOff);
                    if (ss.ch >= 0 && ss.ch < static_cast<int>(channels.size())) {
                        channels[static_cast<size_t>(ss.ch)].mute = on;
                        m_model->snapshots.markDirty(mixerParamPath(ss.ch, "mute"));
                        if (ss.slaveCh >= 0 && ss.slaveCh < static_cast<int>(channels.size())) {
                            channels[static_cast<size_t>(ss.slaveCh)].mute = on;
                            m_model->snapshots.markDirty(mixerParamPath(ss.slaveCh, "mute"));
                        }
                    }
                }
            }
            applyAll();
        });
        vl->addWidget(s.muteBtn);

        auto* faderSep = new QFrame;
        faderSep->setFrameShape(QFrame::HLine);
        vl->addWidget(faderSep);

        // ── Fader + peak meter(s) ─────────────────────────────────────────
        s.fader = new QSlider(Qt::Vertical);
        s.fader->setRange(kFaderMin, kFaderMax);
        s.fader->setTickPosition(QSlider::NoTicks);
        s.fader->setMinimumHeight(180);
        s.fader->installEventFilter(this);
        const int initVal = std::clamp(
            static_cast<int>(std::round(dbToPos(c.masterGainDb) * 1000.0f)), kFaderMin, kFaderMax);
        s.lastFaderVal = initVal;
        s.fader->setValue(initVal);

        s.gainLabel = new QLineEdit(fmtDb(c.masterGainDb) + " dB");
        s.gainLabel->setAlignment(Qt::AlignHCenter);
        s.gainLabel->setFixedHeight(18);
        s.gainLabel->setFrame(false);
        s.gainLabel->setStyleSheet(
            "QLineEdit { background:transparent; color:#aaa; font-size:10px; }"
            "QLineEdit:focus { color:#fff; border-bottom:1px solid #666; }");

        connect(s.fader, &QSlider::valueChanged, this, [this, ch](int val) {
            const float db = posToDb(static_cast<float>(val) / 1000.0f);

            // Compute delta before updating lastFaderVal
            int prevVal = val;
            for (auto& ss : m_strips)
                if (ss.ch == ch) { prevVal = ss.lastFaderVal; break; }
            const int delta = val - prevVal;

            // Update this strip
            for (auto& ss : m_strips) {
                if (ss.ch != ch) continue;
                ss.gainLabel->setText(fmtDb(db) + " dB");
                ss.lastFaderVal = val;
                break;
            }

            m_model->snapshots.markDirty(mixerParamPath(ch, "fader"));

            // Option: move other selected faders by the same delta
            if (delta != 0 && (QApplication::keyboardModifiers() & Qt::AltModifier)
                            && m_selectedChs.count(ch)) {
                auto& channels = m_model->sf.audioSetup.channels;
                for (auto& ss : m_strips) {
                    if (ss.ch == ch || !m_selectedChs.count(ss.ch) || !ss.fader) continue;
                    const int nv = std::clamp(ss.fader->value() + delta, kFaderMin, kFaderMax);
                    ss.fader->blockSignals(true);
                    ss.fader->setValue(nv);
                    ss.fader->blockSignals(false);
                    const float ndb = posToDb(static_cast<float>(nv) / 1000.0f);
                    if (ss.gainLabel) ss.gainLabel->setText(fmtDb(ndb) + " dB");
                    ss.lastFaderVal = nv;
                    m_model->snapshots.markDirty(mixerParamPath(ss.ch, "fader"));
                    if (ss.ch >= 0 && ss.ch < static_cast<int>(channels.size())) {
                        channels[static_cast<size_t>(ss.ch)].masterGainDb = ndb;
                        if (channels[static_cast<size_t>(ss.ch)].linkedStereo &&
                            ss.slaveCh >= 0 && ss.slaveCh < static_cast<int>(channels.size())) {
                            channels[static_cast<size_t>(ss.slaveCh)].masterGainDb = ndb;
                            m_model->snapshots.markDirty(mixerParamPath(ss.slaveCh, "fader"));
                        }
                    }
                }
            }

            applyFader(ch, val);
        });

        connect(s.gainLabel, &QLineEdit::editingFinished, this, [this, ch]() {
            Strip* sp = nullptr;
            for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
            if (!sp) return;
            QString t = sp->gainLabel->text().trimmed();
            if (t.endsWith("dB", Qt::CaseInsensitive)) t.chop(2);
            t = t.trimmed();
            if (t == "-inf" || t.isEmpty()) {
                sp->fader->setValue(kFaderMin);
            } else {
                bool ok = false;
                float db = t.toFloat(&ok);
                if (ok) {
                    db = std::clamp(db, kTaperMinDb, kTaperMaxDb);
                    sp->fader->setValue(static_cast<int>(std::round(dbToPos(db) * 1000.0f)));
                } else {
                    const float cur = posToDb(static_cast<float>(sp->fader->value()) / 1000.0f);
                    sp->gainLabel->setText(fmtDb(cur) + " dB");
                }
            }
            sp->gainLabel->clearFocus();
        });

        {
            auto* faderArea = new QWidget;
            auto* fhl = new QHBoxLayout(faderArea);
            fhl->setContentsMargins(0, 0, 0, 0);
            fhl->setSpacing(2);

            fhl->addWidget(new FaderScaleWidget);
            fhl->addWidget(s.fader, 1);

            s.peakMeter = new PeakMeterWidget;
            fhl->addWidget(s.peakMeter);

            if (isLinked) {
                s.peakMeter2 = new PeakMeterWidget;
                fhl->addWidget(s.peakMeter2);
            }

            vl->addWidget(faderArea, 1);
        }
        vl->addWidget(s.gainLabel);

        auto* nameSep = new QFrame;
        nameSep->setFrameShape(QFrame::HLine);
        vl->addWidget(nameSep);

        // ── Name label(s) — single-click selects, double-click edits ──────
        auto makeNameLabel = [this](const QString& text) -> QLineEdit* {
            auto* le = new QLineEdit(text);
            le->setAlignment(Qt::AlignHCenter);
            le->setFrame(false);
            le->setReadOnly(true);
            le->setFocusPolicy(Qt::NoFocus);
            le->installEventFilter(this);
            le->setStyleSheet(
                "QLineEdit { background:transparent; color:#aaa; font-size:10px; }"
                "QLineEdit:focus { color:#fff; border-bottom:1px solid #666; }");
            return le;
        };

        if (isLinked) {
            auto* nameRow = new QWidget;
            auto* nhl = new QHBoxLayout(nameRow);
            nhl->setContentsMargins(0, 0, 0, 0);
            nhl->setSpacing(3);

            const QString name = c.name.empty()
                ? QString("Ch %1").arg(ch + 1) : QString::fromStdString(c.name);
            s.nameLabel = makeNameLabel(name);
            connect(s.nameLabel, &QLineEdit::editingFinished, this,
                    [this, ch]() { commitNameEdit(ch, false); });

            const int sch = ch + 1;
            const QString slaveName = cs->name.empty()
                ? QString("Ch %1").arg(sch + 1) : QString::fromStdString(cs->name);
            s.nameLabel2 = makeNameLabel(slaveName);
            connect(s.nameLabel2, &QLineEdit::editingFinished, this,
                    [this, ch]() { commitNameEdit(ch, true); });

            nhl->addWidget(s.nameLabel, 1);
            nhl->addWidget(s.nameLabel2, 1);
            vl->addWidget(nameRow);

            s.linkBtn = new QPushButton("⊟ Unlink");
            s.linkBtn->setFixedHeight(20);
            s.linkBtn->setAutoDefault(false);
            s.linkBtn->setDefault(false);
            s.linkBtn->setStyleSheet(kLinkOn);
            connect(s.linkBtn, &QPushButton::clicked, this, [this, ch]() {
                applyLink(ch, false);
            });
            vl->addWidget(s.linkBtn);
        } else {
            const QString name = c.name.empty()
                ? QString("Ch %1").arg(ch + 1) : QString::fromStdString(c.name);
            s.nameLabel = makeNameLabel(name);
            s.nameLabel->setFixedWidth(innerW);
            connect(s.nameLabel, &QLineEdit::editingFinished, this,
                    [this, ch]() { commitNameEdit(ch, false); });
            vl->addWidget(s.nameLabel);

            s.linkBtn = new QPushButton("⊞ Link");
            s.linkBtn->setFixedHeight(20);
            s.linkBtn->setEnabled(ch + 1 < nCh);
            s.linkBtn->setToolTip(ch + 1 < nCh ? "Link with next channel" : "No next channel");
            s.linkBtn->setAutoDefault(false);
            s.linkBtn->setDefault(false);
            s.linkBtn->setStyleSheet(kLinkOff);
            connect(s.linkBtn, &QPushButton::clicked, this, [this, ch]() {
                applyLink(ch, true);
            });
            vl->addWidget(s.linkBtn);
        }

        m_strips.push_back(std::move(s));
        row->addWidget(strip);
    }

    row->addStretch();
    m_content->adjustSize();
    m_content->installEventFilter(this);
    m_scroll->setWidget(m_content);

    // Reapply selection highlight after rebuild
    for (const int selCh : m_selectedChs)
        updateStripVisual(selCh);
}

// ---------------------------------------------------------------------------
void MixConsoleDialog::refresh() {
    const auto& as = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());

    // Compute expected strip count for the current channel/link configuration.
    int expectedStrips = 0;
    for (int ch = 0; ch < nCh; ++ch) {
        if (ch > 0 && as.channels[static_cast<size_t>(ch - 1)].linkedStereo) continue;
        ++expectedStrips;
    }

    // Full rebuild only when structure changed; otherwise just update values.
    if (static_cast<int>(m_strips.size()) != expectedStrips)
        buildConsole();
    else
        refreshValues();
}

void MixConsoleDialog::refreshValues() {
    const auto& as = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());

    for (auto& s : m_strips) {
        const int ch = s.ch;
        if (ch < 0 || ch >= nCh) continue;
        const auto& c = as.channels[static_cast<size_t>(ch)];

        // Fader — only repaint if value actually changed.
        if (s.fader) {
            const int sliderVal = std::clamp(
                static_cast<int>(std::round(dbToPos(c.masterGainDb) * 1000.0f)),
                kFaderMin, kFaderMax);
            if (s.fader->value() != sliderVal) {
                s.fader->blockSignals(true);
                s.fader->setValue(sliderVal);
                s.lastFaderVal = sliderVal;
                s.fader->blockSignals(false);
                if (s.gainLabel)
                    s.gainLabel->setText(fmtDb(c.masterGainDb) + " dB");
            }
        }

        // Mute
        if (s.muteBtn && s.muteBtn->isChecked() != c.mute) {
            s.muteBtn->blockSignals(true);
            s.muteBtn->setChecked(c.mute);
            s.muteBtn->setStyleSheet(c.mute ? kMuteOn : kMuteOff);
            s.muteBtn->blockSignals(false);
        }

        // Phase (master)
        if (s.phaseBtn && s.phaseBtn->isChecked() != c.phaseInvert) {
            s.phaseBtn->blockSignals(true);
            s.phaseBtn->setChecked(c.phaseInvert);
            s.phaseBtn->setStyleSheet(c.phaseInvert ? kPhaseOn : kPhaseOff);
            s.phaseBtn->blockSignals(false);
        }

        // Phase (slave)
        const int sch = s.slaveCh;
        if (sch >= 0 && sch < nCh && s.phaseBtn2) {
            const bool sInv = as.channels[static_cast<size_t>(sch)].phaseInvert;
            if (s.phaseBtn2->isChecked() != sInv) {
                s.phaseBtn2->blockSignals(true);
                s.phaseBtn2->setChecked(sInv);
                s.phaseBtn2->setStyleSheet(sInv ? kPhaseOn : kPhaseOff);
                s.phaseBtn2->blockSignals(false);
            }
        }
    }

    updateSnapToolbar();
}

void MixConsoleDialog::applyAll() { m_model->applyMixing(); }

void MixConsoleDialog::commitNameEdit(int ch, bool isSlave)
{
    Strip* sp = nullptr;
    for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
    if (!sp) return;

    auto* le = isSlave ? sp->nameLabel2 : sp->nameLabel;
    if (!le) return;

    const int targetCh = isSlave ? sp->slaveCh : ch;
    auto& channels = m_model->sf.audioSetup.channels;
    if (targetCh >= 0 && targetCh < static_cast<int>(channels.size()))
        channels[static_cast<size_t>(targetCh)].name = le->text().toStdString();
    m_model->dirty = true;

    le->setReadOnly(true);
    le->setFocusPolicy(Qt::NoFocus);
    le->clearFocus();
}

void MixConsoleDialog::applyChannelDsp(int ch)
{
    auto& channels = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(channels.size())) return;
    Strip* sp = nullptr;
    for (auto& s : m_strips) if (s.ch == ch) { sp = &s; break; }
    if (!sp) return;

    auto& c = channels[static_cast<size_t>(ch)];
    c.phaseInvert    = sp->phaseBtn->isChecked();
    const bool isSamp = (sp->delayUnit->currentIndex() == 1);
    c.delayInSamples  = isSamp;
    if (isSamp) c.delaySamples = static_cast<int>(sp->delaySpin->value());
    else        c.delayMs      = sp->delaySpin->value();

    // Propagate delay to slave (phase is independent)
    if (sp->slaveCh >= 0 && sp->slaveCh < static_cast<int>(channels.size())) {
        auto& cs = channels[static_cast<size_t>(sp->slaveCh)];
        cs.delayInSamples = isSamp;
        cs.delaySamples   = c.delaySamples;
        cs.delayMs        = c.delayMs;
    }
    applyAll();
}

void MixConsoleDialog::applyXp(int ch, int out, float db)
{
    auto& as = m_model->sf.audioSetup;

    // Option-key sync: compute delta before modifying xpEntries
    const bool doSync = (QApplication::keyboardModifiers() & Qt::AltModifier)
                        && m_selectedChs.count(ch);
    const float oldDb  = findXp(as, ch, out);
    const float delta  = (doSync && oldDb > kInfDb && db > kInfDb) ? db - oldDb : 0.0f;

    // Helper: update one (chX, out) entry
    auto setEntry = [&](int chX, float val) {
        const bool isDef = (chX != out) && (val <= kInfDb);
        auto& xp = as.xpEntries;
        xp.erase(std::remove_if(xp.begin(), xp.end(),
            [chX, out](const mcp::ShowFile::AudioSetup::XpEntry& xe) {
                return xe.ch == chX && xe.out == out;
            }), xp.end());
        if (!isDef) xp.push_back({chX, out, val});
        // Refresh cell widgets
        for (auto& s : m_strips) {
            if (s.ch == chX && out < static_cast<int>(s.xpCells.size()))
                if (auto* cell = s.xpCells[static_cast<size_t>(out)]) cell->setDb(val);
            if (s.slaveCh == chX && out < static_cast<int>(s.xpCells2.size()))
                if (auto* cell = s.xpCells2[static_cast<size_t>(out)]) cell->setDb(val);
        }
    };

    setEntry(ch, db);
    m_model->snapshots.markDirty(xpPath(ch, out));

    if (doSync && delta != 0.0f) {
        for (auto& ss : m_strips) {
            if (ss.ch == ch || !m_selectedChs.count(ss.ch)) continue;
            const float old2   = findXp(as, ss.ch, out);
            const float newDb2 = (old2 <= kInfDb) ? kInfDb
                                  : std::clamp(old2 + delta, -60.0f, 10.0f);
            setEntry(ss.ch, newDb2);
            m_model->snapshots.markDirty(xpPath(ss.ch, out));
        }
    }

    applyAll();
}

void MixConsoleDialog::applyFader(int ch, int sliderVal)
{
    auto& channels = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(channels.size())) return;
    const float db = posToDb(static_cast<float>(sliderVal) / 1000.0f);
    channels[static_cast<size_t>(ch)].masterGainDb = db;
    if (channels[static_cast<size_t>(ch)].linkedStereo &&
        ch + 1 < static_cast<int>(channels.size()))
        channels[static_cast<size_t>(ch + 1)].masterGainDb = db;
    applyAll();
}

void MixConsoleDialog::applyMute(int ch, bool muted)
{
    auto& channels = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(channels.size())) return;
    channels[static_cast<size_t>(ch)].mute = muted;
    if (channels[static_cast<size_t>(ch)].linkedStereo &&
        ch + 1 < static_cast<int>(channels.size()))
        channels[static_cast<size_t>(ch + 1)].mute = muted;
    applyAll();
}

void MixConsoleDialog::applyLink(int ch, bool linked)
{
    auto& channels = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(channels.size())) return;
    channels[static_cast<size_t>(ch)].linkedStereo = linked;
    m_model->dirty = true;

    const int scrollX = m_scroll->horizontalScrollBar()->value();
    const int scrollY = m_scroll->verticalScrollBar()->value();
    QTimer::singleShot(0, this, [this, scrollX, scrollY]() {
        refresh();
        m_scroll->horizontalScrollBar()->setValue(scrollX);
        m_scroll->verticalScrollBar()->setValue(scrollY);
        applyAll();
        raise();
        activateWindow();
    });
}

void MixConsoleDialog::addChannel()
{
    auto& as = m_model->sf.audioSetup;
    mcp::ShowFile::AudioSetup::Channel ch;
    ch.name = "Ch " + std::to_string(as.channels.size() + 1);
    as.channels.push_back(ch);
    int nPhys = 0;
    if (as.devices.empty())
        nPhys = m_model->engineOk ? m_model->engine.channels() : 2;
    else
        for (const auto& d : as.devices) nPhys += d.channelCount;
    const int newIdx = static_cast<int>(as.channels.size()) - 1;
    as.xpEntries.push_back({newIdx, newIdx, 0.0f});
    m_model->dirty = true;
    QTimer::singleShot(0, this, [this]() {
        refresh();
        raise();
        activateWindow();
    });
}

void MixConsoleDialog::removeSelectedChannels()
{
    auto& as = m_model->sf.audioSetup;
    if (as.channels.empty()) return;

    std::set<int> toRemove = m_selectedChs;
    if (toRemove.empty())
        toRemove.insert(static_cast<int>(as.channels.size()) - 1);

    // Build old→new index map
    std::vector<int> newIdx(as.channels.size(), -1);
    int next = 0;
    for (int i = 0; i < static_cast<int>(as.channels.size()); ++i)
        if (!toRemove.count(i)) newIdx[static_cast<size_t>(i)] = next++;

    // Remove channels (reverse order)
    for (int i = static_cast<int>(as.channels.size()) - 1; i >= 0; --i)
        if (toRemove.count(i))
            as.channels.erase(as.channels.begin() + i);

    // Remove xpEntries whose logical channel is deleted; renumber the rest
    auto& xp = as.xpEntries;
    xp.erase(std::remove_if(xp.begin(), xp.end(),
        [&toRemove](const mcp::ShowFile::AudioSetup::XpEntry& xe) {
            return toRemove.count(xe.ch) > 0;
        }), xp.end());
    for (auto& xe : xp)
        if (xe.ch >= 0 && xe.ch < static_cast<int>(newIdx.size()))
            xe.ch = newIdx[static_cast<size_t>(xe.ch)];

    m_selectedChs.clear();
    m_model->dirty = true;
    QTimer::singleShot(0, this, [this]() {
        refresh();
        raise();
        activateWindow();
    });
}

// ---------------------------------------------------------------------------
void MixConsoleDialog::updateSnapToolbar()
{
    if (!m_snapPrevBtn) return;
    const auto& snap = m_model->snapshots;
    const int count = snap.snapshotCount();
    const int idx   = snap.currentIndex();

    const auto& snapSlots = m_model->sf.snapshotList.snapshots;
    if (!snap.isEmptySlot() && static_cast<size_t>(idx) < snapSlots.size() && snapSlots[static_cast<size_t>(idx)])
        m_snapNameBtn->setText(QString("[%1] %2")
            .arg(idx + 1)
            .arg(QString::fromStdString(snapSlots[static_cast<size_t>(idx)]->name)));
    else
        m_snapNameBtn->setText(tr("[%1] — (empty) —").arg(idx + 1));

    m_snapPrevBtn->setEnabled(idx > 0);
    m_snapNextBtn->setEnabled(idx < count);
    m_snapRecallBtn->setEnabled(!snap.isEmptySlot());
}

void MixConsoleDialog::storeSnapshot()
{
    m_model->storeSnapshot();
    updateSnapToolbar();
}

void MixConsoleDialog::recallSnapshot()
{
    m_model->recallSnapshot();
    QTimer::singleShot(0, this, [this]() {
        refresh();
        raise();
        activateWindow();
    });
}

// ---------------------------------------------------------------------------
// Snapshot list dialog — lists all snapshots with rename/delete/scope controls.
void MixConsoleDialog::openSnapshotList()
{
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Snapshots"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setMinimumWidth(420);
    dlg->setMinimumHeight(200);

    auto* vl = new QVBoxLayout(dlg);

    auto pBuildList = std::make_shared<std::function<void()>>();
    *pBuildList = [this, dlg, vl, pBuildList]() {
        // Remove all widgets except the last (Close button) when rebuilding
        while (vl->count() > 1) {
            auto* item = vl->takeAt(0);
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }

        auto& sl = m_model->sf.snapshotList;
        if (sl.snapshots.empty()) {
            vl->insertWidget(0, new QLabel(tr("No snapshots stored yet.")));
            return;
        }

        auto* scroll  = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* content = new QWidget;
        auto* grid    = new QGridLayout(content);
        grid->setSpacing(4);
        grid->setContentsMargins(4, 4, 4, 4);

        for (int i = 0; i < static_cast<int>(sl.snapshots.size()); ++i) {
            const bool filled = sl.snapshots[static_cast<size_t>(i)].has_value();

            auto* navBtn = new QPushButton(QString::number(i + 1));
            navBtn->setFixedSize(30, 24);
            navBtn->setAutoDefault(false); navBtn->setDefault(false);
            if (i == sl.currentIndex)
                navBtn->setStyleSheet("font-weight:bold; color:#fff;");
            connect(navBtn, &QPushButton::clicked, dlg, [this, dlg, i]() {
                m_model->snapshots.setCurrentIndex(i);
                updateSnapToolbar();
                dlg->close();
            });

            grid->addWidget(navBtn, i, 0);

            if (filled) {
                auto& snap = sl.snapshots[static_cast<size_t>(i)].value();

                auto* nameEdit = new QLineEdit(QString::fromStdString(snap.name));
                connect(nameEdit, &QLineEdit::editingFinished, dlg, [this, &snap, nameEdit]() {
                    snap.name = nameEdit->text().toStdString();
                    m_model->dirty = true;
                    updateSnapToolbar();
                });

                auto* scopeBtn = new QPushButton(tr("Scope…"));
                scopeBtn->setFixedHeight(24);
                scopeBtn->setAutoDefault(false); scopeBtn->setDefault(false);
                connect(scopeBtn, &QPushButton::clicked, dlg, [this, i]() {
                    openScopeEditor(i);
                });

                auto* delBtn = new QPushButton("✕");
                delBtn->setFixedSize(24, 24);
                delBtn->setAutoDefault(false); delBtn->setDefault(false);
                connect(delBtn, &QPushButton::clicked, dlg, [this, pBuildList, i]() {
                    m_model->sf.snapshotList.snapshots[static_cast<size_t>(i)] = std::nullopt;
                    m_model->dirty = true;
                    updateSnapToolbar();
                    (*pBuildList)();
                });

                grid->addWidget(nameEdit, i, 1);
                grid->addWidget(scopeBtn, i, 2);
                grid->addWidget(delBtn,   i, 3);
            } else {
                auto* emptyLbl = new QLabel(tr("— (empty) —"));
                emptyLbl->setStyleSheet("color:#555;");
                grid->addWidget(emptyLbl, i, 1, 1, 3);
            }
        }
        grid->setColumnStretch(1, 1);
        scroll->setWidget(content);
        vl->insertWidget(0, scroll, 1);
    };

    auto* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setAutoDefault(false); closeBtn->setDefault(false);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    vl->addWidget(closeBtn);

    (*pBuildList)();
    dlg->show();
}

// ---------------------------------------------------------------------------
// Scope editor — matrix of (channel × param-path) checkboxes for one snapshot.
// Columns: Ch | Delay | Polarity | Mute | Fader | →Out1 | →Out2 | …
void MixConsoleDialog::openScopeEditor(int snapIdx)
{
    auto& sl = m_model->sf.snapshotList;
    if (snapIdx < 0 || snapIdx >= static_cast<int>(sl.snapshots.size())) return;
    if (!sl.snapshots[static_cast<size_t>(snapIdx)]) return;
    auto& snap = sl.snapshots[static_cast<size_t>(snapIdx)].value();
    const auto& as = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());
    if (nCh == 0) return;

    int nPhys = m_model->engineOk ? m_model->engine.channels() : 2;
    if (!as.devices.empty()) { nPhys = 0; for (const auto& d : as.devices) nPhys += d.channelCount; }

    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Scope — %1").arg(QString::fromStdString(snap.name)));
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto* vl   = new QVBoxLayout(dlg);
    auto* grid = new QGridLayout;
    grid->setSpacing(6);
    grid->setContentsMargins(4, 4, 4, 4);

    // Column layout: Ch | Delay | Polarity | Mute | Fader | →Out1 … →OutN
    enum { kColCh=0, kColDelay=1, kColPolarity=2, kColMute=3, kColFader=4, kColXpFirst=5 };

    const QStringList fixedHdrs = {tr("Channel"), tr("Delay"), tr("Polarity"), tr("Mute"), tr("Fader")};
    for (int c = 0; c < fixedHdrs.size(); ++c) {
        auto* lbl = new QLabel(fixedHdrs[c]);
        lbl->setAlignment(Qt::AlignHCenter);
        grid->addWidget(lbl, 0, c);
    }
    for (int out = 0; out < nPhys; ++out) {
        auto* lbl = new QLabel(QString(tr("→%1")).arg(out + 1));
        lbl->setAlignment(Qt::AlignHCenter);
        grid->addWidget(lbl, 0, kColXpFirst + out);
    }

    struct Row {
        int ch{-1};
        QCheckBox* delay{};
        QCheckBox* polarity{};
        QCheckBox* mute{};
        QCheckBox* fader{};
        std::vector<QCheckBox*> xpOuts;
    };
    std::vector<Row> rows(static_cast<size_t>(nCh));

    // Returns true if `path` (or any of its parents) is in snap.scope.
    auto inSnapScope = [&](const std::string& path) {
        for (const auto& s : snap.scope) {
            if (path == s) return true;
            if (path.size() > s.size() && path[s.size()] == '/' &&
                path.compare(0, s.size(), s) == 0) return true;
        }
        return false;
    };

    for (int i = 0; i < nCh; ++i) {
        const std::string base = "/mixer/" + std::to_string(i);
        auto& row = rows[static_cast<size_t>(i)];
        row.ch = i;

        auto makeCheck = [&](int col, bool checked) {
            auto* cb = new QCheckBox;
            cb->setChecked(checked);
            grid->addWidget(cb, i + 1, col, Qt::AlignHCenter);
            return cb;
        };

        grid->addWidget(new QLabel(m_model->channelName(i)), i + 1, kColCh);
        row.delay    = makeCheck(kColDelay,    inSnapScope(base + "/delay"));
        row.polarity = makeCheck(kColPolarity, inSnapScope(base + "/polarity"));
        row.mute     = makeCheck(kColMute,     inSnapScope(base + "/mute"));
        row.fader    = makeCheck(kColFader,    inSnapScope(base + "/fader"));
        row.xpOuts.resize(static_cast<size_t>(nPhys), nullptr);
        for (int out = 0; out < nPhys; ++out)
            row.xpOuts[static_cast<size_t>(out)] =
                makeCheck(kColXpFirst + out,
                          inSnapScope(base + "/crosspoint/" + std::to_string(out)));
    }

    auto* scrollWidget = new QWidget;
    scrollWidget->setLayout(grid);
    auto* scroll = new QScrollArea;
    scroll->setWidget(scrollWidget);
    scroll->setWidgetResizable(true);
    vl->addWidget(scroll, 1);

    auto* btnRow    = new QHBoxLayout;
    auto* okBtn     = new QPushButton(tr("OK"));
    auto* cancelBtn = new QPushButton(tr("Cancel"));
    okBtn->setDefault(true);
    cancelBtn->setAutoDefault(false);
    btnRow->addStretch();
    btnRow->addWidget(okBtn);
    btnRow->addWidget(cancelBtn);
    vl->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, dlg, [this, dlg, &snap, &rows, &as, nPhys]() {
        std::vector<std::string> newScope;

        for (const auto& r : rows) {
            const std::string base = "/mixer/" + std::to_string(r.ch);

            // Lazily find-or-create ChannelState for value capture
            ShowFile::SnapshotList::Snapshot::ChannelState* cs = nullptr;
            auto ensureCs = [&]() -> ShowFile::SnapshotList::Snapshot::ChannelState& {
                if (!cs) {
                    for (auto& existing : snap.channels)
                        if (existing.ch == r.ch) { cs = &existing; break; }
                    if (!cs) { snap.channels.push_back({}); cs = &snap.channels.back(); cs->ch = r.ch; }
                }
                return *cs;
            };

            if (r.delay->isChecked()) {
                newScope.push_back(base + "/delay");
                if (r.ch < static_cast<int>(as.channels.size()) && !ensureCs().delayMs) {
                    const auto& c = as.channels[static_cast<size_t>(r.ch)];
                    cs->delayMs = c.delayMs; cs->delayInSamples = c.delayInSamples;
                    cs->delaySamples = c.delaySamples;
                }
            }
            if (r.polarity->isChecked()) {
                newScope.push_back(base + "/polarity");
                if (r.ch < static_cast<int>(as.channels.size()) && !ensureCs().polarity)
                    cs->polarity = as.channels[static_cast<size_t>(r.ch)].phaseInvert;
            }
            if (r.mute->isChecked()) {
                newScope.push_back(base + "/mute");
                if (r.ch < static_cast<int>(as.channels.size()) && !ensureCs().mute)
                    cs->mute = as.channels[static_cast<size_t>(r.ch)].mute;
            }
            if (r.fader->isChecked()) {
                newScope.push_back(base + "/fader");
                if (r.ch < static_cast<int>(as.channels.size()) && !ensureCs().faderDb)
                    cs->faderDb = as.channels[static_cast<size_t>(r.ch)].masterGainDb;
            }
            for (int out = 0; out < nPhys; ++out) {
                if (out >= static_cast<int>(r.xpOuts.size()) || !r.xpOuts[static_cast<size_t>(out)]) continue;
                if (!r.xpOuts[static_cast<size_t>(out)]->isChecked()) continue;
                newScope.push_back(base + "/crosspoint/" + std::to_string(out));
                auto& s = ensureCs();
                bool found = false;
                for (const auto& xs : s.xpSends) if (xs.out == out) { found = true; break; }
                if (!found) {
                    float db = (r.ch == out) ? 0.0f : -144.0f;
                    for (const auto& xe : as.xpEntries)
                        if (xe.ch == r.ch && xe.out == out) { db = xe.db; break; }
                    s.xpSends.push_back({out, db});
                }
            }
        }

        snap.scope = std::move(newScope);
        m_model->dirty = true;
        dlg->accept();
    });

    dlg->resize(std::min(280 + nPhys * 42, 900), std::min(80 + nCh * 32 + 70, 520));
    dlg->exec();
}

// ---------------------------------------------------------------------------
void MixConsoleDialog::selectStrip(int ch, Qt::KeyboardModifiers mods)
{
    if (mods & Qt::ControlModifier) {          // Cmd on macOS = toggle
        if (m_selectedChs.count(ch))
            m_selectedChs.erase(ch);
        else
            m_selectedChs.insert(ch);
        m_selectionAnchor = ch;
    } else if ((mods & Qt::ShiftModifier) && m_selectionAnchor >= 0) {
        const int lo = std::min(m_selectionAnchor, ch);
        const int hi = std::max(m_selectionAnchor, ch);
        for (int i = lo; i <= hi; ++i) m_selectedChs.insert(i);
    } else {
        m_selectedChs.clear();
        m_selectedChs.insert(ch);
        m_selectionAnchor = ch;
    }
    for (auto& s : m_strips) updateStripVisual(s.ch);
}

void MixConsoleDialog::updateStripVisual(int ch)
{
    for (auto& s : m_strips) {
        if (s.ch != ch || !s.frame) continue;
        s.frame->setStyleSheet(m_selectedChs.count(ch) ? kStripSelStyle() : QString());
        break;
    }
}

bool MixConsoleDialog::eventFilter(QObject* obj, QEvent* event)
{
    // ── Click on empty content area → clear selection ──
    if (obj == m_content && event->type() == QEvent::MouseButtonPress) {
        if (!m_selectedChs.empty()) {
            m_selectedChs.clear();
            m_selectionAnchor = -1;
            for (auto& s : m_strips) updateStripVisual(s.ch);
        }
        return false;  // let the event propagate normally
    }

    // ── Fader double-click → reset to 0 dB (all selected if Option held) ──
    if (event->type() == QEvent::MouseButtonDblClick) {
        for (auto& s : m_strips) {
            if (s.fader == obj) {
                const int unity = static_cast<int>(std::round(dbToPos(0.0f) * 1000.0f));
                s.fader->setValue(unity);
                // Option + double-click: reset all selected faders too
                if ((QApplication::keyboardModifiers() & Qt::AltModifier)
                        && m_selectedChs.count(s.ch)) {
                    for (auto& ss : m_strips) {
                        if (ss.ch == s.ch || !m_selectedChs.count(ss.ch) || !ss.fader) continue;
                        ss.fader->blockSignals(true);
                        ss.fader->setValue(unity);
                        ss.fader->blockSignals(false);
                        const float ndb = posToDb(static_cast<float>(unity) / 1000.0f);
                        if (ss.gainLabel) ss.gainLabel->setText(fmtDb(ndb) + " dB");
                        ss.lastFaderVal = unity;
                        applyFader(ss.ch, unity);
                    }
                }
                return true;
            }
            // Name label double-click → enter edit mode
            if (s.nameLabel == obj || s.nameLabel2 == obj) {
                auto* le = static_cast<QLineEdit*>(obj);
                le->setReadOnly(false);
                le->setFocusPolicy(Qt::StrongFocus);
                le->setFocus();
                le->selectAll();
                return true;
            }
        }
    }

    // ── Name label single-click → select strip ────────────────────────────
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* me = static_cast<const QMouseEvent*>(event);
        for (auto& s : m_strips) {
            if (s.nameLabel != obj && s.nameLabel2 != obj) continue;
            // If already in edit mode, let QLineEdit handle clicks normally
            const auto* le = static_cast<const QLineEdit*>(obj);
            if (!le->isReadOnly()) return false;
            selectStrip(s.ch, me->modifiers());
            return true;
        }
    }

    // ── Name label focus-out → exit edit mode (fallback if Enter not pressed) ─
    if (event->type() == QEvent::FocusOut) {
        for (auto& s : m_strips) {
            if (s.nameLabel == obj) {
                auto* le = s.nameLabel;
                if (le && !le->isReadOnly()) commitNameEdit(s.ch, false);
                break;
            }
            if (s.nameLabel2 == obj) {
                auto* le = s.nameLabel2;
                if (le && !le->isReadOnly()) commitNameEdit(s.ch, true);
                break;
            }
        }
    }

    return QDialog::eventFilter(obj, event);
}
