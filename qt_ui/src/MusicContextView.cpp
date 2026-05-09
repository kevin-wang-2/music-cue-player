#include "MusicContextView.h"
#include "AppModel.h"

#include "engine/CueList.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// ── helpers ──────────────────────────────────────────────────────────────────

static const QColor kTSColors[] = {
    QColor(0x1a, 0x38, 0x58),
    QColor(0x18, 0x48, 0x28),
    QColor(0x48, 0x22, 0x12),
    QColor(0x3e, 0x34, 0x0e),
    QColor(0x28, 0x16, 0x48),
};

// ── ctor ──────────────────────────────────────────────────────────────────────

MusicContextView::MusicContextView(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setStyleSheet("background:#111;");
    setMinimumHeight(totalH());
}

// ── coordinate helpers ────────────────────────────────────────────────────────

mcp::MusicContext* MusicContextView::getMC() const {
    if (m_cueIdx < 0) return nullptr;
    const auto* c = m_model->cues().cueAt(m_cueIdx);
    return c ? m_model->cues().musicContextOf(m_cueIdx) : nullptr;
}

double MusicContextView::qnToX(double qn) const {
    return (qn - m_viewStartQN) * m_pixPerQN;
}
double MusicContextView::xToQN(double x) const {
    return x / m_pixPerQN + m_viewStartQN;
}
int MusicContextView::bpmToY(double bpm, BpmRange r) const {
    const double t = (r.hi - bpm) / (r.hi - r.lo);
    return tempoTop() + static_cast<int>(t * kTempoH);
}
double MusicContextView::yToBpm(double y, BpmRange r) const {
    const double t = (y - tempoTop()) / kTempoH;
    return r.hi - t * (r.hi - r.lo);
}

MusicContextView::BpmRange MusicContextView::computeBpmRange() const {
    auto* mc = getMC();
    if (!mc || mc->points.empty()) return {60.0, 180.0};
    double mn = mc->points[0].bpm, mx = mn;
    for (const auto& pt : mc->points) { mn = std::min(mn, pt.bpm); mx = std::max(mx, pt.bpm); }
    const double mid   = (mn + mx) / 2.0;
    const double range = std::max(60.0, (mx - mn) * 1.6 + 20.0);
    return {mid - range / 2.0, mid + range / 2.0};
}

// ── public API ────────────────────────────────────────────────────────────────

QSize MusicContextView::sizeHint()    const { return {600, totalH()}; }
QSize MusicContextView::minimumSizeHint() const { return {200, totalH()}; }

void MusicContextView::setCueIndex(int idx) {
    m_cueIdx = idx;
    m_selPt  = -1;
    m_hovPt  = -1;
    fitView();
    update();
}

// ── view helpers ──────────────────────────────────────────────────────────────

void MusicContextView::fitView() {
    auto* mc = getMC();
    if (!mc || mc->points.empty()) {
        m_viewStartQN = 0.0;
        m_pixPerQN    = (width() > 0) ? width() / 16.0 : 40.0;
        return;
    }
    // Earliest bar is the first point's bar — never show bars before it.
    const int firstBar = mc->points.front().bar;
    const double floorQN = mc->musicalToQN(firstBar, 1);

    double minQN = floorQN, maxQN = floorQN;
    for (const auto& pt : mc->points) {
        const double qn = mc->musicalToQN(pt.bar, pt.beat);
        minQN = std::min(minQN, qn);
        maxQN = std::max(maxQN, qn);
    }
    // Small left margin (half a beat) so the first bar line isn't flush with the edge.
    minQN -= 0.5;
    maxQN += 4.0;
    const double W = std::max(1, width());
    m_pixPerQN    = W / (maxQN - minQN);
    m_viewStartQN = minQN;
}

double MusicContextView::snapQN(double qn) const {
    if (m_quantSubdiv == 0) return qn;
    auto* mc = getMC();
    if (!mc) return qn;
    if (m_quantSubdiv == 1) {
        // Snap to nearest bar
        auto pos   = mc->qnToMusical(qn);
        double qn0 = mc->musicalToQN(pos.bar, 1);
        double qn1 = mc->musicalToQN(pos.bar + 1, 1);
        return (qn - qn0 < qn1 - qn) ? qn0 : qn1;
    }
    const double grid = 4.0 / m_quantSubdiv;
    return std::round(qn / grid) * grid;
}

int MusicContextView::hitTestPoint(int px, int py) const {
    auto* mc = getMC();
    if (!mc) return -1;
    const auto r = computeBpmRange();
    const int N  = (int)mc->points.size();
    for (int i = 0; i < N; i++) {
        const auto& pt = mc->points[i];
        const int x = (int)qnToX(mc->musicalToQN(pt.bar, pt.beat));
        // Check in tempo track
        if (py >= tempoTop() && py < tempoTop() + kTempoH) {
            const int y = bpmToY(pt.bpm, r);
            if ((px-x)*(px-x) + (py-y)*(py-y) <= kPtRad*kPtRad*4) return i;
        }
        // Check in TS track (horizontal proximity only)
        if (py >= tsTop() && py < tsTop() + kTSH) {
            if (std::abs(px - x) <= kPtRad + 2) return i;
        }
    }
    return -1;
}

// Returns the index i such that a new point should be inserted at points[i+1].
// Returns N-1 ("after last") when the click is to the right of the last point.
// Returns -1 for no hit (wrong y-band or left of first point).
int MusicContextView::hitTestSegment(int px, int py) const {
    auto* mc = getMC();
    if (!mc || mc->points.empty()) return -1;
    if (py < tempoTop() || py >= tempoTop() + kTempoH) return -1;
    const auto r = computeBpmRange();
    const int N  = (int)mc->points.size();

    // Segments between consecutive points
    for (int i = 0; i + 1 < N; i++) {
        const auto& p0 = mc->points[i];
        const auto& p1 = mc->points[i + 1];
        const double qn0 = mc->musicalToQN(p0.bar, p0.beat);
        const double qn1 = mc->musicalToQN(p1.bar, p1.beat);
        const int x0 = (int)qnToX(qn0);
        const int x1 = (int)qnToX(qn1);
        if (px < x0 - kPtRad || px > x1 + kPtRad) continue;
        const double t   = (x1 > x0) ? (double)(px - x0) / (x1 - x0) : 0.0;
        const double bpm = p1.isRamp ? (p0.bpm + (p1.bpm - p0.bpm) * t) : p0.bpm;
        if (std::abs(py - bpmToY(bpm, r)) <= 8) return i;
    }

    // Right extrapolation: to the right of the last point
    {
        const auto& last = mc->points[N - 1];
        const int xL = (int)qnToX(mc->musicalToQN(last.bar, last.beat));
        if (px > xL && std::abs(py - bpmToY(last.bpm, r)) <= 8)
            return N - 1;
    }
    return -1;
}

// ── painting ──────────────────────────────────────────────────────────────────

void MusicContextView::paintRuler(QPainter& p) const {
    auto* mc = getMC();
    const int W = width();
    p.fillRect(0, 0, W, kRulerH, QColor(0x1a, 0x1a, 0x1a));
    p.setPen(QColor(0x44, 0x44, 0x44));
    p.drawLine(0, kRulerH - 1, W, kRulerH - 1);
    if (!mc) return;

    // Never render bars before the first defined point.
    const int firstBar = mc->points.empty() ? 1 : mc->points.front().bar;
    const auto startPos = mc->qnToMusical(m_viewStartQN);
    int bar = std::max(startPos.bar - 1, firstBar);
    int lastLabelX = -100;
    p.setPen(QColor(0x88, 0x88, 0x88));

    for (int safety = 0; safety < 4000; safety++, bar++) {
        const double barQN = mc->musicalToQN(bar, 1);
        const int bx = (int)qnToX(barQN);
        if (bx > W) break;
        if (bx >= -2) {
            // Taller tick for bar lines
            p.drawLine(bx, kRulerH - 8, bx, kRulerH);
            if (bx - lastLabelX >= 36) {
                p.drawText(bx + 2, 1, 54, kRulerH - 2,
                           Qt::AlignLeft | Qt::AlignVCenter, QString::number(bar));
                lastLabelX = bx;
            }
        }
        // Beat subdivision ticks
        const auto ts = mc->timeSigAt(bar, 1);
        for (int beat = 2; beat <= ts.num; beat++) {
            const int tx = (int)qnToX(mc->musicalToQN(bar, beat));
            if (tx < 0 || tx > W) continue;
            p.drawLine(tx, kRulerH - 4, tx, kRulerH);
        }
    }
}

void MusicContextView::paintTempoTrack(QPainter& p, BpmRange r) const {
    auto* mc = getMC();
    if (!mc || mc->points.empty()) return;

    const int W   = width();
    const int top = tempoTop();
    p.fillRect(0, top, W, kTempoH, QColor(0x13, 0x13, 0x13));

    // BPM grid lines (every ~20 BPM)
    {
        p.setPen(QPen(QColor(0x2a, 0x2a, 0x2a)));
        const double step = 20.0;
        const double first = std::ceil(r.lo / step) * step;
        for (double bpm = first; bpm <= r.hi; bpm += step) {
            const int y = bpmToY(bpm, r);
            if (y >= top && y < top + kTempoH) p.drawLine(0, y, W, y);
        }
    }

    const int N = (int)mc->points.size();

    // Curve segments
    p.setPen(QPen(QColor(0x44, 0xaa, 0xff), 1.5));

    // Extrapolate left of first point
    {
        const int x0 = (int)qnToX(mc->musicalToQN(mc->points[0].bar, mc->points[0].beat));
        if (x0 > 0) p.drawLine(0, bpmToY(mc->points[0].bpm, r), x0, bpmToY(mc->points[0].bpm, r));
    }

    for (int i = 0; i + 1 < N; i++) {
        const auto& pt0 = mc->points[i];
        const auto& pt1 = mc->points[i + 1];
        const int x0 = (int)qnToX(mc->musicalToQN(pt0.bar, pt0.beat));
        const int x1 = (int)qnToX(mc->musicalToQN(pt1.bar, pt1.beat));
        if (x1 < 0 || x0 > W) continue;
        const int y0 = bpmToY(pt0.bpm, r);
        const int y1 = bpmToY(pt1.bpm, r);
        if (pt1.isRamp) {
            p.drawLine(x0, y0, x1, y1);
        } else {
            p.drawLine(x0, y0, x1, y0);
            if (y0 != y1) p.drawLine(x1, y0, x1, y1);
        }
    }

    // Extrapolate right of last point
    {
        const auto& last = mc->points[N - 1];
        const int xL = (int)qnToX(mc->musicalToQN(last.bar, last.beat));
        if (xL < W) p.drawLine(xL, bpmToY(last.bpm, r), W, bpmToY(last.bpm, r));
    }

    // Point circles
    for (int i = 0; i < N; i++) {
        const auto& pt = mc->points[i];
        const int x = (int)qnToX(mc->musicalToQN(pt.bar, pt.beat));
        if (x < -kPtRad || x > W + kPtRad) continue;
        const int y = bpmToY(pt.bpm, r);
        const bool isSel = (i == m_selPt);
        const bool isHov = (i == m_hovPt);
        if (isSel) {
            p.setBrush(QColor(0xaa, 0xdd, 0xff));
            p.setPen(QPen(Qt::white, 1.5));
        } else if (isHov) {
            p.setBrush(QColor(0x55, 0xaa, 0xdd));
            p.setPen(QPen(QColor(0x88, 0xcc, 0xff)));
        } else {
            p.setBrush(QColor(0x22, 0x66, 0xaa));
            p.setPen(QPen(QColor(0x44, 0x88, 0xcc)));
        }
        p.drawEllipse(x - kPtRad, y - kPtRad, kPtRad * 2, kPtRad * 2);

        // BPM label near point
        if (isSel || isHov) {
            p.setPen(QColor(0xdd, 0xdd, 0xdd));
            p.drawText(x + kPtRad + 2, y - 8, 60, 16,
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(pt.bpm, 'f', 1));
        }
    }

    // Track label
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.drawText(4, top + 2, 60, 14, Qt::AlignLeft, "Tempo");
}

void MusicContextView::paintTSTrack(QPainter& p) const {
    auto* mc = getMC();
    if (!mc || mc->points.empty()) return;

    const int W   = width();
    const int top = tsTop();
    p.fillRect(0, top, W, kTSH, QColor(0x18, 0x18, 0x20));
    p.setPen(QColor(0x3a, 0x3a, 0x3a));
    p.drawLine(0, top, W, top);

    // Build list of TS-change events
    struct TSSeg { double startQN; int num; int den; };
    std::vector<TSSeg> segs;
    for (const auto& pt : mc->points) {
        if (pt.hasTimeSig) {
            segs.push_back({mc->musicalToQN(pt.bar, pt.beat), pt.timeSigNum, pt.timeSigDen});
        }
    }
    if (segs.empty()) segs.push_back({0.0, 4, 4});
    // Sentinel at view right edge + padding
    const double endQN = m_viewStartQN + (double)W / m_pixPerQN + 16.0;
    segs.push_back({endQN, 0, 0});

    for (int i = 0; i + 1 < (int)segs.size(); i++) {
        const int x0 = std::max(0, (int)qnToX(segs[i].startQN));
        const int x1 = std::min(W, (int)qnToX(segs[i + 1].startQN));
        if (x1 <= x0) continue;
        const int ci = ((segs[i].num * 7) + (segs[i].den * 13)) % 5;
        p.fillRect(x0, top, x1 - x0, kTSH, kTSColors[ci]);
        if (x1 - x0 >= 22) {
            p.setPen(QColor(0xcc, 0xcc, 0xcc));
            p.drawText(x0 + 4, top, x1 - x0 - 4, kTSH,
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString("%1/%2").arg(segs[i].num).arg(segs[i].den));
        }
    }

    // Point markers in TS track (small triangles at change positions)
    for (int i = 0; i < (int)mc->points.size(); i++) {
        const auto& pt = mc->points[i];
        if (!pt.hasTimeSig && i > 0) continue;
        const int x = (int)qnToX(mc->musicalToQN(pt.bar, pt.beat));
        if (x < 0 || x > W) continue;
        p.setPen((i == m_selPt) ? QPen(Qt::white, 1.5) : QPen(QColor(0xcc, 0xcc, 0xcc)));
        p.drawLine(x, top, x, top + kTSH);
    }
}

void MusicContextView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    auto* mc = getMC();
    if (!mc || mc->points.empty()) {
        p.fillRect(rect(), QColor(0x11, 0x11, 0x11));
        p.setPen(QColor(0x44, 0x44, 0x44));
        p.drawText(rect(), Qt::AlignCenter, "No tempo map");
        return;
    }

    const auto r = computeBpmRange();
    paintRuler(p);
    paintTempoTrack(p, r);
    paintTSTrack(p);
}

// ── mouse ────────────────────────────────────────────────────────────────────

void MusicContextView::mousePressEvent(QMouseEvent* ev) {
    auto* mc = getMC();
    if (!mc) return;

    if (ev->button() != Qt::LeftButton) return;
    {
        const int px = (int)ev->position().x();
        const int py = (int)ev->position().y();

        int hit = hitTestPoint(px, py);
        if (hit >= 0) {
            if (m_selPt != hit) { m_selPt = hit; emit pointSelected(m_selPt); }
            const auto& pt = mc->points[hit];
            m_dragging    = true;
            m_didMove     = false;
            m_dragPt      = hit;
            m_dragPressX  = ev->position().x();
            m_dragPressY  = ev->position().y();
            m_dragOrigQN  = mc->musicalToQN(pt.bar, pt.beat);
            m_dragOrigBpm = pt.bpm;
            m_dragHorz    = (hit > 0);
            m_dragVert    = (py >= tempoTop() && py < tempoTop() + kTempoH);
            m_model->pushUndo();
            update();
            return;
        }

        // Click on segment or right extrapolation → insert point
        int seg = hitTestSegment(px, py);
        if (seg >= 0) {
            const double rawQN = xToQN(px);
            const auto& p0 = mc->points[seg];
            const double qn0 = mc->musicalToQN(p0.bar, p0.beat);
            const bool afterLast = (seg + 1 >= (int)mc->points.size());

            // Clamp new position to be strictly after p0 (min 1 beat = 1 QN gap)
            const double minQN = qn0 + 1.0;
            double newQN = snapQN(rawQN);
            if (newQN < minQN) newQN = minQN;

            double bpm = p0.bpm;
            if (!afterLast) {
                const auto& p1 = mc->points[seg + 1];
                const double qn1 = mc->musicalToQN(p1.bar, p1.beat);
                const double t = (qn1 > qn0)
                    ? std::clamp((newQN - qn0) / (qn1 - qn0), 0.0, 1.0) : 0.0;
                bpm = p1.isRamp ? (p0.bpm + (p1.bpm - p0.bpm) * t) : p0.bpm;
            }
            const auto   pos = mc->qnToMusical(newQN);

            mcp::MusicContext::Point np;
            np.bar = pos.bar; np.beat = pos.beat;
            np.bpm = bpm; np.isRamp = false; np.hasTimeSig = false;

            m_model->pushUndo();
            mc->points.insert(mc->points.begin() + seg + 1, np);
            mc->markDirty();
            m_selPt = seg + 1;
            emit pointSelected(m_selPt);
            emit mcChanged();
            update();
            return;
        }

        // Empty click → deselect
        if (m_selPt >= 0) { m_selPt = -1; emit pointSelected(-1); update(); }
    }
}

void MusicContextView::mouseMoveEvent(QMouseEvent* ev) {
    auto* mc = getMC();
    if (!mc) return;

    if (m_dragging && m_dragPt < (int)mc->points.size()) {
        auto& pt      = mc->points[m_dragPt];
        const auto r  = computeBpmRange();

        if (m_dragHorz) {
            const double newQN = snapQN(m_dragOrigQN + (ev->position().x() - m_dragPressX) / m_pixPerQN);
            const int prev = m_dragPt - 1;
            const int next = m_dragPt + 1;
            const double lo = (prev >= 0)
                ? mc->musicalToQN(mc->points[prev].bar, mc->points[prev].beat) + 0.25 : -1e9;
            const double hi = (next < (int)mc->points.size())
                ? mc->musicalToQN(mc->points[next].bar, mc->points[next].beat) - 0.25 :  1e9;
            const auto np = mc->qnToMusical(std::clamp(newQN, lo, hi));
            pt.bar = np.bar; pt.beat = np.beat;
        }
        if (m_dragVert) {
            const double dy  = ev->position().y() - m_dragPressY;
            pt.bpm = std::clamp(m_dragOrigBpm - dy * (r.hi - r.lo) / kTempoH, 10.0, 999.0);
        }
        mc->markDirty();
        m_didMove = true;
        update();
        return;
    }

    // Hover
    const int newHov = hitTestPoint((int)ev->position().x(), (int)ev->position().y());
    if (newHov != m_hovPt) {
        m_hovPt = newHov;
        setCursor(m_hovPt >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        update();
    }
}

void MusicContextView::mouseReleaseEvent(QMouseEvent* ev) {
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        if (m_didMove) emit mcChanged();
        m_didMove = false;
    }
}

void MusicContextView::wheelEvent(QWheelEvent* ev) {
    if (ev->modifiers() & Qt::ControlModifier) {
        // Cmd+scroll → zoom around mouse position
        // factor<1 when steps>0: new pixPerQN = old/factor > old → zoom in
        const double steps   = ev->angleDelta().y() / 120.0;
        const double factor  = std::pow(0.8, steps);
        const double mouseQN = xToQN(ev->position().x());
        m_pixPerQN    = std::clamp(m_pixPerQN / factor, 2.0, 600.0);
        // invariant: mouseQN stays at the same screen x
        // newViewStart = mouseQN - (mouseQN - oldViewStart) * (oldPPQ / newPPQ)
        //             = mouseQN - (mouseQN - oldViewStart) * factor
        m_viewStartQN = mouseQN - (mouseQN - m_viewStartQN) * factor;
    } else if ((ev->modifiers() & Qt::ShiftModifier) || ev->angleDelta().x() != 0) {
        // Shift+scroll → horizontal pan
        // macOS trackpad redirects Shift+vertical scroll to angleDelta.x
        const double delta = ev->angleDelta().x() != 0
            ? ev->angleDelta().x() : ev->angleDelta().y();
        const double steps = delta / 120.0;
        m_viewStartQN -= steps * (width() / m_pixPerQN) * 0.08;
    } else {
        // Plain scroll → zoom
        const double steps   = ev->angleDelta().y() / 120.0;
        const double factor  = std::pow(0.8, steps);
        const double mouseQN = xToQN(ev->position().x());
        m_pixPerQN    = std::clamp(m_pixPerQN / factor, 2.0, 600.0);
        m_viewStartQN = mouseQN - (mouseQN - m_viewStartQN) * factor;
    }

    // Hard left limit: never scroll past (firstBar - 0.5 QN) so the first
    // bar line always stays reachable.
    if (const auto* mc = getMC(); mc && !mc->points.empty()) {
        const double floor = mc->musicalToQN(mc->points.front().bar, 1) - 0.5;
        m_viewStartQN = std::max(m_viewStartQN, floor);
    }

    ev->accept();
    update();
}

void MusicContextView::contextMenuEvent(QContextMenuEvent* ev) {
    auto* mc = getMC();
    if (!mc) return;
    const int hit = hitTestPoint(ev->pos().x(), ev->pos().y());
    if (hit <= 0) {
        // Right-click with no point: quantization menu
        QMenu menu(this);
        menu.addSection("Edit Quantization");
        struct { const char* label; int subdiv; } opts[] = {
            {"1/1 (bar)", 1}, {"1/2", 2}, {"1/4", 4},
            {"1/8", 8}, {"1/16", 16}, {"1/32", 32}, {"None", 0}
        };
        for (auto& o : opts) {
            auto* a = menu.addAction(o.label);
            a->setCheckable(true);
            a->setChecked(m_quantSubdiv == o.subdiv);
            connect(a, &QAction::triggered, this, [this, s=o.subdiv]{ m_quantSubdiv = s; });
        }
        menu.exec(ev->globalPos());
        return;
    }
    // Right-click on non-first point
    QMenu menu(this);
    auto* actDel = menu.addAction("Delete point");
    if (menu.exec(ev->globalPos()) == actDel) {
        m_model->pushUndo();
        mc->points.erase(mc->points.begin() + hit);
        mc->markDirty();
        if (m_selPt >= (int)mc->points.size() || m_selPt == hit) m_selPt = -1;
        emit pointSelected(m_selPt);
        emit mcChanged();
        update();
    }
}

void MusicContextView::leaveEvent(QEvent*) {
    if (m_hovPt >= 0) { m_hovPt = -1; update(); }
}

void MusicContextView::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
        auto* mc = getMC();
        if (mc && m_selPt > 0 && m_selPt < (int)mc->points.size()) {
            m_model->pushUndo();
            mc->points.erase(mc->points.begin() + m_selPt);
            mc->markDirty();
            m_selPt = -1;
            emit pointSelected(-1);
            emit mcChanged();
            update();
            return;
        }
    }
    QWidget::keyPressEvent(ev);
}

void MusicContextView::resizeEvent(QResizeEvent*) {
    update();
}
