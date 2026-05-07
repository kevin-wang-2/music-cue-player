#pragma once

#include <QWidget>
#include <string>
#include <unordered_map>
#include <vector>

class AppModel;
class QLineEdit;

// Visual editor for a Synchronization-mode Group cue.
// Displays a ruler, child-cue blocks at their timeline offsets (like
// TimelineGroupView), and group-level slice markers + loop-count strip
// (like WaveformView).  Right-click adds / deletes markers; double-click
// the loop strip edits the count inline.
class SyncGroupView : public QWidget {
    Q_OBJECT
public:
    explicit SyncGroupView(AppModel* model, QWidget* parent = nullptr);

    void setGroupCueIndex(int groupFlatIdx);
    void clearSelMarker();

    QSize sizeHint() const override;

signals:
    void markerSelected(int markerIdx);   // -1 = deselected
    void cueModified();                   // model was changed; caller should emit cueEdited()

protected:
    void paintEvent(QPaintEvent*)            override;
    void mousePressEvent(QMouseEvent*)       override;
    void mouseMoveEvent(QMouseEvent*)        override;
    void mouseReleaseEvent(QMouseEvent*)     override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*)            override;
    void contextMenuEvent(QContextMenuEvent*)override;
    void resizeEvent(QResizeEvent*)          override;

private:
    struct ChildBlock {
        int         flatIdx{-1};
        double      offset{0.0};
        double      duration{0.0};
        double      startTime{0.0};
        QString     label;
        std::string audioPath;
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
    double viewDuration() const;
    double pixToSec(double px) const;
    int    secToPix(double sec) const;
    int    laneY(int i) const;
    void   startLoopEdit(int sliceIdx, int blX, int brX, const QString& current);
    void   commitLoopEdit();

    static constexpr int kRulerH  = 20;
    static constexpr int kBlockH  = 44;
    static constexpr int kLaneGap = 3;
    static constexpr int kTopPad  = 4;
    static constexpr int kLoopH   = 18;

    AppModel* m_model{nullptr};
    int       m_groupIdx{-1};

    std::vector<ChildBlock> m_blocks;
    std::unordered_map<std::string, PeakCache> m_peakCache;

    double m_viewStart{0.0};
    double m_pixPerSec{80.0};
    int    m_laneScrollPx{0};   // vertical scroll offset for the lane area

    // Child block drag
    int    m_dragBlock{-1};
    int    m_dragStartX{0};
    double m_dragStartOffset{0.0};

    // Marker drag: -2 = none, >= 0 = marker index
    int    m_dragMarker{-2};
    double m_dragMarkerOrig{0.0};
    int    m_dragMarkerPxOrig{0};
    int    m_selMarker{-1};

    // Right-button pan
    double m_panOriginX{0.0};
    double m_panViewStart{0.0};

    // Loop count inline editor
    QLineEdit* m_loopEdit{nullptr};
    int        m_editLoopSlice{-1};
};
