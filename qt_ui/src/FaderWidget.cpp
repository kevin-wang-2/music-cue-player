#include "FaderWidget.h"

#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <algorithm>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>

FaderWidget::FaderWidget(const QString& label, QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);

    m_topLabel = new QLabel(label, this);
    m_topLabel->setAlignment(Qt::AlignHCenter);
    lay->addWidget(m_topLabel);

    m_slider = new QSlider(Qt::Vertical, this);
    m_slider->setRange(0, kSteps);
    m_slider->setValue(dbToSlider(0.0f));
    m_slider->setFixedWidth(28);
    m_slider->setMinimumHeight(80);
    connect(m_slider, &QSlider::valueChanged, this, &FaderWidget::onSliderMoved);
    connect(m_slider, &QSlider::sliderPressed, this, [this]() {
        if (!m_toggleable) emit dragStarted();
    });
    lay->addWidget(m_slider, 1, Qt::AlignHCenter);

    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignHCenter);
    m_label->setToolTip("Double-click to type a value");
    lay->addWidget(m_label);

    m_edit = new QLineEdit(this);
    m_edit->hide();
    m_edit->setFixedWidth(52);
    m_edit->setAlignment(Qt::AlignHCenter);
    connect(m_edit, &QLineEdit::editingFinished, this, &FaderWidget::onEditFinished);
    lay->addWidget(m_edit, 0, Qt::AlignHCenter);

    updateLabel();
    setFixedWidth(60);
}

float FaderWidget::value() const {
    const float dB = sliderToDb(m_slider->value());
    // At the floor the display reads "-inf"; return kFaderInf (-144) so the engine
    // computes dBToLinear(-144) = 0 (true silence), not dBToLinear(-60) ≈ 0.001.
    return (dB <= kFaderMin + 0.05f) ? kFaderInf : dB;
}

void FaderWidget::setValue(float dB) {
    QSignalBlocker blk(m_slider);
    m_slider->setValue(dbToSlider(dB));
    updateLabel();
}

void FaderWidget::setEnabled(bool en) {
    m_slider->setEnabled(en);
    m_label->setEnabled(en);
}

void FaderWidget::setToggleable(bool on) {
    m_toggleable = on;
    if (on) {
        // Make the slider transparent to mouse events so FaderWidget receives
        // all clicks/drags and can distinguish toggle-click from value-drag.
        m_slider->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Click to toggle active / drag to set value");
    }
}

void FaderWidget::setActivated(bool on) {
    m_activated = on;
    applyActivatedStyle();
}

static const char* kSliderBlue =
    "QSlider::groove:vertical{"
    "  background:#1a2e4a; width:8px; border-radius:4px;}"
    "QSlider::handle:vertical{"
    "  background:#2a6ab8; width:16px; height:10px;"
    "  margin:0px -4px; border-radius:3px;}"
    "QSlider::sub-page:vertical{"
    "  background:#2a6ab8; border-radius:4px;}";

static const char* kSliderGray =
    "QSlider::groove:vertical{"
    "  background:#252525; width:8px; border-radius:4px;}"
    "QSlider::handle:vertical{"
    "  background:#404040; width:16px; height:10px;"
    "  margin:0px -4px; border-radius:3px;}"
    "QSlider::sub-page:vertical{"
    "  background:#333; border-radius:4px;}";

void FaderWidget::applyActivatedStyle() {
    if (m_activated) {
        m_topLabel->setStyleSheet("color:#5599ff; font-weight:600;");
        m_slider->setStyleSheet(kSliderBlue);
        m_label->setStyleSheet("color:#aaa;");
    } else {
        m_topLabel->setStyleSheet("color:#505050; text-decoration:line-through;");
        m_slider->setStyleSheet(kSliderGray);
        m_label->setStyleSheet("color:#444;");
    }
}

int FaderWidget::dbToSlider(float dB) const {
    dB = std::max(kFaderMin, std::min(kFaderMax, dB));
    return static_cast<int>(std::round((dB - kFaderMin) / (kFaderMax - kFaderMin) * kSteps));
}

float FaderWidget::sliderToDb(int pos) const {
    return kFaderMin + static_cast<float>(pos) / kSteps * (kFaderMax - kFaderMin);
}

void FaderWidget::updateLabel() {
    const float dB = value();
    char buf[16];
    if (dB <= kFaderMin + 0.05f) std::snprintf(buf, sizeof(buf), "-inf");
    else                         std::snprintf(buf, sizeof(buf), "%+.1f", (double)dB);
    m_label->setText(QString::fromLatin1(buf) + " dB");
}

void FaderWidget::onSliderMoved(int) {
    updateLabel();
    emit valueChanged(value());
}

void FaderWidget::mousePressEvent(QMouseEvent* ev) {
    if (m_toggleable && ev->button() == Qt::LeftButton) {
        m_dragStartPos = ev->pos();
        m_isDragging   = false;
        emit dragStarted();   // fired for both toggle-clicks and drags
        ev->accept();
        return;  // capture; decide toggle vs drag in release/move
    }
    QWidget::mousePressEvent(ev);
}

void FaderWidget::mouseMoveEvent(QMouseEvent* ev) {
    if (m_toggleable && (ev->buttons() & Qt::LeftButton)) {
        if (!m_isDragging &&
            (ev->pos() - m_dragStartPos).manhattanLength() > 4) {
            m_isDragging = true;
        }
        if (m_isDragging && m_slider->geometry().height() > 0) {
            // Map cursor Y to a slider value (vertical: top=max, bottom=min).
            const QRect sr = m_slider->geometry();
            const int   y  = std::max(sr.top(), std::min(sr.bottom(), ev->pos().y()));
            const float t  = 1.0f - static_cast<float>(y - sr.top()) / sr.height();
            const int   v  = static_cast<int>(std::round(t * kSteps));
            QSignalBlocker blk(m_slider);
            m_slider->setValue(std::max(0, std::min(kSteps, v)));
            updateLabel();
            emit valueChanged(value());
        }
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent(ev);
}

void FaderWidget::mouseReleaseEvent(QMouseEvent* ev) {
    if (m_toggleable && ev->button() == Qt::LeftButton) {
        if (!m_isDragging) {
            // Pure click → toggle activated state.
            m_activated = !m_activated;
            applyActivatedStyle();
            emit toggled(m_activated);
        }
        m_isDragging = false;
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void FaderWidget::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (m_slider->geometry().contains(ev->pos())) {
        // Double-click on slider track: reset to 0 dB.
        setValue(0.0f);
        emit valueChanged(0.0f);
    } else {
        // Double-click on label area: open inline text edit.
        char buf[16];
        const float dB = value();
        if (dB <= kFaderMin + 0.05f) std::snprintf(buf, sizeof(buf), "-inf");
        else                         std::snprintf(buf, sizeof(buf), "%+.1f", (double)dB);
        m_edit->setText(QString::fromLatin1(buf));
        m_label->hide();
        m_edit->show();
        m_edit->setFocus();
        m_edit->selectAll();
    }
    ev->accept();
}

void FaderWidget::onEditFinished() {
    m_edit->hide();
    m_label->show();

    // Text entry is a discrete edit — push undo at commit time (not at double-click,
    // since the user may cancel by pressing Escape).
    emit dragStarted();

    QString txt = m_edit->text().trimmed();
    float dB;
    if (txt.compare("-inf", Qt::CaseInsensitive) == 0) {
        dB = kFaderMin;
    } else {
        bool ok;
        dB = txt.toFloat(&ok);
        if (!ok) return;
    }
    dB = std::max(kFaderMin, std::min(kFaderMax, dB));
    setValue(dB);
    emit valueChanged(dB);
}
