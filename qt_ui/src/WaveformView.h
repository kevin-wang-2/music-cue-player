#pragma once

#include "engine/AudioDecoder.h"
#include "engine/Cue.h"
#include "engine/MusicContext.h"

#include <QWidget>
#include <string>
#include <unordered_map>
#include <vector>

class AppModel;
class QLineEdit;

// Custom waveform editor canvas.
// Renders a scrollable/zoomable view of the audio file, with:
//   - Waveform peak data (one line per pixel column)
//   - Ruler with time ticks (or bar/beat if Music Context is set)
//   - Start/end handle triangles (draggable)
//   - Slice markers (draggable amber triangles)
//   - Per-slice loop-count labels (click to edit)
//   - Green playhead line
//   - Yellow arm cursor
//   - Scroll-wheel zoom around mouse; right-drag pan
class WaveformView : public QWidget {
    Q_OBJECT
public:
    explicit WaveformView(AppModel* model, QWidget* parent = nullptr);

    // Call when the selected cue changes (rebuilds cache).
    void setCueIndex(int idx);
    // Call on each timer tick while playing.
    void updatePlayhead();
    // Set a Music Context to use for bar/beat ruler (null = seconds ruler).
    // cueStartTimeSec: audio file offset where the cue starts (cue.startTime).
    void setMusicContext(const mcp::MusicContext* mc, double cueStartTimeSec = 0.0);

signals:
    void armPositionChanged(double sec);
    void markerAdded(double sec);
    void markerSelectionChanged(int markerIdx);   // -1 = deselected

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*) override;
    QSize sizeHint() const override { return {600, 160}; }

private:
    // Geometry constants
    static constexpr int kRulerH  = 20;
    static constexpr int kLoopH   = 18;

    double secToX(double t) const;
    double xToSec(double x) const;
    QString fmtTick(double s) const;
    void    rebuildPeaks(const std::string& path);
    void    launchPeakBuild(const std::string& path);
    void    drawLoopCountAt(QPainter&, int sliceIdx, double bL, double bR,
                            float waveBot);
    void    commitDrag();
    void    startLoopEdit(int sliceIdx, int blX, int brX, const QString& current);
    void    commitLoopEdit();
    double  snapToGrid(double sec) const;

    AppModel*  m_model{nullptr};
    QLineEdit* m_loopEdit{nullptr};   // inline overlay, nullptr when hidden
    int       m_cueIdx{-1};

    // Peak cache
    struct PeakCache {
        std::string        path;
        std::vector<float> minPk[2];
        std::vector<float> maxPk[2];
        double             fileDur{0.0};
        int                fileCh{0};
        bool               valid{false};
    } m_peaks;

    // View state
    double m_viewStart{0.0};
    double m_viewDur{1.0};

    // Arm cursor
    double m_armSec{-1.0};

    // Drag state: -2=none, -1=startHandle, -3=endHandle, >=0=markerIdx
    int    m_drag{-2};
    double m_dragValOrig{0.0};
    double m_dragPxOrig{0.0};

    int m_selMarker{-1};

    // Loop count inline editing
    int  m_editLoopSlice{-1};

    // Right-button pan
    double m_panOriginX{0.0};
    double m_panViewStart{0.0};

    bool m_rightDragging{false};

    // Async peak build — generation counter lets us discard stale results.
    int  m_buildGeneration{0};

    // Per-cue zoom memory: cueIdx → (viewStart, viewDur).
    // Populated when switching away from a cue that has been viewed.
    std::unordered_map<int, std::pair<double,double>> m_zoomState;

    // Music Context for bar/beat ruler (not owned)
    const mcp::MusicContext* m_mc{nullptr};
    double                   m_mcCueStart{0.0};  // cue.startTime offset in file seconds

    // Snap quantization: 0=none, 1=bar, 2=half, 4=quarter, 8=8th, 16=16th, 32=32nd
    int m_quantSubdiv{0};
};
