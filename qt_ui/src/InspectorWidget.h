#pragma once

#include <QTabWidget>
#include <QWidget>
#include <vector>

class AppModel;
class FaderWidget;
class TimelineGroupView;
class WaveformView;

class SyncGroupView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;

// Right-hand inspector panel.
// Shows a QTabWidget with tabs appropriate to the selected cue type:
//   Basic     — always visible: cue#, name, pre-wait, auto-continue, auto-follow
//   Levels    — audio: master fader + per-output faders + crosspoint matrix
//               fade: master target + per-output targets + XP targets
//   Trim      — audio only: trim fader
//   Time&Loop — audio only: start/duration + WaveformView + per-slice loop counts
//   Curve     — fade only: curve selector + stop-when-done checkbox
// Tabs hidden when not relevant to the cue type.
class InspectorWidget : public QWidget {
    Q_OBJECT
public:
    explicit InspectorWidget(AppModel* model, QWidget* parent = nullptr);

    // Call when selection changes (rebuilds the whole inspector).
    void setCueIndex(int idx);
    // Call on timer tick to update the waveform playhead.
    void updatePlayhead();
    // Clear the timeline arm cursor (call after GO fires or panic/stop).
    void clearTimelineArm();

    int  currentTabIndex() const;
    void restoreTabIndex(int idx);   // restores only if idx is a visible tab

signals:
    void cueEdited();

private slots:
    void onBasicChanged();
    void onLevelFaderChanged(int outCh, float dB);
    void onMasterFaderChanged(float dB);
    void onTrimFaderChanged(float dB);
    void onFadeMasterTargetChanged(float dB);
    void onFadeOutTargetChanged(int outCh, float dB);

private:
    void buildBasicTab();
    void buildLevelsTab();
    void buildTrimTab();
    void buildTimeTab();
    void buildCurveTab();
    void buildModeTab();
    void buildTimelineTab();

    void loadBasic();
    void loadLevels();
    void loadTrim();
    void loadTime();
    void loadCurve();
    void loadMode();
    void loadSyncSection();

    void rebuildLevelsForCue();   // re-creates fader layout on cue change

    AppModel*    m_model{nullptr};
    int          m_cueIdx{-1};

    QTabWidget*  m_tabs{nullptr};

    // Tab page widgets (built once, populated on setCueIndex)
    QWidget*     m_basicPage{nullptr};
    QWidget*     m_levelsPage{nullptr};
    QWidget*     m_trimPage{nullptr};
    QWidget*     m_timePage{nullptr};
    QWidget*     m_curvePage{nullptr};
    QWidget*     m_modePage{nullptr};
    QWidget*     m_timelinePage{nullptr};

    // Basic tab controls
    QLineEdit*    m_editNum{nullptr};
    QLineEdit*    m_editName{nullptr};
    QDoubleSpinBox* m_spinPreWait{nullptr};
    QDoubleSpinBox* m_spinDurationBasic{nullptr};  // shown for Fade/Group (Audio uses Time tab)
    QCheckBox*    m_chkAutoCont{nullptr};
    QCheckBox*    m_chkAutoFollow{nullptr};
    // Devamp-specific
    QWidget*      m_devampGroup{nullptr};
    QComboBox*    m_comboDevampMode{nullptr};
    QCheckBox*    m_chkDevampPreVamp{nullptr};
    // Arm-specific
    QWidget*      m_armGroup{nullptr};
    QDoubleSpinBox* m_spinArmStart{nullptr};

    // Levels tab
    QScrollArea*  m_levelsScroll{nullptr};
    QWidget*      m_levelsContent{nullptr};
    FaderWidget*  m_masterFader{nullptr};
    std::vector<FaderWidget*> m_outFaders;           // per-output channel
    std::vector<std::vector<QLineEdit*>> m_xpCells;  // [srcCh][outCh] crosspoint

    // Fade-level controls (in Levels tab for Fade cues)
    FaderWidget*  m_fadeMasterFader{nullptr};
    std::vector<FaderWidget*> m_fadeOutFaders;
    std::vector<std::vector<QLineEdit*>> m_fadeXpCells;  // [srcCh][outCh]

    // Trim tab
    FaderWidget*  m_trimFader{nullptr};

    // Time & Loop tab
    QWidget*        m_audioTimeSection{nullptr};  // wrapper for audio-only controls
    QDoubleSpinBox* m_spinStart{nullptr};
    QDoubleSpinBox* m_spinDuration{nullptr};
    WaveformView*   m_waveform{nullptr};

    // SyncGroup visual editor (inside Time & Loop tab)
    SyncGroupView* m_syncGroupView{nullptr};

    // Marker editor (shown when a marker is selected in the waveform)
    QWidget*        m_markerPanel{nullptr};
    QLabel*         m_markerLabel{nullptr};
    QDoubleSpinBox* m_markerTimeSpin{nullptr};
    QLineEdit*      m_markerNameEdit{nullptr};
    int             m_selMarker{-1};

    // Curve tab
    QComboBox*    m_comboCurve{nullptr};
    QCheckBox*    m_chkStopWhenDone{nullptr};

    // Mode tab (group cues)
    QComboBox*    m_comboGroupMode{nullptr};
    QCheckBox*    m_chkGroupRandom{nullptr};

    // Timeline tab (Timeline group cues)
    TimelineGroupView* m_timelineView{nullptr};

    bool m_loading{false};   // guard re-entrant signals during load
};
