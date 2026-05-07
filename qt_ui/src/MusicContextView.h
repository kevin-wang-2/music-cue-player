#pragma once

#include "engine/MusicContext.h"
#include <QWidget>

class AppModel;

// Two-track tempo-map editor: ruler (bar|beat), BPM curve, time-signature blocks.
// Embedded inside the inspector's Music Context tab.
// The property panel for the selected point is kept in InspectorWidget.
class MusicContextView : public QWidget {
    Q_OBJECT
public:
    explicit MusicContextView(AppModel* model, QWidget* parent = nullptr);

    void setCueIndex(int idx);
    int  selectedPoint() const { return m_selPt; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void pointSelected(int ptIdx);   // -1 = deselected
    void mcChanged();                // MC data was edited; caller syncs ShowFile

protected:
    void paintEvent(QPaintEvent*)    override;
    void mousePressEvent(QMouseEvent*)   override;
    void mouseMoveEvent(QMouseEvent*)    override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*)    override;
    void keyPressEvent(QKeyEvent*)   override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*)         override;
    void resizeEvent(QResizeEvent*)  override;

private:
    // Track geometry (all in pixels)
    static constexpr int kRulerH = 20;
    static constexpr int kTempoH = 76;
    static constexpr int kTSH    = 30;
    static constexpr int kPtRad  = 5;

    int tempoTop() const { return kRulerH; }
    int tsTop()    const { return kRulerH + kTempoH + 2; }
    int totalH()   const { return kRulerH + kTempoH + 2 + kTSH; }

    struct BpmRange { double lo{60.0}; double hi{180.0}; };

    mcp::MusicContext* getMC() const;

    double qnToX(double qn) const;
    double xToQN(double x)  const;
    int    bpmToY(double bpm, BpmRange) const;
    double yToBpm(double y,   BpmRange) const;
    BpmRange computeBpmRange() const;

    void   fitView();
    double snapQN(double qn) const;
    int    hitTestPoint(int x, int y) const;   // point index or -1
    int    hitTestSegment(int x, int y) const; // segment index or -1

    void paintRuler(QPainter&) const;
    void paintTempoTrack(QPainter&, BpmRange) const;
    void paintTSTrack(QPainter&) const;

    AppModel* m_model{nullptr};
    int       m_cueIdx{-1};

    double m_viewStartQN{-2.0};
    double m_pixPerQN{40.0};

    int m_selPt{-1};
    int m_hovPt{-1};

    // Drag
    bool   m_dragging{false};
    bool   m_didMove{false};
    int    m_dragPt{-1};
    double m_dragPressX{0.0};
    double m_dragPressY{0.0};
    double m_dragOrigQN{0.0};
    double m_dragOrigBpm{0.0};
    bool   m_dragHorz{false};
    bool   m_dragVert{false};

    // Edit quantization: 0=none, 1=bar, 2=half, 4=quarter, 8=8th, 16=16th, 32=32nd
    int m_quantSubdiv{1};
};
