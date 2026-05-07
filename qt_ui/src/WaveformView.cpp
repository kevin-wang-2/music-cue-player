#include "WaveformView.h"
#include "AppModel.h"
#include "ShowHelpers.h"

#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

WaveformView::WaveformView(AppModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(120);
}

void WaveformView::setCueIndex(int idx) {
    if (m_cueIdx == idx) return;

    // Save zoom state for the cue we're leaving.
    if (m_cueIdx >= 0 && m_viewDur > 0.0)
        m_zoomState[m_cueIdx] = {m_viewStart, m_viewDur};

    m_cueIdx        = idx;
    m_armSec        = -1.0;
    m_drag          = -2;
    m_selMarker     = -1;
    m_editLoopSlice = -1;
    m_peaks.valid   = false;

    // Restore saved zoom for the new cue, or mark for fit-to-file on load.
    auto it = m_zoomState.find(idx);
    if (it != m_zoomState.end()) {
        m_viewStart = it->second.first;
        m_viewDur   = it->second.second;
    } else {
        m_viewStart = 0.0;
        m_viewDur   = 0.0;   // 0 = "fit to file once peaks arrive"
    }

    const mcp::Cue* c = m_model->cues.cueAt(idx);
    if (!c || c->type != mcp::CueType::Audio) { update(); return; }

    // Resolve absolute path
    auto p = std::filesystem::path(c->path);
    if (!p.is_absolute() && !m_model->baseDir.empty())
        p = std::filesystem::path(m_model->baseDir) / p;
    rebuildPeaks(p.string());
    update();
}

void WaveformView::updatePlayhead() {
    if (m_cueIdx >= 0 && m_model->cues.isCuePlaying(m_cueIdx))
        update();
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

double WaveformView::secToX(double t) const {
    if (m_viewDur <= 0) return 0;
    return (t - m_viewStart) / m_viewDur * width();
}

double WaveformView::xToSec(double x) const {
    if (m_viewDur <= 0) return 0;
    return m_viewStart + x / width() * m_viewDur;
}

QString WaveformView::fmtTick(double s) const {
    return QString::fromStdString(ShowHelpers::fmtTime(s));
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int W = width();
    const int H = height();
    const int waveH  = H - kRulerH - kLoopH;
    const int waveTop = kRulerH;
    const int waveBot = waveTop + waveH;

    // Ensure view bounds are sensible.
    // fileDur=0 while peaks are still loading — don't commit m_viewDur to a
    // placeholder value; the async callback will set it once data arrives.
    const double fileDur = m_peaks.valid ? m_peaks.fileDur : 0.0;
    if (fileDur > 0.0) {
        if (m_viewDur <= 0.0 || m_viewDur > fileDur)
            m_viewDur = fileDur;
        m_viewStart = std::clamp(m_viewStart, 0.0, std::max(0.0, fileDur - m_viewDur));
    }

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    const double startSec = c ? c->startTime : 0.0;
    const double endSec   = c
        ? ((c->duration > 0.0) ? c->startTime + c->duration : fileDur)
        : fileDur;

    // Background
    p.fillRect(rect(), QColor(18, 20, 30));

    // Slice tint (alternating)
    if (c) {
        std::vector<double> bounds;
        bounds.push_back(startSec);
        for (const auto& m : c->markers) bounds.push_back(m.time);
        bounds.push_back(endSec);
        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            if (bi % 2 == 1) {
                const int bx  = (int)secToX(bounds[bi]);
                const int bex = (int)secToX(bounds[bi + 1]);
                p.fillRect(std::max(0, bx), waveTop,
                           std::min(W, bex) - std::max(0, bx), waveH,
                           QColor(255, 255, 255, 12));
            }
        }
    }

    // Waveform
    if (m_peaks.valid && fileDur > 0.0) {
        const int nBuckets = (int)m_peaks.minPk[0].size();
        const int dispCh   = std::min(m_peaks.fileCh, 2);
        for (int ch = 0; ch < dispCh; ++ch) {
            const float chTop = waveTop + ch * (waveH / dispCh);
            const float chBot = chTop + waveH / dispCh;
            const float chMid = (chTop + chBot) * 0.5f;
            const float half  = (chBot - chTop) * 0.48f;
            // Centre line
            p.setPen(QColor(255, 255, 255, 20));
            p.drawLine(0, (int)chMid, W, (int)chMid);
            for (int px = 0; px < W; ++px) {
                const double tL = m_viewStart + px * m_viewDur / W;
                const double tR = m_viewStart + (px + 1) * m_viewDur / W;
                const int bL = std::clamp((int)(tL / fileDur * nBuckets), 0, nBuckets - 1);
                const int bR = std::clamp((int)(tR / fileDur * nBuckets), 0, nBuckets - 1);
                float mn = m_peaks.minPk[ch][bL];
                float mx = m_peaks.maxPk[ch][bL];
                for (int b = bL + 1; b <= bR; ++b) {
                    mn = std::min(mn, m_peaks.minPk[ch][b]);
                    mx = std::max(mx, m_peaks.maxPk[ch][b]);
                }
                const double tMid = (tL + tR) * 0.5;
                const bool active = (tMid >= startSec && tMid <= endSec);
                const QColor wfCol = active
                    ? QColor(100, 160, 220, 200)
                    : QColor(70, 70, 85, 100);
                p.setPen(wfCol);
                p.drawLine(px, (int)(chMid - mx * half),
                           px, (int)(chMid - mn * half));
            }
        }
    } else {
        p.setPen(QColor(255, 255, 255, 30));
        p.drawLine(0, waveTop + waveH / 2, W, waveTop + waveH / 2);
    }

    // Ruler
    p.fillRect(0, 0, W, kRulerH, QColor(20, 24, 36));
    {
        // Choose a tick interval that gives ~80px between ticks
        const double secPerPx = m_viewDur / W;
        const double rawStep  = secPerPx * 80.0;
        const double pow10    = std::pow(10.0, std::floor(std::log10(rawStep)));
        const double frac     = rawStep / pow10;
        double step;
        if      (frac < 1.5) step = 1.0   * pow10;
        else if (frac < 3.0) step = 2.5   * pow10;
        else if (frac < 7.0) step = 5.0   * pow10;
        else                 step = 10.0  * pow10;

        const double viewEnd  = m_viewStart + m_viewDur;
        const double first    = std::floor(m_viewStart / step) * step;
        p.setPen(QColor(160, 160, 160, 160));
        for (double t = first; t <= viewEnd + step * 0.01; t += step) {
            if (t < 0.0) continue;
            const int tx = (int)secToX(t);
            if (tx < 0 || tx >= W) continue;
            p.drawLine(tx, kRulerH - 7, tx, kRulerH);
            p.drawText(tx + 2, 2, W - tx, kRulerH - 2,
                       Qt::AlignLeft | Qt::AlignTop, fmtTick(t));
        }
    }

    // Playhead
    if (c && m_model->cues.isCuePlaying(m_cueIdx)) {
        const double fp  = m_model->cues.cuePlayheadFileSeconds(m_cueIdx);
        const int    phx = (int)secToX(fp);
        if (phx >= 0 && phx < W) {
            p.setPen(QPen(QColor(100, 255, 120, 200), 1.5));
            p.drawLine(phx, 0, phx, waveBot);
            // Triangle at ruler top
            QPolygon tri;
            tri << QPoint(phx, kRulerH)
                << QPoint(phx - 5, 3)
                << QPoint(phx + 5, 3);
            p.setBrush(QColor(100, 255, 120, 220));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }

    // Arm cursor (yellow)
    if (m_armSec >= 0.0) {
        const int ax = (int)secToX(m_armSec);
        if (ax >= 0 && ax < W) {
            p.setPen(QPen(QColor(255, 220, 0, 180), 1));
            p.drawLine(ax, waveTop, ax, waveBot);
            QPolygon tri;
            tri << QPoint(ax, waveBot)
                << QPoint(ax - 5, waveBot + 7)
                << QPoint(ax + 5, waveBot + 7);
            p.setBrush(QColor(255, 220, 0, 220));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }

    // Handles (start=bright white, end=dimmer white).
    // Don't draw when outside the current view range.
    auto drawHandle = [&](double sec, QColor col, QColor lineCol) {
        if (m_viewDur <= 0.0) return;
        const double rawX = secToX(sec);
        if (rawX < -1.0 || rawX > (double)W + 1.0) return;
        const int hx = std::clamp((int)rawX, 0, W - 1);
        p.setPen(QPen(lineCol, 1.5));
        p.drawLine(hx, kRulerH, hx, waveBot);
        QPolygon tri;
        tri << QPoint(hx, kRulerH)
            << QPoint(hx - 5, 3)
            << QPoint(hx + 5, 3);
        p.setBrush(col);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
    };
    drawHandle(startSec, QColor(255, 255, 255, 220), QColor(255, 255, 255, 100));
    drawHandle(endSec,   QColor(255, 255, 255, 180), QColor(255, 255, 255, 70));

    // Markers (amber)
    if (c) {
        for (int mi = 0; mi < (int)c->markers.size(); ++mi) {
            const int mx_ = (int)secToX(c->markers[mi].time);
            if (mx_ < 0 || mx_ >= W) continue;
            const bool selM   = (m_selMarker == mi);
            const QColor mcol = selM ? QColor(255, 220, 60) : QColor(220, 160, 40);
            // Dashed line
            for (int dy = kRulerH; dy < waveBot; dy += 5) {
                p.setPen(QColor(mcol.red(), mcol.green(), mcol.blue(), 128));
                p.drawLine(mx_, dy, mx_, std::min(dy + 3, waveBot));
            }
            QPolygon tri;
            tri << QPoint(mx_, kRulerH)
                << QPoint(mx_ - 5, 3)
                << QPoint(mx_ + 5, 3);
            p.setBrush(mcol);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            // Index badge
            p.setPen(QColor(255, 220, 80, 180));
            p.drawText(mx_ + 3, 3, 20, kRulerH - 3,
                       Qt::AlignLeft, QString::number(mi + 1));
        }
    }

    // Loop count labels at the bottom strip
    if (c) {
        std::vector<double> bounds;
        bounds.push_back(startSec);
        for (const auto& mk : c->markers) bounds.push_back(mk.time);
        bounds.push_back(endSec);
        const int loopTop = waveBot;

        p.fillRect(0, loopTop, W, kLoopH, QColor(14, 16, 24));

        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            const int blX = std::max(0, (int)secToX(bounds[bi]));
            const int brX = std::min(W, (int)secToX(bounds[bi + 1]));
            if (brX <= blX + 2) continue;

            const int lc = (bi < (int)c->sliceLoops.size()) ? c->sliceLoops[bi] : 1;
            const int midX = (blX + brX) / 2;
            const QString lbl = (lc == 0) ? "∞" : QString::number(lc);

            const QColor lblCol = (lc == 0)
                ? QColor(255, 160, 60, 240)
                : QColor(180, 200, 255, 200);
            p.setPen(lblCol);
            p.drawText(blX, loopTop, brX - blX, kLoopH,
                       Qt::AlignHCenter | Qt::AlignVCenter, lbl);
            (void)midX;
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void WaveformView::mousePressEvent(QMouseEvent* ev) {
    const double xd = ev->position().x();
    const double yd = ev->position().y();
    const double sec = xToSec(xd);

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    const double fileDur = m_peaks.valid ? m_peaks.fileDur : 1.0;
    const double startSec = c->startTime;
    const double endSec   = (c->duration > 0.0)
        ? c->startTime + c->duration : fileDur;

    const int waveH  = height() - kRulerH - kLoopH;
    const int waveTop = kRulerH;
    const int waveBot = waveTop + waveH;

    constexpr double kSnapPx = 8.0;
    // Use clamped pixel coords for hit-testing so the end handle at W is reachable
    auto snapPx = [&](double sec) {
        return std::clamp(secToX(sec), 0.0, (double)(width() - 1));
    };

    if (ev->button() == Qt::LeftButton) {
        // Check handles and markers (ruler zone)
        if (yd < kRulerH + 10) {
            if (std::abs(snapPx(startSec) - xd) < kSnapPx) {
                m_model->pushUndo();
                m_drag = -1; m_dragValOrig = startSec; m_dragPxOrig = xd;
                ev->accept(); return;
            }
            if (std::abs(snapPx(endSec) - xd) < kSnapPx) {
                m_model->pushUndo();
                m_drag = -3; m_dragValOrig = endSec; m_dragPxOrig = xd;
                ev->accept(); return;
            }
            for (int mi = 0; mi < (int)c->markers.size(); ++mi) {
                if (std::abs(snapPx(c->markers[mi].time) - xd) < kSnapPx) {
                    m_model->pushUndo();
                    m_drag = mi; m_dragValOrig = c->markers[mi].time; m_dragPxOrig = xd;
                    if (m_selMarker != mi) {
                        m_selMarker = mi;
                        emit markerSelectionChanged(mi);
                    }
                    ev->accept(); return;
                }
            }
        }

        // Waveform body click — arm cursor only (does NOT change cue startTime)
        if (yd >= waveTop && yd < waveBot && sec >= 0.0 && sec <= fileDur) {
            if (m_selMarker != -1) {
                m_selMarker = -1;
                emit markerSelectionChanged(-1);
            }
            m_armSec = sec;
            m_model->cues.arm(m_cueIdx, sec);
            emit armPositionChanged(sec);
            update();
        }
    } else if (ev->button() == Qt::RightButton) {
        m_rightDragging   = false;
        m_panOriginX      = xd;
        m_panViewStart    = m_viewStart;
    }
    ev->accept();
}

void WaveformView::mouseMoveEvent(QMouseEvent* ev) {
    const double xd = ev->position().x();

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    const double fileDur = m_peaks.valid ? m_peaks.fileDur : 1.0;
    const double startSec = c->startTime;
    const double endSec   = (c->duration > 0.0)
        ? c->startTime + c->duration : fileDur;

    if ((ev->buttons() & Qt::LeftButton) && m_drag != -2) {
        const double delta = (xd - m_dragPxOrig) / width() * m_viewDur;
        if (m_drag == -1) {
            double ns = std::clamp(m_dragValOrig + delta, 0.0, endSec - 0.001);
            m_model->cues.setCueStartTime(m_cueIdx, ns);
            ShowHelpers::syncSfFromCues(*m_model);
        } else if (m_drag == -3) {
            double ne = std::clamp(m_dragValOrig + delta, startSec + 0.001, fileDur);
            m_model->cues.setCueDuration(m_cueIdx, ne - c->startTime);
            ShowHelpers::syncSfFromCues(*m_model);
        } else if (m_drag >= 0) {
            double nt = std::clamp(m_dragValOrig + delta, startSec + 0.001, endSec - 0.001);
            m_model->cues.setCueMarkerTime(m_cueIdx, m_drag, nt);
            ShowHelpers::syncSfFromCues(*m_model);
        }
        update();
    }

    if ((ev->buttons() & Qt::RightButton)) {
        const double dx = xd - m_panOriginX;
        if (std::abs(dx) > 2.0) m_rightDragging = true;
        if (m_rightDragging) {
            const double shift = -dx / width() * m_viewDur;
            const double fd = m_peaks.valid ? m_peaks.fileDur : 1.0;
            m_viewStart = std::clamp(m_panViewStart + shift,
                                     0.0, std::max(0.0, fd - m_viewDur));
            update();
        }
    }

    ev->accept();
}

void WaveformView::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton && m_drag != -2) {
        commitDrag();
        m_drag = -2;
        update();
    }
    if (ev->button() == Qt::RightButton)
        m_rightDragging = false;
    ev->accept();
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;

    const double xd = ev->position().x();
    const double yd = ev->position().y();

    const mcp::Cue* c = (m_cueIdx >= 0) ? m_model->cues.cueAt(m_cueIdx) : nullptr;
    if (!c) return;

    const double fileDur  = m_peaks.valid ? m_peaks.fileDur : 1.0;
    const double startSec = c->startTime;
    const double endSec   = (c->duration > 0.0)
        ? c->startTime + c->duration : fileDur;

    const int waveH  = height() - kRulerH - kLoopH;
    const int waveBot = kRulerH + waveH;

    // Loop count strip double-click → inline editor
    if (yd >= waveBot) {
        std::vector<double> bounds;
        bounds.push_back(startSec);
        for (const auto& mk : c->markers) bounds.push_back(mk.time);
        bounds.push_back(endSec);
        for (int bi = 0; bi + 1 < (int)bounds.size(); ++bi) {
            const int blX = std::max(0, (int)secToX(bounds[bi]));
            const int brX = std::min(width(), (int)secToX(bounds[bi + 1]));
            if ((int)xd >= blX && (int)xd < brX && brX > blX + 4) {
                const int lc = (bi < (int)c->sliceLoops.size()) ? c->sliceLoops[bi] : 1;
                const QString cur = (lc == 0) ? "∞" : QString::number(lc);
                startLoopEdit(bi, blX, brX, cur);
                ev->accept();
                return;
            }
        }
    }
    ev->accept();
}

void WaveformView::wheelEvent(QWheelEvent* ev) {
    const double pivot = xToSec(ev->position().x());
    const double steps = ev->angleDelta().y() / 120.0;
    const double factor = std::pow(0.8, steps);
    const double fd = m_peaks.valid ? m_peaks.fileDur : 1.0;
    m_viewDur = std::clamp(m_viewDur * factor, 0.05, fd > 0 ? fd : 60.0);
    m_viewStart = pivot - (pivot - m_viewStart) * factor;
    m_viewStart = std::clamp(m_viewStart, 0.0, std::max(0.0, fd - m_viewDur));
    update();
    ev->accept();
}

void WaveformView::contextMenuEvent(QContextMenuEvent* ev) {
    if (m_rightDragging) { ev->accept(); return; }
    const double sec = xToSec(ev->x());
    QMenu menu(this);
    menu.addAction(
        QString("Add marker at %1")
            .arg(QString::fromStdString(ShowHelpers::fmtTime(sec))),
        [this, sec]() {
            m_model->pushUndo();
            m_model->cues.addCueMarker(m_cueIdx, sec);
            ShowHelpers::syncSfFromCues(*m_model);
            emit markerAdded(sec);
            update();
        });
    menu.addSeparator();
    menu.addAction("Fit view", [this]() {
        const double fd = m_peaks.valid ? m_peaks.fileDur : 1.0;
        m_viewStart = 0.0;
        m_viewDur   = std::max(fd, 0.01);
        update();
    });
    menu.exec(ev->globalPos());
    ev->accept();
}

void WaveformView::leaveEvent(QEvent* ev) {
    QWidget::leaveEvent(ev);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WaveformView::commitDrag() {
    if (m_drag >= 0 && m_cueIdx >= 0) {
        const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
        if (c && m_drag < (int)c->markers.size()) {
            m_model->cues.setCueMarkerTime(m_cueIdx, m_drag, c->markers[m_drag].time);
            ShowHelpers::syncSfFromCues(*m_model);
        }
    }
}

void WaveformView::startLoopEdit(int sliceIdx, int blX, int brX, const QString& current) {
    // Dismiss any existing editor first
    if (m_loopEdit) {
        m_loopEdit->disconnect();
        delete m_loopEdit;
        m_loopEdit = nullptr;
    }

    const int W = width();
    const int H = height();
    const int waveBot = H - kLoopH;

    // Clamp to visible area
    const int ex = std::max(0, blX);
    const int ew = std::min(W, brX) - ex;
    if (ew < 8) return;

    m_editLoopSlice = sliceIdx;
    m_loopEdit = new QLineEdit(this);
    m_loopEdit->setGeometry(ex, waveBot, ew, kLoopH);
    m_loopEdit->setText(current);
    m_loopEdit->selectAll();
    m_loopEdit->setAlignment(Qt::AlignHCenter);
    m_loopEdit->setStyleSheet(
        "QLineEdit { background:#1a2040; color:#ffffff; "
        "  border:1px solid #3a5aaa; border-radius:0; font-size:12px; }");
    m_loopEdit->show();
    m_loopEdit->setFocus();

    connect(m_loopEdit, &QLineEdit::editingFinished,
            this, &WaveformView::commitLoopEdit);
}

void WaveformView::commitLoopEdit() {
    if (!m_loopEdit) return;

    const QString raw = m_loopEdit->text().trimmed();
    const int sliceIdx = m_editLoopSlice;

    // Clean up the widget first so focusOut doesn't re-trigger
    m_loopEdit->blockSignals(true);
    m_loopEdit->deleteLater();
    m_loopEdit = nullptr;
    m_editLoopSlice = -1;

    if (m_cueIdx < 0 || sliceIdx < 0) { update(); return; }
    const mcp::Cue* c = m_model->cues.cueAt(m_cueIdx);
    if (!c || sliceIdx >= (int)c->sliceLoops.size()) { update(); return; }

    int newLc;
    if (raw == "∞" || raw == "0") {
        newLc = 0;
    } else {
        bool ok = false;
        const int v = raw.toInt(&ok);
        if (!ok || v < 1) {
            // Reject invalid input — restore display without changing model
            update();
            return;
        }
        newLc = v;
    }

    m_model->pushUndo();
    auto sl = c->sliceLoops;
    sl[sliceIdx] = newLc;
    m_model->cues.setCueSliceLoops(m_cueIdx, sl);
    ShowHelpers::syncSfFromCues(*m_model);
    update();
}

void WaveformView::rebuildPeaks(const std::string& path) {
    if (m_peaks.valid && m_peaks.path == path) return;
    m_peaks = {};       // invalidate immediately so paintEvent draws placeholder
    launchPeakBuild(path);
}

void WaveformView::launchPeakBuild(const std::string& path) {
    const int gen = ++m_buildGeneration;
    std::thread([this, path, gen]() {
        PeakCache pc;
        pc.path  = path;
        pc.valid = mcp::buildWaveformPeaks(path, 2000,
            pc.minPk, pc.maxPk, pc.fileDur, pc.fileCh);
        // Deliver result to the main thread.
        // Qt discards the invocation safely if 'this' has been destroyed.
        QMetaObject::invokeMethod(this, [this, pc = std::move(pc), gen]() mutable {
            if (gen == m_buildGeneration) {
                m_peaks = std::move(pc);
                // Fit to file if no saved zoom was restored.
                if (m_viewDur <= 0.0 && m_peaks.valid)
                    m_viewDur = m_peaks.fileDur;
                update();
            }
        }, Qt::QueuedConnection);
    }).detach();
}
