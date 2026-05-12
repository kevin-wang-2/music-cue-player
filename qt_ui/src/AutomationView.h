#pragma once

#include "engine/AutoParam.h"
#include "engine/Cue.h"
#include "engine/MusicContext.h"
#include <QWidget>
#include <QString>
#include <functional>
#include <vector>

// Curve editor for Automation cues.
// Horizontal axis: time (0..duration seconds).
// Vertical axis: parameter value range (depends on parameter mode).
//
// Data model: m_pts stores breakpoints (isHandle=false) and per-segment shape
// handles (isHandle=true) interleaved as BP, H, BP, H, ..., BP.
// PCHIP runs through all points so dragging a handle reshapes the adjacent curve.
//
// Interactions:
//   Left-click empty space  → add a breakpoint (+ auto-insert handle for new segment)
//   Left-drag breakpoint    → move (time + value)
//   Left-drag handle        → move within segment bounding box
//   Right-click breakpoint  → context menu: delete / set value / set time (bar.beat if MC)
//   Right-click handle      → context menu: reset to midpoint
class AutomationView : public QWidget {
    Q_OBJECT
public:
    explicit AutomationView(QWidget* parent = nullptr);

    void setPoints      (const std::vector<mcp::Cue::AutomationPoint>& pts);
    void setDuration    (double seconds);
    void setParamMode   (mcp::Cue::AutomationParamMode mode);
    void setMusicContext(const mcp::MusicContext* mc);  // null = no MC / no grid
    void setQuantize    (int mode);                     // 0=none 1=bar 2=beat

    // Override value range and unit label for plugin/custom parameters.
    // Call resetParamRange() to go back to defaults (fader range).
    void setParamRange  (double minVal, double maxVal, const QString& unit = "");
    void setParamDomain (mcp::AutoParam::Domain domain);  // controls reference lines
    void resetParamRange();                               // back to fader defaults

    // Callback used by "Reset to current value" in the right-click menu.
    // Should return the live value of the currently assigned parameter.
    void setCurrentValueGetter(std::function<double()> fn) { m_getCurrentValue = std::move(fn); }

    const std::vector<mcp::Cue::AutomationPoint>& points() const { return m_pts; }

signals:
    // Emitted after any breakpoint or handle add/move/delete.
    void curveChanged(const std::vector<mcp::Cue::AutomationPoint>& pts);

protected:
    void paintEvent             (QPaintEvent*)  override;
    void mousePressEvent        (QMouseEvent*)  override;
    void mouseMoveEvent         (QMouseEvent*)  override;
    void mouseReleaseEvent      (QMouseEvent*)  override;
    void mouseDoubleClickEvent  (QMouseEvent*)  override;

private:
    // Coordinate transforms
    double timeFromX (int x) const;
    double valueFromY(int y) const;
    int    xFromTime (double t) const;
    int    yFromValue(double v) const;
    QRect  plotRect  ()         const;

    // Value range for the current param mode
    double minValue() const;
    double maxValue() const;
    double valueGridStep() const;

    // Hit testing: returns index into m_pts or -1
    int hitTest(int x, int y) const;

    // Snap t to nearest bar/beat boundary when m_quantize != 0 and m_mc is set.
    double snapToGrid(double t) const;

    // PCHIP-interpolated value at absolute time t (uses all m_pts as control points)
    double curveValueAt(double t) const;

    // Ensure handles exist between every pair of adjacent breakpoints.
    // Creates midpoint handles for new segments; removes orphaned handles.
    // Returns true if m_pts was modified.
    bool ensureHandles();

    // Clamp handle to its segment bounding box (prevBP, nextBP).
    void clampHandle(int handleIdx);

    void sortPoints();
    void clampBreakpoint(mcp::Cue::AutomationPoint& p) const;

    std::vector<mcp::Cue::AutomationPoint> m_pts;
    double                        m_duration{5.0};
    mcp::Cue::AutomationParamMode m_mode{mcp::Cue::AutomationParamMode::Linear};
    const mcp::MusicContext*      m_mc{nullptr};
    int                           m_quantize{0};

    bool                    m_useCustomRange{false};
    double                  m_customMin{0.0};
    double                  m_customMax{1.0};
    QString                 m_unit;
    mcp::AutoParam::Domain  m_domain{mcp::AutoParam::Domain::Linear};

    int  m_dragIdx{-1};     // index in m_pts being dragged, or -1
    bool m_dragging{false};

    std::function<double()> m_getCurrentValue;
};
