#include "ScriptletLibraryDialog.h"
#include "AppModel.h"
#include "ScriptEditorWidget.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QVBoxLayout>

ScriptletLibraryDialog::ScriptletLibraryDialog(AppModel* model, QWidget* parent)
    : QDialog(parent, Qt::Window)
    , m_model(model)
{
    setWindowTitle("Scriptlet Library");
    resize(850, 520);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->setSpacing(6);

    // Splitter: list on the left, editor on the right
    auto* splitter = new QSplitter(Qt::Horizontal);
    outerLayout->addWidget(splitter, 1);

    // Left panel: list + buttons
    auto* leftWidget = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    m_list = new QListWidget;
    m_list->setStyleSheet(
        "QListWidget { background:#1e1e2e; color:#e0e0ff; border:1px solid #444; }"
        "QListWidget::item:selected { background:#2a4a8a; }");
    leftLayout->addWidget(m_list, 1);

    auto* btnRow = new QHBoxLayout;
    auto* btnAdd    = new QPushButton("+");
    m_btnRemove     = new QPushButton("−");
    m_btnRename     = new QPushButton("Rename");
    btnAdd->setFixedWidth(28);
    m_btnRemove->setFixedWidth(28);
    const char* btnStyle =
        "QPushButton { background:#2a2a3e; color:#ccc; border:1px solid #444;"
        "  border-radius:2px; padding:2px 6px; }"
        "QPushButton:hover { background:#3a3a5e; }";
    for (auto* b : {btnAdd, m_btnRemove, m_btnRename}) b->setStyleSheet(btnStyle);
    btnRow->addWidget(btnAdd);
    btnRow->addWidget(m_btnRemove);
    btnRow->addWidget(m_btnRename);
    btnRow->addStretch();
    leftLayout->addLayout(btnRow);

    leftWidget->setMinimumWidth(160);
    leftWidget->setMaximumWidth(220);
    splitter->addWidget(leftWidget);

    // Right panel: reusable ScriptEditorWidget
    m_editor = new ScriptEditorWidget;
    m_editor->setEnabled(false);
    splitter->addWidget(m_editor);
    splitter->setStretchFactor(1, 1);

    connect(btnAdd,      &QPushButton::clicked, this, &ScriptletLibraryDialog::onAdd);
    connect(m_btnRemove, &QPushButton::clicked, this, &ScriptletLibraryDialog::onRemove);
    connect(m_btnRename, &QPushButton::clicked, this, &ScriptletLibraryDialog::onRename);
    connect(m_list, &QListWidget::currentRowChanged,
            this, &ScriptletLibraryDialog::onSelectionChanged);
    connect(m_editor, &ScriptEditorWidget::codeChanged,
            this, &ScriptletLibraryDialog::saveCurrentCode);

    refreshList();
    onSelectionChanged();
}

void ScriptletLibraryDialog::refreshList() {
    m_loading = true;
    const int prevRow = m_list->currentRow();
    m_list->clear();

    // Built-in entries at the top — visually distinct, read-only.
    const auto& builtins = m_model->builtinScriptlets();
    m_builtinCount = static_cast<int>(builtins.size());
    for (const auto& e : builtins) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(e.name) + " [built-in]");
        item->setForeground(QColor("#888888"));
        item->setToolTip("Loaded from the scriptlets/ directory — read-only");
        m_list->addItem(item);
    }

    // User entries below.
    for (const auto& e : m_model->sf.scriptletLibrary.entries)
        m_list->addItem(QString::fromStdString(e.name));

    if (prevRow >= 0 && prevRow < m_list->count())
        m_list->setCurrentRow(prevRow);
    else if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    m_loading = false;
    onSelectionChanged();
}

bool ScriptletLibraryDialog::isBuiltinRow(int row) const {
    return row >= 0 && row < m_builtinCount;
}

int ScriptletLibraryDialog::userEntryIdx(int row) const {
    const int idx = row - m_builtinCount;
    return (idx >= 0 && idx < (int)m_model->sf.scriptletLibrary.entries.size())
        ? idx : -1;
}

int ScriptletLibraryDialog::currentEntryIndex() const {
    return m_list->currentRow();
}

void ScriptletLibraryDialog::onSelectionChanged() {
    const int row = currentEntryIndex();
    const bool builtin = isBuiltinRow(row);
    const int  uid     = userEntryIdx(row);
    const bool userValid = (uid >= 0);
    const bool anyValid  = builtin || userValid;

    m_btnRemove->setEnabled(userValid);
    m_btnRename->setEnabled(userValid);
    m_editor->setEnabled(anyValid);

    if (!anyValid) {
        m_loading = true;
        m_editor->setCode({});
        m_editor->setContext({});
        m_loading = false;
        return;
    }

    m_loading = true;
    if (builtin) {
        const auto& entry = m_model->builtinScriptlets()[row];
        m_editor->setContext(QString::fromStdString("lib_builtin_" + entry.name));
        m_editor->setCode(QString::fromStdString(entry.code));
    } else {
        const auto& entry = m_model->sf.scriptletLibrary.entries[uid];
        m_editor->setContext(QString::fromStdString("lib_" + entry.name));
        m_editor->setCode(QString::fromStdString(entry.code));
    }
    // Prevent editing built-in code (make the widget look readonly).
    m_editor->setEnabled(!builtin);
    m_loading = false;
}

void ScriptletLibraryDialog::saveCurrentCode(const QString& code) {
    if (m_loading) return;
    const int uid = userEntryIdx(currentEntryIndex());
    if (uid < 0) return;   // built-in or nothing selected — never write
    m_model->sf.scriptletLibrary.entries[uid].code = code.toStdString();
    m_model->applyScriptletLibrary();
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
}

void ScriptletLibraryDialog::onAdd() {
    bool ok = false;
    QString name = QInputDialog::getText(
        this, "New Library Module",
        "Module name (valid Python identifier, e.g. utils):",
        QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    static const QRegularExpression validId("^[A-Za-z_][A-Za-z0-9_]*$");
    if (!validId.match(name).hasMatch()) {
        QMessageBox::warning(this, "Invalid Name",
            "Module name must be a valid Python identifier (letters, digits, underscore).");
        return;
    }
    for (const auto& e : m_model->sf.scriptletLibrary.entries) {
        if (QString::fromStdString(e.name) == name) {
            QMessageBox::warning(this, "Duplicate Name",
                QString("A module named '%1' already exists.").arg(name));
            return;
        }
    }

    mcp::ShowFile::ScriptletLibrary::Entry entry;
    entry.name = name.toStdString();
    m_model->sf.scriptletLibrary.entries.push_back(std::move(entry));
    m_model->applyScriptletLibrary();
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);

    refreshList();
    // New user entry is always appended at the end (after built-ins).
    m_list->setCurrentRow(m_list->count() - 1);
}

void ScriptletLibraryDialog::onRemove() {
    const int uid = userEntryIdx(currentEntryIndex());
    if (uid < 0) return;

    const QString name = QString::fromStdString(
        m_model->sf.scriptletLibrary.entries[uid].name);
    if (QMessageBox::question(this, "Remove Module",
            QString("Remove library module '%1'?").arg(name),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    m_model->sf.scriptletLibrary.entries.erase(
        m_model->sf.scriptletLibrary.entries.begin() + uid);
    m_model->applyScriptletLibrary();
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    refreshList();
}

void ScriptletLibraryDialog::onRename() {
    const int uid = userEntryIdx(currentEntryIndex());
    if (uid < 0) return;

    const QString oldName = QString::fromStdString(
        m_model->sf.scriptletLibrary.entries[uid].name);
    bool ok = false;
    QString newName = QInputDialog::getText(
        this, "Rename Module", "New name:", QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == oldName) return;
    newName = newName.trimmed();

    static const QRegularExpression validId("^[A-Za-z_][A-Za-z0-9_]*$");
    if (!validId.match(newName).hasMatch()) {
        QMessageBox::warning(this, "Invalid Name",
            "Module name must be a valid Python identifier.");
        return;
    }
    // Check against both user entries and built-in names.
    for (int i = 0; i < (int)m_model->sf.scriptletLibrary.entries.size(); ++i) {
        if (i != uid &&
            QString::fromStdString(m_model->sf.scriptletLibrary.entries[i].name) == newName) {
            QMessageBox::warning(this, "Duplicate Name",
                QString("A module named '%1' already exists.").arg(newName));
            return;
        }
    }
    for (const auto& e : m_model->builtinScriptlets()) {
        if (QString::fromStdString(e.name) == newName) {
            QMessageBox::warning(this, "Duplicate Name",
                QString("'%1' is a built-in module name.").arg(newName));
            return;
        }
    }

    m_model->sf.scriptletLibrary.entries[uid].name = newName.toStdString();
    m_model->applyScriptletLibrary();
    m_model->dirty = true;
    emit m_model->dirtyChanged(true);
    refreshList();
    m_list->setCurrentRow(m_builtinCount + uid);
}
