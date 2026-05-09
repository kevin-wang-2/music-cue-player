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

    // Returns true when the currently selected row is a built-in (read-only) entry.
    bool isBuiltinRow(int row) const;
    // Maps a list widget row to an index in sf.scriptletLibrary.entries (-1 if built-in).
    int  userEntryIdx(int row) const;

    AppModel*         m_model{nullptr};
    QListWidget*      m_list{nullptr};
    QPushButton*      m_btnRemove{nullptr};
    QPushButton*      m_btnRename{nullptr};
    ScriptEditorWidget* m_editor{nullptr};

    int  m_builtinCount{0};  // number of built-in rows at the top of m_list
    bool m_loading{false};
};
