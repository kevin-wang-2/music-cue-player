#pragma once

#include <QWidget>

class AppModel;
class QLabel;
class QListWidget;
class QPushButton;

// Collapsible right-sidebar that lists all CueLists with status icons.
//
// Status icons (leftmost column of each item):
//   ▶ green  — list has at least one playing cue
//   ◆ yellow — list has at least one armed cue (and none playing)
//   ✕ red    — list has at least one broken cue
//   · grey   — idle
//
// Clicking a row calls AppModel::setActiveList().
// "+ New" / rename (double-click) / "– Del" manage the list collection.
class CueListPanel : public QWidget {
    Q_OBJECT
public:
    explicit CueListPanel(AppModel* model, QWidget* parent = nullptr);

    // Called by MainWindow::onTick() every frame to refresh status icons.
    void refresh();

public slots:
    void onModelListsChanged();

private slots:
    void onListClicked(int row);
    void onListDoubleClicked(int row);
    void onAddList();
    void onDeleteList();

private:
    void rebuild();

    AppModel*    m_model{nullptr};
    QListWidget* m_list{nullptr};
    QPushButton* m_btnAdd{nullptr};
    QPushButton* m_btnDel{nullptr};
};
