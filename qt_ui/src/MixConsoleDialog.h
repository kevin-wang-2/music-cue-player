#pragma once

#include "engine/SnapshotManager.h"
#include <QDialog>
#include <QPointer>
#include <map>
#include <set>
#include <vector>
#include <functional>
#ifdef __APPLE__
#  include "engine/plugin/AUComponentEnumerator.h"
#endif

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
class QVBoxLayout;
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
    // Full teardown + rebuild — call after loading a new show file.
    void resetForNewShow();
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

        QPushButton*      pdcIsoBtn{nullptr};    // PDC isolation toggle

        // Plugin slot section (rebuilt when slots change).
        QPushButton*      pluginHdr{nullptr};   // collapsible header
        QWidget*          pluginBody{nullptr};  // container for slot rows
        std::vector<QPushButton*> pluginSlotBtns;  // one per displayed slot

        // Send slot section (rebuilt when send topology changes).
        QPushButton*      sendHdr{nullptr};     // collapsible header
        QWidget*          sendBody{nullptr};    // container for send slot rows
        std::vector<QWidget*> sendSlotWidgets;  // one container per slot (btn + detail)
    };

    // Plugin slot helpers (use Strip, so declared after it).
    QString pluginHdrText(int ch, bool open) const;
    void buildPluginSection(Strip& s);
    void rebuildPluginSection(int ch);
    void addPluginSlotButton(QVBoxLayout* bl, Strip& s, int ch, int slot);

    // Send slot helpers.
    void buildSendSection(Strip& s);
    void rebuildSendSection(int ch);
    void rebuildSendSlots(Strip& s, QVBoxLayout* bl, int ch);
    void addSendSlotRow(QVBoxLayout* bl, Strip& s, int ch, int slot);
    void openSendEditor(int ch, int slot);
    void closeSendEditor(int ch, int slot);
    void openSendPicker(int ch, int slot);
    void removeSend(int ch, int slot);
    QString sendSlotLabel(int ch, int slot) const;
    void showPluginInfo(int ch, int slot);
    void openPluginPicker(int ch, int slot);
    void removePlugin(int ch, int slot);
    void openPluginEditor(int ch, int slot, bool pinToTop = false);
    // Close editor for (ch, slot) if open; returns true if an editor was closed.
    bool closePluginEditor(int ch, int slot);
    // Move/swap (copy=false) or copy (copy=true) plugin slots after a drag-and-drop.
    void executeDragDrop(int srcCh, int srcSlot, int dstCh, int dstSlot, bool copy = false);

    AppModel*    m_model{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget*     m_content{nullptr};
    QTimer*      m_meterTimer{nullptr};
    bool         m_routingVisible{true};
    std::vector<Strip> m_strips;

    std::set<int> m_selectedChs;
    int           m_selectionAnchor{-1};

    std::map<std::pair<int,int>, QPointer<QDialog>> m_pluginEditors;
    std::map<std::pair<int,int>, QPointer<QDialog>> m_sendEditors;

    // Drag-and-drop state for plugin slot reordering.
    int      m_dragSrcCh{-1};
    int      m_dragSrcSlot{-1};
    QPoint   m_dragStartPos;
    QWidget* m_dragHoverBtn{nullptr};
    QString  m_dragHoverOrigStyle;
    bool     m_dragHoverValid{true};  // false when the hovered drop target is incompatible

    // Per-param cache for diff-based autoscope of AU plugin editors.
    // Keyed by (ch, slot); populated when an editor opens, cleared when it closes.
    using ParamCache = std::vector<std::pair<std::string, float>>;
    std::map<std::pair<int,int>, ParamCache> m_pluginParamCaches;

    // Snapshot helpers for diff-based autoscope.
    // diff: compare cache vs live processor, mark per-param dirty paths for changed params.
    void diffAndMarkPluginDirty(int ch, int slot);
    // reset: rebuild cache from current live processor values (call after recall).
    void resetPluginCache(int ch, int slot);
    // Reset all open editor caches (connected to mixStateChanged signal).
    void resetAllPluginCaches();

#ifdef __APPLE__
    // Lazily populated AU component list (populated on first openPluginPicker call).
    std::vector<mcp::plugin::AUComponentEntry> m_auEntries;
    bool m_auCacheValid{false};
    void ensureAUCache();
#endif

    // Snapshot toolbar controls (created once in constructor)
    QPushButton*  m_snapPrevBtn{nullptr};
    QPushButton*  m_snapNameBtn{nullptr};
    QPushButton*  m_snapNextBtn{nullptr};
    QToolButton*  m_snapStoreBtn{nullptr};
    QPushButton*  m_snapRecallBtn{nullptr};
};
