#pragma once

#include <QWidget>
#include <QString>

class PythonEditor;
class QFileSystemWatcher;
class QPushButton;

// Composite widget: PythonEditor + "Open in Editor" toolbar + file-watcher sync.
// Owns the temp-file lifecycle.  Call setContext() each time the edited entity
// changes (different cue, different library module) so the watcher follows.
class ScriptEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit ScriptEditorWidget(QWidget* parent = nullptr);

    // Set code without emitting codeChanged (for programmatic loads).
    void setCode(const QString& code);
    QString code() const;

    // Set the stem used for the temp file, e.g. "cue_5" or "lib_utils".
    // Stops watching any previously-watched file when the stem changes.
    void setContext(const QString& stem);

    // Error-line highlighting delegated to the inner PythonEditor.
    void markErrorLine(int line);
    void clearErrorLines();

    PythonEditor* editor() const { return m_editor; }

signals:
    // Emitted on every keystroke (text changed in the editor).
    void codeChanged(const QString& code);

private:
    void openInExternalEditor();

    PythonEditor*       m_editor{nullptr};
    QFileSystemWatcher* m_watcher{nullptr};
    QString             m_currentStem;
    QString             m_tempPath;
};
