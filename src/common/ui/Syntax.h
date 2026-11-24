#pragma once
// Syntax colours for code in the transcript (tool arguments, results, written files, scripts awaiting approval).
// scanLine colours one line at a time; CodeHighlighter runs it as a QSyntaxHighlighter with the theme's colours.
#include <QList>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace rk::ui {

// Language ids.
inline const QString kLanguagePlain;  // ""
inline const QString kLanguagePython = QStringLiteral("python");
inline const QString kLanguageC = QStringLiteral("c");
inline const QString kLanguageShell = QStringLiteral("shell");
inline const QString kLanguageJson = QStringLiteral("json");
inline const QString kLanguageConfig = QStringLiteral("config");

QString languageForPath(const QString& path);

struct CodeSpan {
    int start = 0;
    int length = 0;
    QString kind;  // keyword, string, comment, number, builtin, meta
};

// Colours one line; `state` is the multi-line token the line starts inside (-1: none). Returns the spans and
// the state for the next line.
QList<CodeSpan> scanLine(const QString& language, const QString& text, int state, int* nextState);

// The theme's format for a span kind.
QTextCharFormat syntaxFormat(const QString& kind);

class CodeHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    CodeHighlighter(QTextDocument* document, const QString& language);

protected:
    void highlightBlock(const QString& text) override;

private:
    QString language_;
};

}  // namespace rk::ui
