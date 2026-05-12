#pragma once
#include <QWidget>

class QSlider;
class QLabel;
class QLineEdit;

// Vertical dB fader with a label above and a dB readout below.
// Range: kFaderMin..kFaderMax dB (default -60..+10).
// Double-click the dB label to type a value.
// Double-click the slider track to reset to 0 dB.
//
// When setToggleable(true): single-click on the channel label (top) toggles
// the activated state and emits toggled(bool). Use this for fade targets so
// the user can click the label to enable/disable without a separate checkbox.
class FaderWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(float value READ value WRITE setValue NOTIFY valueChanged)
public:
    static constexpr float kFaderMin = -60.0f;   // slider floor (display "-inf" at or below)
    static constexpr float kFaderMax =  10.0f;
    static constexpr float kFaderInf = -144.0f;  // returned by value() at floor; engine maps to 0 linear

    explicit FaderWidget(const QString& label,
                         QWidget* parent = nullptr);

    float  value() const;       // current dB (-60 = -inf floor)
    void   setValue(float dB);  // programmatic set, no signal emit

    // Enable or disable the fader track (greyed out when false).
    void setEnabled(bool en);

    // Toggleable mode: clicking the channel label toggles activated state.
    void setToggleable(bool on);
    bool isActivated() const { return m_activated; }
    void setActivated(bool on);

signals:
    void valueChanged(float dB);
    void toggled(bool activated);  // only emitted in toggleable mode
    void dragStarted();            // emitted once at the start of each drag/edit gesture

protected:
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onSliderMoved(int pos);
    void onEditFinished();

private:
    static constexpr int kSteps = 700;   // maps -60..+10 in 0.1 dB steps

    int   dbToSlider(float dB) const;
    float sliderToDb(int pos) const;
    void  updateLabel();
    void  applyActivatedStyle();

    QSlider*   m_slider{nullptr};
    QLabel*    m_topLabel{nullptr};  // channel name label (clickable in toggleable mode)
    QLabel*    m_label{nullptr};     // dB value display
    QLineEdit* m_edit{nullptr};      // inline text entry (hidden by default)

    bool   m_toggleable{false};
    bool   m_activated{true};
    QPoint m_dragStartPos;
    bool   m_isDragging{false};
};
