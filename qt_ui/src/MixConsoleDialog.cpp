#include "MixConsoleDialog.h"
#include "AppModel.h"
#include "FaderWidget.h"

#include "engine/AudioEngine.h"
#include "engine/ShowFile.h"
#include "engine/plugin/MissingPluginProcessor.h"
#include "PluginManagerDialog.h"
#ifdef __APPLE__
#  include "engine/plugin/AUPluginAdapter.h"
#  include "AUEditorBridge.h"
#endif
#ifdef MCP_HAVE_VST3
#  include "engine/plugin/VST3Scanner.h"
#  include "engine/plugin/VST3PluginAdapter.h"
#  include <QSettings>
#  ifdef __APPLE__
#    include "VST3EditorBridge.h"
#  endif
#endif

using ShowFile = mcp::ShowFile;

static std::string mixerParamPath(int ch, const char* sub) {
    return "/mixer/" + std::to_string(ch) + "/" + sub;
}
static std::string xpPath(int ch, int out) {
    return "/mixer/" + std::to_string(ch) + "/crosspoint/" + std::to_string(out);
}
static std::string sendParamPath(int ch, int slot, const char* sub) {
    return "/mixer/" + std::to_string(ch) + "/send/" + std::to_string(slot) + "/" + sub;
}

#include <QApplication>
#include <QCheckBox>
#include <QDrag>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QScreen>
#include <QMessageBox>
#include <QToolButton>
#include <QTreeWidget>
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

// Enable scroll arrows for a QMenu.  Qt naturally caps the menu at screen
// height; without this property it clips silently instead of scrolling.
// No explicit height cap is set — the OS determines the natural maximum.
static void enableMenuScroll(QMenu* m) {
    m->setStyleSheet("QMenu { menu-scrollable: 1; }");
}

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
// PanKnob — rotary pan control, range -1..+1.
// Drag up/down to adjust; double-click resets to center.
// ---------------------------------------------------------------------------
class PanKnob : public QWidget {
public:
    std::function<void(float)> onCommit;

    explicit PanKnob(float initVal = 0.0f, QWidget* parent = nullptr)
        : QWidget(parent), m_val(std::clamp(initVal, -1.0f, 1.0f))
    {
        setFixedSize(44, 44);
        setToolTip("Drag up/down  •  Double-click to center");
        setCursor(Qt::SizeVerCursor);
    }

    float value() const { return m_val; }
    void setValue(float v) { m_val = std::clamp(v, -1.0f, 1.0f); update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(3, 3, -3, -3);
        const QPointF c = r.center();

        // Background arc: 7-o'clock (225° Qt) → 5-o'clock (315° Qt), 270° CW
        p.setPen(QPen(QColor(55, 55, 60), 3.5));
        p.setBrush(Qt::NoBrush);
        p.drawArc(r.toRect(), 225 * 16, -(270 * 16));

        // Value arc: from center top (90° Qt) sweeping toward the pan position.
        // m_val < 0 = left → CCW (positive Qt span); m_val > 0 = right → CW (negative span).
        const float sweepDeg = -m_val * 135.0f;
        if (std::abs(sweepDeg) > 0.5f) {
            p.setPen(QPen(QColor(100, 150, 230), 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(r.toRect(), 90 * 16, static_cast<int>(sweepDeg * 16.0f));
        }

        // Knob face
        const QRectF inner = r.adjusted(5, 5, -5, -5);
        p.setBrush(QColor(50, 52, 58));
        p.setPen(QPen(QColor(80, 82, 90), 1));
        p.drawEllipse(inner);

        // Indicator dot: center = 12-o'clock (-90° math), left = 7-o'clock, right = 5-o'clock
        const float angleDeg = -90.0f + m_val * 135.0f;
        const float angleRad = angleDeg * 3.14159265f / 180.0f;
        const float ir = static_cast<float>(inner.width()) * 0.5f - 4.0f;
        const QPointF dot(c.x() + ir * std::cos(angleRad),
                          c.y() + ir * std::sin(angleRad));
        p.setBrush(QColor(200, 210, 255));
        p.setPen(Qt::NoPen);
        p.drawEllipse(dot, 3.0, 3.0);
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton) {
            m_dragY   = ev->pos().y();
            m_dragVal = m_val;
        }
    }

    void mouseMoveEvent(QMouseEvent* ev) override {
        if (!(ev->buttons() & Qt::LeftButton)) return;
        const float delta = static_cast<float>(m_dragY - ev->pos().y()) / 90.0f;
        m_val = std::clamp(m_dragVal + delta, -1.0f, 1.0f);
        update();
        if (onCommit) onCommit(m_val);
    }

    void mouseDoubleClickEvent(QMouseEvent*) override {
        m_val = 0.0f;
        update();
        if (onCommit) onCommit(m_val);
    }

private:
    float m_val{0.0f};
    int   m_dragY{0};
    float m_dragVal{0.0f};
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
        resetAllPluginCaches();  // storeAll just captured everything — this is the new baseline
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

    // Reset AU param caches whenever a snapshot is recalled or mixing state changes,
    // so the next diff baseline reflects the recalled (not pre-recall) values.
    connect(m_model, &AppModel::mixStateChanged,
            this, &MixConsoleDialog::resetAllPluginCaches);

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

        // ── PDC Isolation ─────────────────────────────────────────────────
        {
            s.pdcIsoBtn = new QPushButton("PDC Iso");
            s.pdcIsoBtn->setCheckable(true);
            s.pdcIsoBtn->setChecked(c.pdcIsolated);
            s.pdcIsoBtn->setFixedHeight(20);
            s.pdcIsoBtn->setAutoDefault(false);
            s.pdcIsoBtn->setDefault(false);
            s.pdcIsoBtn->setStyleSheet(c.pdcIsolated ? kPhaseOn : kPhaseOff);
            s.pdcIsoBtn->setToolTip(
                "PDC Isolated: this channel's plugin latency is not propagated "
                "to downstream buses. Input alignment is still applied.");
            connect(s.pdcIsoBtn, &QPushButton::toggled, this, [this, ch](bool on) {
                auto& channels = m_model->sf.audioSetup.channels;
                if (ch >= 0 && ch < static_cast<int>(channels.size()))
                    channels[static_cast<size_t>(ch)].pdcIsolated = on;
                for (auto& ss : m_strips)
                    if (ss.ch == ch && ss.pdcIsoBtn)
                        ss.pdcIsoBtn->setStyleSheet(on ? kPhaseOn : kPhaseOff);
                m_model->markDirty();
                m_model->rebuildPDC();
            });
            vl->addWidget(s.pdcIsoBtn);
        }

        // ── Plugin slots ──────────────────────────────────────────────────
        buildPluginSection(s);
        vl->addWidget(s.pluginHdr);
        vl->addWidget(s.pluginBody);

        // ── Send slots ────────────────────────────────────────────────────
        buildSendSection(s);
        vl->addWidget(s.sendHdr);
        vl->addWidget(s.sendBody);

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

    // Refresh plugin slot buttons (bypass state, labels) for open plugin sections
    for (auto& s : m_strips) {
        if (s.pluginBody && s.pluginBody->isVisible())
            rebuildPluginSection(s.ch);
    }

    // Refresh any open plugin editor windows (bypass button + spinbox values)
    for (auto& [key, dlgPtr] : m_pluginEditors) {
        if (!dlgPtr) continue;
        auto* dlg = dlgPtr.data();
        const int eCh   = key.first;
        const int eSlot = key.second;

        // Update bypass button (always the first widget in the layout)
        auto* vl = qobject_cast<QVBoxLayout*>(dlg->layout());
        if (vl && vl->count() > 0) {
            auto* bypassBtn = qobject_cast<QPushButton*>(vl->itemAt(0)->widget());
            if (bypassBtn && bypassBtn->isCheckable()) {
                const auto& pSlots = m_model->sf.audioSetup.channels[static_cast<size_t>(eCh)].plugins;
                const bool bp = eSlot < static_cast<int>(pSlots.size())
                                && pSlots[static_cast<size_t>(eSlot)].bypassed;
                if (bypassBtn->isChecked() != bp) {
                    bypassBtn->blockSignals(true);
                    bypassBtn->setChecked(bp);
                    bypassBtn->blockSignals(false);
                }
            }
        }

        // For AU plugins: re-send parameter notifications so the NSView refreshes.
        auto wrapper = m_model->channelPlugin(eCh, eSlot);
        if (!wrapper || !wrapper->getProcessor()) continue;
        auto* proc = wrapper->getProcessor();
#ifdef __APPLE__
        if (auto* auProc = dynamic_cast<mcp::plugin::AUPluginAdapter*>(proc))
            auProc->notifyViewRefresh();
#endif

        // Update spinboxes tagged with pluginParamId (internal plugins / AU fallback UI)
        const auto spins = dlg->findChildren<QDoubleSpinBox*>();
        for (auto* spin : spins) {
            const QVariant pid = spin->property("pluginParamId");
            if (!pid.isValid()) continue;
            const float val = proc->getParameterValue(pid.toString().toStdString());
            if (std::abs(spin->value() - static_cast<double>(val)) > 1e-6) {
                spin->blockSignals(true);
                spin->setValue(static_cast<double>(val));
                spin->blockSignals(false);
            }
        }
    }

    updateSnapToolbar();
}

void MixConsoleDialog::resetForNewShow()
{
    // Close any open plugin editor windows — they reference old plugin instances.
    for (auto& [key, dlgPtr] : m_pluginEditors)
        if (dlgPtr) dlgPtr->close();
    m_pluginEditors.clear();
    m_pluginParamCaches.clear();  // stale baselines are invalid after a new show is loaded
    m_selectedChs.clear();
    m_selectionAnchor = -1;
    buildConsole();
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
    m_model->markDirty();

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
    m_model->markDirty();

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
    m_model->markDirty();
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
    m_model->markDirty();
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
    // Diff all open AU editors before storing so per-param scope is recorded.
    for (const auto& [key, dlgPtr] : m_pluginEditors)
        if (dlgPtr) diffAndMarkPluginDirty(key.first, key.second);

    m_model->storeSnapshot();

    // Reset caches to the just-stored values as the new baseline.
    for (const auto& [key, dlgPtr] : m_pluginEditors)
        if (dlgPtr) resetPluginCache(key.first, key.second);

    updateSnapToolbar();
}

void MixConsoleDialog::recallSnapshot()
{
    m_model->recallSnapshot();
    resetAllPluginCaches();  // recalled values are the new baseline for diff
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
                    m_model->markDirty();
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
                    m_model->markDirty();
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

    connect(dlg, &QDialog::finished, this, [this]() {
        raise();
        activateWindow();
    });

    (*pBuildList)();
    dlg->show();
}

// ---------------------------------------------------------------------------
// Scope editor — matrix of (channel × param-path) checkboxes for one snapshot.
// Columns: Ch | Delay | Polarity | Mute | Fader | →Out1 | →Out2 | …
void MixConsoleDialog::openScopeEditor(int snapIdx)
{
    auto& snapList = m_model->sf.snapshotList;
    if (snapIdx < 0 || snapIdx >= static_cast<int>(snapList.snapshots.size())) return;
    if (!snapList.snapshots[static_cast<size_t>(snapIdx)]) return;
    auto& snap = snapList.snapshots[static_cast<size_t>(snapIdx)].value();
    const auto& as = m_model->sf.audioSetup;
    if (as.channels.empty()) return;

    // Prefix-match: returns true if path is covered by an entry in snap.scope.
    auto inSnapScope = [&](const std::string& path) {
        for (const auto& sc : snap.scope) {
            if (path == sc) return true;
            if (path.size() > sc.size() && path[sc.size()] == '/' &&
                path.compare(0, sc.size(), sc) == 0) return true;
        }
        return false;
    };

    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Scope — %1").arg(QString::fromStdString(snap.name)));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setMinimumWidth(320);
    dlg->setMinimumHeight(400);

    auto* vl = new QVBoxLayout(dlg);

    auto* tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setUniformRowHeights(true);
    tree->setRootIsDecorated(true);
    vl->addWidget(tree, 1);

    // Propagate check-state upward from a changed item.
    std::function<void(QTreeWidgetItem*)> updateAncestors;
    updateAncestors = [&](QTreeWidgetItem* item) {
        auto* p = item ? item->parent() : nullptr;
        if (!p) return;
        int nChecked = 0, nUnchecked = 0;
        for (int i = 0; i < p->childCount(); ++i) {
            const Qt::CheckState cs = p->child(i)->checkState(0);
            if (cs == Qt::Checked)   ++nChecked;
            if (cs == Qt::Unchecked) ++nUnchecked;
        }
        const int total = p->childCount();
        p->setCheckState(0, nChecked == total ? Qt::Checked
                          : nUnchecked == total ? Qt::Unchecked
                          : Qt::PartiallyChecked);
        updateAncestors(p);
    };

    // Propagate check-state downward to all descendants.
    std::function<void(QTreeWidgetItem*, Qt::CheckState)> setDescendants;
    setDescendants = [&](QTreeWidgetItem* item, Qt::CheckState state) {
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
            setDescendants(item->child(i), state);
        }
    };

    const int nCh = static_cast<int>(as.channels.size());
    int nPhys = m_model->engineOk ? m_model->engine.channels() : 2;
    if (!as.devices.empty()) { nPhys = 0; for (const auto& d : as.devices) nPhys += d.channelCount; }

    // Helper: create a checkable tree item (terminal or group)
    auto makeNode = [&](QTreeWidgetItem* parent, const QString& label,
                        const QString& path, bool terminal) {
        auto* node = new QTreeWidgetItem(parent, {label});
        node->setFlags(node->flags() | Qt::ItemIsUserCheckable);
        node->setData(0, Qt::UserRole, terminal ? path : QString());
        if (terminal) {
            node->setCheckState(0, inSnapScope(path.toStdString())
                                   ? Qt::Checked : Qt::Unchecked);
            node->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
        } else {
            node->setCheckState(0, Qt::Unchecked);
        }
        return node;
    };

    // Build tree: Mixer › Ch N › Fader / Mute / Polarity / Delay / Crosspoint › … / Plugin › Slot
    auto* mixerNode = makeNode(tree->invisibleRootItem(), tr("Mixer"), "", false);
    for (int ch = 0; ch < nCh; ++ch) {
        if (ch > 0 && as.channels[static_cast<size_t>(ch - 1)].linkedStereo) continue;
        const std::string base = "/mixer/" + std::to_string(ch);
        const QString qbase = QString::fromStdString(base);

        auto* chNode = makeNode(mixerNode, m_model->channelName(ch), "", false);

        makeNode(chNode, tr("Fader"),    qbase + "/fader",    true);
        makeNode(chNode, tr("Mute"),     qbase + "/mute",     true);
        makeNode(chNode, tr("Polarity"), qbase + "/polarity", true);
        makeNode(chNode, tr("Delay"),    qbase + "/delay",    true);

        // Crosspoints
        auto* xpNode = makeNode(chNode, tr("Crosspoint"), "", false);
        for (int out = 0; out < nPhys; ++out) {
            const QString chName = (out < nCh && !as.channels[static_cast<size_t>(out)].name.empty())
                ? QString::fromStdString(as.channels[static_cast<size_t>(out)].name)
                : QStringLiteral("Ch %1").arg(out + 1);
            makeNode(xpNode, QStringLiteral("→ %1").arg(chName),
                     qbase + "/crosspoint/" + QString::number(out), true);
        }

        // Plugin slots — per-parameter granularity
        const auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
        bool hasPlugins = false;
        for (const auto& psl : pSlots) if (!psl.pluginId.empty()) { hasPlugins = true; break; }
        if (hasPlugins) {
            auto* plugNode = makeNode(chNode, tr("Plugin"), "", false);
            for (int s = 0; s < static_cast<int>(pSlots.size()); ++s) {
                if (pSlots[static_cast<size_t>(s)].pluginId.empty()) continue;
                const auto& psl_ref = pSlots[static_cast<size_t>(s)];
                const QString dispName = !psl_ref.extName.empty()
                    ? QString::fromStdString(psl_ref.extName)
                    : QString::fromStdString(psl_ref.pluginId);
                const QString slotLabel = dispName + QStringLiteral(" (Slot %1)").arg(s + 1);
                const QString slotPath = qbase + "/plugin/" + QString::number(s);

                auto* slotNode = makeNode(plugNode, slotLabel, "", false);
                makeNode(slotNode, tr("Bypass"), slotPath + "/bypass", true);

                auto wrapper = m_model->channelPlugin(ch, s);
                if (wrapper && wrapper->getProcessor()) {
                    for (const auto& p : wrapper->getProcessor()->getParameters()) {
                        // Skip parameters named "Bypass": external plugins (e.g. Waves)
                        // expose bypass as a regular parameter AND via
                        // kAudioUnitProperty_BypassEffect.  We handle bypass via the
                        // explicit node above; a second "Bypass" entry would create a
                        // duplicate label in the scope tree.
                        QString pName = QString::fromStdString(p.name);
                        if (pName.compare("bypass", Qt::CaseInsensitive) == 0) continue;
                        makeNode(slotNode, pName,
                                 slotPath + "/" + QString::fromStdString(p.id), true);
                    }
                }
            }
        }
    }

    // Bottom-up pass: set parent check states based on their (already built) children.
    std::function<void(QTreeWidgetItem*)> initParentStates;
    initParentStates = [&](QTreeWidgetItem* item) {
        for (int i = 0; i < item->childCount(); ++i)
            initParentStates(item->child(i));
        if (item->childCount() > 0)
            updateAncestors(item->child(0)); // kick the parent of this subtree
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        initParentStates(tree->topLevelItem(i));

    tree->expandAll();

    // Propagate toggling: check/uncheck all descendants + update ancestors.
    connect(tree, &QTreeWidget::itemChanged, tree, [&setDescendants, &updateAncestors, tree]
            (QTreeWidgetItem* item, int col) {
        if (col != 0) return;
        tree->blockSignals(true);
        if (item->checkState(0) != Qt::PartiallyChecked)
            setDescendants(item, item->checkState(0));
        updateAncestors(item);
        tree->blockSignals(false);
    });

    auto* btnRow = new QHBoxLayout;
    auto* okBtn  = new QPushButton(tr("OK"));
    auto* canBtn = new QPushButton(tr("Cancel"));
    okBtn->setDefault(true); canBtn->setAutoDefault(false);
    btnRow->addStretch(); btnRow->addWidget(okBtn); btnRow->addWidget(canBtn);
    vl->addLayout(btnRow);

    connect(canBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    connect(okBtn,  &QPushButton::clicked, dlg, [this, dlg, &snap, &as, tree]() {
        // Sync AU state blobs before capturing plugin state
        m_model->syncPluginStatesToShowFile();

        std::vector<std::string> newScope;

        // Helper: lazily find-or-create the ChannelState for channel ch
        auto ensureCs = [&](int ch) -> ShowFile::SnapshotList::Snapshot::ChannelState& {
            for (auto& existing : snap.channels)
                if (existing.ch == ch) return existing;
            snap.channels.push_back({});
            snap.channels.back().ch = ch;
            return snap.channels.back();
        };

        // Walk all checked terminal items and capture their current values
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->checkState(0) == Qt::Checked) {
                const QString qpath = (*it)->data(0, Qt::UserRole).toString();
                if (!qpath.isEmpty()) {
                    const std::string path = qpath.toStdString();
                    newScope.push_back(path);

                    // Parse channel index from "/mixer/{ch}/..."
                    int ch = -1;
                    if (path.size() > 7 && path.compare(0, 7, "/mixer/") == 0) {
                        const size_t sl2 = path.find('/', 7);
                        try { ch = std::stoi(path.substr(7, sl2 == std::string::npos
                                                          ? std::string::npos : sl2 - 7)); }
                        catch (...) {}
                    }
                    if (ch < 0 || ch >= static_cast<int>(as.channels.size())) { ++it; continue; }
                    const auto& chan = as.channels[static_cast<size_t>(ch)];
                    auto& cs = ensureCs(ch);
                    const std::string base = "/mixer/" + std::to_string(ch);

                    if (path == base + "/fader" && !cs.faderDb)
                        cs.faderDb = chan.masterGainDb;
                    else if (path == base + "/mute" && !cs.mute)
                        cs.mute = chan.mute;
                    else if (path == base + "/polarity" && !cs.polarity)
                        cs.polarity = chan.phaseInvert;
                    else if (path == base + "/delay" && !cs.delayMs) {
                        cs.delayMs = chan.delayMs;
                        cs.delayInSamples = chan.delayInSamples;
                        cs.delaySamples   = chan.delaySamples;
                    } else if (path.find("/crosspoint/") != std::string::npos) {
                        int out = -1;
                        try { out = std::stoi(path.substr(path.rfind('/') + 1)); } catch (...) {}
                        if (out >= 0) {
                            bool found = false;
                            for (const auto& xs : cs.xpSends) if (xs.out == out) { found = true; break; }
                            if (!found) {
                                float db = (ch == out) ? 0.0f : -144.0f;
                                for (const auto& xe : as.xpEntries)
                                    if (xe.ch == ch && xe.out == out) { db = xe.db; break; }
                                cs.xpSends.push_back({out, db});
                            }
                        }
                    } else if (path.find("/plugin/") != std::string::npos) {
                        // Parse slot index from "/mixer/{ch}/plugin/{slot}/..."
                        const std::string plugMid = "/plugin/";
                        const size_t plugPos = path.find(plugMid, base.size());
                        if (plugPos != std::string::npos) {
                            const size_t afterSlot = plugPos + plugMid.size();
                            const size_t paramSlash = path.find('/', afterSlot);
                            int slotIdx = -1;
                            try {
                                slotIdx = std::stoi(path.substr(afterSlot,
                                    paramSlash == std::string::npos
                                        ? std::string::npos : paramSlash - afterSlot));
                            } catch (...) {}
                            if (slotIdx >= 0 && slotIdx < static_cast<int>(chan.plugins.size())) {
                                const auto& psl = chan.plugins[static_cast<size_t>(slotIdx)];
                                // Find or create PluginParamState baseline in the snapshot
                                ShowFile::SnapshotList::Snapshot::ChannelState::PluginParamState* psPtr = nullptr;
                                for (auto& existing : cs.pluginStates)
                                    if (existing.slot == slotIdx) { psPtr = &existing; break; }
                                if (!psPtr) {
                                    cs.pluginStates.push_back({});
                                    auto& np = cs.pluginStates.back();
                                    np.slot             = slotIdx;
                                    np.bypassed         = psl.bypassed;
                                    np.parameters       = psl.parameters;
                                    np.extStateBlob     = psl.extStateBlob;
                                    np.extParamSnapshot = psl.extParamSnapshot;
                                    psPtr = &np;
                                }
                                (void)psPtr; // scope provides recall granularity
                            }
                        }
                    }
                }
            }
            ++it;
        }

        // Collapse: when all per-param paths of a slot are selected, replace with
        // the slot-level path so AU plugins can use blob-based recall (more accurate).
        for (int ch2 = 0; ch2 < static_cast<int>(as.channels.size()); ++ch2) {
            if (ch2 > 0 && as.channels[static_cast<size_t>(ch2 - 1)].linkedStereo) continue;
            const std::string base2 = "/mixer/" + std::to_string(ch2);
            const auto& pSlots2 = as.channels[static_cast<size_t>(ch2)].plugins;
            for (int s2 = 0; s2 < static_cast<int>(pSlots2.size()); ++s2) {
                if (pSlots2[static_cast<size_t>(s2)].pluginId.empty()) continue;
                const std::string slotPath2   = base2 + "/plugin/" + std::to_string(s2);
                const std::string bypassPath2 = slotPath2 + "/bypass";
                if (std::find(newScope.begin(), newScope.end(), bypassPath2) == newScope.end())
                    continue;
                auto w2 = m_model->channelPlugin(ch2, s2);
                if (!w2 || !w2->getProcessor()) continue;
                bool allParams = true;
                for (const auto& p2 : w2->getProcessor()->getParameters()) {
                    if (std::find(newScope.begin(), newScope.end(), slotPath2 + "/" + p2.id)
                            == newScope.end()) { allParams = false; break; }
                }
                if (!allParams) continue;
                newScope.erase(std::remove(newScope.begin(), newScope.end(), bypassPath2), newScope.end());
                for (const auto& p2 : w2->getProcessor()->getParameters()) {
                    const std::string pp = slotPath2 + "/" + p2.id;
                    newScope.erase(std::remove(newScope.begin(), newScope.end(), pp), newScope.end());
                }
                newScope.push_back(slotPath2);
            }
        }

        snap.scope = std::move(newScope);
        m_model->markDirty();
        dlg->accept();
    });

    dlg->exec();
    raise();
    activateWindow();
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

    // ── Plugin slot drag-and-drop ──────────────────────────────────────────────
    if (auto* btn = qobject_cast<QPushButton*>(obj); btn && btn->property("pluginCh").isValid()) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* me = static_cast<const QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragSrcCh    = btn->property("pluginCh").toInt();
                m_dragSrcSlot  = btn->property("pluginSlot").toInt();
                m_dragStartPos = me->globalPosition().toPoint();
            }
            // Do not consume — let the click signal fire normally.
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            m_dragSrcCh = m_dragSrcSlot = -1;
        }

        if (event->type() == QEvent::MouseMove && m_dragSrcCh >= 0) {
            const auto* me = static_cast<const QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) &&
                (me->globalPosition().toPoint() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
                // Only start a drag if the source slot actually has a plugin.
                const auto& as = m_model->sf.audioSetup;
                const bool hasSrc =
                    m_dragSrcCh < static_cast<int>(as.channels.size()) &&
                    m_dragSrcSlot < static_cast<int>(as.channels[static_cast<size_t>(m_dragSrcCh)].plugins.size()) &&
                    !as.channels[static_cast<size_t>(m_dragSrcCh)].plugins[static_cast<size_t>(m_dragSrcSlot)].pluginId.empty();
                if (hasSrc) {
                    auto* drag = new QDrag(btn);
                    auto* mime = new QMimeData;
                    mime->setData("application/x-mcp-plugin-slot",
                        (QString::number(m_dragSrcCh) + ":" + QString::number(m_dragSrcSlot)).toLatin1());
                    drag->setMimeData(mime);
                    drag->exec(Qt::MoveAction | Qt::CopyAction);
                    m_dragSrcCh = m_dragSrcSlot = -1;
                    return true;
                }
            }
        }

        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-mcp-plugin-slot")) {
                if (m_dragHoverBtn && m_dragHoverBtn != btn) {
                    m_dragHoverBtn->setStyleSheet(m_dragHoverOrigStyle);
                    m_dragHoverBtn = nullptr;
                }
                m_dragHoverBtn       = btn;
                m_dragHoverOrigStyle = btn->styleSheet();

                // Validate: mono-only plugins can't go onto linked-stereo channels.
                m_dragHoverValid = true;
                {
                    const QByteArray mdata = de->mimeData()->data("application/x-mcp-plugin-slot");
                    const QStringList pts  = QString::fromLatin1(mdata).split(':');
                    if (pts.size() == 2) {
                        const int srcCh   = pts[0].toInt();
                        const int srcSlot = pts[1].toInt();
                        const int dstCh   = btn->property("pluginCh").toInt();
                        const auto& chans = m_model->sf.audioSetup.channels;
                        const bool dstIsLinked =
                            (dstCh >= 0 && dstCh < static_cast<int>(chans.size()) && chans[static_cast<size_t>(dstCh)].linkedStereo) ||
                            (dstCh > 0  && dstCh <= static_cast<int>(chans.size()) && chans[static_cast<size_t>(dstCh - 1)].linkedStereo);
                        if (dstIsLinked &&
                            srcCh >= 0 && srcCh < static_cast<int>(chans.size())) {
                            const auto& srcPlugs = chans[static_cast<size_t>(srcCh)].plugins;
                            if (srcSlot >= 0 && srcSlot < static_cast<int>(srcPlugs.size()) &&
                                srcPlugs[static_cast<size_t>(srcSlot)].isExternal() &&
                                srcPlugs[static_cast<size_t>(srcSlot)].extNumChannels == 1)
                                m_dragHoverValid = false;
                        }
                    }
                }

                if (m_dragHoverValid) {
                    btn->setStyleSheet(
                        "QPushButton { background:#1a3a2a; color:#ccc; font-size:10px;"
                        " text-align:left; padding-left:4px; border:2px solid #4c9; }");
                    de->acceptProposedAction();
                } else {
                    btn->setStyleSheet(
                        "QPushButton { background:#3a1a1a; color:#ccc; font-size:10px;"
                        " text-align:left; padding-left:4px; border:2px solid #c44; }");
                    de->ignore();
                }
                return true;
            }
        }

        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-mcp-plugin-slot")) {
                if (m_dragHoverValid) de->acceptProposedAction();
                else                  de->ignore();
                return true;
            }
        }

        if (event->type() == QEvent::DragLeave) {
            if (btn == m_dragHoverBtn) {
                btn->setStyleSheet(m_dragHoverOrigStyle);
                m_dragHoverBtn = nullptr;
            }
            return false;
        }

        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasFormat("application/x-mcp-plugin-slot")) {
                if (m_dragHoverBtn == btn) {
                    btn->setStyleSheet(m_dragHoverOrigStyle);
                    m_dragHoverBtn = nullptr;
                }
                if (!m_dragHoverValid) {
                    de->ignore();
                    return true;
                }
                const QByteArray data = de->mimeData()->data("application/x-mcp-plugin-slot");
                const QStringList parts = QString::fromLatin1(data).split(':');
                if (parts.size() == 2) {
                    const int srcCh   = parts[0].toInt();
                    const int srcSlot = parts[1].toInt();
                    const int dstCh   = btn->property("pluginCh").toInt();
                    const int dstSlot = btn->property("pluginSlot").toInt();
                    if (srcCh != dstCh || srcSlot != dstSlot) {
                        const bool isCopy = (de->dropAction() == Qt::CopyAction);
                        executeDragDrop(srcCh, srcSlot, dstCh, dstSlot, isCopy);
                    }
                }
                de->acceptProposedAction();
                return true;
            }
        }
    }

    return QDialog::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// Plugin slot UI
// ---------------------------------------------------------------------------

using PluginSlot = ShowFile::AudioSetup::Channel::PluginSlot;

static QString pluginDisplayName(const PluginSlot& sl) {
    if (!sl.extName.empty())                     return QString::fromStdString(sl.extName);
    if (sl.pluginId == "internal.trim.mono"   )  return "Trim (Mono)";
    if (sl.pluginId == "internal.trim.stereo" )  return "Trim (Stereo)";
    if (sl.pluginId == "internal.delay.mono"  )  return "Delay (Mono)";
    if (sl.pluginId == "internal.delay.stereo")  return "Delay (Stereo)";
    return QString::fromStdString(sl.pluginId);
}

static QString pluginSlotLabel(const PluginSlot& sl,
                                mcp::plugin::PluginRuntimeStatus status,
                                int latencySamples, int sampleRate)
{
    if (sl.pluginId.empty()) return "+ Add Plugin";
    const QString name = pluginDisplayName(sl);

    using S = mcp::plugin::PluginRuntimeStatus;
    if (sl.disabled || status == S::Disabled)           return "⊝ " + name; // ⊝
    if (sl.bypassed)                                    return "⊘ " + name; // ⊘
    if (status == S::Missing || status == S::Failed)    return "✕ " + name; // ✕
    if (status == S::StateRestoreFailed)                return "⚠ " + name; // ⚠

    // Latency warning (> 10 ms at given sample rate)
    if (latencySamples > 0 && sampleRate > 0) {
        const float ms = latencySamples * 1000.0f / static_cast<float>(sampleRate);
        if (ms > 10.0f)
            return name + QString(" (+%1ms)").arg(qRound(ms));
    }
    return name;
}

static QString pluginSlotStyle(const PluginSlot& sl,
                                mcp::plugin::PluginRuntimeStatus status)
{
    using S = mcp::plugin::PluginRuntimeStatus;
    const auto css = [](const char* bg, const char* fg, const char* border) {
        return QString("QPushButton { background:%1; color:%2; font-size:10px; "
                       "text-align:left; padding-left:4px; border:1px solid %3; }")
               .arg(bg).arg(fg).arg(border);
    };
    if (sl.pluginId.empty())
        return "QPushButton { background:#2a2a2a; color:#555; font-size:10px; "
               "text-align:left; padding-left:4px; border:1px dashed #444; }";
    if (sl.disabled || status == S::Disabled)           return css("#2a2a2a","#666","#555");
    if (status == S::Missing || status == S::Failed)    return css("#3a2020","#c88","#7a3a3a");
    if (status == S::StateRestoreFailed)                return css("#3a3020","#cc8","#7a6a3a");
    if (sl.bypassed)                                    return css("#2a2a10","#aa9","#6a5a2a");
    return css("#2a3a2a","#9c9","#4a6a4a"); // ok
}

QString MixConsoleDialog::pluginHdrText(int ch, bool open) const {
    int totalLat = 0;
    const auto& as = m_model->sf.audioSetup;
    if (ch >= 0 && ch < static_cast<int>(as.channels.size())) {
        const auto& plugSlots = as.channels[static_cast<size_t>(ch)].plugins;
        for (int sl = 0; sl < static_cast<int>(plugSlots.size()); ++sl) {
            auto wp = m_model->channelPlugin(ch, sl);
            if (wp) totalLat += wp->getLatencySamples();
        }
    }
    const QString arrow = open ? "▾" : "▸";
    if (totalLat <= 0)
        return arrow + " Plugins";
    const int sr = m_model->engine.sampleRate() > 0 ? m_model->engine.sampleRate() : 48000;
    const double ms = totalLat * 1000.0 / sr;
    return QString("%1 Plugins  ·  %2 smp (%3 ms)")
               .arg(arrow).arg(totalLat).arg(ms, 0, 'f', 1);
}

void MixConsoleDialog::buildPluginSection(Strip& s) {
    const int ch  = s.ch;
    auto& as      = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());
    if (ch < 0 || ch >= nCh) return;

    s.pluginHdr = new QPushButton(pluginHdrText(ch, false));
    s.pluginHdr->setAutoDefault(false);
    s.pluginHdr->setDefault(false);
    s.pluginHdr->setFixedHeight(20);
    s.pluginHdr->setCheckable(true);
    s.pluginHdr->setStyleSheet(
        "QPushButton { background:#222; color:#888; font-size:10px; border:none; "
        "text-align:left; padding-left:4px; }"
        "QPushButton:checked { color:#ccc; }");
    connect(s.pluginHdr, &QPushButton::toggled, this, [this, ch](bool open) {
        for (auto& ss : m_strips) {
            if (ss.ch == ch && ss.pluginBody) {
                ss.pluginBody->setVisible(open);
                if (ss.pluginHdr) ss.pluginHdr->setText(pluginHdrText(ch, open));
            }
        }
    });

    s.pluginBody = new QWidget;
    s.pluginBody->setVisible(false);
    auto* bl = new QVBoxLayout(s.pluginBody);
    bl->setContentsMargins(0, 2, 0, 2);
    bl->setSpacing(2);

    auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
    const int displayCount = std::min(
        static_cast<int>(pSlots.size()) + 1,
        mcp::plugin::ChannelPluginChain::kMaxSlots);

    s.pluginSlotBtns.clear();
    for (int slot = 0; slot < displayCount; ++slot)
        addPluginSlotButton(bl, s, ch, slot);
}

void MixConsoleDialog::rebuildPluginSection(int ch) {
    Strip* sp = nullptr;
    for (auto& s : m_strips) if (s.ch == ch) { sp = &s; break; }
    if (!sp) return;

    const bool wasOpen = sp->pluginHdr && sp->pluginHdr->isChecked();

    // Clear old body contents without reparenting the container.
    if (sp->pluginBody) {
        auto* bl = sp->pluginBody->layout();
        if (bl) {
            while (bl->count()) {
                auto* item = bl->takeAt(0);
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
        }
    }
    sp->pluginSlotBtns.clear();

    if (sp->pluginHdr) sp->pluginHdr->setText(pluginHdrText(ch, wasOpen));
    if (sp->pluginBody) sp->pluginBody->setVisible(wasOpen);

    auto& as  = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());
    if (ch < 0 || ch >= nCh) return;
    auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;

    auto* bl = sp->pluginBody ? qobject_cast<QVBoxLayout*>(sp->pluginBody->layout()) : nullptr;
    if (!bl) return;

    const int displayCount = std::min(
        static_cast<int>(pSlots.size()) + 1,
        mcp::plugin::ChannelPluginChain::kMaxSlots);

    for (int slot = 0; slot < displayCount; ++slot)
        addPluginSlotButton(bl, *sp, ch, slot);
}

void MixConsoleDialog::addPluginSlotButton(QVBoxLayout* bl, Strip& s, int ch, int slot)
{
    const auto& pSlots = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
    const bool hasPlugin = (slot < static_cast<int>(pSlots.size()))
                           && !pSlots[static_cast<size_t>(slot)].pluginId.empty();

    const PluginSlot emptySlot{};
    const PluginSlot& sl = hasPlugin ? pSlots[static_cast<size_t>(slot)] : emptySlot;
    const auto status = hasPlugin ? m_model->channelPluginStatus(ch, slot)
                                  : mcp::plugin::PluginRuntimeStatus::Ok;
    int latencySamples = 0;
    if (hasPlugin) {
        auto w = m_model->channelPlugin(ch, slot);
        if (w && w->getProcessor())
            latencySamples = w->getProcessor()->getLatencySamples();
    }
    const int sr = m_model->engine.sampleRate() > 0 ? m_model->engine.sampleRate() : 48000;

    auto* btn = new QPushButton(pluginSlotLabel(sl, status, latencySamples, sr));
    btn->setFixedHeight(22);
    btn->setAutoDefault(false);
    btn->setDefault(false);
    btn->setStyleSheet(pluginSlotStyle(sl, status));
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    // Drag-and-drop: tag each button with its (ch, slot) address.
    btn->setProperty("pluginCh",   ch);
    btn->setProperty("pluginSlot", slot);
    btn->setAcceptDrops(true);
    btn->installEventFilter(this);

    connect(btn, &QPushButton::clicked, this, [this, ch, slot]() {
        const auto& ps = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
        const bool hp = (slot < static_cast<int>(ps.size())) && !ps[static_cast<size_t>(slot)].pluginId.empty();
        if (hp) {
            const bool pin = QApplication::keyboardModifiers() & Qt::ShiftModifier;
            openPluginEditor(ch, slot, pin);
        } else {
            openPluginPicker(ch, slot);
        }
    });
    connect(btn, &QPushButton::customContextMenuRequested, this,
            [this, ch, slot](const QPoint& pos) {
        const auto& ps = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
        const bool hp = (slot < static_cast<int>(ps.size())) && !ps[static_cast<size_t>(slot)].pluginId.empty();
        if (!hp) return;
        const auto& slRef = ps[static_cast<size_t>(slot)];
        QMenu menu;

        menu.addAction(slRef.bypassed ? "Unbypass" : "Bypass", this, [this, ch, slot]() {
            auto& sl2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins[static_cast<size_t>(slot)];
            sl2.bypassed = !sl2.bypassed;
            auto w = m_model->channelPlugin(ch, slot);
            if (w) {
                w->setBypassed(sl2.bypassed);
#ifdef __APPLE__
                if (auto* au = dynamic_cast<mcp::plugin::AUPluginAdapter*>(w->getProcessor()))
                    au->setNativeBypass(sl2.bypassed);
#endif
            }
            m_model->markDirty();
            m_model->snapshots.markDirty("/mixer/" + std::to_string(ch) + "/plugin/" + std::to_string(slot));
            rebuildPluginSection(ch);
        });
        menu.addAction("Info…", this, [this, ch, slot]() { showPluginInfo(ch, slot); });
        menu.addSeparator();
        menu.addAction("Replace…", this, [this, ch, slot]() { openPluginPicker(ch, slot); });
        menu.addAction("Remove",   this, [this, ch, slot]() { removePlugin(ch, slot); });
        menu.addSeparator();
        const bool isDisabled = slRef.disabled;
        menu.addAction(isDisabled ? "Enable Slot" : "Disable Slot", this,
                       [this, ch, slot, isDisabled]() {
            // Close any open editor — plugin chain is about to be rebuilt
            auto ekey = std::make_pair(ch, slot);
            auto eit = m_pluginEditors.find(ekey);
            if (eit != m_pluginEditors.end() && eit->second)
                eit->second->close();

            auto& sl2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins[static_cast<size_t>(slot)];
            sl2.disabled = !isDisabled;
            if (isDisabled) sl2.loadFailCount = 0; // reset on manual enable
            m_model->markDirty();
            m_model->buildChannelPluginChains();
            m_model->applyMixing();
            rebuildPluginSection(ch);
        });
        menu.exec(static_cast<QPushButton*>(sender())->mapToGlobal(pos));
    });

    bl->addWidget(btn);
    s.pluginSlotBtns.push_back(btn);
}

void MixConsoleDialog::showPluginInfo(int ch, int slot)
{
    const auto& as = m_model->sf.audioSetup;
    if (ch < 0 || ch >= static_cast<int>(as.channels.size())) return;
    const auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
    if (slot >= static_cast<int>(pSlots.size())) return;
    const auto& sl = pSlots[static_cast<size_t>(slot)];

    const auto status = m_model->channelPluginStatus(ch, slot);
    auto w = m_model->channelPlugin(ch, slot);
    int latSamples = 0, tailSamples = 0, paramCount = 0;
    if (w && w->getProcessor()) {
        latSamples  = w->getProcessor()->getLatencySamples();
        tailSamples = w->getProcessor()->getTailSamples();
        paramCount  = static_cast<int>(w->getProcessor()->getParameters().size());
    }
    const int sr = m_model->engine.sampleRate() > 0 ? m_model->engine.sampleRate() : 48000;
    const float latMs  = sr > 0 ? latSamples  * 1000.0f / static_cast<float>(sr) : 0.0f;
    const float tailMs = sr > 0 ? tailSamples * 1000.0f / static_cast<float>(sr) : 0.0f;

    const QString name = pluginDisplayName(sl);
    const QString vendor  = sl.extVendor.empty()  ? "—" : QString::fromStdString(sl.extVendor);
    const QString version = sl.extVersion.empty() ? "—" : QString::fromStdString(sl.extVersion);
    const QString backend = sl.extBackend.empty() ? "internal"
                                                  : QString::fromStdString(sl.extBackend);

    using S = mcp::plugin::PluginRuntimeStatus;
    const char* statusStr = "OK";
    switch (status) {
    case S::Ok:                 statusStr = "OK"; break;
    case S::Missing:            statusStr = "Missing"; break;
    case S::Failed:             statusStr = "Failed"; break;
    case S::UnsupportedLayout:  statusStr = "Unsupported layout"; break;
    case S::StateRestoreFailed: statusStr = "State restore failed"; break;
    case S::Disabled:           statusStr = "Disabled (safe-load)"; break;
    }

    const QString text = QString(
        "Name:        %1\n"
        "Vendor:      %2\n"
        "Version:     %3\n"
        "Backend:     %4\n"
        "Status:      %5%6%7\n"
        "Fail count:  %8\n"
        "\n"
        "Latency:     %9 samples  (%10 ms)%11\n"
        "Tail:        %12 samples  (%13 ms)\n"
        "Parameters:  %14"
    ).arg(name).arg(vendor).arg(version).arg(backend)
     .arg(statusStr)
     .arg(sl.bypassed ? "  [bypassed]" : "")
     .arg(sl.disabled ? "  [disabled]" : "")
     .arg(sl.loadFailCount)
     .arg(latSamples).arg(latMs, 0, 'f', 1).arg(latMs > 10.0f ? "  ⚠ >10 ms" : "")
     .arg(tailSamples).arg(tailMs, 0, 'f', 1)
     .arg(paramCount);

    QMessageBox::information(this, name, text);
}

void MixConsoleDialog::openPluginPicker(int ch, int slot) {
    auto& as  = m_model->sf.audioSetup;
    const int nCh = static_cast<int>(as.channels.size());
    if (ch < 0 || ch >= nCh) return;

    const bool isStereo = as.channels[static_cast<size_t>(ch)].linkedStereo
                          && (ch + 1 < nCh);
    const int numCh = isStereo ? 2 : 1;

    const auto descs = m_model->pluginFactory.scan();
    QMenu menu;

    // Internal plugins
    for (const auto& d : descs) {
        bool ok = false;
        for (const auto& lay : d.supportedLayouts) {
            const int outCh = lay.outputs.empty() ? 0
                : static_cast<int>(lay.outputs[0].numChannels);
            if (isStereo && outCh == 2) { ok = true; break; }
            if (!isStereo) { ok = true; break; }
        }
        if (!ok) continue;
        const std::string id = d.id;
        menu.addAction(QString::fromStdString(d.name), this, [this, ch, slot, id]() {
            const bool editorWasOpen = closePluginEditor(ch, slot);
            auto& pSlots2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
            while (static_cast<int>(pSlots2.size()) <= slot)
                pSlots2.emplace_back();
            pSlots2[static_cast<size_t>(slot)].pluginId   = id;
            pSlots2[static_cast<size_t>(slot)].parameters = {};
            m_model->markDirty();
            m_model->buildChannelPluginChains();
            m_model->applyMixing();
            rebuildPluginSection(ch);
            if (editorWasOpen) openPluginEditor(ch, slot);
        });
    }

#ifdef __APPLE__
    ensureAUCache();
    if (!m_auEntries.empty()) {
        if (!descs.empty())
            menu.addSeparator();

        // Build vendor → [entry indices] map (preserves insertion order for stable menus).
        std::vector<std::string> vendorOrder;
        std::map<std::string, std::vector<int>> vendorMap;
        for (int i = 0; i < static_cast<int>(m_auEntries.size()); ++i) {
            const auto& e = m_auEntries[static_cast<size_t>(i)];
            if (vendorMap.find(e.manufacturerName) == vendorMap.end())
                vendorOrder.push_back(e.manufacturerName);
            vendorMap[e.manufacturerName].push_back(i);
        }

        // One submenu per vendor under an "AU Plugins" top-level menu.
        auto* auMenu = menu.addMenu("AU Plugins");
        enableMenuScroll(auMenu);
        for (const auto& vendor : vendorOrder) {
            auto* vMenu = auMenu->addMenu(QString::fromStdString(vendor));
            enableMenuScroll(vMenu);
            for (int idx : vendorMap[vendor]) {
                const auto& e = m_auEntries[static_cast<size_t>(idx)];
                const uint32_t type = e.type, sub = e.subtype, mfr = e.manufacturer;
                const int reqCh = numCh;
                vMenu->addAction(QString::fromStdString(e.name), this,
                    [this, ch, slot, type, sub, mfr, reqCh,
                     name = e.name, vendor2 = e.manufacturerName,
                     version = e.version]()
                {
                    const bool editorWasOpen = closePluginEditor(ch, slot);

                    // Build the au:type/subtype/manufacturer plugin ID.
                    auto fcc = [](uint32_t v, char* out) {
                        out[0] = static_cast<char>((v >> 24) & 0xFF);
                        out[1] = static_cast<char>((v >> 16) & 0xFF);
                        out[2] = static_cast<char>((v >>  8) & 0xFF);
                        out[3] = static_cast<char>( v        & 0xFF);
                        out[4] = '\0';
                    };
                    char t[5], s[5], m[5];
                    fcc(type, t); fcc(sub, s); fcc(mfr, m);
                    const std::string pluginId = std::string("au:") + t + "/" + s + "/" + m;

                    auto& pSlots2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
                    while (static_cast<int>(pSlots2.size()) <= slot)
                        pSlots2.emplace_back();
                    auto& sl2 = pSlots2[static_cast<size_t>(slot)];
                    sl2.pluginId       = pluginId;
                    sl2.extBackend     = "au";
                    sl2.extName        = name;
                    sl2.extVendor      = vendor2;
                    sl2.extVersion     = version;
                    sl2.extNumChannels = reqCh;
                    sl2.extStateBlob.clear();
                    sl2.extParamSnapshot.clear();
                    sl2.parameters.clear();
                    m_model->markDirty();
                    m_model->buildChannelPluginChains();
                    m_model->applyMixing();
                    rebuildPluginSection(ch);
                    if (editorWasOpen) openPluginEditor(ch, slot);
                });
            }
        }

        auMenu->addSeparator();
        auMenu->addAction("Refresh Plugin List", this, [this]() {
            m_auCacheValid = false;
        });
    }
#endif

#ifdef MCP_HAVE_VST3
    ensureVST3Cache();
    if (!m_vst3Entries.empty()) {
        menu.addSeparator();

        // Build vendor → [entry indices] map
        std::vector<std::string> vst3VendorOrder;
        std::map<std::string, std::vector<int>> vst3VendorMap;
        for (int i = 0; i < static_cast<int>(m_vst3Entries.size()); ++i) {
            const auto& e = m_vst3Entries[static_cast<size_t>(i)];
            if (vst3VendorMap.find(e.vendor) == vst3VendorMap.end())
                vst3VendorOrder.push_back(e.vendor);
            vst3VendorMap[e.vendor].push_back(i);
        }

        auto* vst3Menu = menu.addMenu("VST3 Plugins");
        enableMenuScroll(vst3Menu);
        for (const auto& vendor : vst3VendorOrder) {
            auto* vMenu = vst3Menu->addMenu(
                vendor.empty() ? "(Unknown)" : QString::fromStdString(vendor));
            enableMenuScroll(vMenu);
            for (int idx : vst3VendorMap[vendor]) {
                const auto& e = m_vst3Entries[static_cast<size_t>(idx)];
                const int reqCh = numCh;
                vMenu->addAction(QString::fromStdString(e.name), this,
                    [this, ch, slot, reqCh,
                     pluginId = e.pluginId, name = e.name, vendor2 = e.vendor,
                     version = e.version, path = e.path]()
                {
                    const bool editorWasOpen = closePluginEditor(ch, slot);

                    auto& pSlots2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
                    while (static_cast<int>(pSlots2.size()) <= slot)
                        pSlots2.emplace_back();
                    auto& sl2 = pSlots2[static_cast<size_t>(slot)];
                    sl2.pluginId        = pluginId;
                    sl2.extBackend      = "vst3";
                    sl2.extPath         = path;
                    sl2.extName         = name;
                    sl2.extVendor       = vendor2;
                    sl2.extVersion      = version;
                    sl2.extNumChannels  = reqCh;
                    sl2.extStateBlob.clear();
                    sl2.extParamSnapshot.clear();
                    sl2.parameters.clear();
                    m_model->markDirty();
                    m_model->buildChannelPluginChains();
                    m_model->applyMixing();
                    rebuildPluginSection(ch);
                    if (editorWasOpen) openPluginEditor(ch, slot);
                });
            }
        }

        vst3Menu->addSeparator();
        vst3Menu->addAction("Reload Cached List", this, [this]() {
            m_vst3CacheValid = false;
        });
    }

#endif

    menu.addSeparator();
    menu.addAction("Plugin Manager…", this, [this, ch, slot]() {
        const bool editorWasOpen = closePluginEditor(ch, slot);
        auto* dlg = new PluginManagerDialog(m_model, ch, slot, this);
        const bool accepted = dlg->exec() == QDialog::Accepted;
#ifdef __APPLE__
        m_auCacheValid   = false;
#endif
#ifdef MCP_HAVE_VST3
        m_vst3CacheValid = false;  // always reload in case user ran a scan
#endif
        if (accepted) {
            rebuildPluginSection(ch);
            if (editorWasOpen) openPluginEditor(ch, slot);
        }
        raise();
        activateWindow();
    });

    // Find the button to anchor the menu.
    Strip* sp = nullptr;
    for (auto& s : m_strips) if (s.ch == ch) { sp = &s; break; }
    QPoint anchor;
    if (sp && slot < static_cast<int>(sp->pluginSlotBtns.size()) && sp->pluginSlotBtns[static_cast<size_t>(slot)])
        anchor = sp->pluginSlotBtns[static_cast<size_t>(slot)]->mapToGlobal(QPoint(0,22));
    menu.exec(anchor);
}

#ifdef __APPLE__
void MixConsoleDialog::ensureAUCache() {
    if (m_auCacheValid) return;
    m_auEntries = mcp::plugin::AUComponentEnumerator::enumerate();
    m_auCacheValid = true;
}
#endif

#ifdef MCP_HAVE_VST3
void MixConsoleDialog::ensureVST3Cache() {
    if (m_vst3CacheValid) return;
    // Load results persisted by PluginManagerDialog after a manual scan.
    // Never trigger a scan here — scanning is always an explicit user action.
    QSettings s("click-in", "MusicCuePlayer");
    const QVariantList list = s.value("vst3/cachedPlugins").toList();
    m_vst3Entries.clear();
    for (const auto& v : list) {
        const QVariantMap m = v.toMap();
        mcp::plugin::VST3Entry e;
        e.name        = m.value("name").toString().toStdString();
        e.vendor      = m.value("vendor").toString().toStdString();
        e.version     = m.value("version").toString().toStdString();
        e.path        = m.value("path").toString().toStdString();
        e.pluginId    = m.value("pluginId").toString().toStdString();
        e.classIndex  = m.value("classIndex").toInt();
        m_vst3Entries.push_back(std::move(e));
    }
    m_vst3CacheValid = true;
}
#endif

// ---------------------------------------------------------------------------
// Diff-based autoscope helpers

void MixConsoleDialog::resetPluginCache(int ch, int slot) {
    auto wrapper = m_model->channelPlugin(ch, slot);
    if (!wrapper || !wrapper->getProcessor()) return;
    auto* proc = wrapper->getProcessor();
    ParamCache cache;
    for (const auto& info : proc->getParameters())
        cache.emplace_back(info.id, proc->getParameterValue(info.id));
    m_pluginParamCaches[{ch, slot}] = std::move(cache);
}

void MixConsoleDialog::resetAllPluginCaches() {
    for (const auto& [key, dlgPtr] : m_pluginEditors)
        if (dlgPtr) resetPluginCache(key.first, key.second);
}

void MixConsoleDialog::diffAndMarkPluginDirty(int ch, int slot) {
    auto cacheIt = m_pluginParamCaches.find({ch, slot});
    if (cacheIt == m_pluginParamCaches.end()) return;

    auto wrapper = m_model->channelPlugin(ch, slot);
    if (!wrapper || !wrapper->getProcessor()) return;
    auto* proc = wrapper->getProcessor();

    auto& as = m_model->sf.audioSetup;
    if (ch >= static_cast<int>(as.channels.size())) return;
    auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
    if (slot >= static_cast<int>(pSlots.size())) return;
    auto& psl = pSlots[static_cast<size_t>(slot)];

    const std::string slotBase =
        "/mixer/" + std::to_string(ch) + "/plugin/" + std::to_string(slot);

    for (auto& [id, cached] : cacheIt->second) {
        const float cur = proc->getParameterValue(id);
        if (cur != cached) {
            cached = cur;
            psl.extParamSnapshot[id] = cur;
            m_model->snapshots.markDirty(slotBase + "/" + id);
            m_model->markDirty();
        }
    }
}

bool MixConsoleDialog::closePluginEditor(int ch, int slot) {
    auto key = std::make_pair(ch, slot);
    auto it = m_pluginEditors.find(key);
    if (it != m_pluginEditors.end() && it->second) {
        it->second->close();
        return true;
    }
    return false;
}

void MixConsoleDialog::removePlugin(int ch, int slot) {
    auto& as = m_model->sf.audioSetup;
    if (ch < 0 || ch >= static_cast<int>(as.channels.size())) return;
    auto& pSlots = as.channels[static_cast<size_t>(ch)].plugins;
    if (slot >= static_cast<int>(pSlots.size())) return;
    closePluginEditor(ch, slot);
    pSlots.erase(pSlots.begin() + slot);
    while (!pSlots.empty() && pSlots.back().pluginId.empty())
        pSlots.pop_back();
    m_model->markDirty();
    m_model->buildChannelPluginChains();
    m_model->applyMixing();
    rebuildPluginSection(ch);
}

void MixConsoleDialog::executeDragDrop(int srcCh, int srcSlot, int dstCh, int dstSlot, bool copy) {
    auto& as = m_model->sf.audioSetup;
    if (srcCh < 0 || srcCh >= static_cast<int>(as.channels.size())) return;
    if (dstCh < 0 || dstCh >= static_cast<int>(as.channels.size())) return;

    auto& srcPlugins = as.channels[static_cast<size_t>(srcCh)].plugins;
    if (srcSlot < 0 || srcSlot >= static_cast<int>(srcPlugins.size())) return;
    if (srcPlugins[static_cast<size_t>(srcSlot)].pluginId.empty()) return;

    // Flush live AU state (stateBlob, extParamSnapshot) to sf before the operation
    // so the moved/copied plugin carries its current parameters to the new slot.
    m_model->syncPluginStatesToShowFile();

    auto& dstPlugins = as.channels[static_cast<size_t>(dstCh)].plugins;
    while (static_cast<int>(dstPlugins.size()) <= dstSlot)
        dstPlugins.emplace_back();

    if (copy) {
        // Copy: duplicate src into dst without touching src.
        closePluginEditor(dstCh, dstSlot);
        dstPlugins[static_cast<size_t>(dstSlot)] = srcPlugins[static_cast<size_t>(srcSlot)];
        while (!dstPlugins.empty() && dstPlugins.back().pluginId.empty())
            dstPlugins.pop_back();
        m_model->markDirty();
        m_model->buildChannelPluginChains();
        m_model->applyMixing();
        rebuildPluginSection(dstCh);
        if (dstCh != srcCh)
            rebuildPluginSection(srcCh);
    } else {
        // Move/swap: close both editors, swap slots, trim empties.
        closePluginEditor(srcCh, srcSlot);
        closePluginEditor(dstCh, dstSlot);
        std::swap(srcPlugins[static_cast<size_t>(srcSlot)],
                  dstPlugins[static_cast<size_t>(dstSlot)]);
        while (!srcPlugins.empty() && srcPlugins.back().pluginId.empty())
            srcPlugins.pop_back();
        while (!dstPlugins.empty() && dstPlugins.back().pluginId.empty())
            dstPlugins.pop_back();
        m_model->markDirty();
        m_model->buildChannelPluginChains();
        m_model->applyMixing();
        rebuildPluginSection(srcCh);
        if (dstCh != srcCh)
            rebuildPluginSection(dstCh);
    }
}

void MixConsoleDialog::openPluginEditor(int ch, int slot, bool pinToTop) {
    auto key = std::make_pair(ch, slot);
    auto it = m_pluginEditors.find(key);
    if (it != m_pluginEditors.end() && it->second) {
        auto* existingDlg = it->second.data();
        if (pinToTop) {
            existingDlg->setWindowFlags(existingDlg->windowFlags() | Qt::WindowStaysOnTopHint);
            existingDlg->show();
        }
        existingDlg->raise();
        existingDlg->activateWindow();
        return;
    }

    auto wrapper = m_model->channelPlugin(ch, slot);
    if (!wrapper || !wrapper->getProcessor()) return;

    auto* proc = wrapper->getProcessor();

    // Determine window title and whether this is an external plugin.
    const auto& pSlots = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
    const bool isExt   = (slot < static_cast<int>(pSlots.size()))
                         && pSlots[static_cast<size_t>(slot)].isExternal();
    QString title;
    if (isExt && !pSlots[static_cast<size_t>(slot)].extName.empty())
        title = QString::fromStdString(pSlots[static_cast<size_t>(slot)].extName);
    else
        title = QString("Ch %1 — Slot %2").arg(ch + 1).arg(slot + 1);

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(title);
    if (pinToTop)
        dlg->setWindowFlags(dlg->windowFlags() | Qt::WindowStaysOnTopHint);
    m_pluginEditors[key] = dlg;

    // When a pinned editor closes, WindowStaysOnTopHint can cause macOS to
    // move the MixConsole behind other windows.  Re-raise it on close.
    if (pinToTop)
        connect(dlg, &QDialog::finished, this, [this]() {
            raise();
            activateWindow();
        });

    auto* vl = new QVBoxLayout(dlg);
    vl->setSpacing(4);
    vl->setContentsMargins(4, 4, 4, 4);

    // Bypass button — always visible at the top of every editor window
    {
        const auto& pSlotsBp = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
        const bool curBypassed = (slot < static_cast<int>(pSlotsBp.size()))
                                 && pSlotsBp[static_cast<size_t>(slot)].bypassed;
        auto* bypassBtn = new QPushButton("Bypass");
        bypassBtn->setCheckable(true);
        bypassBtn->setChecked(curBypassed);
        bypassBtn->setFixedHeight(24);
        bypassBtn->setStyleSheet(
            "QPushButton { font-size:11px; }"
            "QPushButton:checked { background:#554; color:#cc8; border:1px solid #886; }");
        connect(bypassBtn, &QPushButton::toggled, this, [this, ch, slot](bool on) {
            auto& pSlots2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
            if (slot < static_cast<int>(pSlots2.size())) {
                pSlots2[static_cast<size_t>(slot)].bypassed = on;
                auto w2 = m_model->channelPlugin(ch, slot);
                if (w2) {
                    w2->setBypassed(on);
#ifdef __APPLE__
                    if (auto* au = dynamic_cast<mcp::plugin::AUPluginAdapter*>(w2->getProcessor()))
                        au->setNativeBypass(on);
#endif
                }
                m_model->markDirty();
                m_model->snapshots.markDirty("/mixer/" + std::to_string(ch) + "/plugin/" + std::to_string(slot));
                rebuildPluginSection(ch);
            }
        });
        vl->addWidget(bypassBtn);
    }

#ifdef __APPLE__
    // Try native Cocoa view for AU plugins.
    if (auto* auProc = dynamic_cast<mcp::plugin::AUPluginAdapter*>(proc)) {
        int w = 0, h = 0;
        void* nsView = auProc->createCocoaView(w, h);
        if (nsView) {
            auto* container = auCreateEditorWidget(nsView, w, h, dlg);
            if (container) {
                // Snapshot the current param values as baseline for diff-based autoscope.
                // Changes are detected at store/close time — no listener needed.
                resetPluginCache(ch, slot);

                // Plugin→host bypass sync: if the native AU editor has a bypass button,
                // reflect its state in our host bypass and show data.
                QPointer<QPushButton> bypassBtnPtr =
                    qobject_cast<QPushButton*>(vl->itemAt(0)->widget());
                auProc->startWatchingBypass([this, ch, slot, bypassBtnPtr](bool nativeBp) {
                    auto& pSlots3 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
                    if (slot >= static_cast<int>(pSlots3.size())) return;
                    auto& slBp = pSlots3[static_cast<size_t>(slot)];
                    if (slBp.bypassed == nativeBp) return;
                    slBp.bypassed = nativeBp;
                    auto w3 = m_model->channelPlugin(ch, slot);
                    if (w3) w3->setBypassed(nativeBp);
                    m_model->markDirty();
                    m_model->snapshots.markDirty("/mixer/" + std::to_string(ch) + "/plugin/" + std::to_string(slot));
                    // Update host bypass button to match plugin's native state.
                    if (bypassBtnPtr) {
                        bypassBtnPtr->blockSignals(true);
                        bypassBtnPtr->setChecked(nativeBp);
                        bypassBtnPtr->blockSignals(false);
                    }
                    rebuildPluginSection(ch);
                });

                // Polling fallback: some AUs don't dispatch kAudioUnitEvent_PropertyChange
                // for bypass — poll at 500 ms so the host always stays in sync.
                auto* bypassPollTimer = new QTimer(dlg);
                bypassPollTimer->setInterval(500);
                connect(bypassPollTimer, &QTimer::timeout, this,
                    [this, ch, slot, bypassBtnPtr, auProc]() {
                        if (!auProc) return;
                        const bool nativeBp = auProc->getNativeBypass();
                        auto& pSlots4 = m_model->sf.audioSetup.channels[
                                            static_cast<size_t>(ch)].plugins;
                        if (slot >= static_cast<int>(pSlots4.size())) return;
                        if (pSlots4[static_cast<size_t>(slot)].bypassed == nativeBp) return;
                        // Native bypass changed without a notification — sync host state.
                        pSlots4[static_cast<size_t>(slot)].bypassed = nativeBp;
                        auto w4 = m_model->channelPlugin(ch, slot);
                        if (w4) w4->setBypassed(nativeBp);
                        m_model->markDirty();
                        m_model->snapshots.markDirty("/mixer/" + std::to_string(ch) +
                                                     "/plugin/" + std::to_string(slot));
                        if (bypassBtnPtr) {
                            bypassBtnPtr->blockSignals(true);
                            bypassBtnPtr->setChecked(nativeBp);
                            bypassBtnPtr->blockSignals(false);
                        }
                        rebuildPluginSection(ch);
                });
                bypassPollTimer->start();

                connect(dlg, &QDialog::finished, this, [this, ch, slot, bypassPollTimer]() {
                    bypassPollTimer->stop();
                    // Diff on close so any unsaved changes are captured by the next store.
                    diffAndMarkPluginDirty(ch, slot);
                    m_pluginParamCaches.erase({ch, slot});
                    auto w2 = m_model->channelPlugin(ch, slot);
                    if (w2) {
                        if (auto* ap = dynamic_cast<mcp::plugin::AUPluginAdapter*>(
                                w2->getProcessor()))
                            ap->stopWatchingBypass();
                    }
                });

                vl->addWidget(container);
                dlg->adjustSize();
                dlg->show();
                return;
            }
        }
    }
#endif

#if defined(MCP_HAVE_VST3) && defined(__APPLE__)
    // Try VST3 native editor view (two-phase: create → show/settle → attach).
    if (auto* vst3Proc = dynamic_cast<mcp::plugin::VST3PluginAdapter*>(proc)) {
        auto* container = vst3CreateContainer(vst3Proc, dlg);
        if (container) {
            // Add to layout and show the dialog BEFORE calling attached() so the
            // container's NSView is inside a live NSWindow — strict plugins
            // (e.g. FabFilter) throw or fail if they can't reach [nsview window].
            vl->addWidget(container);
            dlg->show();
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

            int outW = 0, outH = 0;
            if (vst3AttachEditor(container, vst3Proc, outW, outH)) {
                container->setFixedSize(outW, outH);
                dlg->adjustSize();
                resetPluginCache(ch, slot);

                QPointer<QDialog> dlgGuard(dlg);
                vst3Proc->startWatchingParameters([this, ch, slot, dlgGuard]() {
                    if (!dlgGuard) return;
                    diffAndMarkPluginDirty(ch, slot);
                });
                connect(dlg, &QDialog::finished, this, [this, ch, slot]() {
                    auto w2 = m_model->channelPlugin(ch, slot);
                    if (w2) {
                        if (auto* vp = dynamic_cast<mcp::plugin::VST3PluginAdapter*>(
                                          w2->getProcessor()))
                            vp->stopWatchingParameters();
                    }
                    diffAndMarkPluginDirty(ch, slot);
                    m_pluginParamCaches.erase({ch, slot});
                });
                return;
            }

            // Attach failed — remove container and fall through to generic editor.
            vl->removeWidget(container);
            delete container;
        }
    }
#endif

    // Generic parameter editor — internal plugins and AUs/VST3 without custom UI.
    const auto& params = proc->getParameters();

    if (params.empty()) {
        // Plugin is missing or has no parameters — show status.
        QString msg;
        if (const auto* mp =
                dynamic_cast<const mcp::plugin::MissingPluginProcessor*>(proc)) {
            const auto& ref = mp->reference();
            const std::string detail = ref.runtimeMessage.empty()
                                       ? ref.pluginId
                                       : ref.runtimeMessage;
            msg = QString("Plugin not available:\n%1")
                      .arg(QString::fromStdString(detail));
        } else {
            msg = QString("Plugin loaded\n(no editable parameters)");
        }
        auto* lbl = new QLabel(msg, dlg);
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignCenter);
        vl->addWidget(lbl);
        dlg->setMinimumWidth(300);
        dlg->show();
        return;
    }

    // Cap the generic editor to prevent building hundreds of spinboxes for
    // large plugins (e.g. FabFilter Pro-Q 3 exposes 500+ parameters).
    static constexpr int kMaxGenericParams = 64;
    if (static_cast<int>(params.size()) > kMaxGenericParams) {
        auto* lbl = new QLabel(
            QString("Plugin has %1 parameters.\nOpen the plugin window to edit them.")
                .arg(static_cast<int>(params.size())), dlg);
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignCenter);
        vl->addWidget(lbl);
        dlg->setMinimumWidth(300);
        dlg->show();
        return;
    }

    dlg->setMinimumWidth(300);
    for (const auto& info : params) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QString::fromStdString(info.name) + ":"));

        auto* spin = new QDoubleSpinBox;
        spin->setRange(static_cast<double>(info.minValue),
                       static_cast<double>(info.maxValue));
        spin->setValue(static_cast<double>(proc->getParameterValue(info.id)));
        spin->setSingleStep(
            static_cast<double>((info.maxValue - info.minValue) / 100.0f));
        if (!info.unit.empty())
            spin->setSuffix(QString(" ") + QString::fromStdString(info.unit));
        spin->setProperty("pluginParamId", QString::fromStdString(info.id));
        row->addWidget(spin);
        vl->addLayout(row);

        const std::string pid = info.id;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                dlg, [this, ch, slot, pid, isExt](double v) {
            auto w2 = m_model->channelPlugin(ch, slot);
            if (w2 && w2->getProcessor())
                w2->getProcessor()->setParameterValue(pid, static_cast<float>(v));
            // Persist to ShowFile — external plugins use extParamSnapshot.
            auto& pSlots2 =
                m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].plugins;
            if (slot < static_cast<int>(pSlots2.size())) {
                if (isExt)
                    pSlots2[static_cast<size_t>(slot)].extParamSnapshot[pid] =
                        static_cast<float>(v);
                else
                    pSlots2[static_cast<size_t>(slot)].parameters[pid] =
                        static_cast<float>(v);
            }
            m_model->markDirty();
            m_model->snapshots.markDirty("/mixer/" + std::to_string(ch) + "/plugin/" + std::to_string(slot));
        });
    }

    dlg->show();
}

// ---------------------------------------------------------------------------
// Send slot UI
// ---------------------------------------------------------------------------

static QString panText(float v) {
    const int i = static_cast<int>(std::round(v * 100.0f));
    if (i == 0) return QStringLiteral("0");
    return (i < 0 ? QStringLiteral("L") : QStringLiteral("R")) + QString::number(std::abs(i));
}

static QString sendSectionStyle() {
    return "QPushButton { background:#1e1e2e; color:#aab; font-size:10px;"
           " text-align:left; padding-left:4px; border:none; }";
}

static QString sendSlotBtnStyle(bool muted) {
    return muted
        ? "QPushButton { background:#2a1a1a; color:#888; font-size:10px;"
          " text-align:left; padding-left:4px; border:1px solid #533; }"
        : "QPushButton { background:#1a1a2e; color:#ccd; font-size:10px;"
          " text-align:left; padding-left:4px; border:1px solid #446; }";
}

static QString sendEmptySlotStyle() {
    return "QPushButton { background:#181818; color:#555; font-size:10px;"
           " text-align:left; padding-left:4px; border:1px dashed #333; }";
}

QString MixConsoleDialog::sendSlotLabel(int ch, int slot) const {
    const auto& chans = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(chans.size())) return "—";
    const auto& sends = chans[static_cast<size_t>(ch)].sends;
    if (slot >= static_cast<int>(sends.size())) return "—";
    const auto& ss = sends[static_cast<size_t>(slot)];
    if (!ss.isActive()) return "—";
    const int dst = ss.dstChannel;
    if (dst < 0 || dst >= static_cast<int>(chans.size())) return "—";
    const QString dstName = QString::fromStdString(chans[static_cast<size_t>(dst)].name);
    const QString lvl     = QString::number(static_cast<double>(ss.levelDb), 'f', 1) + " dB";
    return (ss.muted ? "[M] " : "") + dstName + "  " + lvl;
}

void MixConsoleDialog::buildSendSection(Strip& s)
{
    const int ch = s.ch;
    s.sendHdr = new QPushButton("▸ Sends");
    s.sendHdr->setFlat(false);
    s.sendHdr->setFixedHeight(20);
    s.sendHdr->setAutoDefault(false);
    s.sendHdr->setDefault(false);
    s.sendHdr->setStyleSheet(sendSectionStyle());

    s.sendBody = new QWidget;
    s.sendBody->setVisible(false);
    auto* bl = new QVBoxLayout(s.sendBody);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);

    connect(s.sendHdr, &QPushButton::clicked, this, [this, ch]() {
        Strip* sp = nullptr;
        for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
        if (!sp) return;
        const bool nowVisible = !sp->sendBody->isVisible();
        sp->sendBody->setVisible(nowVisible);
        sp->sendHdr->setText(nowVisible ? "▾ Sends" : "▸ Sends");
    });

    rebuildSendSlots(s, bl, ch);
}

void MixConsoleDialog::rebuildSendSection(int ch)
{
    Strip* sp = nullptr;
    for (auto& ss : m_strips) if (ss.ch == ch) { sp = &ss; break; }
    if (!sp || !sp->sendBody) return;

    const bool wasOpen = sp->sendBody->isVisible();

    sp->sendSlotWidgets.clear();
    auto* bl = qobject_cast<QVBoxLayout*>(sp->sendBody->layout());
    if (!bl) return;
    while (QLayoutItem* item = bl->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (sp->sendHdr) sp->sendHdr->setText(wasOpen ? "▾ Sends" : "▸ Sends");
    if (sp->sendBody) sp->sendBody->setVisible(wasOpen);

    rebuildSendSlots(*sp, bl, ch);
}

void MixConsoleDialog::rebuildSendSlots(Strip& s, QVBoxLayout* bl, int ch)
{
    const auto& chans = m_model->sf.audioSetup.channels;
    const int nCh = static_cast<int>(chans.size());
    if (ch < 0 || ch >= nCh) return;
    const auto& sends = chans[static_cast<size_t>(ch)].sends;

    const int displayCount = std::min(
        static_cast<int>(sends.size()) + 1,
        ShowFile::AudioSetup::Channel::kMaxSendSlots);

    for (int slot = 0; slot < displayCount; ++slot)
        addSendSlotRow(bl, s, ch, slot);
}

void MixConsoleDialog::addSendSlotRow(QVBoxLayout* bl, Strip& s, int ch, int slot)
{
    const auto& chans = m_model->sf.audioSetup.channels;
    const auto& sends = chans[static_cast<size_t>(ch)].sends;
    const bool hasSlot = (slot < static_cast<int>(sends.size()))
                         && sends[static_cast<size_t>(slot)].isActive();

    auto* btn = new QPushButton(hasSlot ? sendSlotLabel(ch, slot) : "— empty —");
    btn->setFixedHeight(22);
    btn->setAutoDefault(false);
    btn->setDefault(false);
    btn->setStyleSheet(hasSlot
        ? sendSlotBtnStyle(sends[static_cast<size_t>(slot)].muted)
        : sendEmptySlotStyle());
    btn->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(btn, &QPushButton::clicked, this, [this, ch, slot, hasSlot]() {
        if (!hasSlot) openSendPicker(ch, slot);
        else          openSendEditor(ch, slot);
    });

    if (hasSlot) {
        connect(btn, &QPushButton::customContextMenuRequested, this,
                [this, ch, slot](const QPoint& pos) {
            QMenu m;
            m.addAction("Open Editor",  this, [this, ch, slot]() { openSendEditor(ch, slot); });
            m.addSeparator();
            m.addAction("Remove Send",  this, [this, ch, slot]() { removeSend(ch, slot); });
            m.exec(static_cast<QPushButton*>(sender())->mapToGlobal(pos));
        });
    }

    bl->addWidget(btn);
    s.sendSlotWidgets.push_back(btn);
}

void MixConsoleDialog::closeSendEditor(int ch, int slot)
{
    auto it = m_sendEditors.find({ch, slot});
    if (it != m_sendEditors.end() && it->second)
        it->second->close();
}

void MixConsoleDialog::openSendEditor(int ch, int slot)
{
    auto key = std::make_pair(ch, slot);
    auto it = m_sendEditors.find(key);
    if (it != m_sendEditors.end() && it->second) {
        it->second->raise();
        it->second->activateWindow();
        return;
    }

    const auto& chans = m_model->sf.audioSetup.channels;
    const int nCh = static_cast<int>(chans.size());
    if (ch < 0 || ch >= nCh) return;
    const auto& sends = chans[static_cast<size_t>(ch)].sends;
    if (slot < 0 || slot >= static_cast<int>(sends.size())) return;
    if (!sends[static_cast<size_t>(slot)].isActive()) return;

    const int dstMaster = sends[static_cast<size_t>(slot)].dstChannel;
    if (dstMaster < 0 || dstMaster >= nCh) return;

    const QString srcName = QString::fromStdString(chans[static_cast<size_t>(ch)].name);
    const QString dstName = QString::fromStdString(chans[static_cast<size_t>(dstMaster)].name);
    const bool srcStereo = chans[static_cast<size_t>(ch)].linkedStereo && (ch + 1 < nCh);
    const bool dstStereo = chans[static_cast<size_t>(dstMaster)].linkedStereo && (dstMaster + 1 < nCh);
    const bool hasPan   = !srcStereo && dstStereo;   // mono→stereo: one pan
    const bool hasPanLR = srcStereo  && dstStereo;   // stereo→stereo: independent L/R pan

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(srcName + "  →  " + dstName);
    m_sendEditors[key] = dlg;

    auto* vl = new QVBoxLayout(dlg);
    vl->setSpacing(8);
    vl->setContentsMargins(10, 10, 10, 10);

    // ── Mute toggle ──────────────────────────────────────────────────────
    const bool initMuted = sends[static_cast<size_t>(slot)].muted;
    auto* muteBtn = new QPushButton(initMuted ? "Muted" : "Active");
    muteBtn->setCheckable(true);
    muteBtn->setChecked(initMuted);
    muteBtn->setFixedHeight(28);
    muteBtn->setStyleSheet(
        "QPushButton         { background:#1e2e1e; color:#8c8; font-size:11px;"
        "                      border:1px solid #484; border-radius:3px; }"
        "QPushButton:checked { background:#6a2020; color:#faa;"
        "                      border:1px solid #844; }");
    connect(muteBtn, &QPushButton::toggled, this, [this, ch, slot, muteBtn](bool on) {
        auto& s2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
        if (slot >= static_cast<int>(s2.size())) return;
        s2[static_cast<size_t>(slot)].muted = on;
        muteBtn->setText(on ? "Muted" : "Active");
        m_model->markDirty();
        m_model->snapshots.markDirty(sendParamPath(ch, slot, "mute"));
        m_model->rebuildSendGains();
        rebuildSendSection(ch);
    });
    vl->addWidget(muteBtn);

    // ── Controls row: Level fader + Pan knob(s) ──────────────────────────
    auto* ctrlRow = new QWidget;
    auto* hl = new QHBoxLayout(ctrlRow);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(16);

    // Level fader
    auto* fader = new FaderWidget("Level");
    fader->setValue(sends[static_cast<size_t>(slot)].levelDb);
    connect(fader, &FaderWidget::valueChanged, this, [this, ch, slot](float db) {
        auto& s2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
        if (slot >= static_cast<int>(s2.size())) return;
        s2[static_cast<size_t>(slot)].levelDb = db;
        m_model->markDirty();
        m_model->snapshots.markDirty(sendParamPath(ch, slot, "level"));
        m_model->rebuildSendGains();
        rebuildSendSection(ch);  // refresh button label
    });
    hl->addWidget(fader, 0, Qt::AlignHCenter);

    // Helper: build one pan column (label / knob / value readout)
    auto makePanCol = [&](const QString& label, float initPan,
                           std::function<void(float)> onChange) {
        auto* col = new QWidget;
        auto* cvl2 = new QVBoxLayout(col);
        cvl2->setContentsMargins(0, 0, 0, 0);
        cvl2->setSpacing(2);

        auto* lbl = new QLabel(label);
        lbl->setAlignment(Qt::AlignHCenter);
        lbl->setStyleSheet("font-size:10px; color:#aaa;");
        cvl2->addWidget(lbl);

        auto* knob = new PanKnob(initPan);
        cvl2->addWidget(knob, 0, Qt::AlignHCenter);

        auto* valLbl = new QLabel(panText(initPan));
        valLbl->setAlignment(Qt::AlignHCenter);
        valLbl->setStyleSheet("font-size:10px; color:#aaa;");
        cvl2->addWidget(valLbl);

        knob->onCommit = [onChange, valLbl](float v) {
            valLbl->setText(panText(v));
            onChange(v);
        };
        return col;
    };

    if (hasPan) {
        hl->addWidget(makePanCol("Pan", sends[static_cast<size_t>(slot)].panL,
            [this, ch, slot](float v) {
                auto& s2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
                if (slot < static_cast<int>(s2.size())) s2[static_cast<size_t>(slot)].panL = v;
                m_model->markDirty();
                m_model->snapshots.markDirty(sendParamPath(ch, slot, "panL"));
                m_model->rebuildSendGains();
            }), 0, Qt::AlignHCenter);
    } else if (hasPanLR) {
        hl->addWidget(makePanCol("Pan L", sends[static_cast<size_t>(slot)].panL,
            [this, ch, slot](float v) {
                auto& s2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
                if (slot < static_cast<int>(s2.size())) s2[static_cast<size_t>(slot)].panL = v;
                m_model->markDirty();
                m_model->snapshots.markDirty(sendParamPath(ch, slot, "panL"));
                m_model->rebuildSendGains();
            }), 0, Qt::AlignHCenter);
        hl->addWidget(makePanCol("Pan R", sends[static_cast<size_t>(slot)].panR,
            [this, ch, slot](float v) {
                auto& s2 = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
                if (slot < static_cast<int>(s2.size())) s2[static_cast<size_t>(slot)].panR = v;
                m_model->markDirty();
                m_model->snapshots.markDirty(sendParamPath(ch, slot, "panR"));
                m_model->rebuildSendGains();
            }), 0, Qt::AlignHCenter);
    }

    vl->addWidget(ctrlRow);

    // ── Remove Send ───────────────────────────────────────────────────────
    auto* removeBtn = new QPushButton("Remove Send");
    removeBtn->setFixedHeight(26);
    removeBtn->setAutoDefault(false);
    removeBtn->setDefault(false);
    removeBtn->setStyleSheet(
        "QPushButton { background:#2a1a1a; color:#c88; font-size:11px;"
        "              border:1px solid #644; border-radius:3px; }");
    connect(removeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    connect(removeBtn, &QPushButton::clicked, this, [this, ch, slot]() {
        removeSend(ch, slot);
    });
    vl->addWidget(removeBtn);

    dlg->adjustSize();
    dlg->show();
}

void MixConsoleDialog::openSendPicker(int ch, int slot)
{
    const auto& chans = m_model->sf.audioSetup.channels;
    const int nCh = static_cast<int>(chans.size());
    if (ch < 0 || ch >= nCh) return;

    QMenu menu;

    for (int dst = 0; dst < nCh; ++dst) {
        if (dst == ch) continue;
        // Skip slave channels — represented by master.
        if (dst > 0 && chans[static_cast<size_t>(dst - 1)].linkedStereo) continue;
        // Skip if adding this would form a cycle.
        if (m_model->sendWouldCreateCycle(ch, dst)) continue;
        // Skip already-used destinations.
        bool alreadyUsed = false;
        for (const auto& ss : chans[static_cast<size_t>(ch)].sends) {
            if (ss.isActive() && ss.dstChannel == dst) { alreadyUsed = true; break; }
        }
        if (alreadyUsed) continue;

        const QString name = QString::fromStdString(chans[static_cast<size_t>(dst)].name);
        menu.addAction(name, this, [this, ch, slot, dst, nCh]() {
            const auto& chans2 = m_model->sf.audioSetup.channels;
            const bool srcStereo = chans2[static_cast<size_t>(ch)].linkedStereo
                                   && (ch + 1 < nCh);
            const bool dstStereo = chans2[static_cast<size_t>(dst)].linkedStereo
                                   && (dst + 1 < nCh);
            auto& sends = m_model->sf.audioSetup.channels[static_cast<size_t>(ch)].sends;
            while (static_cast<int>(sends.size()) <= slot) sends.emplace_back();
            sends[static_cast<size_t>(slot)].dstChannel = dst;
            sends[static_cast<size_t>(slot)].levelDb    = 0.0f;
            // stereo→stereo defaults to hard L/R; all other combos default to center
            sends[static_cast<size_t>(slot)].panL       = (srcStereo && dstStereo) ? -1.0f : 0.0f;
            sends[static_cast<size_t>(slot)].panR       = (srcStereo && dstStereo) ? +1.0f : 0.0f;
            sends[static_cast<size_t>(slot)].muted      = false;
            m_model->markDirty();
            m_model->rebuildSendTopology();
            rebuildSendSection(ch);
        });
    }

    if (menu.isEmpty())
        menu.addAction("No available destinations")->setEnabled(false);

    menu.exec(QCursor::pos());
}

void MixConsoleDialog::removeSend(int ch, int slot)
{
    auto& chans = m_model->sf.audioSetup.channels;
    if (ch < 0 || ch >= static_cast<int>(chans.size())) return;
    auto& sends = chans[static_cast<size_t>(ch)].sends;
    if (slot < 0 || slot >= static_cast<int>(sends.size())) return;
    closeSendEditor(ch, slot);
    sends.erase(sends.begin() + slot);
    m_model->markDirty();
    m_model->rebuildSendTopology();
    rebuildSendSection(ch);
}
