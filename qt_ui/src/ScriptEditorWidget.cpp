#include "ScriptEditorWidget.h"
#include "PythonEditor.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

ScriptEditorWidget::ScriptEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    // Top bar: hint + external editor button
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);

    auto* hint = new QLabel(
        "import mcp   →   mcp.go()  /  mcp.select(\"5\")  /  mcp.library.<name>");
    hint->setStyleSheet("color:#888; font-size:11px;");
    topRow->addWidget(hint, 1);

    auto* btnExt = new QPushButton("Open in Editor");
    btnExt->setToolTip(
        "Write to a temp file and open in your system editor.\n"
        "Changes are synced back automatically.");
    btnExt->setStyleSheet(
        "QPushButton { background:#2a2a3e; color:#aaa; border:1px solid #444;"
        "  border-radius:2px; padding:2px 8px; font-size:11px; }"
        "QPushButton:hover { background:#3a3a5e; color:#fff; }");
    topRow->addWidget(btnExt);
    lay->addLayout(topRow);

    m_editor = new PythonEditor;
    m_editor->setPlaceholderText("import mcp\n\nmcp.go()");
    lay->addWidget(m_editor, 1);

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this]() {
        emit codeChanged(m_editor->toPlainText());
    });

    connect(btnExt, &QPushButton::clicked, this, &ScriptEditorWidget::openInExternalEditor);
}

void ScriptEditorWidget::setCode(const QString& code) {
    m_editor->blockSignals(true);
    m_editor->setPlainText(code);
    m_editor->blockSignals(false);
}

QString ScriptEditorWidget::code() const {
    return m_editor->toPlainText();
}

void ScriptEditorWidget::setContext(const QString& stem) {
    if (stem == m_currentStem) return;
    m_currentStem = stem;

    // Stop watching the previous file — it belongs to a different entity.
    if (m_watcher && !m_tempPath.isEmpty()) {
        m_watcher->removePath(m_tempPath);
        m_tempPath.clear();
    }
}

void ScriptEditorWidget::markErrorLine(int line) {
    m_editor->markErrorLine(line);
}

void ScriptEditorWidget::clearErrorLines() {
    m_editor->clearErrorLines();
}

void ScriptEditorWidget::openInExternalEditor() {
    const QString safeStem = QString(m_currentStem).replace('/', '_').replace(' ', '_');
    const QString tmpPath  = QDir::tempPath() + "/mcp_script_" + safeStem + ".py";

    // Write current content
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(m_editor->toPlainText().toUtf8());
    f.close();

    // Set up watcher if not already watching this file
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged,
                this, [this](const QString& path) {
            // Some editors delete+recreate on save — wait briefly then read.
            QTimer::singleShot(150, this, [this, path]() {
                QFile rf(path);
                if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) return;
                const QString newCode = QString::fromUtf8(rf.readAll());
                rf.close();

                if (newCode != m_editor->toPlainText()) {
                    m_editor->blockSignals(true);
                    m_editor->setPlainText(newCode);
                    m_editor->blockSignals(false);
                    emit codeChanged(newCode);
                }
                // Re-add path after delete+recreate
                if (!m_watcher->files().contains(path))
                    m_watcher->addPath(path);
            });
        });
    }

    if (!m_tempPath.isEmpty() && m_tempPath != tmpPath)
        m_watcher->removePath(m_tempPath);

    m_tempPath = tmpPath;
    if (!m_watcher->files().contains(tmpPath))
        m_watcher->addPath(tmpPath);

    // Prefer VS Code; fall back to system default
    if (!QProcess::startDetached("code", {tmpPath}))
        QDesktopServices::openUrl(QUrl::fromLocalFile(tmpPath));
}
