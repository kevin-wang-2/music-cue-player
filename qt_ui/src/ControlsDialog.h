#pragma once

#include "engine/TriggerData.h"
#include <QDialog>
#include <vector>

class AppModel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

// System Controls dialog — two tabs:
//   MIDI Learn: assign MIDI messages to system actions (GO, Arm, Panic, …)
//   OSC:        assign custom OSC paths to the same actions
class ControlsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ControlsDialog(AppModel* model, QWidget* parent = nullptr);

private:
    void buildMidiTab(QWidget* page);
    void buildOscTab(QWidget* page);
    void load();
    void save();

    struct ActionRow {
        mcp::ControlAction action;
        QString            label;
        // MIDI
        QCheckBox* midiEnable{nullptr};
        QComboBox* midiType{nullptr};
        QSpinBox*  midiCh{nullptr};
        QSpinBox*  midiD1{nullptr};
        QSpinBox*  midiD2{nullptr};
        QPushButton* midiCapture{nullptr};
        // OSC
        QCheckBox* oscEnable{nullptr};
        QLineEdit* oscPath{nullptr};
    };
    std::vector<ActionRow> m_rows;

    AppModel* m_model{nullptr};
    bool m_loading{false};
};
