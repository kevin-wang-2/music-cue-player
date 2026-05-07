#pragma once

#include <QWidget>
#include <string>
#include <unordered_map>
#include <vector>

class AppModel;

class TimelineGroupView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineGroupView(AppModel* model, QWidget* parent = nullptr);

    void setGroupCueIndex(int groupFlatIdx);
    void clearArmCursor();   // call after GO fires or Esc clears the arm

    QSize sizeHint() const override;

signals:
    void childOffsetChanged(int childFlatIdx, double newOffsetSec);
    void rulerClicked(double timeSec);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct ChildBlock {
        int         flatIdx{-1};
        double      offset{0.0};
        double      duration{0.0};
        double      startTime{0.0};   // audio file start offset
        QString     label;
        std::string audioPath;        // non-empty for Audio cues
    };

    struct PeakCache {
        std::vector<float> minPk[2];
        std::vector<float> maxPk[2];
        double fileDur{0.0};
        int    fileCh{0};
        bool   valid{false};
    };

    void   rebuildBlocks();
    void   buildPeaksAsync(const std::string& path);
    double pixToSec(int px) const;
    int    secToPix(double sec) const;
    double viewDuration() const;
    void   updateSnapTargets();
    int    laneY(int blockIdx) const;  // top-Y of lane i (below ruler)

    AppModel* m_model{nullptr};
    int       m_groupIdx{-1};

    std::vector<ChildBlock> m_blocks;

    // Ruler / geometry constants
    static constexpr int kRulerH  = 20;
    static constexpr int kBlockH  = 44;   // tall enough for mini waveform
    static constexpr int kLaneGap = 3;    // vertical gap between lanes
    static constexpr int kTopPad  = 4;    // gap between ruler and first lane

    double m_viewStart{0.0};
    double m_pixPerSec{80.0};
    int    m_laneScrollPx{0};   // vertical scroll offset for the lane area

    // Drag state
    int    m_dragBlock{-1};
    int    m_dragStartX{0};
    double m_dragStartOffset{0.0};

    std::vector<double> m_snapTimes;

    double m_armSec{-1.0};   // ruler arm cursor position (-1 = none)

    // Per-path peak cache (shared across group changes)
    std::unordered_map<std::string, PeakCache> m_peakCache;
};
