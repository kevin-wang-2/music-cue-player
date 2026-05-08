#pragma once

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <vector>

// ---------------------------------------------------------------------------
// Python syntax highlighter
class PythonHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit PythonHighlighter(QTextDocument* parent = nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule { QRegularExpression pattern; QTextCharFormat format; };
    std::vector<Rule> m_rules;
    QTextCharFormat   m_strFormat;
    QTextCharFormat   m_mlStrFormat;
};

// ---------------------------------------------------------------------------
// QPlainTextEdit with Python-aware editing and error highlighting
class PythonEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit PythonEditor(QWidget* parent = nullptr);

    // Highlight the given 1-based line number with an error underline.
    void markErrorLine(int line);
    void clearErrorLines();

protected:
    void keyPressEvent(QKeyEvent* ev) override;

private:
    PythonHighlighter* m_hl{nullptr};
};
