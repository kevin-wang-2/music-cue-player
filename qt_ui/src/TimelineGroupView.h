#pragma once

#include "engine/MusicContext.h"
#include "PeakRegistry.h"
#include "ViewportSampler.h"
#include <QWidget>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class AppModel;

class TimelineGroupView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineGroupView(AppModel* model, QWidget* parent = nullptr);
    ~TimelineGroupView() override;

    void setGroupCueIndex(int groupFlatIdx);
    void clearArmCursor();
    void setMusicContext(const mcp::MusicContext* mc);

    QSize sizeHint() const override;

signals:
    void childOffsetChanged(int childFlatIdx, double newOffsetSec);
    void childTrimChanged(int childFlatIdx, double newOffsetSec,
                          double newStartTimeSec, double newDurationSec);
    void rulerClicked(double timeSec);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct ChildBlock {
        int         flatIdx{-1};
        double      offset{0.0};
        double      duration{0.0};
        double      startTime{0.0};
        double      fileDur{0.0};     // total audio file duration (for trim clamping)
        QString     label;
        std::string audioPath;
    };

    enum class DragMode { None, Move, TrimLeft, TrimRight };

    void   rebuildBlocks();
    void   requestPeaks(const std::string& path);
    double pixToSec(int px) const;
    int    secToPix(double sec) const;
    double viewDuration() const;
    void   updateSnapTargets();
    int    laneY(int blockIdx) const;
    void   updateHoverCursor(int px, int py);
    double snapToGrid(double sec) const;

    AppModel* m_model{nullptr};
    int       m_groupIdx{-1};

    std::vector<ChildBlock> m_blocks;

    static constexpr int kRulerH  = 20;
    static constexpr int kBlockH  = 44;
    static constexpr int kLaneGap = 3;
    static constexpr int kTopPad  = 4;
    static constexpr int kHandleW = 6;   // trim-handle hit width in px

    double m_viewStart{0.0};
    double m_pixPerSec{80.0};
    int    m_laneScrollPx{0};

    int       m_selBlock{-1};
    int       m_dragBlock{-1};
    DragMode  m_dragMode{DragMode::None};
    int       m_dragStartX{0};
    double    m_dragStartOffset{0.0};
    double    m_dragStartStartTime{0.0};
    double    m_dragStartDuration{0.0};

    std::vector<double> m_snapTimes;
    double m_armSec{-1.0};

    const mcp::MusicContext* m_mc{nullptr};

    // Snap quantization: 0=none, 1=bar, 2=half, 4=quarter, 8=8th, 16=16th, 32=32nd
    int m_quantSubdiv{0};

    std::unordered_map<std::string, std::shared_ptr<PeakData>>      m_peakData;
    std::unordered_map<std::string, int>                             m_subTokens;
    std::unordered_map<std::string, std::unique_ptr<ViewportSampler>> m_samplers;
};
