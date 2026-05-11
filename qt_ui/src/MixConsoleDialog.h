#pragma once

#include "engine/SnapshotManager.h"
#include <QDialog>
#include <set>
#include <vector>

class AppModel;
class QEvent;
class QFrame;
class QToolButton;
class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QScrollArea;
class QSlider;
class QTimer;
class QWidget;
class PeakMeterWidget;  // defined in MixConsoleDialog.cpp
class XpCellWidget;     // defined in MixConsoleDialog.cpp

// Non-modal mixing console dialog.
//
// Horizontal: one strip per logical channel (or one wide strip per stereo pair).
// Each strip (top→bottom): Polarity, Delay, Routing (collapsible), Mute, Fader, Name.
// Linked stereo pairs share one fader/mute; routing toggles between L and R view.
//
// Selection: single-click name → select strip (Cmd = toggle, Shift = range).
// Option key held while moving any control → same delta applied to all selected strips.
//
// All controls write to sf.audioSetup immediately and call applyMixing().
class MixConsoleDialog : public QDialog {
    Q_OBJECT
public:
    explicit MixConsoleDialog(AppModel* model, QWidget* parent = nullptr);

    void refresh();
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildConsole();
    void refreshValues();   // partial update — widget values only, no rebuild
    void applyAll();

    void applyChannelDsp(int ch);
    void applyXp(int ch, int out, float db);
    void applyFader(int ch, int sliderVal);
    void applyMute(int ch, bool muted);
    void applyLink(int ch, bool linked);
    void addChannel();
    void removeSelectedChannels();

    void selectStrip(int ch, Qt::KeyboardModifiers mods);
    void updateStripVisual(int ch);

    // Save the name of channel `ch` from its QLineEdit and exit edit mode.
    void commitNameEdit(int ch, bool isSlave);

    // Snapshot UI
    void storeSnapshot();
    void recallSnapshot();
    void updateSnapToolbar();
    void openSnapshotList();
    void openScopeEditor(int snapIdx);

    struct Strip {
        int ch{-1};
        int slaveCh{-1};       // = ch+1 if linkedStereo, else -1
        int routingView{0};    // 0 = master/L, 1 = slave/R (linked only)
        int lastFaderVal{0};   // previous slider value — used for Option-key delta sync

        QFrame*           frame{nullptr};
        QPushButton*      phaseBtn{nullptr};
        QPushButton*      phaseBtn2{nullptr};    // slave phase (linked only)
        QDoubleSpinBox*   delaySpin{nullptr};
        QComboBox*        delayUnit{nullptr};
        QPushButton*      routingHdr{nullptr};
        QWidget*          routingBody{nullptr};
        QWidget*          routingBody2{nullptr}; // slave routing body (linked only)
        std::vector<XpCellWidget*> xpCells;
        std::vector<XpCellWidget*> xpCells2;     // slave xpCells (linked only)
        QPushButton*      muteBtn{nullptr};
        QSlider*          fader{nullptr};
        PeakMeterWidget*  peakMeter{nullptr};
        PeakMeterWidget*  peakMeter2{nullptr};   // slave meter (linked only)
        QLineEdit*        gainLabel{nullptr};
        QLineEdit*        nameLabel{nullptr};
        QLineEdit*        nameLabel2{nullptr};   // slave name (linked only)
        QPushButton*      linkBtn{nullptr};
    };

    AppModel*    m_model{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget*     m_content{nullptr};
    QTimer*      m_meterTimer{nullptr};
    bool         m_routingVisible{true};
    std::vector<Strip> m_strips;

    std::set<int> m_selectedChs;
    int           m_selectionAnchor{-1};

    // Snapshot toolbar controls (created once in constructor)
    QPushButton*  m_snapPrevBtn{nullptr};
    QPushButton*  m_snapNameBtn{nullptr};
    QPushButton*  m_snapNextBtn{nullptr};
    QToolButton*  m_snapStoreBtn{nullptr};
    QPushButton*  m_snapRecallBtn{nullptr};
};
