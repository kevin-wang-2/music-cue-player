#include "RenumberDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

static QString fmtNum(double v) {
    // Show integer without decimal point, otherwise up to 4 sig digits trimmed.
    if (v == std::floor(v) && std::abs(v) < 1e9)
        return QString::number(static_cast<long long>(v));
    return QString::number(v, 'g', 4);
}

RenumberDialog::RenumberDialog(int selectedCount, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Renumber Cues");
    setMinimumWidth(320);

    const QString dark =
        "QDialog{background:#1a1a1a;color:#ddd;}"
        "QGroupBox{color:#aaa;border:1px solid #333;border-radius:4px;"
        "  margin-top:8px;padding-top:6px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}"
        "QCheckBox{color:#ddd;spacing:6px;}"
        "QCheckBox::indicator{width:14px;height:14px;border:1px solid #555;border-radius:2px;background:#222;}"
        "QCheckBox::indicator:checked{background:#2a6ab8;border-color:#2a6ab8;}"
        "QDoubleSpinBox,QLineEdit{background:#252525;color:#ddd;border:1px solid #444;"
        "  border-radius:3px;padding:2px 5px;}"
        "QDoubleSpinBox:disabled,QLineEdit:disabled{color:#555;border-color:#333;}"
        "QLabel{color:#aaa;}"
        "QLabel#preview{color:#88aaff;font-style:italic;}"
        "QPushButton{background:#2a2a2a;color:#ddd;border:1px solid #444;border-radius:3px;"
        "  padding:4px 16px;}"
        "QPushButton:hover{background:#383838;}"
        "QPushButton:default{background:#1a4a88;border-color:#2a6ab8;}";
    setStyleSheet(dark);

    auto* vlay = new QVBoxLayout(this);
    vlay->setSpacing(10);

    // Header
    auto* hdr = new QLabel(
        QString("Applies to <b>%1</b> selected cue%2.")
            .arg(selectedCount).arg(selectedCount == 1 ? "" : "s"));
    hdr->setStyleSheet("color:#bbb;");
    vlay->addWidget(hdr);

    // ── Renumber group ────────────────────────────────────────────────────────
    auto* grpRe = new QGroupBox;
    {
        auto* hdr2 = new QHBoxLayout;
        m_chkRenumber = new QCheckBox("Renumber");
        m_chkRenumber->setChecked(true);
        hdr2->addWidget(m_chkRenumber);
        hdr2->addStretch();

        auto* form = new QFormLayout;
        form->setSpacing(5);
        m_spinStart = new QDoubleSpinBox;
        m_spinStart->setRange(-9999999, 9999999);
        m_spinStart->setDecimals(2);
        m_spinStart->setSingleStep(1.0);
        m_spinStart->setValue(1.0);
        m_spinStart->setMaximumWidth(100);

        m_spinIncrement = new QDoubleSpinBox;
        m_spinIncrement->setRange(-9999, 9999);
        m_spinIncrement->setDecimals(2);
        m_spinIncrement->setSingleStep(0.5);
        m_spinIncrement->setValue(1.0);
        m_spinIncrement->setMaximumWidth(100);

        auto* lblStart = new QLabel("Start at:");
        auto* lblInc   = new QLabel("Increment by:");
        form->addRow(lblStart, m_spinStart);
        form->addRow(lblInc,   m_spinIncrement);

        auto* vl = new QVBoxLayout(grpRe);
        vl->addLayout(hdr2);
        vl->addLayout(form);

        // Enable/disable the spinners based on checkbox
        auto toggleRe = [=](bool on) {
            m_spinStart->setEnabled(on);
            m_spinIncrement->setEnabled(on);
            lblStart->setEnabled(on);
            lblInc->setEnabled(on);
            updatePreview();
        };
        connect(m_chkRenumber, &QCheckBox::toggled, this, toggleRe);
    }
    vlay->addWidget(grpRe);

    // ── Prefix group ──────────────────────────────────────────────────────────
    auto* grpPre = new QGroupBox;
    {
        auto* hl = new QHBoxLayout(grpPre);
        m_chkPrefix = new QCheckBox("Prefix");
        m_editPrefix = new QLineEdit;
        m_editPrefix->setEnabled(false);
        m_editPrefix->setPlaceholderText("e.g.  A-");
        hl->addWidget(m_chkPrefix);
        hl->addWidget(m_editPrefix);

        connect(m_chkPrefix, &QCheckBox::toggled, this, [=](bool on) {
            m_editPrefix->setEnabled(on);
            updatePreview();
        });
        connect(m_editPrefix, &QLineEdit::textChanged, this, [=]{ updatePreview(); });
    }
    vlay->addWidget(grpPre);

    // ── Suffix group ──────────────────────────────────────────────────────────
    auto* grpSuf = new QGroupBox;
    {
        auto* hl = new QHBoxLayout(grpSuf);
        m_chkSuffix = new QCheckBox("Suffix");
        m_editSuffix = new QLineEdit;
        m_editSuffix->setEnabled(false);
        m_editSuffix->setPlaceholderText("e.g.  -B");
        hl->addWidget(m_chkSuffix);
        hl->addWidget(m_editSuffix);

        connect(m_chkSuffix, &QCheckBox::toggled, this, [=](bool on) {
            m_editSuffix->setEnabled(on);
            updatePreview();
        });
        connect(m_editSuffix, &QLineEdit::textChanged, this, [=]{ updatePreview(); });
    }
    vlay->addWidget(grpSuf);

    // ── Preview ───────────────────────────────────────────────────────────────
    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#333;");
    vlay->addWidget(sep);

    m_lblPreview = new QLabel;
    m_lblPreview->setObjectName("preview");
    m_lblPreview->setWordWrap(true);
    vlay->addWidget(m_lblPreview);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText("Apply");
    btns->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    vlay->addWidget(btns);

    // Connect remaining signals for live preview
    connect(m_spinStart,     QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [=]{ updatePreview(); });
    connect(m_spinIncrement, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [=]{ updatePreview(); });

    updatePreview();
}

void RenumberDialog::updatePreview() {
    const bool re  = m_chkRenumber->isChecked();
    const bool pre = m_chkPrefix->isChecked();
    const bool suf = m_chkSuffix->isChecked();

    if (!re && !pre && !suf) {
        m_lblPreview->setText("No changes will be made.");
        return;
    }

    const double start = m_spinStart->value();
    const double inc   = m_spinIncrement->value();
    const QString pfx  = pre ? m_editPrefix->text()  : QString{};
    const QString sfx  = suf ? m_editSuffix->text()  : QString{};

    QStringList examples;
    for (int i = 0; i < 4; ++i) {
        QString base = re ? fmtNum(start + i * inc) : QString("N");
        examples << pfx + base + sfx;
    }
    m_lblPreview->setText("Preview: " + examples.join(", ") + ", …");
}

bool    RenumberDialog::renumberChecked() const { return m_chkRenumber->isChecked(); }
double  RenumberDialog::startAt()         const { return m_spinStart->value(); }
double  RenumberDialog::incrementBy()     const { return m_spinIncrement->value(); }
bool    RenumberDialog::prefixChecked()   const { return m_chkPrefix->isChecked(); }
QString RenumberDialog::prefix()          const { return m_editPrefix->text(); }
bool    RenumberDialog::suffixChecked()   const { return m_chkSuffix->isChecked(); }
QString RenumberDialog::suffix()          const { return m_editSuffix->text(); }
