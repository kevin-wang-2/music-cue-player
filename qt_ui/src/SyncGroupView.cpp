#include "SyncGroupView.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include "engine/AudioDecoder.h"
#include "engine/Cue.h"

#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <thread>

static constexpr double kMinViewSec   = 5.0;
static constexpr double kSnapThreshPx = 8.0;

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

SyncGroupView::SyncGroupView(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setMinimumHeight(kRulerH + kTopPad + kBlockH + kLoopH + 8);
    setMouseTracking(true);
    setStyleSheet("background:#111;");
}

void SyncGroupView::setMusicContext(const mcp::MusicContext* mc) {
    m_mc = mc;
    update();
}

void SyncGroupView::setGroupCueIndex(int groupFlatIdx) {
    const bool groupChanged = (groupFlatIdx != m_groupIdx);
    m_groupIdx     = groupFlatIdx;
    m_dragBlock    = -1;
    m_dragMode     = DragMode::None;
    m_dragMarker   = -2;
    m_selBlock     = -1;
    m_selMarker    = -1;   // reset silently; inspector hides the panel itself
    rebuildBlocks();
    if (groupChanged) {
        m_laneScrollPx = 0;
        m_viewStart    = 0.0;
        const double dur = viewDuration();
        if (dur > 0.0 && width() > 0)
            m_pixPerSec = width() / dur;
    }
    updateGeometry();
    update();
}

void SyncGroupView::clearArmCursor() {
    m_armSec = -1.0;
    update();
}

void SyncGroupView::clearSelMarker() {
    if (m_selMarker >= 0) {
        m_selMarker = -1;
        emit markerSelected(-1);
        update();
    }
}

QSize SyncGroupView::sizeHint() const {
    const int n = std::max(1, (int)m_blocks.size());
    return { width(), kRulerH + kTopPad + n * (kBlockH + kLaneGap) + kLoopH + 4 };
}

int SyncGroupView::laneY(int i) const {
    return kRulerH + kTopPad + i * (kBlockH + kLaneGap) - m_laneScrollPx;
}

double SyncGroupView::viewDuration() const {
    double maxEnd = kMinViewSec;
    if (m_groupIdx >= 0) {
        const double base = m_model->cues().syncGroupBaseDuration(m_groupIdx);
        if (base > 0.0) maxEnd = std::max(maxEnd, base);
        const mcp::Cue* g = m_model->cues().cueAt(m_groupIdx);
        if (g) for (const auto& mk : g->markers)
            maxEnd = std::max(maxEnd, mk.time + 0.5);
    }
    for (const auto& b : m_blocks)
        maxEnd = std::max(maxEnd, b.offset + b.duration);
    return maxEnd * 1.2;
}

double SyncGroupView::pixToSec(double px) const {
    return m_viewStart + px / m_pixPerSec;
}

int SyncGroupView::secToPix(double sec) const {
    return static_cast<int>((sec - m_viewStart) * m_pixPerSec);
}

// ── Block rebuild ─────────────────────────────────────────────────────────────

void SyncGroupView::rebuildBlocks() {
    m_blocks.clear();
    if (m_groupIdx < 0) return;
    const mcp::Cue* group = m_model->cues().cueAt(m_groupIdx);
    if (!group || group->type != mcp::CueType::Group) return;

    for (int i = m_groupIdx + 1; i <= m_groupIdx + group->childCount; ) {
        const mcp::Cue* c = m_model->cues().cueAt(i);
        if (!c) { ++i; continue; }
        if (c->parentIndex == m_groupIdx) {
            ChildBlock b;
            b.flatIdx   = i;
            b.offset    = c->timelineOffset;
            b.startTime = c->startTime;
            b.fileDur   = (c->type == mcp::CueType::Audio)
                          ? m_model->cues().cueTotalSeconds(i) : 0.0;
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

void SyncGroupView::buildPeaksAsync(const std::string& path) {
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

// ── Paint ─────────────────────────────────────────────────────────────────────

void SyncGroupView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int W = width();
    const int H = height();
    const int loopStripY = H - kLoopH;

    const mcp::Cue* group = (m_groupIdx >= 0) ? m_model->cues().cueAt(m_groupIdx) : nullptr;
    const double baseDur  = (m_groupIdx >= 0)
        ? m_model->cues().syncGroupBaseDuration(m_groupIdx) : 0.0;

    // ── Ruler ──────────────────────────────────────────────────────────────
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
        // Seconds ruler
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

    // ── Slice tint (alternating bands between markers) ─────────────────────
    if (group && baseDur > 0.0) {
        std::vector<double> bounds { 0.0 };
        for (const auto& mk : group->markers) bounds.push_back(mk.time);
        bounds.push_back(baseDur);
        const int laneTop = kRulerH + kTopPad;
        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            if (bi % 2 != 1) continue;
            const int bx  = secToPix(bounds[bi]);
            const int bex = secToPix(bounds[bi + 1]);
            p.fillRect(std::max(0, bx), laneTop,
                       std::min(W, bex) - std::max(0, bx),
                       loopStripY - laneTop,
                       QColor(255, 255, 255, 10));
        }
    }

    // ── Child-cue lanes ────────────────────────────────────────────────────
    p.setClipRect(0, kRulerH, W, loopStripY - kRulerH);
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const auto& b = m_blocks[i];
        const int ly  = laneY(i);
        if (ly + kBlockH > loopStripY) break;

        const int x1 = secToPix(b.offset);
        const int x2 = secToPix(b.offset + b.duration);
        const int bw = std::max(4, x2 - x1);

        p.fillRect(0, ly, W, kBlockH, QColor(0x18, 0x18, 0x18));

        const bool  isDrag  = (i == m_dragBlock);
        const bool  isSel   = (i == m_selBlock);
        QColor fill, border;
        if (isDrag)     { fill = QColor(0x44,0x88,0xdd); border = QColor(0x88,0xcc,0xff); }
        else if (isSel) { fill = QColor(0x30,0x70,0xc0); border = QColor(0x66,0xaa,0xff); }
        else            { fill = QColor(0x28,0x60,0xa8); border = QColor(0x44,0x88,0xcc); }
        p.fillRect(x1, ly, bw, kBlockH, fill);

        // Mini waveform thumbnail (below label area)
        if (!b.audioPath.empty()) {
            auto it = m_peakCache.find(b.audioPath);
            if (it != m_peakCache.end() && it->second.valid && it->second.fileDur > 0.0) {
                const PeakCache& pk = it->second;
                const int nBuckets = (int)pk.minPk[0].size();
                const int wTop = ly + 14, wBot = ly + kBlockH - 3;
                const int mid  = (wTop + wBot) / 2;
                const int half = (wBot - wTop) / 2;
                p.setPen(QPen(QColor(0xaa, 0xdd, 0xff, 0xb0), 1));
                for (int px2 = std::max(0, x1); px2 < std::min(W, x1 + bw); ++px2) {
                    const double t  = b.startTime + (double)(px2 - x1) / bw * b.duration;
                    const int bi    = std::clamp((int)(t / pk.fileDur * nBuckets), 0, nBuckets - 1);
                    p.drawLine(px2, std::max(wTop, mid - (int)(pk.maxPk[0][bi] * half)),
                               px2, std::min(wBot, mid - (int)(pk.minPk[0][bi] * half)));
                }
            }
        }

        // Trim handles
        p.fillRect(x1,            ly, kHandleW, kBlockH, QColor(255,255,255,25));
        p.fillRect(x1+bw-kHandleW, ly, kHandleW, kBlockH, QColor(255,255,255,25));

        p.setPen(border);
        p.drawRect(x1, ly, bw - 1, kBlockH - 1);

        // Sticky label + prewait sub-label
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

    // ── Group-level markers (amber triangles on ruler) ─────────────────────
    if (group) {
        for (int mi = 0; mi < (int)group->markers.size(); ++mi) {
            const int mx = secToPix(group->markers[mi].time);
            if (mx < 0 || mx >= W) continue;
            const bool sel = (m_selMarker == mi);
            const QColor mcol = sel ? QColor(255, 220, 60) : QColor(220, 160, 40);
            // Dashed vertical line through lanes
            p.setPen(QColor(mcol.red(), mcol.green(), mcol.blue(), 140));
            for (int dy = kRulerH; dy < loopStripY; dy += 5)
                p.drawLine(mx, dy, mx, std::min(dy + 3, loopStripY));
            // Triangle handle on ruler
            QPolygon tri;
            tri << QPoint(mx, kRulerH)
                << QPoint(mx - 5, 3)
                << QPoint(mx + 5, 3);
            p.setBrush(mcol);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            // Index badge
            p.setPen(QColor(255, 220, 80, 180));
            p.drawText(mx + 3, 3, 20, kRulerH - 3, Qt::AlignLeft, QString::number(mi + 1));
        }
    }

    // ── Arm cursor ─────────────────────────────────────────────────────────
    if (m_armSec >= 0.0) {
        const int ax = secToPix(m_armSec);
        if (ax >= 0 && ax < W) {
            p.setPen(QPen(QColor(0x40, 0xee, 0x80, 0xc0), 1));
            p.drawLine(ax, kRulerH, ax, loopStripY);
            QPolygon tri;
            tri << QPoint(ax, kRulerH)
                << QPoint(ax - 5, 4)
                << QPoint(ax + 5, 4);
            p.setBrush(QColor(0x40, 0xee, 0x80, 0xc0));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }

    // ── Loop count strip ───────────────────────────────────────────────────
    p.fillRect(0, loopStripY, W, kLoopH, QColor(14, 16, 24));
    if (group && baseDur > 0.0) {
        std::vector<double> bounds { 0.0 };
        for (const auto& mk : group->markers) bounds.push_back(mk.time);
        bounds.push_back(baseDur);
        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            const int blX = std::max(0, secToPix(bounds[bi]));
            const int brX = std::min(W, secToPix(bounds[bi + 1]));
            if (brX <= blX + 2) continue;
            const int lc = (bi < (int)group->sliceLoops.size()) ? group->sliceLoops[bi] : 1;
            const QString lbl = (lc == 0)
                ? QString::fromUtf8("\xe2\x88\x9e") : QString::number(lc);
            const QColor col = (lc == 0)
                ? QColor(255, 160, 60, 240) : QColor(180, 200, 255, 200);
            p.setPen(col);
            p.drawText(blX, loopStripY, brX - blX, kLoopH,
                       Qt::AlignHCenter | Qt::AlignVCenter, lbl);
        }
    }
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void SyncGroupView::updateHoverCursor(int px, int py) {
    const int lsY = height() - kLoopH;
    for (int i = 0; i < (int)m_blocks.size(); ++i) {
        const int ly = laneY(i);
        if (ly + kBlockH <= kRulerH || ly >= lsY) continue;
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

void SyncGroupView::mousePressEvent(QMouseEvent* ev) {
    const int    px  = ev->pos().x();
    const int    py  = ev->pos().y();
    const int    lsY = height() - kLoopH;

    const mcp::Cue* group = (m_groupIdx >= 0) ? m_model->cues().cueAt(m_groupIdx) : nullptr;

    if (ev->button() == Qt::LeftButton) {
        // Hit-test marker triangles (ruler zone + small hit area below ruler)
        if (py < kRulerH + 12 && group) {
            for (int mi = 0; mi < (int)group->markers.size(); ++mi) {
                if (std::abs(secToPix(group->markers[mi].time) - px) < kSnapThreshPx) {
                    m_model->pushUndo();
                    m_dragMarker       = mi;
                    m_dragMarkerOrig   = group->markers[mi].time;
                    m_dragMarkerPxOrig = px;
                    if (m_selMarker != mi) {
                        m_selMarker = mi;
                        emit markerSelected(mi);
                    }
                    m_armSec = group->markers[mi].time;
                    update();
                    emit rulerClicked(m_armSec);
                    ev->accept(); return;
                }
            }
        }

        // Ruler click (no marker hit) → set arm cursor (snapped to grid)
        if (py < kRulerH) {
            m_armSec = snapToGrid(std::max(0.0, pixToSec(px)));
            update();
            emit rulerClicked(m_armSec);
            ev->accept(); return;
        }

        // Child block drag in lane area
        if (py >= kRulerH && py < lsY) {
            for (int i = 0; i < (int)m_blocks.size(); ++i) {
                const int ly = laneY(i);
                if (ly + kBlockH <= kRulerH) continue;
                if (ly + kBlockH > lsY) break;
                if (py < ly || py >= ly + kBlockH) continue;
                const int x1 = secToPix(m_blocks[i].offset);
                const int x2 = secToPix(m_blocks[i].offset + m_blocks[i].duration);
                if (px < x1 || px > x2) continue;

                m_selBlock           = i;
                m_dragBlock          = i;
                m_dragStartX         = px;
                m_dragStartOffset    = m_blocks[i].offset;
                m_dragStartStartTime = m_blocks[i].startTime;
                m_dragStartDuration  = m_blocks[i].duration;

                if (px <= x1 + kHandleW)
                    m_dragMode = DragMode::TrimLeft;
                else if (px >= x2 - kHandleW)
                    m_dragMode = DragMode::TrimRight;
                else
                    m_dragMode = DragMode::Move;

                update();
                ev->accept(); return;
            }
        }

        // Clicking in empty area deselects marker
        if (m_selMarker >= 0) {
            m_selMarker = -1;
            emit markerSelected(-1);
            update();
        }

    } else if (ev->button() == Qt::RightButton) {
        m_panOriginX   = px;
        m_panViewStart = m_viewStart;
    }
    ev->accept();
}

void SyncGroupView::mouseMoveEvent(QMouseEvent* ev) {
    const int px = ev->pos().x();
    const int py = ev->pos().y();

    if (!(ev->buttons() & Qt::LeftButton) && !(ev->buttons() & Qt::RightButton))
        updateHoverCursor(px, py);

    if ((ev->buttons() & Qt::LeftButton) && m_dragMarker >= 0) {
        const double delta = static_cast<double>(px - m_dragMarkerPxOrig) / m_pixPerSec;
        double nt = std::max(0.001, m_dragMarkerOrig + delta);
        if (m_mc && m_quantSubdiv > 0) nt = snapToGrid(nt);
        const mcp::Cue* group = m_model->cues().cueAt(m_groupIdx);
        if (group) {
            const double lo = (m_dragMarker > 0)
                ? group->markers[m_dragMarker - 1].time + 0.001 : 0.001;
            const double baseDur = m_model->cues().syncGroupBaseDuration(m_groupIdx);
            const double hi = (m_dragMarker + 1 < (int)group->markers.size())
                ? group->markers[m_dragMarker + 1].time - 0.001
                : (baseDur > 0.0 ? baseDur - 0.001 : 99999.0);
            nt = std::clamp(nt, lo, hi);
            m_model->cues().setCueMarkerTime(m_groupIdx, m_dragMarker, nt);
            ShowHelpers::syncSfFromCues(*m_model);
            update();
        }
    }

    if ((ev->buttons() & Qt::LeftButton) && m_dragBlock >= 0) {
        const double delta = static_cast<double>(px - m_dragStartX) / m_pixPerSec;

        if (m_dragMode == DragMode::Move) {
            double newOffset = std::max(0.0, m_dragStartOffset + delta);
            if (m_mc && m_quantSubdiv > 0) newOffset = snapToGrid(newOffset);
            m_blocks[m_dragBlock].offset = newOffset;
        } else if (m_dragMode == DragMode::TrimLeft) {
            double newOffset   = std::max(0.0, m_dragStartOffset + delta);
            if (m_mc && m_quantSubdiv > 0) newOffset = snapToGrid(newOffset);
            const double actualDelta = newOffset - m_dragStartOffset;
            double newStartTime      = m_dragStartStartTime + actualDelta;
            double newDuration       = m_dragStartDuration   - actualDelta;
            const double fileDur     = m_blocks[m_dragBlock].fileDur;
            if (newStartTime < 0.0) { newDuration += -newStartTime; newStartTime = 0.0; }
            if (fileDur > 0.0 && newStartTime > fileDur - 0.05) newStartTime = fileDur - 0.05;
            newDuration = std::max(0.05, newDuration);
            m_blocks[m_dragBlock].offset    = m_dragStartOffset + (newStartTime - m_dragStartStartTime);
            m_blocks[m_dragBlock].startTime = newStartTime;
            m_blocks[m_dragBlock].duration  = newDuration;
        } else if (m_dragMode == DragMode::TrimRight) {
            double newDuration = std::max(0.05, m_dragStartDuration + delta);
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

    if ((ev->buttons() & Qt::RightButton)) {
        const double dx = static_cast<double>(px - m_panOriginX);
        if (std::abs(dx) > 2.0) {
            m_viewStart = std::max(0.0, m_panViewStart - dx / m_pixPerSec);
            update();
        }
    }

    ev->accept();
}

void SyncGroupView::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) {
        if (m_dragMarker >= 0) {
            const mcp::Cue* group = m_model->cues().cueAt(m_groupIdx);
            if (group && m_dragMarker < (int)group->markers.size())
                emit cueModified();
            m_dragMarker = -2;
            update();
        }
        if (m_dragBlock >= 0) {
            const int    flatIdx = m_blocks[m_dragBlock].flatIdx;
            const DragMode mode  = m_dragMode;
            const double newOff  = m_blocks[m_dragBlock].offset;
            const double newSt   = m_blocks[m_dragBlock].startTime;
            const double newDur  = m_blocks[m_dragBlock].duration;
            m_dragBlock = -1;
            m_dragMode  = DragMode::None;

            m_model->pushUndo();
            m_model->cues().setCueTimelineOffset(flatIdx, newOff);
            if (mode == DragMode::TrimLeft || mode == DragMode::TrimRight) {
                m_model->cues().setCueStartTime(flatIdx, newSt);
                m_model->cues().setCueDuration(flatIdx, newDur);
            }
            ShowHelpers::syncSfFromCues(*m_model);
            emit cueModified();
        }
    }
    ev->accept();
}

void SyncGroupView::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) { ev->accept(); return; }
    const int px = ev->pos().x();
    const int py = ev->pos().y();
    const int lsY = height() - kLoopH;

    if (py >= lsY) {
        const mcp::Cue* group = (m_groupIdx >= 0) ? m_model->cues().cueAt(m_groupIdx) : nullptr;
        if (!group) { ev->accept(); return; }
        const double baseDur = m_model->cues().syncGroupBaseDuration(m_groupIdx);
        if (baseDur <= 0.0) { ev->accept(); return; }

        std::vector<double> bounds { 0.0 };
        for (const auto& mk : group->markers) bounds.push_back(mk.time);
        bounds.push_back(baseDur);
        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            const int blX = std::max(0, secToPix(bounds[bi]));
            const int brX = std::min(width(), secToPix(bounds[bi + 1]));
            if (px >= blX && px < brX && brX > blX + 4) {
                const int lc = (bi < (int)group->sliceLoops.size()) ? group->sliceLoops[bi] : 1;
                const QString cur = (lc == 0)
                    ? QString::fromUtf8("\xe2\x88\x9e") : QString::number(lc);
                startLoopEdit(bi, blX, brX, cur);
                ev->accept(); return;
            }
        }
    }
    ev->accept();
}

void SyncGroupView::wheelEvent(QWheelEvent* ev) {
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
        const int lsY    = height() - kLoopH;
        const int totalH = kTopPad + n * (kBlockH + kLaneGap);
        const int visH   = lsY - kRulerH;
        const int maxScroll = std::max(0, totalH - visH);
        m_laneScrollPx = std::clamp(
            (int)(m_laneScrollPx - steps * kBlockH), 0, maxScroll);
    }
    update();
    ev->accept();
}

void SyncGroupView::contextMenuEvent(QContextMenuEvent* ev) {
    // Suppress menu when right-drag pan covered significant distance.
    if (std::abs((double)ev->x() - m_panOriginX) > 4.0) { ev->accept(); return; }
    if (m_groupIdx < 0) { ev->accept(); return; }

    const double sec = pixToSec(ev->x());
    QMenu menu(this);

    // Delete marker if cursor is near one
    const mcp::Cue* group = m_model->cues().cueAt(m_groupIdx);
    int hitMarker = -1;
    if (group) {
        for (int mi = 0; mi < (int)group->markers.size(); ++mi) {
            if (std::abs(secToPix(group->markers[mi].time) - ev->x()) < kSnapThreshPx) {
                hitMarker = mi; break;
            }
        }
    }
    if (hitMarker >= 0) {
        menu.addAction(QString("Delete marker %1").arg(hitMarker + 1), [this, hitMarker]() {
            m_model->pushUndo();
            m_model->cues().removeCueMarker(m_groupIdx, hitMarker);
            ShowHelpers::syncSfFromCues(*m_model);
            if (m_selMarker == hitMarker) {
                m_selMarker = -1;
                emit markerSelected(-1);
            } else if (m_selMarker > hitMarker) {
                --m_selMarker;
            }
            emit cueModified();
            update();
        });
        menu.addSeparator();
    }

    const double snapSec = snapToGrid(sec);
    QString posLabel;
    if (m_mc) {
        const auto pos = m_mc->secondsToMusical(std::max(0.0, snapSec));
        posLabel = QString("%1 | %2").arg(pos.bar).arg(pos.beat);
    } else {
        posLabel = QString::fromStdString(ShowHelpers::fmtTime(snapSec));
    }
    menu.addAction(
        QString("Add marker at %1").arg(posLabel),
        [this, snapSec]() {
            m_model->pushUndo();
            m_model->cues().addCueMarker(m_groupIdx, snapSec);
            ShowHelpers::syncSfFromCues(*m_model);
            emit cueModified();
            update();
        });

    menu.addSeparator();

    if (m_mc) {
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
        menu.addSeparator();
    }

    menu.addAction("Fit view", [this]() {
        m_viewStart = 0.0;
        const double dur = viewDuration();
        if (dur > 0.0 && width() > 0) m_pixPerSec = width() / dur;
        update();
    });

    menu.exec(ev->globalPos());
    ev->accept();
}

void SyncGroupView::resizeEvent(QResizeEvent*) {
    const double dur = viewDuration();
    if (dur > 0.0 && width() > 0)
        m_pixPerSec = width() / dur;
}

double SyncGroupView::snapToGrid(double sec) const {
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

// ── Loop count inline editor ──────────────────────────────────────────────────

void SyncGroupView::startLoopEdit(int sliceIdx, int blX, int brX, const QString& current) {
    if (m_loopEdit) {
        m_loopEdit->blockSignals(true);
        delete m_loopEdit;
        m_loopEdit = nullptr;
    }
    const int ex = std::max(0, blX);
    const int ew = std::min(width(), brX) - ex;
    if (ew < 8) return;

    m_editLoopSlice = sliceIdx;
    m_loopEdit = new QLineEdit(this);
    m_loopEdit->setGeometry(ex, height() - kLoopH, ew, kLoopH);
    m_loopEdit->setText(current);
    m_loopEdit->selectAll();
    m_loopEdit->setAlignment(Qt::AlignHCenter);
    m_loopEdit->setStyleSheet(
        "QLineEdit { background:#1a2040; color:#ffffff; "
        "  border:1px solid #3a5aaa; border-radius:0; font-size:12px; }");
    m_loopEdit->show();
    m_loopEdit->setFocus();
    connect(m_loopEdit, &QLineEdit::editingFinished, this, &SyncGroupView::commitLoopEdit);
}

void SyncGroupView::commitLoopEdit() {
    if (!m_loopEdit) return;
    const QString raw       = m_loopEdit->text().trimmed();
    const int     sliceIdx  = m_editLoopSlice;
    m_loopEdit->blockSignals(true);
    m_loopEdit->deleteLater();
    m_loopEdit      = nullptr;
    m_editLoopSlice = -1;

    if (m_groupIdx < 0 || sliceIdx < 0) { update(); return; }
    const mcp::Cue* c = m_model->cues().cueAt(m_groupIdx);
    if (!c || sliceIdx >= (int)c->sliceLoops.size()) { update(); return; }

    int newLc;
    if (raw == QString::fromUtf8("\xe2\x88\x9e") || raw == "0") {
        newLc = 0;
    } else {
        bool ok = false;
        const int v = raw.toInt(&ok);
        newLc = (ok && v >= 1) ? v : 0;   // non-numeric or out-of-range → infinity
    }

    m_model->pushUndo();
    auto sl = c->sliceLoops;
    sl[sliceIdx] = newLc;
    m_model->cues().setCueSliceLoops(m_groupIdx, sl);
    ShowHelpers::syncSfFromCues(*m_model);
    emit cueModified();
    update();
}
