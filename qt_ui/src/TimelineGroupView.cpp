#include "TimelineGroupView.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/AudioDecoder.h"
#include "engine/Cue.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <thread>

static constexpr double kSnapThreshPx = 8.0;
static constexpr double kMinViewSec   = 5.0;

// Ruler tick label: no minutes → "SS.cc" (centiseconds); has minutes → "M:SS"
static QString fmtRulerTick(double t) {
    if (t < 0.0) t = 0.0;
    const int t100 = static_cast<int>(std::round(t * 100.0));
    const int cs   = t100 % 100;
    const int sec  = (t100 / 100) % 60;
    const int min  = t100 / 6000;
    char buf[24];
    if (min > 0) std::snprintf(buf, sizeof(buf), "%d:%02d", min, sec);
    else         std::snprintf(buf, sizeof(buf), "%d.%02d", sec, cs);
    return QString(buf);
}

TimelineGroupView::TimelineGroupView(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setMinimumHeight(kRulerH + kTopPad + kBlockH + 8);
    setMouseTracking(true);
    setStyleSheet("background:#111;");
}

// ---------------------------------------------------------------------------
// Layout helper

int TimelineGroupView::laneY(int i) const {
    return kRulerH + kTopPad + i * (kBlockH + kLaneGap) - m_laneScrollPx;
}

QSize TimelineGroupView::sizeHint() const {
    const int n = (int)m_blocks.size();
    const int h = kRulerH + kTopPad + std::max(1, n) * (kBlockH + kLaneGap) + 4;
    return {width(), h};
}

// ---------------------------------------------------------------------------
// Internal helpers

void TimelineGroupView::clearArmCursor() {
    m_armSec = -1.0;
    update();
}

void TimelineGroupView::setGroupCueIndex(int groupFlatIdx) {
    m_groupIdx      = groupFlatIdx;
    m_dragBlock     = -1;
    m_laneScrollPx  = 0;
    rebuildBlocks();
    updateGeometry();
    update();
}

void TimelineGroupView::rebuildBlocks() {
    m_blocks.clear();
    if (m_groupIdx < 0) return;
    const mcp::Cue* group = m_model->cues.cueAt(m_groupIdx);
    if (!group || group->type != mcp::CueType::Group) return;

    for (int i = m_groupIdx + 1; i <= m_groupIdx + group->childCount; ) {
        const mcp::Cue* c = m_model->cues.cueAt(i);
        if (!c) { ++i; continue; }
        if (c->parentIndex == m_groupIdx) {
            ChildBlock b;
            b.flatIdx   = i;
            b.offset    = c->timelineOffset;
            b.startTime = c->startTime;
            b.duration  = (c->duration > 0.0) ? c->duration
                        : (c->type == mcp::CueType::Audio
                           ? m_model->cues.cueTotalSeconds(i) : 2.0);
            b.label = QString::fromStdString(
                c->cueNumber.empty() ? c->name
                                     : (c->cueNumber + (c->name.empty() ? "" : " " + c->name)));
            if (c->type == mcp::CueType::Audio && !c->path.empty()) {
                b.audioPath = c->path;
                buildPeaksAsync(c->path);
            }
            m_blocks.push_back(b);
            const int cc = (c->type == mcp::CueType::Group) ? c->childCount : 0;
            i += cc + 1;
        } else {
            ++i;
        }
    }
}

void TimelineGroupView::buildPeaksAsync(const std::string& path) {
    if (m_peakCache.count(path)) return;   // already cached or building
    m_peakCache[path] = {};                // placeholder — prevents double launch
    std::thread([this, path]() {
        PeakCache pc;
        pc.valid = mcp::buildWaveformPeaks(path, 800,
                                            pc.minPk, pc.maxPk,
                                            pc.fileDur, pc.fileCh);
        QMetaObject::invokeMethod(this, [this, path, pc = std::move(pc)]() mutable {
            m_peakCache[path] = std::move(pc);
            update();
        }, Qt::QueuedConnection);
    }).detach();
}

double TimelineGroupView::viewDuration() const {
    double maxEnd = kMinViewSec;
    for (const auto& b : m_blocks)
        maxEnd = std::max(maxEnd, b.offset + b.duration);
    return maxEnd * 1.2;
}

double TimelineGroupView::pixToSec(int px) const {
    return m_viewStart + (px > 0 ? static_cast<double>(px) / m_pixPerSec : 0.0);
}

int TimelineGroupView::secToPix(double sec) const {
    return static_cast<int>((sec - m_viewStart) * m_pixPerSec);
}

void TimelineGroupView::updateSnapTargets() {
    m_snapTimes.clear();
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        if (i == m_dragBlock) continue;
        m_snapTimes.push_back(m_blocks[i].offset);
        m_snapTimes.push_back(m_blocks[i].offset + m_blocks[i].duration);
    }
    m_snapTimes.push_back(0.0);
}

// ---------------------------------------------------------------------------
// Paint

void TimelineGroupView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int W = width();

    // ── Ruler ────────────────────────────────────────────────────────────────
    p.fillRect(0, 0, W, kRulerH, QColor(0x1a, 0x1a, 0x1a));
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.drawLine(0, kRulerH, W, kRulerH);

    // Pick the smallest tick step giving >= 55 px between ticks.
    const double vis = viewDuration();
    double tickStep = 300.0;
    for (double s : {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0})
        if (W * s / vis > 55.0) { tickStep = s; break; }

    // Label only every Nth tick so labels stay >= 65 px apart.
    const int labelMult = std::max(1, (int)std::ceil(65.0 / (m_pixPerSec * tickStep)));

    p.setPen(QColor(0x77, 0x77, 0x77));
    const double firstTick = std::ceil(m_viewStart / tickStep) * tickStep;
    for (double t = firstTick; secToPix(t) < W; t += tickStep) {
        const int  tx      = secToPix(t);
        const int  tidx    = static_cast<int>(std::round(t / tickStep));
        const bool isLabel = (tidx % labelMult == 0);
        p.drawLine(tx, kRulerH - (isLabel ? 6 : 3), tx, kRulerH);
        if (isLabel)
            p.drawText(tx + 2, 1, 64, kRulerH - 2, Qt::AlignLeft | Qt::AlignVCenter,
                       fmtRulerTick(t));
    }

    // ── Lanes ────────────────────────────────────────────────────────────────
    p.setClipRect(0, kRulerH, W, height() - kRulerH);
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const auto& b       = m_blocks[i];
        const int   ly      = laneY(i);
        const int   x1      = secToPix(b.offset);
        const int   x2      = secToPix(b.offset + b.duration);
        const int   bw      = std::max(4, x2 - x1);
        const bool  isDrag  = (i == m_dragBlock);

        // Lane background track
        p.fillRect(0, ly, W, kBlockH, QColor(0x18, 0x18, 0x18));

        // Block fill
        const QColor fill   = isDrag ? QColor(0x44, 0x88, 0xdd) : QColor(0x28, 0x60, 0xa8);
        const QColor border = isDrag ? QColor(0x88, 0xcc, 0xff) : QColor(0x44, 0x88, 0xcc);
        p.fillRect(x1, ly, bw, kBlockH, fill);

        // ── Waveform thumbnail (audio cues only) ─────────────────────────
        if (!b.audioPath.empty()) {
            auto it = m_peakCache.find(b.audioPath);
            if (it != m_peakCache.end() && it->second.valid
                && it->second.fileDur > 0.0 && !it->second.minPk[0].empty()) {
                const PeakCache& pk = it->second;
                const int nBuckets  = (int)pk.minPk[0].size();
                const int waveTop   = ly + 4;
                const int waveBot   = ly + kBlockH - 4;
                const int mid       = (waveTop + waveBot) / 2;
                const int halfH     = (waveBot - waveTop) / 2;

                p.setPen(QPen(QColor(0xaa, 0xdd, 0xff, 0xb0), 1));
                for (int px = x1; px < x1 + bw; ++px) {
                    if (px < 0 || px >= W) continue;
                    const double t  = b.startTime
                                    + (double)(px - x1) / bw * b.duration;
                    const int bi = std::clamp(
                        (int)(t / pk.fileDur * nBuckets), 0, nBuckets - 1);
                    const float mn = pk.minPk[0][bi];
                    const float mx = pk.maxPk[0][bi];
                    const int y1p  = mid - (int)(mx * halfH);
                    const int y2p  = mid - (int)(mn * halfH);
                    p.drawLine(px, std::max(waveTop, y1p),
                               px, std::min(waveBot, y2p));
                }
            }
        }

        // Block border
        p.setPen(border);
        p.drawRect(x1, ly, bw - 1, kBlockH - 1);

        // Label (clipped to block)
        p.setClipRect(x1 + 1, ly + 1, bw - 2, kBlockH - 2);
        p.setPen(Qt::white);
        p.drawText(x1 + 4, ly, bw - 8, kBlockH,
                   Qt::AlignLeft | Qt::AlignVCenter, b.label);
        p.setClipping(false);
    }
    p.setClipping(false);

    // ── Arm cursor (drawn last so it's on top of waveform thumbnails) ────────
    if (m_armSec >= 0.0) {
        const int ax = secToPix(m_armSec);
        if (ax >= 0 && ax < W) {
            p.setPen(QPen(QColor(0x40, 0xee, 0x80, 0xc0), 1));
            p.drawLine(ax, kRulerH, ax, height());
            QPolygon tri;
            tri << QPoint(ax, kRulerH)
                << QPoint(ax - 5, 4)
                << QPoint(ax + 5, 4);
            p.setBrush(QColor(0x40, 0xee, 0x80, 0xc0));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse

void TimelineGroupView::mousePressEvent(QMouseEvent* ev) {
    const int py = ev->pos().y();
    const int px = ev->pos().x();

    // Block drag: hit-test per lane
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const int ly = laneY(i);
        if (ly + kBlockH <= kRulerH) continue;  // scrolled above ruler
        if (ly >= height()) break;              // scrolled below view
        if (py >= ly && py < ly + kBlockH) {
            const int x1 = secToPix(m_blocks[i].offset);
            const int x2 = secToPix(m_blocks[i].offset + m_blocks[i].duration);
            if (px >= x1 && px <= x2) {
                m_dragBlock       = i;
                m_dragStartX      = px;
                m_dragStartOffset = m_blocks[i].offset;
                updateSnapTargets();
                return;
            }
        }
    }

    // Ruler click → set arm cursor and notify engine to arm from this position.
    if (py < kRulerH) {
        m_armSec = std::max(0.0, pixToSec(px));
        update();
        emit rulerClicked(m_armSec);
    }
}

void TimelineGroupView::mouseMoveEvent(QMouseEvent* ev) {
    if (m_dragBlock < 0) return;

    const double deltaSec = static_cast<double>(ev->pos().x() - m_dragStartX) / m_pixPerSec;
    double newOffset = std::max(0.0, m_dragStartOffset + deltaSec);

    const double snapThreshSec = kSnapThreshPx / m_pixPerSec;
    for (double t : m_snapTimes) {
        if (std::fabs(newOffset - t) < snapThreshSec) {
            newOffset = t;
            break;
        }
        const double blockEnd = newOffset + m_blocks[m_dragBlock].duration;
        const double endSnap  = t - m_blocks[m_dragBlock].duration;
        if (std::fabs(blockEnd - t) < snapThreshSec && endSnap >= 0.0) {
            newOffset = endSnap;
            break;
        }
    }

    m_blocks[m_dragBlock].offset = newOffset;
    update();
}

void TimelineGroupView::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragBlock >= 0) {
        const double newOff = m_blocks[m_dragBlock].offset;
        const int flatIdx   = m_blocks[m_dragBlock].flatIdx;
        m_dragBlock = -1;
        emit childOffsetChanged(flatIdx, newOff);
    }
}

void TimelineGroupView::wheelEvent(QWheelEvent* ev) {
    if (ev->modifiers() & Qt::ControlModifier) {
        // Cmd/Ctrl+scroll → zoom around mouse pivot
        const double steps = ev->angleDelta().y() / 120.0;
        const double pivotSec = pixToSec(ev->position().x());
        m_pixPerSec = std::clamp(m_pixPerSec * std::pow(1.25, steps), 4.0, 8000.0);
        m_viewStart = std::max(0.0, pivotSec - ev->position().x() / m_pixPerSec);
    } else if ((ev->modifiers() & Qt::ShiftModifier) || ev->angleDelta().x() != 0) {
        // Shift+scroll → horizontal pan (macOS converts Shift+vertical to angleDelta.x)
        const double delta = ev->angleDelta().x() != 0
            ? ev->angleDelta().x() : ev->angleDelta().y();
        const double steps = delta / 120.0;
        const double viewWidth = width() > 0 ? width() / m_pixPerSec : 1.0;
        m_viewStart = std::max(0.0, m_viewStart - steps * viewWidth * 0.3);
    } else {
        // Plain scroll → vertical lane scroll
        const double steps = ev->angleDelta().y() / 120.0;
        const int n = (int)m_blocks.size();
        const int totalH = kTopPad + n * (kBlockH + kLaneGap);
        const int visH   = height() - kRulerH;
        const int maxScroll = std::max(0, totalH - visH);
        m_laneScrollPx = std::clamp(
            (int)(m_laneScrollPx - steps * kBlockH), 0, maxScroll);
    }
    update();
    ev->accept();
}

void TimelineGroupView::resizeEvent(QResizeEvent*) {
    const double dur = viewDuration();
    if (dur > 0.0 && width() > 0)
        m_pixPerSec = width() / dur;
}
