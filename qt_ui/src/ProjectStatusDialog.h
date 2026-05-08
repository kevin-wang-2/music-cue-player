#pragma once

#include "engine/TriggerData.h"
#include <QDialog>
#include <QVariantList>

class AppModel;
class QTabWidget;
class QTreeWidget;
class QPlainTextEdit;
class QCheckBox;

class ProjectStatusDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectStatusDialog(AppModel* model, QWidget* parent = nullptr);

    // Refresh the Warnings tab (call when cue list changes).
    void refreshWarnings();

public slots:
    void onMidiInput(mcp::MidiMsgType type, int ch, int d1, int d2);
    void onOscInput(const QString& path, const QVariantList& args);
    void onScriptletOutput(const QString& text);

private:
    void buildWarningsTab();
    void buildLogsTab();
    void buildScriptletTab();

    AppModel*      m_model;
    QTabWidget*    m_tabs{nullptr};

    // Warnings tab
    QWidget*     m_warningsPage{nullptr};
    QTreeWidget* m_warningsList{nullptr};

    // Logs tab
    QWidget*        m_logsPage{nullptr};
    QCheckBox*      m_chkMidiLog{nullptr};
    QCheckBox*      m_chkOscLog{nullptr};
    QPlainTextEdit* m_logsView{nullptr};

    // Scriptlet tab
    QWidget*        m_scriptletPage{nullptr};
    QPlainTextEdit* m_scriptletView{nullptr};
};
