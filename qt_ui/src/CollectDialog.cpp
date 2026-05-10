#include "CollectDialog.h"
#include "ShowHelpers.h"
#include "AppModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QProgressDialog>
#include <QMessageBox>
#include <QApplication>

CollectDialog::CollectDialog(AppModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Collect All Files");
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);

    // ── Destination ───────────────────────────────────────────────────────────
    auto* destBox = new QGroupBox("Destination folder");
    auto* destRow = new QHBoxLayout(destBox);
    m_destEdit    = new QLineEdit;
    m_destEdit->setPlaceholderText("Choose a folder…");
    auto* browseBtn = new QPushButton("Browse…");
    destRow->addWidget(m_destEdit);
    destRow->addWidget(browseBtn);
    layout->addWidget(destBox);

    connect(browseBtn, &QPushButton::clicked, this, &CollectDialog::onBrowse);

    // ── Convert option ────────────────────────────────────────────────────────
    m_convertCheck = new QCheckBox("Convert all audio files to WAV");
    layout->addWidget(m_convertCheck);

    m_fmtBox = new QGroupBox("Output format");
    m_fmtBox->setEnabled(false);
    auto* fmtForm = new QFormLayout(m_fmtBox);

    m_srCombo = new QComboBox;
    m_srCombo->addItem("44 100 Hz",  44100);
    m_srCombo->addItem("48 000 Hz",  48000);
    m_srCombo->addItem("88 200 Hz",  88200);
    m_srCombo->addItem("96 000 Hz",  96000);
    m_srCombo->setCurrentIndex(1); // 48k default
    fmtForm->addRow("Sample rate:", m_srCombo);

    m_bdCombo = new QComboBox;
    m_bdCombo->addItem("16-bit integer", 16);
    m_bdCombo->addItem("24-bit integer", 24);
    m_bdCombo->addItem("32-bit float",   32);
    m_bdCombo->setCurrentIndex(1); // 24-bit default
    fmtForm->addRow("Bit depth:",    m_bdCombo);

    layout->addWidget(m_fmtBox);
    connect(m_convertCheck, &QCheckBox::toggled,
            this, &CollectDialog::onConvertToggled);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QDialogButtonBox;
    auto* collectBtn = btns->addButton("Collect", QDialogButtonBox::AcceptRole);
    btns->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(btns);

    connect(collectBtn,       &QPushButton::clicked, this, &CollectDialog::onCollect);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CollectDialog::onBrowse() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Choose destination folder", m_destEdit->text());
    if (!dir.isEmpty()) m_destEdit->setText(dir);
}

void CollectDialog::onConvertToggled(bool checked) {
    m_fmtBox->setEnabled(checked);
}

void CollectDialog::onCollect() {
    const QString dest = m_destEdit->text().trimmed();
    if (dest.isEmpty()) {
        QMessageBox::warning(this, "Collect", "Please choose a destination folder.");
        return;
    }

    CollectOptions opts;
    opts.destDir      = dest.toStdString();
    opts.convertToWav = m_convertCheck->isChecked();
    opts.sampleRate   = m_srCombo->currentData().toInt();
    opts.bitDepth     = m_bdCombo->currentData().toInt();

    // Count how many audio files we'll process for the progress bar.
    int total = ShowHelpers::countAudioCues(*m_model);

    QProgressDialog prog("Collecting files…", "Cancel", 0, total, this);
    prog.setWindowTitle("Collect All Files");
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    prog.setValue(0);

    int done = 0;
    std::string err;
    bool ok = ShowHelpers::collectAllFiles(
        *m_model, opts,
        [&](const std::string& filename) {
            prog.setLabelText(QString("Copying: %1").arg(QString::fromStdString(filename)));
            prog.setValue(++done);
            QApplication::processEvents();
        },
        err);

    prog.setValue(total);

    if (prog.wasCanceled()) {
        QMessageBox::information(this, "Collect", "Collection was cancelled.");
        return;
    }

    if (!err.empty()) {
        const QString msg = ok ? "Collection completed with warnings:"
                               : "Collection failed:";
        QMessageBox::warning(this, "Collect", msg + "\n\n" +
                             QString::fromStdString(err));
    } else {
        QMessageBox::information(this, "Collect",
            QString("Done. Files saved to:\n%1").arg(dest));
    }

    if (ok) accept();
}
