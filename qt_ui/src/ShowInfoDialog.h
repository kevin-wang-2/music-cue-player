#pragma once
#include <QDialog>

class AppModel;
class QLabel;

// Floating window showing Current Cue, Next Cue, and Current Memo.
// Updated via AppModel::showInfoChanged() and AppModel::selectionChanged().
class ShowInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShowInfoDialog(AppModel* model, QWidget* parent = nullptr);

private slots:
    void refresh();

private:
    static QLabel* makeValueLabel();
    static QString cueText(const QString& number, const QString& name);

    AppModel* m_model{nullptr};

    QLabel* m_lblCurrentCue{nullptr};
    QLabel* m_lblNextCue{nullptr};
    QLabel* m_lblMemo{nullptr};
};
