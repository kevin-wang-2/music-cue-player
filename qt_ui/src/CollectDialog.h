#pragma once
#include "ShowHelpers.h"
#include <QDialog>

class QLineEdit;
class QCheckBox;
class QComboBox;
class QGroupBox;

class AppModel;

// ─── CollectDialog ────────────────────────────────────────────────────────────
// Shows collection options. On accept, runs collectAllFiles() with a progress
// dialog. Returns QDialog::Accepted / Rejected.
class CollectDialog : public QDialog {
    Q_OBJECT
public:
    explicit CollectDialog(AppModel* model, QWidget* parent = nullptr);

private slots:
    void onBrowse();
    void onCollect();
    void onConvertToggled(bool checked);

private:
    AppModel*  m_model;
    QLineEdit* m_destEdit;
    QCheckBox* m_convertCheck;
    QGroupBox* m_fmtBox;
    QComboBox* m_srCombo;
    QComboBox* m_bdCombo;
};
