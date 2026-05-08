#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>

class AppModel;
class CueTableView;
class InspectorWidget;
class ProjectStatusDialog;
class QSplitter;
class QTimer;
class QToolButton;

// Top-level application window.
//
// Layout (top to bottom):
//   GoBar    — large GO button (left) + current cue info panel (right)
//   IconBar  — small icon buttons for adding cue types, with hover tooltips
//   Splitter — CueTableView (top, stretches) / InspectorWidget (bottom)
//
// A 16ms QTimer drives tick(), waveform playhead updates, and status refresh.
// Forced dark Fusion theme set in main.cpp.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppModel* model, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onTick();
    void onNewShow();
    void onOpenShow();
    void onSaveShow();
    void onSaveShowAs();
    void onOpenDeviceDialog();
    void onCueListModified();
    void onRowSelected(int idx);
    void onUndo();
    void onRedo();
    void onRenumberCues();
    void onOpenSettings();

private:
    void buildMenuBar();
    void buildGoBar();       // GO button + cue info
    void buildIconBar();     // add-cue icon buttons
    void buildSplitter();

    QToolButton* makeIconBtn(const QString& icon, const QString& tip,
                             const QString& extraStyle = {});

    bool confirmDirty();
    void loadShowFile(const QString& path);
    void updateTitle();
    void updateCueInfo();     // refresh name/notes in GoBar
    void showToast(const QString& msg, int ms = 2500);

    AppModel*             m_model{nullptr};
    CueTableView*         m_cueTable{nullptr};
    InspectorWidget*      m_inspector{nullptr};
    ProjectStatusDialog*  m_statusDialog{nullptr};
    QSplitter*       m_splitter{nullptr};
    QTimer*          m_timer{nullptr};

    // GoBar
    QPushButton* m_goBtn{nullptr};
    QLabel*      m_lblCueName{nullptr};
    QLabel*      m_lblCueDetail{nullptr};   // number · type · duration
    QLabel*      m_lblGlobalMC{nullptr};    // global MC position (hidden when inactive)

    // Toast
    QLabel*  m_toastLabel{nullptr};
    QTimer*  m_toastTimer{nullptr};

    QAction* m_actSave{nullptr};
    QAction* m_actUndo{nullptr};
    QAction* m_actRedo{nullptr};
};
