#include "TimelineGroupView.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/AudioDecoder.h"
#include "engine/Cue.h"

#include <QContextMenuEvent>
#include <QMenu>
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

void TimelineGroupView::setMusicContext(const mcp::MusicContext* mc) {
    m_mc = mc;
    update();
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
    const bool groupChanged = (groupFlatIdx != m_groupIdx);
    m_groupIdx     = groupFlatIdx;
    m_dragBlock    = -1;
    m_dragMode     = DragMode::None;
    m_selBlock     = -1;
    if (groupChanged) {
        m_laneScrollPx = 0;
        m_viewStart    = 0.0;
        m_pixPerSec    = 80.0;
    }
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
            b.fileDur   = (c->type == mcp::CueType::Audio)
                          ? m_model->cues.cueTotalSeconds(i) : 0.0;
            b.duration  = (c->duration > 0.0) ? c->duration
                        : (b.fileDur > 0.0 ? b.fileDur - c->startTime : 2.0);
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
    if (m_peakCache.count(path)) return;
    m_peakCache[path] = {};
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

void TimelineGroupView::updateHoverCursor(int px, int py) {
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const int ly = laneY(i);
        if (ly + kBlockH <= kRulerH || ly >= height()) continue;
        if (py < ly || py >= ly + kBlockH) continue;
        const int x1 = secToPix(m_blocks[i].offset);
        const int x2 = secToPix(m_blocks[i].offset + m_blocks[i].duration);
        if (px < x1 || px > x2) continue;
        if (px <= x1 + kHandleW || px >= x2 - kHandleW)
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(Qt::SizeAllCursor);
        return;
    }
    unsetCursor();
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

    p.setPen(QColor(0x77, 0x77, 0x77));
    if (m_mc) {
        // Bar/beat ruler from Music Context
        const auto startPos = m_mc->secondsToMusical(m_viewStart);
        int bar = startPos.bar - 1;
        int lastLabelX = -100;
        for (int safety = 0; safety < 4000; safety++, bar++) {
            const int bx = secToPix(m_mc->musicalToSeconds(bar, 1));
            if (bx > W) break;
            if (bx >= -2) {
                p.drawLine(bx, kRulerH - 6, bx, kRulerH);
                if (bx - lastLabelX >= 36) {
                    p.drawText(bx + 2, 1, 54, kRulerH - 2,
                               Qt::AlignLeft | Qt::AlignVCenter, QString::number(bar));
                    lastLabelX = bx;
                }
            }
            const auto ts = m_mc->timeSigAt(bar, 1);
            for (int beat = 2; beat <= ts.num; beat++) {
                const int tx = secToPix(m_mc->musicalToSeconds(bar, beat));
                if (tx < 0 || tx > W) continue;
                p.drawLine(tx, kRulerH - 3, tx, kRulerH);
            }
        }
    } else {
        const double vis = viewDuration();
        double tickStep = 300.0;
        for (double s : {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0})
            if (W * s / vis > 55.0) { tickStep = s; break; }
        const int labelMult = std::max(1, (int)std::ceil(65.0 / (m_pixPerSec * tickStep)));
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
        const bool  isSel   = (i == m_selBlock);

        // Lane background track
        p.fillRect(0, ly, W, kBlockH, QColor(0x18, 0x18, 0x18));

        // Block fill
        QColor fill, border;
        if (isDrag)        { fill = QColor(0x44, 0x88, 0xdd); border = QColor(0x88, 0xcc, 0xff); }
        else if (isSel)    { fill = QColor(0x30, 0x70, 0xc0); border = QColor(0x66, 0xaa, 0xff); }
        else               { fill = QColor(0x28, 0x60, 0xa8); border = QColor(0x44, 0x88, 0xcc); }
        p.fillRect(x1, ly, bw, kBlockH, fill);

        // ── Waveform thumbnail ────────────────────────────────────────────
        if (!b.audioPath.empty()) {
            auto it = m_peakCache.find(b.audioPath);
            if (it != m_peakCache.end() && it->second.valid
                && it->second.fileDur > 0.0 && !it->second.minPk[0].empty()) {
                const PeakCache& pk = it->second;
                const int nBuckets  = (int)pk.minPk[0].size();
                const int waveTop   = ly + 14;   // leave room for label
                const int waveBot   = ly + kBlockH - 3;
                const int mid       = (waveTop + waveBot) / 2;
                const int halfH     = (waveBot - waveTop) / 2;

                p.setPen(QPen(QColor(0xaa, 0xdd, 0xff, 0xb0), 1));
                for (int px2 = std::max(0, x1); px2 < std::min(W, x1 + bw); ++px2) {
                    const double t  = b.startTime
                                    + (double)(px2 - x1) / bw * b.duration;
                    const int bi = std::clamp(
                        (int)(t / pk.fileDur * nBuckets), 0, nBuckets - 1);
                    const float mn = pk.minPk[0][bi];
                    const float mx = pk.maxPk[0][bi];
                    p.drawLine(px2, std::max(waveTop, mid - (int)(mx * halfH)),
                               px2, std::min(waveBot, mid - (int)(mn * halfH)));
                }
            }
        }

        // Trim handles (subtle bright strip at edges)
        p.fillRect(x1,           ly, kHandleW, kBlockH, QColor(255,255,255,25));
        p.fillRect(x1+bw-kHandleW, ly, kHandleW, kBlockH, QColor(255,255,255,25));

        // Block border
        p.setPen(border);
        p.drawRect(x1, ly, bw - 1, kBlockH - 1);

        // ── Sticky label + prewait sub-label ──────────────────────────────
        // labelX: anchored to max(block left, viewport left) so label stays
        // visible even when block starts off the left edge of the view.
        const int labelX = std::max(x1 + 4, 2);
        const int labelR = x1 + bw - 2;
        if (labelR > labelX) {
            p.save();
            p.setClipRect(labelX, ly + 1, labelR - labelX, kBlockH - 2, Qt::IntersectClip);
            p.setPen(Qt::white);
            p.drawText(labelX, ly, labelR - labelX, kBlockH / 2 + 2,
                       Qt::AlignLeft | Qt::AlignVCenter, b.label);
            p.setPen(QColor(180, 210, 255, 180));
            p.drawText(labelX, ly + kBlockH / 2, labelR - labelX, kBlockH / 2,
                       Qt::AlignLeft | Qt::AlignVCenter, fmtRulerTick(b.offset));
            p.restore();
        }
    }
    p.setClipping(false);

    // ── Arm cursor ───────────────────────────────────────────────────────────
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

    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const int ly = laneY(i);
        if (ly + kBlockH <= kRulerH) continue;
        if (ly >= height()) break;
        if (py < ly || py >= ly + kBlockH) continue;

        const int x1 = secToPix(m_blocks[i].offset);
        const int x2 = secToPix(m_blocks[i].offset + m_blocks[i].duration);
        if (px < x1 || px > x2) continue;

        m_selBlock            = i;
        m_dragBlock           = i;
        m_dragStartX          = px;
        m_dragStartOffset     = m_blocks[i].offset;
        m_dragStartStartTime  = m_blocks[i].startTime;
        m_dragStartDuration   = m_blocks[i].duration;

        if (px <= x1 + kHandleW)
            m_dragMode = DragMode::TrimLeft;
        else if (px >= x2 - kHandleW)
            m_dragMode = DragMode::TrimRight;
        else {
            m_dragMode = DragMode::Move;
            updateSnapTargets();
        }
        update();
        return;
    }

    // Ruler click → arm cursor (snapped to grid if quantize is set)
    if (py < kRulerH) {
        m_armSec = snapToGrid(std::max(0.0, pixToSec(px)));
        update();
        emit rulerClicked(m_armSec);
    } else {
        // Click in empty lane area → deselect
        m_selBlock = -1;
        update();
    }
}

void TimelineGroupView::mouseMoveEvent(QMouseEvent* ev) {
    const int px = ev->pos().x();
    const int py = ev->pos().y();

    if (m_dragBlock < 0) {
        updateHoverCursor(px, py);
        return;
    }

    const double deltaSec = static_cast<double>(px - m_dragStartX) / m_pixPerSec;

    if (m_dragMode == DragMode::Move) {
        double newOffset = std::max(0.0, m_dragStartOffset + deltaSec);
        if (m_mc && m_quantSubdiv > 0) {
            newOffset = snapToGrid(newOffset);
        } else {
            const double snapThreshSec = kSnapThreshPx / m_pixPerSec;
            for (double t : m_snapTimes) {
                if (std::fabs(newOffset - t) < snapThreshSec) { newOffset = t; break; }
                const double blockEnd = newOffset + m_blocks[m_dragBlock].duration;
                const double endSnap  = t - m_blocks[m_dragBlock].duration;
                if (std::fabs(blockEnd - t) < snapThreshSec && endSnap >= 0.0) {
                    newOffset = endSnap; break;
                }
            }
        }
        m_blocks[m_dragBlock].offset = newOffset;

    } else if (m_dragMode == DragMode::TrimLeft) {
        // Both offset and startTime shift together; duration adjusts to keep right edge fixed.
        double newOffset    = std::max(0.0, m_dragStartOffset + deltaSec);
        if (m_mc && m_quantSubdiv > 0)
            newOffset = snapToGrid(newOffset);
        const double actualDelta  = newOffset - m_dragStartOffset;
        double newStartTime       = m_dragStartStartTime + actualDelta;
        double newDuration        = m_dragStartDuration   - actualDelta;
        const double fileDur      = m_blocks[m_dragBlock].fileDur;

        // Clamp: startTime >= 0, duration >= 0.05, startTime stays within file
        if (newStartTime < 0.0) {
            const double adj = -newStartTime;
            newStartTime = 0.0;
            newDuration  += adj;
        }
        if (fileDur > 0.0 && newStartTime > fileDur - 0.05)
            newStartTime = fileDur - 0.05;
        newDuration = std::max(0.05, newDuration);

        m_blocks[m_dragBlock].offset    = m_dragStartOffset + (newStartTime - m_dragStartStartTime);
        m_blocks[m_dragBlock].startTime = newStartTime;
        m_blocks[m_dragBlock].duration  = newDuration;

    } else if (m_dragMode == DragMode::TrimRight) {
        double newDuration = std::max(0.05, m_dragStartDuration + deltaSec);
        if (m_mc && m_quantSubdiv > 0) {
            const double snappedEnd = snapToGrid(m_blocks[m_dragBlock].offset + newDuration);
            newDuration = std::max(0.05, snappedEnd - m_blocks[m_dragBlock].offset);
        }
        const double fileDur = m_blocks[m_dragBlock].fileDur;
        if (fileDur > 0.0)
            newDuration = std::min(newDuration, fileDur - m_blocks[m_dragBlock].startTime);
        m_blocks[m_dragBlock].duration = newDuration;
    }

    update();
}

void TimelineGroupView::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragBlock < 0) return;

    const int flatIdx = m_blocks[m_dragBlock].flatIdx;
    const DragMode mode = m_dragMode;
    const double newOff  = m_blocks[m_dragBlock].offset;
    const double newSt   = m_blocks[m_dragBlock].startTime;
    const double newDur  = m_blocks[m_dragBlock].duration;

    m_dragBlock = -1;
    m_dragMode  = DragMode::None;

    if (mode == DragMode::Move)
        emit childOffsetChanged(flatIdx, newOff);
    else if (mode == DragMode::TrimLeft || mode == DragMode::TrimRight)
        emit childTrimChanged(flatIdx, newOff, newSt, newDur);
}

void TimelineGroupView::wheelEvent(QWheelEvent* ev) {
    if (ev->modifiers() & Qt::ControlModifier) {
        const double steps = ev->angleDelta().y() / 120.0;
        const double pivotSec = pixToSec(ev->position().x());
        m_pixPerSec = std::clamp(m_pixPerSec * std::pow(1.25, steps), 4.0, 8000.0);
        m_viewStart = std::max(0.0, pivotSec - ev->position().x() / m_pixPerSec);
    } else if ((ev->modifiers() & Qt::ShiftModifier) || ev->angleDelta().x() != 0) {
        const double delta = ev->angleDelta().x() != 0
            ? ev->angleDelta().x() : ev->angleDelta().y();
        const double steps = delta / 120.0;
        const double viewWidth = width() > 0 ? width() / m_pixPerSec : 1.0;
        m_viewStart = std::max(0.0, m_viewStart - steps * viewWidth * 0.3);
    } else {
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

double TimelineGroupView::snapToGrid(double sec) const {
    if (!m_mc || m_quantSubdiv == 0) return sec;
    if (sec < 0.0) return sec;
    const auto   pos = m_mc->secondsToMusical(sec);
    const double qn  = m_mc->musicalToQN(pos.bar, pos.beat, pos.fraction);
    double snappedQN;
    if (m_quantSubdiv == 1) {
        const double qn0 = m_mc->musicalToQN(pos.bar, 1);
        const double qn1 = m_mc->musicalToQN(pos.bar + 1, 1);
        snappedQN = (qn - qn0 < qn1 - qn) ? qn0 : qn1;
    } else {
        const double grid = 4.0 / m_quantSubdiv;
        snappedQN = std::round(qn / grid) * grid;
    }
    const auto snapped = m_mc->qnToMusical(snappedQN);
    return m_mc->musicalToSeconds(snapped.bar, snapped.beat, snapped.fraction);
}

void TimelineGroupView::contextMenuEvent(QContextMenuEvent* ev) {
    if (!m_mc) { ev->accept(); return; }
    QMenu menu(this);
    QMenu* snapMenu = menu.addMenu("Snap to grid");
    struct { const char* label; int subdiv; } opts[] = {
        {"None", 0}, {"1/1 (bar)", 1}, {"1/2", 2}, {"1/4", 4},
        {"1/8", 8}, {"1/16", 16}, {"1/32", 32}
    };
    for (auto& o : opts) {
        auto* a = snapMenu->addAction(o.label);
        a->setCheckable(true);
        a->setChecked(m_quantSubdiv == o.subdiv);
        connect(a, &QAction::triggered, this,
                [this, s = o.subdiv] { m_quantSubdiv = s; });
    }
    menu.exec(ev->globalPos());
    ev->accept();
}
