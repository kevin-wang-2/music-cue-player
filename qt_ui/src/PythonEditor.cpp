#include "PythonEditor.h"

#include <QKeyEvent>
#include <QTextCursor>
#include <QTextBlock>
#include <QFont>

// ---------------------------------------------------------------------------
// PythonHighlighter

PythonHighlighter::PythonHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    QTextCharFormat kwFmt;
    kwFmt.setForeground(QColor(0x56, 0x9c, 0xd6));   // blue
    kwFmt.setFontWeight(QFont::Bold);

    QTextCharFormat builtinFmt;
    builtinFmt.setForeground(QColor(0x4e, 0xc9, 0xb0)); // teal

    QTextCharFormat numFmt;
    numFmt.setForeground(QColor(0xb5, 0xce, 0xa8));    // light green

    QTextCharFormat commentFmt;
    commentFmt.setForeground(QColor(0x6a, 0x99, 0x55)); // dark green
    commentFmt.setFontItalic(true);

    m_strFormat.setForeground(QColor(0xce, 0x91, 0x78));  // orange/salmon
    m_mlStrFormat.setForeground(QColor(0xce, 0x91, 0x78));

    // Keywords
    const QStringList keywords = {
        "False","None","True","and","as","assert","async","await",
        "break","class","continue","def","del","elif","else","except",
        "finally","for","from","global","if","import","in","is",
        "lambda","nonlocal","not","or","pass","raise","return","try",
        "while","with","yield"
    };
    for (const auto& kw : keywords) {
        Rule r;
        r.pattern = QRegularExpression("\\b" + kw + "\\b");
        r.format  = kwFmt;
        m_rules.push_back(r);
    }

    // Built-ins
    const QStringList builtins = {
        "abs","all","any","bin","bool","bytes","callable","chr","dict",
        "dir","divmod","enumerate","eval","exec","filter","float","format",
        "frozenset","getattr","globals","hasattr","hash","help","hex","id",
        "input","int","isinstance","issubclass","iter","len","list","locals",
        "map","max","memoryview","min","next","object","oct","open","ord",
        "pow","print","property","range","repr","reversed","round","set",
        "setattr","slice","sorted","staticmethod","str","sum","super","tuple",
        "type","vars","zip"
    };
    for (const auto& bn : builtins) {
        Rule r;
        r.pattern = QRegularExpression("\\b" + bn + "\\b");
        r.format  = builtinFmt;
        m_rules.push_back(r);
    }

    // Numbers (int, float, hex, binary)
    {
        Rule r;
        r.pattern = QRegularExpression(
            "\\b(0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+\\.?[0-9]*([eE][+-]?[0-9]+)?)\\b");
        r.format  = numFmt;
        m_rules.push_back(r);
    }

    // Single-line comment
    {
        Rule r;
        r.pattern = QRegularExpression("#[^\n]*");
        r.format  = commentFmt;
        m_rules.push_back(r);
    }
}

void PythonHighlighter::highlightBlock(const QString& text) {
    // State: 0 = normal, 1 = inside triple-double, 2 = inside triple-single
    int state = previousBlockState();
    if (state < 0) state = 0;

    // Handle multi-line triple-quoted strings
    static const QRegularExpression tripleDouble("\"\"\"");
    static const QRegularExpression tripleSingle("'''");

    int pos = 0;
    while (pos < text.length()) {
        if (state == 1) {
            auto m = tripleDouble.match(text, pos);
            if (m.hasMatch()) {
                int end = m.capturedStart() + 3;
                setFormat(pos, end - pos, m_mlStrFormat);
                pos = end;
                state = 0;
            } else {
                setFormat(pos, text.length() - pos, m_mlStrFormat);
                pos = text.length();
            }
        } else if (state == 2) {
            auto m = tripleSingle.match(text, pos);
            if (m.hasMatch()) {
                int end = m.capturedStart() + 3;
                setFormat(pos, end - pos, m_mlStrFormat);
                pos = end;
                state = 0;
            } else {
                setFormat(pos, text.length() - pos, m_mlStrFormat);
                pos = text.length();
            }
        } else {
            // Check for opening triple quotes
            auto md = tripleDouble.match(text, pos);
            auto ms = tripleSingle.match(text, pos);
            int nextTriple = -1;
            bool isDouble  = false;
            if (md.hasMatch() && (!ms.hasMatch() || md.capturedStart() <= ms.capturedStart())) {
                nextTriple = md.capturedStart(); isDouble = true;
            } else if (ms.hasMatch()) {
                nextTriple = ms.capturedStart(); isDouble = false;
            }

            // Apply single-line rules up to the next triple quote (or end)
            int limit = (nextTriple >= 0) ? nextTriple : text.length();
            QString segment = text.left(limit);
            for (const auto& rule : m_rules) {
                auto it = rule.pattern.globalMatch(segment, pos);
                while (it.hasNext()) {
                    auto m2 = it.next();
                    // Don't overwrite if already inside a string
                    setFormat(m2.capturedStart(), m2.capturedLength(), rule.format);
                }
            }

            // Handle inline single/double-quoted strings (not triple) between pos and limit
            static const QRegularExpression strRe(
                R"((\"(?:[^\"\\]|\\.)*\"|'(?:[^'\\]|\\.)*'))");
            auto sit = strRe.globalMatch(text, pos);
            while (sit.hasNext()) {
                auto sm = sit.next();
                if (sm.capturedStart() < limit)
                    setFormat(sm.capturedStart(), sm.capturedLength(), m_strFormat);
            }

            if (nextTriple < 0) break;

            // We're opening a triple-quoted string
            int closeStart = nextTriple + 3;
            auto& endRe = isDouble ? tripleDouble : tripleSingle;
            auto mend   = endRe.match(text, closeStart);
            if (mend.hasMatch()) {
                int end = mend.capturedStart() + 3;
                setFormat(nextTriple, end - nextTriple, m_mlStrFormat);
                pos = end;
                state = 0;
            } else {
                setFormat(nextTriple, text.length() - nextTriple, m_mlStrFormat);
                pos = text.length();
                state = isDouble ? 1 : 2;
            }
        }
    }
    setCurrentBlockState(state);
}

// ---------------------------------------------------------------------------
// PythonEditor

PythonEditor::PythonEditor(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setStyleSheet(
        "QPlainTextEdit { background:#1a1a2e; color:#e0e0ff;"
        "  border:1px solid #444; border-radius:3px;"
        "  font-family:monospace; font-size:12px; }");

    QFont f("Menlo");
    if (!f.exactMatch()) f.setFamily("Courier New");
    f.setPointSize(12);
    f.setFixedPitch(true);
    setFont(f);

    m_hl = new PythonHighlighter(document());
}

void PythonEditor::markErrorLine(int line) {
    clearErrorLines();
    if (line < 1) return;

    QTextBlock block = document()->findBlockByLineNumber(line - 1);
    if (!block.isValid()) return;

    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(block);
    sel.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    sel.format.setBackground(QColor(0x66, 0x10, 0x10));
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    setExtraSelections({sel});
}

void PythonEditor::clearErrorLines() {
    setExtraSelections({});
}

void PythonEditor::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Tab) {
        // Insert 4 spaces instead of a tab character
        QTextCursor c = textCursor();
        c.insertText("    ");
        setTextCursor(c);
        return;
    }

    if (ev->key() == Qt::Key_Return || ev->key() == Qt::Key_Enter) {
        QTextCursor c = textCursor();
        const QString line = c.block().text();

        // Compute leading whitespace of current line
        QString indent;
        for (const QChar ch : line) {
            if (ch == ' ')  { indent += ' '; }
            else if (ch == '\t') { indent += '\t'; }
            else break;
        }

        // If the non-whitespace content ends with ':', add one more indent level
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed.back() == ':')
            indent += "    ";

        QPlainTextEdit::keyPressEvent(ev);
        c = textCursor();
        c.insertText(indent);
        setTextCursor(c);
        return;
    }

    if (ev->key() == Qt::Key_Backspace) {
        QTextCursor c = textCursor();
        if (!c.hasSelection()) {
            const QString line = c.block().text();
            const int col = c.positionInBlock();
            // If we're at a 4-space indent boundary, delete 4 spaces at once
            if (col > 0 && col % 4 == 0) {
                const QString before = line.left(col);
                if (before == QString(col, ' ')) {
                    c.movePosition(QTextCursor::Left,
                                   QTextCursor::KeepAnchor, 4);
                    c.removeSelectedText();
                    setTextCursor(c);
                    return;
                }
            }
        }
    }

    QPlainTextEdit::keyPressEvent(ev);
}
