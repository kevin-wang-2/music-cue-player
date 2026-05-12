#include "AutomationView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <vector>

static constexpr int kMarginL  = 44;
static constexpr int kMarginR  = 8;
static constexpr int kMarginT  = 8;
static constexpr int kMarginB  = 24;
static constexpr int kHitPx    = 8;

AutomationView::AutomationView(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(200, 120);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::NoContextMenu);
}

// ---------------------------------------------------------------------------
// public setters

void AutomationView::setPoints(const std::vector<mcp::Cue::AutomationPoint>& pts) {
    m_pts = pts;
    sortPoints();
    ensureHandles();
    update();
}

void AutomationView::setDuration(double s) {
    m_duration = (s > 0.0) ? s : 1.0;
    update();
}

void AutomationView::setParamMode(mcp::Cue::AutomationParamMode mode) {
    m_mode = mode;
    update();
}

void AutomationView::setParamRange(double minVal, double maxVal, const QString& unit) {
    m_useCustomRange = true;
    m_customMin = minVal;
    m_customMax = maxVal;
    m_unit = unit;
    update();
}

void AutomationView::setParamDomain(mcp::AutoParam::Domain domain) {
    m_domain = domain;
    update();
}

void AutomationView::resetParamRange() {
    m_useCustomRange = false;
    m_unit.clear();
    m_domain = mcp::AutoParam::Domain::Linear;
    update();
}

void AutomationView::setMusicContext(const mcp::MusicContext* mc) {
    m_mc = mc;
    update();
}

void AutomationView::setQuantize(int mode) {
    m_quantize = mode;
    update();
}

double AutomationView::snapToGrid(double t) const {
    if (m_quantize == 0 || !m_mc) return t;
    t = std::clamp(t, 0.0, m_duration);
    const auto cur = m_mc->secondsToMusical(t);
    if (m_quantize == 1) {
        // Snap to nearest bar
        const double t0 = m_mc->musicalToSeconds(cur.bar, 1);
        const double t1 = m_mc->musicalToSeconds(cur.bar + 1, 1);
        return (t - t0 < t1 - t) ? t0 : t1;
    }
    // Snap to nearest beat
    const double t0 = m_mc->musicalToSeconds(cur.bar, cur.beat);
    const auto ts = m_mc->timeSigAt(cur.bar, cur.beat);
    int nb = cur.beat + 1, nbar = cur.bar;
    if (nb > ts.num) { nb = 1; ++nbar; }
    const double t1 = m_mc->musicalToSeconds(nbar, nb);
    return (t - t0 < t1 - t) ? t0 : t1;
}

// ---------------------------------------------------------------------------
// value range helpers

double AutomationView::minValue() const {
    if (m_mode == mcp::Cue::AutomationParamMode::Step) return 0.0;
    if (m_useCustomRange) return m_customMin;
    return -100.0;
}
double AutomationView::maxValue() const {
    if (m_mode == mcp::Cue::AutomationParamMode::Step) return 1.0;
    if (m_useCustomRange) return m_customMax;
    return 12.0;
}
double AutomationView::valueGridStep() const {
    if (m_mode == mcp::Cue::AutomationParamMode::Step) return 1.0;
    if (!m_useCustomRange) return 12.0;
    // Auto-step: aim for ~5 grid lines
    const double range = m_customMax - m_customMin;
    if (range <= 0.0) return 1.0;
    const double raw = range / 5.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    if (norm < 2.0) return mag;
    if (norm < 5.0) return 2.0 * mag;
    return 5.0 * mag;
}

// ---------------------------------------------------------------------------
// PCHIP interpolation (mirrors backend algorithm)

static void pchipDerivs(const double* t, const double* y, double* d, int n) {
    if (n < 2) return;
    if (n == 2) {
        const double slope = (t[1] > t[0]) ? (y[1] - y[0]) / (t[1] - t[0]) : 0.0;
        d[0] = d[1] = slope;
        return;
    }
    std::vector<double> h(static_cast<size_t>(n - 1));
    std::vector<double> delta(static_cast<size_t>(n - 1));
    for (int k = 0; k < n - 1; ++k) {
        h[static_cast<size_t>(k)]     = t[k + 1] - t[k];
        delta[static_cast<size_t>(k)] = (h[static_cast<size_t>(k)] > 0.0)
            ? (y[k + 1] - y[k]) / h[static_cast<size_t>(k)] : 0.0;
    }
    auto endDeriv = [](double hk, double hk1, double dk, double dk1) -> double {
        const double d0 = ((2.0 * hk + hk1) * dk - hk * dk1) / (hk + hk1);
        if (dk == 0.0 || dk1 == 0.0) return 0.0;
        if ((dk > 0.0) != (d0 > 0.0)) return 0.0;
        if (std::abs(d0) > 3.0 * std::abs(dk)) return 3.0 * dk;
        return d0;
    };
    d[0]     = endDeriv(h[0], h[1], delta[0], delta[1]);
    d[n - 1] = endDeriv(h[static_cast<size_t>(n - 2)], h[static_cast<size_t>(n - 3)],
                        delta[static_cast<size_t>(n - 2)], delta[static_cast<size_t>(n - 3)]);
    for (int k = 1; k < n - 1; ++k) {
        const double dk0 = delta[static_cast<size_t>(k - 1)];
        const double dk1 = delta[static_cast<size_t>(k)];
        if (dk0 * dk1 <= 0.0) {
            d[k] = 0.0;
        } else {
            const double w1 = 2.0 * h[static_cast<size_t>(k)] + h[static_cast<size_t>(k - 1)];
            const double w2 = h[static_cast<size_t>(k)] + 2.0 * h[static_cast<size_t>(k - 1)];
            d[k] = (w1 + w2) / (w1 / dk0 + w2 / dk1);
        }
    }
}

static double pchipEval(const double* t, const double* y, const double* d, int n, double x) {
    if (n <= 0) return 0.0;
    if (x <= t[0]) return y[0];
    if (x >= t[n - 1]) return y[n - 1];
    int lo = 0, hi = n - 2;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (t[mid] <= x) lo = mid; else hi = mid - 1;
    }
    const double hk = t[lo + 1] - t[lo];
    if (hk <= 0.0) return y[lo];
    const double s = (x - t[lo]) / hk;
    const double s2 = s * s, s3 = s2 * s;
    return (2*s3 - 3*s2 + 1) * y[lo]
         + (s3 - 2*s2 + s)   * hk * d[lo]
         + (-2*s3 + 3*s2)    * y[lo + 1]
         + (s3 - s2)         * hk * d[lo + 1];
}

double AutomationView::curveValueAt(double t) const {
    if (m_pts.empty()) return (minValue() + maxValue()) / 2.0;
    const int n = static_cast<int>(m_pts.size());
    std::vector<double> pt(static_cast<size_t>(n));
    std::vector<double> py(static_cast<size_t>(n));
    std::vector<double> pd(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        pt[static_cast<size_t>(k)] = m_pts[static_cast<size_t>(k)].time;
        py[static_cast<size_t>(k)] = m_pts[static_cast<size_t>(k)].value;
    }
    pchipDerivs(pt.data(), py.data(), pd.data(), n);
    return pchipEval(pt.data(), py.data(), pd.data(), n, t);
}

// ---------------------------------------------------------------------------
// coordinate transforms

QRect AutomationView::plotRect() const {
    return QRect(kMarginL, kMarginT,
                 width()  - kMarginL - kMarginR,
                 height() - kMarginT - kMarginB);
}

double AutomationView::timeFromX(int x) const {
    const QRect r = plotRect();
    if (r.width() <= 0) return 0.0;
    return m_duration * static_cast<double>(x - r.left()) / r.width();
}
double AutomationView::valueFromY(int y) const {
    const QRect r = plotRect();
    if (r.height() <= 0) return 0.0;
    const double frac = 1.0 - static_cast<double>(y - r.top()) / r.height();
    return minValue() + frac * (maxValue() - minValue());
}
int AutomationView::xFromTime(double t) const {
    const QRect r = plotRect();
    if (m_duration <= 0.0) return r.left();
    return r.left() + static_cast<int>(t / m_duration * r.width());
}
int AutomationView::yFromValue(double v) const {
    const QRect r = plotRect();
    const double span = maxValue() - minValue();
    if (span <= 0.0) return r.top();
    const double frac = 1.0 - (v - minValue()) / span;
    return r.top() + static_cast<int>(frac * r.height());
}

// ---------------------------------------------------------------------------
// hit test

int AutomationView::hitTest(int x, int y) const {
    // Breakpoints take priority over handles
    for (int i = 0; i < (int)m_pts.size(); ++i) {
        if (m_pts[static_cast<size_t>(i)].isHandle) continue;
        const int px = xFromTime(m_pts[static_cast<size_t>(i)].time);
        const int py = yFromValue(m_pts[static_cast<size_t>(i)].value);
        if (std::abs(x - px) <= kHitPx && std::abs(y - py) <= kHitPx)
            return i;
    }
    for (int i = 0; i < (int)m_pts.size(); ++i) {
        if (!m_pts[static_cast<size_t>(i)].isHandle) continue;
        const int px = xFromTime(m_pts[static_cast<size_t>(i)].time);
        const int py = yFromValue(m_pts[static_cast<size_t>(i)].value);
        if (std::abs(x - px) <= kHitPx && std::abs(y - py) <= kHitPx)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// handle management

bool AutomationView::ensureHandles() {
    // Collect breakpoint indices in time order
    std::vector<int> bpIdx;
    for (int i = 0; i < (int)m_pts.size(); ++i)
        if (!m_pts[static_cast<size_t>(i)].isHandle) bpIdx.push_back(i);

    if (bpIdx.size() < 2) {
        // No segments: remove all handles
        bool changed = false;
        for (int i = (int)m_pts.size() - 1; i >= 0; --i) {
            if (m_pts[static_cast<size_t>(i)].isHandle) {
                m_pts.erase(m_pts.begin() + i);
                changed = true;
            }
        }
        return changed;
    }

    // Rebuild to BP, H, BP, H, ..., BP maintaining existing valid handles
    std::vector<mcp::Cue::AutomationPoint> rebuilt;
    rebuilt.reserve(2 * bpIdx.size() - 1);

    for (size_t seg = 0; seg + 1 < bpIdx.size(); ++seg) {
        const auto& bp0 = m_pts[static_cast<size_t>(bpIdx[seg])];
        const auto& bp1 = m_pts[static_cast<size_t>(bpIdx[seg + 1])];
        rebuilt.push_back(bp0);

        // Look for an existing handle between bp0 and bp1
        mcp::Cue::AutomationPoint handle;
        handle.isHandle = true;
        bool found = false;
        for (int i = bpIdx[seg] + 1; i < bpIdx[seg + 1]; ++i) {
            const auto& p = m_pts[static_cast<size_t>(i)];
            if (p.isHandle && p.time > bp0.time && p.time < bp1.time) {
                handle = p;
                // Clamp value within segment range
                const double lo = std::min(bp0.value, bp1.value);
                const double hi = std::max(bp0.value, bp1.value);
                handle.value = std::clamp(handle.value, lo, hi);
                found = true;
                break;
            }
        }
        if (!found) {
            // Default: midpoint in time, interpolate value on straight line
            handle.time  = (bp0.time + bp1.time) / 2.0;
            handle.value = (bp0.value + bp1.value) / 2.0;
        }
        rebuilt.push_back(handle);
    }
    rebuilt.push_back(m_pts[static_cast<size_t>(bpIdx.back())]);

    if (rebuilt.size() == m_pts.size()) {
        bool same = true;
        for (size_t i = 0; i < rebuilt.size() && same; ++i) {
            const auto& a = rebuilt[i]; const auto& b = m_pts[i];
            if (a.time != b.time || a.value != b.value || a.isHandle != b.isHandle)
                same = false;
        }
        if (same) return false;
    }
    m_pts = std::move(rebuilt);
    return true;
}

void AutomationView::clampHandle(int idx) {
    if (idx < 0 || idx >= (int)m_pts.size()) return;
    auto& h = m_pts[static_cast<size_t>(idx)];
    if (!h.isHandle) return;
    // Find adjacent breakpoints (they are idx-1 and idx+1 in the BP,H,BP pattern)
    if (idx == 0 || idx == (int)m_pts.size() - 1) return;
    const auto& bp0 = m_pts[static_cast<size_t>(idx - 1)];
    const auto& bp1 = m_pts[static_cast<size_t>(idx + 1)];
    h.time  = std::clamp(h.time,  bp0.time + 1e-6, bp1.time - 1e-6);
    const double lo = std::min(bp0.value, bp1.value);
    const double hi = std::max(bp0.value, bp1.value);
    h.value = std::clamp(h.value, lo, hi);
}

void AutomationView::sortPoints() {
    // Sort breakpoints by time; handles stay attached to their segment via ensureHandles
    // Here we just sort everything by time — ensureHandles rebuilds the structure
    std::stable_sort(m_pts.begin(), m_pts.end(),
                     [](const auto& a, const auto& b){ return a.time < b.time; });
}

void AutomationView::clampBreakpoint(mcp::Cue::AutomationPoint& p) const {
    p.time  = std::clamp(p.time,  0.0, m_duration);
    p.value = std::clamp(p.value, minValue(), maxValue());
    if (m_mode == mcp::Cue::AutomationParamMode::Step)
        p.value = (p.value >= 0.5) ? 1.0 : 0.0;
}

// ---------------------------------------------------------------------------
// paint

void AutomationView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));

    const QRect r = plotRect();
    p.fillRect(r, QColor(0x22, 0x22, 0x22));

    // Grid — horizontal (value) lines
    {
        const double step = valueGridStep();
        const double mn   = minValue();
        const double mx   = maxValue();

        // Reference line at 0 for DB/FaderTaper params (when 0 is in range)
        const bool hasRefLine = (m_domain == mcp::AutoParam::Domain::DB ||
                                 m_domain == mcp::AutoParam::Domain::FaderTaper);
        if (hasRefLine && mn < 0.0 && mx > 0.0) {
            const int y0 = yFromValue(0.0);
            p.setPen(QPen(QColor(0x88, 0x88, 0x44), 1, Qt::DashLine));
            p.drawLine(r.left(), y0, r.right(), y0);
        }

        p.setPen(QColor(0x44, 0x44, 0x44));
        for (double v = mn; v <= mx + 1e-6; v += step) {
            const int y = yFromValue(v);
            p.drawLine(r.left(), y, r.right(), y);
            p.setPen(QColor(0x77, 0x77, 0x77));
            QString label;
            if (m_mode == mcp::Cue::AutomationParamMode::Step) {
                label = (v >= 0.5) ? "ON" : "OFF";
            } else if (!m_useCustomRange || m_unit == "dB") {
                label = (v >= 0.0 ? "+" : "") + QString::number(static_cast<int>(v)) + " dB";
            } else if (m_unit.isEmpty()) {
                label = QString::number(v, 'g', 3);
            } else {
                label = QString::number(v, 'g', 3) + " " + m_unit;
            }
            p.drawText(0, y - 7, kMarginL - 4, 14, Qt::AlignRight | Qt::AlignVCenter, label);
            p.setPen(QColor(0x44, 0x44, 0x44));
        }
    }

    // Grid — vertical (time) lines
    if (m_mc) {
        // Bar/beat grid from MusicContext
        double t = 0.0;
        while (t <= m_duration + 1e-6) {
            const auto pos = m_mc->secondsToMusical(t);
            const int x = xFromTime(t);
            if (pos.beat == 1) {
                p.setPen(QColor(0x55, 0x55, 0x55));
                p.drawLine(x, r.top(), x, r.bottom());
                p.setPen(QColor(0x77, 0x77, 0x77));
                p.drawText(x + 2, r.bottom() + 2, 24, kMarginB - 2,
                           Qt::AlignLeft | Qt::AlignTop, QString::number(pos.bar));
            } else {
                p.setPen(QColor(0x38, 0x38, 0x38));
                p.drawLine(x, r.top(), x, r.bottom());
            }
            const auto ts = m_mc->timeSigAt(pos.bar, pos.beat);
            int nb = pos.beat + 1, nbar = pos.bar;
            if (nb > ts.num) { nb = 1; ++nbar; }
            const double tNext = m_mc->musicalToSeconds(nbar, nb);
            if (tNext <= t + 1e-6) break;
            t = tNext;
        }
    } else {
        p.setPen(QColor(0x44, 0x44, 0x44));
        const int numDiv = std::max(2, static_cast<int>(m_duration));
        const double step = m_duration / numDiv;
        for (int ti = 0; ti <= numDiv; ++ti) {
            const double t = ti * step;
            const int x = xFromTime(t);
            p.drawLine(x, r.top(), x, r.bottom());
            p.setPen(QColor(0x77, 0x77, 0x77));
            const QString lbl = QString::number(t, 'f', (step < 1.0) ? 1 : 0) + "s";
            p.drawText(x - 20, r.bottom() + 2, 40, kMarginB - 2, Qt::AlignHCenter | Qt::AlignTop, lbl);
            p.setPen(QColor(0x44, 0x44, 0x44));
        }
    }

    // Count breakpoints
    int bpCount = 0;
    for (const auto& pt : m_pts) if (!pt.isHandle) ++bpCount;

    // Curve path
    if (bpCount >= 1) {
        p.setPen(QPen(QColor(0xff, 0x77, 0xbb), 2));

        if (m_mode == mcp::Cue::AutomationParamMode::Step) {
            // Staircase: hold value until next breakpoint (ignore handles in step mode)
            std::vector<mcp::Cue::AutomationPoint> bps;
            for (const auto& pt : m_pts) if (!pt.isHandle) bps.push_back(pt);
            QPainterPath path;
            path.moveTo(r.left(), yFromValue(bps.front().value));
            for (size_t i = 0; i < bps.size(); ++i) {
                const int x = xFromTime(bps[i].time);
                const int y = yFromValue(bps[i].value);
                path.lineTo(x, path.currentPosition().y());
                path.lineTo(x, y);
                const int nextX = (i + 1 < bps.size())
                    ? xFromTime(bps[i + 1].time) : r.right();
                path.lineTo(nextX, y);
            }
            p.drawPath(path);
        } else if (bpCount >= 2) {
            // PCHIP: sample one point per pixel for smooth rendering
            QPainterPath path;
            path.moveTo(r.left(), yFromValue(curveValueAt(timeFromX(r.left()))));
            for (int px = r.left() + 1; px <= r.right(); ++px) {
                const double t = timeFromX(px);
                path.lineTo(px, yFromValue(curveValueAt(t)));
            }
            p.drawPath(path);
        } else {
            // Single breakpoint: horizontal dashed line
            p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1, Qt::DashLine));
            const int y = yFromValue(m_pts.front().value);
            p.drawLine(r.left(), y, r.right(), y);
        }
    } else {
        p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1, Qt::DashLine));
        const int y = yFromValue((minValue() + maxValue()) / 2.0);
        p.drawLine(r.left(), y, r.right(), y);
    }

    // Breakpoint handles (filled circles)
    for (const auto& pt : m_pts) {
        if (pt.isHandle) continue;
        const int x = xFromTime(pt.time);
        const int y = yFromValue(pt.value);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0x77, 0xbb));
        p.drawEllipse(QPoint(x, y), 5, 5);
    }

    // Per-segment shape handles (hollow diamonds), only in Linear mode with ≥2 BPs
    if (m_mode != mcp::Cue::AutomationParamMode::Step && bpCount >= 2) {
        for (const auto& pt : m_pts) {
            if (!pt.isHandle) continue;
            const int x = xFromTime(pt.time);
            const int y = yFromValue(pt.value);
            p.setPen(QPen(QColor(0xff, 0xcc, 0x55), 2));
            p.setBrush(Qt::NoBrush);
            // Draw diamond
            const QPointF diamond[4] = {
                {(qreal)x,       (qreal)(y - 6)},
                {(qreal)(x + 6), (qreal)y},
                {(qreal)x,       (qreal)(y + 6)},
                {(qreal)(x - 6), (qreal)y}
            };
            p.drawPolygon(diamond, 4);
        }
    }

    // Plot border
    p.setPen(QColor(0x55, 0x55, 0x55));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

// ---------------------------------------------------------------------------
// mouse interaction

void AutomationView::mousePressEvent(QMouseEvent* ev) {
    const int idx = hitTest(ev->pos().x(), ev->pos().y());

    if (ev->button() == Qt::RightButton) {
        if (idx >= 0) {
            const bool isHandle = m_pts[static_cast<size_t>(idx)].isHandle;
            if (isHandle) {
                // Reset handle to segment midpoint
                auto& h = m_pts[static_cast<size_t>(idx)];
                if (idx > 0 && idx < (int)m_pts.size() - 1) {
                    const auto& bp0 = m_pts[static_cast<size_t>(idx - 1)];
                    const auto& bp1 = m_pts[static_cast<size_t>(idx + 1)];
                    h.time  = (bp0.time + bp1.time) / 2.0;
                    h.value = (bp0.value + bp1.value) / 2.0;
                }
            } else {
                // Delete the breakpoint and its adjacent handle
                // In the BP,H,BP,H,...,BP pattern, each BP at position i has:
                // - a handle before it at i-1 (if i > 0 and prev is handle)
                // - a handle after it at i+1 (if i < size-1 and next is handle)
                // After deleting the BP, ensureHandles will rebuild correctly.
                m_pts.erase(m_pts.begin() + idx);
                sortPoints();
                ensureHandles();
            }
            emit curveChanged(m_pts);
            update();
        }
        return;
    }

    if (ev->button() == Qt::LeftButton) {
        if (idx >= 0) {
            m_dragIdx  = idx;
            m_dragging = true;
        } else {
            // Add a new breakpoint (in step mode, no handle; in linear mode, ensureHandles adds one)
            mcp::Cue::AutomationPoint pt;
            pt.time    = snapToGrid(timeFromX(ev->pos().x()));
            pt.value   = valueFromY(ev->pos().y());
            pt.isHandle = false;
            clampBreakpoint(pt);
            m_pts.push_back(pt);
            sortPoints();
            ensureHandles();
            // Find the newly-added breakpoint to start dragging it
            m_dragIdx = -1;
            for (int i = 0; i < (int)m_pts.size(); ++i) {
                if (!m_pts[static_cast<size_t>(i)].isHandle &&
                    std::abs(m_pts[static_cast<size_t>(i)].time  - pt.time)  < 1e-9 &&
                    std::abs(m_pts[static_cast<size_t>(i)].value - pt.value) < 1e-9) {
                    m_dragIdx = i; break;
                }
            }
            m_dragging = true;
            emit curveChanged(m_pts);
            update();
        }
    }
}

void AutomationView::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging || m_dragIdx < 0 || m_dragIdx >= (int)m_pts.size()) return;

    auto& pt = m_pts[static_cast<size_t>(m_dragIdx)];

    if (pt.isHandle) {
        pt.time  = timeFromX(ev->pos().x());
        pt.value = valueFromY(ev->pos().y());
        clampHandle(m_dragIdx);
    } else {
        const double prevTime  = pt.time;
        const double prevValue = pt.value;
        pt.time  = snapToGrid(timeFromX(ev->pos().x()));
        pt.value = valueFromY(ev->pos().y());
        clampBreakpoint(pt);

        if (pt.time != prevTime || pt.value != prevValue) {
            const double movedTime  = pt.time;
            const double movedValue = pt.value;
            sortPoints();
            ensureHandles();
            // Re-find dragged point after sort/rebuild
            m_dragIdx = -1;
            for (int i = 0; i < (int)m_pts.size(); ++i) {
                if (!m_pts[static_cast<size_t>(i)].isHandle &&
                    std::abs(m_pts[static_cast<size_t>(i)].time  - movedTime)  < 1e-9 &&
                    std::abs(m_pts[static_cast<size_t>(i)].value - movedValue) < 1e-9) {
                    m_dragIdx = i; break;
                }
            }
        }
    }

    emit curveChanged(m_pts);
    update();
}

void AutomationView::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) {
        m_dragging = false;
        m_dragIdx  = -1;
    }
}

void AutomationView::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    const int idx = hitTest(ev->pos().x(), ev->pos().y());
    if (idx < 0 || !m_pts[static_cast<size_t>(idx)].isHandle) return;
    // Reset handle to segment midpoint (time and value)
    auto& h = m_pts[static_cast<size_t>(idx)];
    if (idx > 0 && idx < (int)m_pts.size() - 1) {
        const auto& bp0 = m_pts[static_cast<size_t>(idx - 1)];
        const auto& bp1 = m_pts[static_cast<size_t>(idx + 1)];
        h.time  = (bp0.time + bp1.time) / 2.0;
        h.value = (bp0.value + bp1.value) / 2.0;
    }
    emit curveChanged(m_pts);
    update();
}
