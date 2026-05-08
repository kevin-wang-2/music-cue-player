#pragma once

#include <QDialog>

class AppModel;
class ScriptEditorWidget;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// Modeless dialog for managing the scriptlet library.
// Each entry is a named Python module importable as `import mcp.library.<name>`.
class ScriptletLibraryDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScriptletLibraryDialog(AppModel* model, QWidget* parent = nullptr);

    void refreshList();   // rebuild list from model data

private slots:
    void onAdd();
    void onRemove();
    void onRename();
    void onSelectionChanged();

private:
    void saveCurrentCode(const QString& code);  // write code back to the selected entry
    int  currentEntryIndex() const;

    AppModel*         m_model{nullptr};
    QListWidget*      m_list{nullptr};
    QPushButton*      m_btnRemove{nullptr};
    QPushButton*      m_btnRename{nullptr};
    ScriptEditorWidget* m_editor{nullptr};

    bool m_loading{false};
};
