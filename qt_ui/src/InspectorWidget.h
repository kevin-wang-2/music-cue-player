#pragma once

#include "engine/TriggerData.h"
#include <QTabWidget>
#include <QWidget>
#include <vector>

class AppModel;
class FaderWidget;
class MusicContextView;
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
class QPlainTextEdit;
class PythonEditor;
class QPushButton;
class QScrollArea;
class QSpinBox;
class ScriptEditorWidget;

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
    bool eventFilter(QObject* obj, QEvent* ev) override;

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
    void buildMCTab();
    void buildScriptTab();
    void loadScript();
    void refreshScriptErrors();   // re-apply error highlight for current cue
    void buildNetworkTab();
    void loadNetwork();
    void buildMidiTab();
    void loadMidi();
    void updateMidiFields();   // show/hide rows based on message type
    void buildTimecodeTab();
    void loadTimecode();
    void updateTimecodeFields();  // show/hide LTC/MTC rows based on type
    void buildMarkerTab();
    void buildTriggersTab();
    void loadTriggers();
    void saveTriggers();       // writes edited values back to ShowFile + CueList

    void loadBasic();
    void loadLevels();
    void loadTrim();
    void loadTime();
    void loadCurve();
    void loadMode();
    void loadSyncSection();
    void loadMCPropPanel();

    void rebuildLevelsForCue();   // re-creates fader layout on cue change
    void refreshMarkerTargetCombo();   // repopulate m_comboMarkerTarget
    void refreshMarkerMkIdxCombo();    // repopulate m_comboMarkerMkIdx (after target change)
    void refreshMarkerAnchorCombo();   // repopulate m_comboMarkerAnchor for current marker

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
    QWidget*     m_mcPage{nullptr};
    QWidget*     m_markerPage{nullptr};

    // Basic tab controls
    QLineEdit*    m_editNum{nullptr};
    QLineEdit*    m_editName{nullptr};
    QDoubleSpinBox* m_spinPreWait{nullptr};
    QDoubleSpinBox* m_spinDurationBasic{nullptr};  // shown for Fade/Group (Audio uses Time tab)
    QComboBox*    m_comboGoQuantize{nullptr};
    QCheckBox*    m_chkAutoCont{nullptr};
    QCheckBox*    m_chkAutoFollow{nullptr};
    // Devamp-specific
    QWidget*      m_devampGroup{nullptr};
    QComboBox*    m_comboDevampMode{nullptr};
    QCheckBox*    m_chkDevampPreVamp{nullptr};
    // Arm-specific
    QWidget*      m_armGroup{nullptr};
    QDoubleSpinBox* m_spinArmStart{nullptr};
    // Marker-cue-specific (lives in Marker tab)
    QComboBox*  m_comboMarkerTargetList{nullptr};  // which list to target
    QComboBox*  m_comboMarkerTarget{nullptr};
    QComboBox*  m_comboMarkerMkIdx{nullptr};

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
    QComboBox*      m_comboMarkerAnchor{nullptr};  // anchor Marker cue selection
    int             m_selMarker{-1};

    // Curve tab
    QComboBox*    m_comboCurve{nullptr};
    QCheckBox*    m_chkStopWhenDone{nullptr};

    // Mode tab (group cues)
    QComboBox*    m_comboGroupMode{nullptr};
    QCheckBox*    m_chkGroupRandom{nullptr};

    // Timeline tab (Timeline group cues)
    TimelineGroupView* m_timelineView{nullptr};

    // Script tab (Scriptlet cues)
    QWidget*           m_scriptPage{nullptr};
    ScriptEditorWidget* m_editScript{nullptr};

    // Network tab
    QWidget*       m_networkPage{nullptr};
    QComboBox*     m_comboPatch{nullptr};
    QPlainTextEdit* m_editNetCmd{nullptr};

    // MIDI tab
    QWidget*    m_midiPage{nullptr};
    QComboBox*  m_comboMidiPatch{nullptr};
    QComboBox*  m_comboMidiType{nullptr};
    QSpinBox*   m_spinMidiCh{nullptr};
    QLabel*     m_lblMidiNote{nullptr};   QSpinBox* m_spinMidiNote{nullptr};
    QLabel*     m_lblMidiVel{nullptr};    QSpinBox* m_spinMidiVel{nullptr};
    QLabel*     m_lblMidiProg{nullptr};   QSpinBox* m_spinMidiProg{nullptr};
    QLabel*     m_lblMidiCC{nullptr};     QSpinBox* m_spinMidiCC{nullptr};
    QLabel*     m_lblMidiCCVal{nullptr};  QSpinBox* m_spinMidiCCVal{nullptr};
    QLabel*     m_lblMidiBend{nullptr};   QSpinBox* m_spinMidiBend{nullptr};

    // Timecode tab
    QWidget*    m_timecodePage{nullptr};
    QComboBox*  m_comboTcType{nullptr};
    QComboBox*  m_comboTcFps{nullptr};
    QLineEdit*  m_editTcStart{nullptr};
    QLineEdit*  m_editTcEnd{nullptr};
    QLabel*     m_lblTcDuration{nullptr};
    // LTC-specific
    QWidget*    m_ltcRow{nullptr};
    QLabel*     m_lblLtcCh{nullptr};
    QSpinBox*   m_spinLtcCh{nullptr};
    // MTC-specific
    QWidget*    m_mtcRow{nullptr};
    QLabel*     m_lblMtcPatch{nullptr};
    QComboBox*  m_comboMtcPatch{nullptr};

    // Music Context tab
    QCheckBox*         m_chkAttachMC{nullptr};
    QWidget*           m_mcContent{nullptr};
    QCheckBox*         m_chkApplyBefore{nullptr};
    MusicContextView*  m_mcView{nullptr};
    QWidget*           m_mcPropGroup{nullptr};    // property panel (hidden when no selection)
    QComboBox*         m_comboPtType{nullptr};
    QDoubleSpinBox*    m_spinPtBpm{nullptr};
    QSpinBox*          m_spinTSNum{nullptr};
    QSpinBox*          m_spinTSDen{nullptr};
    QCheckBox*         m_chkTSInherit{nullptr};
    QLabel*            m_lblPtPos{nullptr};
    int                m_selMCPt{-1};

    // Triggers tab
    QWidget*    m_triggersPage{nullptr};
    // Hotkey
    QCheckBox*  m_chkHotkeyEnable{nullptr};
    QLineEdit*  m_editHotkey{nullptr};
    QPushButton* m_btnHotkeyClear{nullptr};
    // MIDI trigger
    QCheckBox*  m_chkMidiTrigEnable{nullptr};
    QComboBox*  m_comboMidiTrigType{nullptr};
    QSpinBox*   m_spinMidiTrigCh{nullptr};
    QSpinBox*   m_spinMidiTrigD1{nullptr};
    QSpinBox*   m_spinMidiTrigD2{nullptr};
    QPushButton* m_btnMidiCapture{nullptr};
    // OSC trigger
    QCheckBox*  m_chkOscTrigEnable{nullptr};
    QLineEdit*  m_editOscPath{nullptr};

    bool m_loading{false};   // guard re-entrant signals during load
    bool m_hotkeyCapturing{false};
};
