#include "ControlsDialog.h"
#include "AppModel.h"
#include "MidiInputManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

static const struct { mcp::ControlAction action; const char* label; } kActions[] = {
    { mcp::ControlAction::Go,            "GO"               },
    { mcp::ControlAction::Arm,           "Arm"              },
    { mcp::ControlAction::PanicSelected, "Panic Selected"   },
    { mcp::ControlAction::SelectionUp,   "Selection Up"     },
    { mcp::ControlAction::SelectionDown, "Selection Down"   },
    { mcp::ControlAction::PanicAll,      "Panic All"        },
};

ControlsDialog::ControlsDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Controls");
    setMinimumWidth(680);

    auto* mainLay = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);

    // Build one ActionRow per action (shared between both tabs)
    for (const auto& a : kActions) {
        ActionRow r;
        r.action = a.action;
        r.label  = a.label;
        m_rows.push_back(r);
    }

    auto* midiPage = new QWidget;
    auto* oscPage  = new QWidget;
    buildMidiTab(midiPage);
    buildOscTab(oscPage);
    tabs->addTab(midiPage, "MIDI Learn");
    tabs->addTab(oscPage,  "OSC");
    mainLay->addWidget(tabs);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, [this]() { save(); accept(); });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(btns);

    load();
}

// ---------------------------------------------------------------------------
void ControlsDialog::buildMidiTab(QWidget* page) {
    auto* lay = new QVBoxLayout(page);
    lay->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    grid->addWidget(new QLabel("Action"),   0, 0);
    grid->addWidget(new QLabel("Enable"),   0, 1);
    grid->addWidget(new QLabel("Type"),     0, 2);
    grid->addWidget(new QLabel("Ch"),       0, 3);
    grid->addWidget(new QLabel("D1"),       0, 4);
    grid->addWidget(new QLabel("D2"),       0, 5);
    grid->addWidget(new QLabel(""),         0, 6);

    for (int i = 0; i < (int)m_rows.size(); ++i) {
        auto& r = m_rows[i];
        const int row = i + 1;
        grid->addWidget(new QLabel(r.label), row, 0);
        r.midiEnable  = new QCheckBox;
        r.midiType    = new QComboBox;
        for (const char* s : {"Note On","Note Off","Control Change","Program Change","Pitch Bend"})
            r.midiType->addItem(s);
        r.midiCh = new QSpinBox; r.midiCh->setRange(0,16); r.midiCh->setSpecialValueText("Any");
        r.midiD1 = new QSpinBox; r.midiD1->setRange(0,127);
        r.midiD2 = new QSpinBox; r.midiD2->setRange(-1,127); r.midiD2->setSpecialValueText("Any");
        r.midiCapture = new QPushButton("Capture");
        r.midiCapture->setFixedWidth(70);

        grid->addWidget(r.midiEnable,  row, 1, Qt::AlignHCenter);
        grid->addWidget(r.midiType,    row, 2);
        grid->addWidget(r.midiCh,      row, 3);
        grid->addWidget(r.midiD1,      row, 4);
        grid->addWidget(r.midiD2,      row, 5);
        grid->addWidget(r.midiCapture, row, 6);

        auto* capture = r.midiCapture;
        auto& rowRef  = r;
        connect(capture, &QPushButton::clicked, this, [this, capture, &rowRef]() {
            capture->setText("…");
            capture->setEnabled(false);
            m_model->midiIn.armCapture([this, capture, &rowRef](mcp::MidiMsgType t, int ch, int d1, int d2) {
                m_loading = true;
                rowRef.midiType->setCurrentIndex(static_cast<int>(t));
                rowRef.midiCh->setValue(ch);
                rowRef.midiD1->setValue(d1);
                rowRef.midiD2->setValue(d2);
                m_loading = false;
                capture->setText("Capture");
                capture->setEnabled(true);
            });
        });
    }

    lay->addLayout(grid);
    lay->addStretch();
}

void ControlsDialog::buildOscTab(QWidget* page) {
    auto* lay = new QVBoxLayout(page);
    lay->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    grid->addWidget(new QLabel("Action"), 0, 0);
    grid->addWidget(new QLabel("Enable"), 0, 1);
    grid->addWidget(new QLabel("OSC Path"), 0, 2);

    for (int i = 0; i < (int)m_rows.size(); ++i) {
        auto& r = m_rows[i];
        const int row = i + 1;
        grid->addWidget(new QLabel(r.label), row, 0);
        r.oscEnable = new QCheckBox;
        r.oscPath   = new QLineEdit;
        r.oscPath->setPlaceholderText("/my/go");
        grid->addWidget(r.oscEnable, row, 1, Qt::AlignHCenter);
        grid->addWidget(r.oscPath,   row, 2);
    }

    lay->addLayout(grid);
    lay->addWidget(new QLabel(
        "Note: these paths may conflict with the system vocabulary (/go, /start, /stop, …)\n"
        "but will still work — system vocabulary always takes priority."));
    lay->addStretch();
}

// ---------------------------------------------------------------------------
void ControlsDialog::load() {
    m_loading = true;
    for (auto& r : m_rows) {
        const auto* e = m_model->sf.systemControls.find(r.action);
        if (e) {
            r.midiEnable->setChecked(e->midi.enabled);
            r.midiType->setCurrentIndex(static_cast<int>(e->midi.type));
            r.midiCh->setValue(e->midi.channel);
            r.midiD1->setValue(e->midi.data1);
            r.midiD2->setValue(e->midi.data2);
            r.oscEnable->setChecked(e->osc.enabled);
            r.oscPath->setText(QString::fromStdString(e->osc.path));
        }
    }
    m_loading = false;
}

void ControlsDialog::save() {
    for (const auto& r : m_rows) {
        auto& entry = m_model->sf.systemControls.get(r.action);
        entry.midi.enabled = r.midiEnable->isChecked();
        entry.midi.type    = static_cast<mcp::MidiMsgType>(r.midiType->currentIndex());
        entry.midi.channel = r.midiCh->value();
        entry.midi.data1   = r.midiD1->value();
        entry.midi.data2   = r.midiD2->value();
        entry.osc.enabled  = r.oscEnable->isChecked();
        entry.osc.path     = r.oscPath->text().toStdString();
    }
    m_model->markDirty();
    emit m_model->dirtyChanged(true);
}
