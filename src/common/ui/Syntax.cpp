#include "common/ui/Syntax.h"

#include <algorithm>

#include <QHash>
#include <QRegularExpression>

#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

const QHash<QString, QString>& languageByExtension() {
    static const QHash<QString, QString> table = [] {
        const QList<QPair<QString, QStringList>> extensions = {
            {kLanguagePython, {".py", ".pyw", ".pyi", ".idc.py"}},
            {kLanguageC,
             {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".cs", ".java", ".js", ".jsx", ".ts", ".tsx", ".go",
              ".rs", ".idc", ".kt", ".swift"}},
            {kLanguageShell, {".sh", ".bash", ".zsh", ".ps1", ".psm1"}},
            {kLanguageJson, {".json", ".jsonc"}},
            {kLanguageConfig, {".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf"}},
        };
        QHash<QString, QString> out;
        for (const auto& [language, exts] : extensions)
            for (const QString& ext : exts) out.insert(ext, language);
        return out;
    }();
    return table;
}

QString words(const QStringList& list) { return QStringLiteral(R"(\b(?:)") + list.join('|') + QStringLiteral(R"()\b)"); }

const QString kNumber = QStringLiteral(R"(\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*\.?\d*(?:[eE][+-]?\d+)?)\b)");
const QString kDoubleQuoted = QStringLiteral(R"("(?:[^"\\]|\\.)*"?)");
const QString kSingleQuoted = QStringLiteral(R"('(?:[^'\\]|\\.)*'?)");

// Each is an ordered alternation: earlier groups win a tie at the same position. "open_N" groups start a token
// that can run across lines; MultilineEnd has what ends it.
QString patternFor(const QString& language) {
    if (language == kLanguagePython)
        return QStringLiteral(R"((?P<comment>#.*))") + QStringLiteral(R"(|(?P<open_0>[bBrRfFuU]{0,2}"""))") +
               QStringLiteral(R"(|(?P<open_1>[bBrRfFuU]{0,2}'''))") +
               QStringLiteral(R"(|(?P<string>[bBrRfFuU]{0,2}(?:)") + kDoubleQuoted + '|' + kSingleQuoted +
               QStringLiteral("))") + QStringLiteral("|(?P<number>") + kNumber + ')' +
               QStringLiteral(R"(|(?P<meta>^\s*@[\w.]+))") + QStringLiteral("|(?P<keyword>") +
               words({"and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif",
                      "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
                      "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield"}) +
               QStringLiteral(")|(?P<builtin>") +
               words({"print", "len", "range", "int", "str", "bytes", "list", "dict", "set", "tuple", "hex", "ord",
                      "chr", "type", "isinstance", "enumerate", "zip", "map", "filter", "sorted", "open", "True",
                      "False", "None", "self"}) +
               ')';
    if (language == kLanguageC)
        return QStringLiteral(R"((?P<comment>//.*))") + QStringLiteral(R"(|(?P<open_0>/\*))") +
               QStringLiteral("|(?P<string>") + kDoubleQuoted + '|' + kSingleQuoted + ')' +
               QStringLiteral(R"(|(?P<meta>^\s*#\s*\w+))") + QStringLiteral("|(?P<number>") + kNumber + ')' +
               QStringLiteral("|(?P<keyword>") +
               words({"auto",    "break",     "case",      "catch",  "class",   "const",   "continue", "default",
                      "delete",  "do",        "else",      "enum",   "export",  "extends", "extern",   "fn",
                      "for",     "func",      "function",  "goto",   "if",      "impl",    "import",   "interface",
                      "let",     "match",     "mut",       "namespace", "new",  "package", "private",  "protected",
                      "public",  "return",    "sizeof",    "static", "struct",  "switch",  "template", "this",
                      "throw",   "trait",     "try",       "typedef", "union",  "use",     "using",    "var",
                      "virtual", "volatile",  "while"}) +
               QStringLiteral(")|(?P<builtin>") +
               words({"void", "char", "short", "int", "long", "float", "double", "bool", "signed", "unsigned",
                      "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
                      "int64_t", "true", "false", "null", "nullptr", "NULL"}) +
               ')';
    if (language == kLanguageShell)
        return QStringLiteral(R"((?P<comment>(?:^|(?<=\s))#.*))") + QStringLiteral("|(?P<string>") + kDoubleQuoted +
               '|' + kSingleQuoted + ')' + QStringLiteral(R"(|(?P<meta>\$\{?\w+\}?))") +
               QStringLiteral("|(?P<number>") + kNumber + ')' + QStringLiteral("|(?P<keyword>") +
               words({"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in",
                      "function", "return", "exit"}) +
               QStringLiteral(")|(?P<builtin>") +
               words({"cd", "echo", "export", "source", "sudo", "rm", "mv", "cp", "mkdir", "cat", "grep", "python3",
                      "python"}) +
               ')';
    if (language == kLanguageJson)
        return QStringLiteral(R"((?P<keyword>"(?:[^"\\]|\\.)*"(?=\s*:)))") + QStringLiteral("|(?P<string>") +
               kDoubleQuoted + ')' + QStringLiteral("|(?P<number>") + kNumber + ')' +
               QStringLiteral(R"(|(?P<builtin>\b(?:true|false|null)\b))");
    if (language == kLanguageConfig)
        return QStringLiteral(R"((?P<comment>(?:^|(?<=\s))[#;].*))") + QStringLiteral("|(?P<string>") +
               kDoubleQuoted + '|' + kSingleQuoted + ')' + QStringLiteral("|(?P<number>") + kNumber + ')';
    return {};
}

// For each language, the token an "open_N" group starts: what ends it, and its kind.
struct MultilineEnd {
    QString close;
    QString kind;
};

struct Scanner {
    QRegularExpression pattern;
    QStringList groupNames;  // by capture index; "" for unnamed groups
    QList<MultilineEnd> ends;
};

const Scanner* scannerFor(const QString& language) {
    static const QHash<QString, Scanner> scanners = [] {
        QHash<QString, Scanner> out;
        for (const QString& lang : {kLanguagePython, kLanguageC, kLanguageShell, kLanguageJson, kLanguageConfig}) {
            Scanner s;
            s.pattern = QRegularExpression(patternFor(lang), QRegularExpression::UseUnicodePropertiesOption);
            s.pattern.optimize();
            s.groupNames = s.pattern.namedCaptureGroups();
            if (lang == kLanguagePython)
                s.ends = {{QStringLiteral(R"(""")"), QStringLiteral("string")},
                          {QStringLiteral("'''"), QStringLiteral("string")}};
            else if (lang == kLanguageC)
                s.ends = {{QStringLiteral("*/"), QStringLiteral("comment")}};
            out.insert(lang, s);
        }
        return out;
    }();
    const auto it = scanners.constFind(language);
    return it == scanners.constEnd() ? nullptr : &it.value();
}

}  // namespace

QString languageForPath(const QString& path) {
    const qsizetype slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    QString base = path.mid(slash + 1);
    // os.path.splitext: leading dots belong to the name (".bashrc" has no extension).
    qsizetype lead = 0;
    while (lead < base.size() && base.at(lead) == '.') ++lead;
    const qsizetype dot = base.lastIndexOf('.');
    if (dot < lead) return kLanguagePlain;
    return languageByExtension().value(base.mid(dot).toLower(), kLanguagePlain);
}

QList<CodeSpan> scanLine(const QString& language, const QString& text, int state, int* nextState) {
    QList<CodeSpan> spans;
    auto finish = [&](int s) {
        if (nextState) *nextState = s;
        return spans;
    };
    const Scanner* scanner = scannerFor(language);
    if (!scanner) return finish(-1);
    const auto length = static_cast<int>(text.size());
    int position = 0;

    if (state >= 0 && state < scanner->ends.size()) {
        const MultilineEnd& end = scanner->ends.at(state);
        const auto closing = static_cast<int>(text.indexOf(end.close));
        if (closing < 0) {
            spans.append({0, length, end.kind});
            return finish(state);
        }
        const auto closeEnd = closing + static_cast<int>(end.close.size());
        spans.append({0, closeEnd, end.kind});
        position = closeEnd;
    }

    while (position <= length) {
        const QRegularExpressionMatch match = scanner->pattern.match(text, position);
        if (!match.hasMatch() || match.capturedEnd() == match.capturedStart()) break;
        QString group;
        for (int i = 1; i < scanner->groupNames.size(); ++i) {
            if (!scanner->groupNames.at(i).isEmpty() && match.capturedStart(i) >= 0) {
                group = scanner->groupNames.at(i);
                break;
            }
        }
        const auto start = static_cast<int>(match.capturedStart());
        const auto matchEnd = static_cast<int>(match.capturedEnd());
        if (group.startsWith(QStringLiteral("open_"))) {
            const int index = group.mid(5).toInt();
            const MultilineEnd& end = scanner->ends.at(index);
            const auto closing = static_cast<int>(text.indexOf(end.close, matchEnd));
            if (closing < 0) {
                spans.append({start, length - start, end.kind});
                return finish(index);
            }
            const auto closeEnd = closing + static_cast<int>(end.close.size());
            spans.append({start, closeEnd - start, end.kind});
            position = closeEnd;
            continue;
        }
        spans.append({start, matchEnd - start, group});
        position = matchEnd;
    }
    return finish(-1);
}

QTextCharFormat syntaxFormat(const QString& kind) {
    const Theme& t = theme();
    QTextCharFormat f;
    if (kind == QLatin1String("comment")) {
        f.setForeground(t.synComment);
        f.setFontItalic(true);
    } else if (kind == QLatin1String("string")) {
        f.setForeground(t.synString);
    } else if (kind == QLatin1String("number")) {
        f.setForeground(t.synNumber);
    } else if (kind == QLatin1String("keyword")) {
        f.setForeground(t.synKeyword);
        f.setFontWeight(QFont::DemiBold);
    } else if (kind == QLatin1String("builtin")) {
        f.setForeground(t.synBuiltin);
    } else if (kind == QLatin1String("meta")) {
        f.setForeground(t.synMeta);
    }
    return f;
}

CodeHighlighter::CodeHighlighter(QTextDocument* document, const QString& language)
    : QSyntaxHighlighter(document), language_(language) {}

void CodeHighlighter::highlightBlock(const QString& text) {
    int state = -1;
    const QList<CodeSpan> spans = scanLine(language_, text, previousBlockState(), &state);
    for (const CodeSpan& span : spans) setFormat(span.start, span.length, syntaxFormat(span.kind));
    setCurrentBlockState(state);
}

}  // namespace rk::ui
