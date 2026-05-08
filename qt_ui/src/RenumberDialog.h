#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

class RenumberDialog : public QDialog {
    Q_OBJECT
public:
    explicit RenumberDialog(int selectedCount, QWidget* parent = nullptr);

    bool    renumberChecked() const;
    double  startAt() const;
    double  incrementBy() const;
    bool    prefixChecked() const;
    QString prefix() const;
    bool    suffixChecked() const;
    QString suffix() const;

private:
    void updatePreview();

    QCheckBox*      m_chkRenumber{nullptr};
    QDoubleSpinBox* m_spinStart{nullptr};
    QDoubleSpinBox* m_spinIncrement{nullptr};
    QCheckBox*      m_chkPrefix{nullptr};
    QLineEdit*      m_editPrefix{nullptr};
    QCheckBox*      m_chkSuffix{nullptr};
    QLineEdit*      m_editSuffix{nullptr};
    QLabel*         m_lblPreview{nullptr};
};
