#include "common/ui/BinarySummary.h"

#include <algorithm>
#include <limits>

#include <QHash>
#include <QPair>
#include <QRegularExpression>

namespace rk::ui {

namespace {

constexpr auto kUnicode = QRegularExpression::UseUnicodePropertiesOption;

const QRegularExpression& infoLineRe() {
    static const QRegularExpression re(QStringLiteral(R"(^\s*([A-Za-z][A-Za-z ]*?)\s*:\s*(.+?)\s*$)"), kUnicode);
    return re;
}

const QRegularExpression& totalCountRe() {
    static const QRegularExpression re(QStringLiteral(R"(\bof\s+(\d+)\s*:)"), kUnicode);
    return re;
}

const QRegularExpression& trailingParenRe() {
    static const QRegularExpression re(QStringLiteral(R"(\s*\(([^()]{1,12})\)\s*$)"), kUnicode);
    return re;
}

const QRegularExpression& hexRe() {
    static const QRegularExpression re(QStringLiteral("0x[0-9a-fA-F]+"));
    return re;
}

// str.splitlines() separators.
const QRegularExpression& lineBreakRe() {
    static const QRegularExpression re(QStringLiteral("\r\n|[\n\r\v\f\x1c\x1d\x1e\u0085\u2028\u2029]"));
    return re;
}

// IDA reports the x86 family as "metapc"; the bit width lives on its own line.
const QHash<QPair<QString, QString>, QString>& processorAliases() {
    static const QHash<QPair<QString, QString>, QString> aliases = {
        {{QStringLiteral("metapc"), QStringLiteral("64")}, QStringLiteral("x86_64")},
        {{QStringLiteral("metapc"), QStringLiteral("32")}, QStringLiteral("x86")},
        {{QStringLiteral("metapc"), QStringLiteral("16")}, QStringLiteral("x86_16")},
        {{QStringLiteral("arm"), QStringLiteral("64")}, QStringLiteral("arm64")},
        {{QStringLiteral("arm"), QStringLiteral("32")}, QStringLiteral("arm32")},
        {{QStringLiteral("ppc"), QStringLiteral("64")}, QStringLiteral("ppc64")},
        {{QStringLiteral("mips"), QStringLiteral("64")}, QStringLiteral("mips64")},
    };
    return aliases;
}

constexpr qsizetype kMaxFileTypeChars = 14;

// Width in UTF-16 units of the code point at `i`.
qsizetype codePointWidth(const QString& s, qsizetype i) {
    return (s[i].isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) ? 2 : 1;
}

// Python len(): code points, not UTF-16 units.
qsizetype codePoints(const QString& s) {
    qsizetype count = 0;
    for (qsizetype i = 0; i < s.size(); i += codePointWidth(s, i)) ++count;
    return count;
}

// UTF-16 length of the first `n` code points of `s`: Python slices by code point.
qsizetype codePointPrefix(const QString& s, qsizetype n) {
    qsizetype i = 0;
    for (; i < s.size() && n > 0; --n) i += codePointWidth(s, i);
    return i;
}

// int(text) for a decimal string: surrounding whitespace, a sign, Unicode digits, single "_" between digits.
std::optional<qint64> pyInt(const QString& text) {
    const QString s = text.trimmed();
    qsizetype i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) negative = s[i++] == u'-';
    if (i >= s.size()) return std::nullopt;
    qint64 value = 0;
    bool lastWasDigit = false;
    for (; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == u'_') {
            if (!lastWasDigit) return std::nullopt;
            lastWasDigit = false;
            continue;
        }
        const int digit = c.digitValue();
        if (digit < 0 || !c.isDigit()) return std::nullopt;
        if (value > (std::numeric_limits<qint64>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
        lastWasDigit = true;
    }
    if (!lastWasDigit) return std::nullopt;
    return negative ? -value : value;
}

// f"{n:,}"
QString groupThousands(qint64 n) {
    QString digits = QString::number(n);
    const qsizetype start = n < 0 ? 1 : 0;
    for (qsizetype pos = digits.size() - 3; pos > start; pos -= 3) digits.insert(pos, u',');
    return digits;
}

QString countChip(const QString& value, const QString& unit) {
    QString cleaned = value;
    const std::optional<qint64> count = pyInt(cleaned.remove(u','));
    if (!count) return {};
    return QStringLiteral("%1 %2").arg(groupThousands(*count), unit);
}

}  // namespace

QString BinarySummary::shortName() const {
    if (name.isEmpty()) return {};
#ifdef Q_OS_WIN
    const qsizetype cut = std::max(name.lastIndexOf(u'/'), std::max(name.lastIndexOf(u'\\'), name.lastIndexOf(u':')));
#else
    const qsizetype cut = name.lastIndexOf(u'/');
#endif
    return name.mid(cut + 1);
}

QMap<QString, QString> parseInfoLines(const QString& raw) {
    QMap<QString, QString> fields;
    for (const QString& line : raw.split(lineBreakRe())) {
        const QRegularExpressionMatch match = infoLineRe().match(line);
        if (!match.hasMatch()) continue;
        const QString key = match.captured(1).trimmed().toLower();
        if (!fields.contains(key)) fields.insert(key, match.captured(2).trimmed());
    }
    return fields;
}

std::optional<qint64> parseTotalCount(const QString& raw) {
    const QRegularExpressionMatch match = totalCountRe().match(raw);
    if (!match.hasMatch()) return std::nullopt;
    return pyInt(match.captured(1));
}

QString formatProcessor(const QString& processor, const QString& bits) {
    const QString proc = processor.trimmed();
    if (proc.isEmpty()) return {};
    const QString alias = processorAliases().value({proc.toLower(), bits.trimmed()});
    if (!alias.isEmpty()) return alias;
    if (!bits.isEmpty() && !proc.contains(bits)) return proc + bits;
    return proc;
}

QString formatFileType(const QString& fileType) {
    QString value = fileType.trimmed();
    if (value.isEmpty()) return {};
    QString abbrev;
    const QRegularExpressionMatch paren = trailingParenRe().match(value);
    if (paren.hasMatch()) {
        const QString candidate = paren.captured(1).trimmed();
        if (codePoints(candidate) <= 5) abbrev = candidate;
        value = value.left(paren.capturedStart(0)).trimmed();
    }
    for (const QLatin1StringView suffix : {QLatin1StringView(" file"), QLatin1StringView(" executable"),
                                           QLatin1StringView(" binary")}) {
        if (value.toLower().endsWith(suffix)) {
            value = value.left(value.size() - suffix.size()).trimmed();
            break;
        }
    }
    if (codePoints(value) <= kMaxFileTypeChars) return value;
    if (!abbrev.isEmpty()) return abbrev;
    return value.left(codePointPrefix(value, kMaxFileTypeChars - 1)) + QStringLiteral("…");
}

QString formatAddress(const QString& value) {
    const QRegularExpressionMatch match = hexRe().match(value);
    return match.hasMatch() ? match.captured(0).toLower() : QString();
}

BinarySummary buildBinarySummary(const QString& rawInfo, std::optional<qint64> stringCount) {
    const QMap<QString, QString> fields = parseInfoLines(rawInfo);
    if (fields.isEmpty()) return {};

    BinarySummary summary;
    summary.name = fields.value(QStringLiteral("file"));

    const QString arch =
        formatProcessor(fields.value(QStringLiteral("processor")), fields.value(QStringLiteral("bits")));
    if (!arch.isEmpty()) summary.chips << arch;

    const QString fileType = formatFileType(fields.value(QStringLiteral("file type")));
    if (!fileType.isEmpty()) summary.chips << fileType;

    const QString functions = countChip(fields.value(QStringLiteral("functions")), QStringLiteral("fn"));
    if (!functions.isEmpty()) summary.chips << functions;

    if (stringCount && *stringCount >= 0) summary.chips << groupThousands(*stringCount) + QStringLiteral(" str");

    const QString entry = formatAddress(fields.value(QStringLiteral("entry point")));
    if (!entry.isEmpty()) summary.chips << QStringLiteral("entry ") + entry;

    return summary;
}

}  // namespace rk::ui
