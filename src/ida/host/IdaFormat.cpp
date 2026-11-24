#include "ida/host/IdaFormat.h"

#include <algorithm>
#include <map>

#include <QChar>

namespace rk::ida {

QString ljust(const QString& s, int width) { return s.leftJustified(width, QLatin1Char(' '), false); }

QString rjust(qint64 v, int width) { return QString::number(v).rightJustified(width, QLatin1Char(' '), false); }

QString hexPad(uint64_t v, int width) {
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(width, QLatin1Char('0'), false);
}

QString altHex(qint64 v) {
    if (v < 0) return QStringLiteral("-0x") + QString::number(uint64_t(0) - uint64_t(v), 16);
    return QStringLiteral("0x") + QString::number(uint64_t(v), 16);
}

QString altHex010(uint64_t v) { return hexPad(v, 8); }

bool pyIsPrintable(char32_t c) {
    if (c == U' ') return true;
    switch (QChar::category(c)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return false;
    default:
        return true;
    }
}

QString pyRepr(const QString& s) {
    const QList<uint> cps = s.toUcs4();
    const bool hasSingle = s.contains(QLatin1Char('\''));
    const bool hasDouble = s.contains(QLatin1Char('"'));
    const char32_t quote = hasSingle && !hasDouble ? U'"' : U'\'';
    QString out;
    out.reserve(s.size() + 2);
    out += QChar(char16_t(quote));
    auto hexEsc = [&](const char* prefix, uint c, int width) {
        out += QLatin1String(prefix);
        out += QString::number(c, 16).rightJustified(width, QLatin1Char('0'));
    };
    for (uint c : cps) {
        if (c == quote || c == U'\\') {
            out += QLatin1Char('\\');
            out += QChar(char16_t(c));
        } else if (c == U'\t') {
            out += QLatin1String("\\t");
        } else if (c == U'\n') {
            out += QLatin1String("\\n");
        } else if (c == U'\r') {
            out += QLatin1String("\\r");
        } else if (c < 0x20 || c == 0x7f) {
            hexEsc("\\x", c, 2);
        } else if (c < 0x7f) {
            out += QChar(char16_t(c));
        } else if (pyIsPrintable(c)) {
            out += QString::fromUcs4(reinterpret_cast<const char32_t*>(&c), 1);
        } else if (c <= 0xff) {
            hexEsc("\\x", c, 2);
        } else if (c <= 0xffff) {
            hexEsc("\\u", c, 4);
        } else {
            hexEsc("\\U", c, 8);
        }
    }
    out += QChar(char16_t(quote));
    return out;
}

QString pyIntList(const std::vector<qint64>& v) {
    QStringList parts;
    parts.reserve(qsizetype(v.size()));
    for (qint64 x : v) parts << QString::number(x);
    return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
}

QString decodeUtf8(const QByteArray& b) { return QString::fromUtf8(b); }

std::pair<qsizetype, qsizetype> pySlice(qsizetype len, qint64 start, qint64 stop) {
    auto clamp = [len](qint64 i) -> qsizetype {
        if (i < 0) i += len;
        return qsizetype(std::clamp<qint64>(i, 0, len));
    };
    const qsizetype b = clamp(start), e = clamp(stop);
    return {b, std::max(b, e)};
}

std::pair<qint64, qint64> normalizePage(qint64 offset, qint64 limit) {
    return {std::max<qint64>(0, offset), std::max<qint64>(1, std::min<qint64>(limit, kMaxPageLimit))};
}

QString formatPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title, const QString& empty,
                   const QString& nextHint) {
    std::tie(offset, limit) = normalizePage(offset, limit);
    const qint64 total = rows.size();
    const qint64 begin = std::min(offset, total);
    const qint64 end = std::min(total, offset + limit);
    const QStringList page = rows.mid(begin, end - begin);
    QStringList lines;
    lines << title + QStringLiteral(" %1-%2 of %3:").arg(offset).arg(offset + page.size()).arg(total);
    if (page.isEmpty()) lines << empty;
    else lines << page;
    const qint64 next = offset + page.size();
    if (next < total) {
        const QString hint = nextHint.isEmpty()
                                 ? QStringLiteral("Use offset=%1 limit=%2 for the next page.").arg(next).arg(limit)
                                 : nextHint;
        lines << QStringLiteral("  ... %1 more. %2").arg(total - next).arg(hint);
    }
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------------------- global-value formatting

namespace {

const std::map<QString, int>& typeSizes() {
    static const std::map<QString, int> m{
        {"u8", 1},  {"i8", 1},  {"byte", 1},  {"u16", 2},    {"i16", 2},    {"word", 2},  {"u32", 4},
        {"i32", 4}, {"dword", 4}, {"float", 4}, {"u64", 8},  {"i64", 8},    {"qword", 8}, {"double", 8},
        {"ptr", 0}, {"pointer", 0}};
    return m;
}

int typeSize(const QString& hint, bool* found) {
    const auto it = typeSizes().find(hint);
    *found = it != typeSizes().end();
    return *found ? it->second : 0;
}

QString hexBytes(const QByteArray& data) {
    QStringList parts;
    for (char c : data) parts << QString::number(uchar(c), 16).rightJustified(2, QLatin1Char('0'));
    return parts.join(QLatin1Char(' '));
}

QString printableAscii(const QByteArray& data) {
    QString out;
    for (char c : data) {
        const uchar b = uchar(c);
        out += b >= 0x20 && b < 0x7f ? QChar(char16_t(b)) : QChar(u'.');
    }
    return out;
}

bool mostlyPrintable(const QString& text) {
    const QList<uint> cps = text.toUcs4();
    qint64 printable = 0;
    for (uint c : cps)
        if (pyIsPrintable(c) || c == U'\r' || c == U'\n' || c == U'\t') ++printable;
    return printable >= std::max<qint64>(1, qint64(double(cps.size()) * 0.8));
}

bool decodeUtf8Text(const QByteArray& data, QString* out) {
    const qsizetype nul = data.indexOf('\0');
    const QByteArray raw = nul < 0 ? data : data.left(nul);
    if (raw.isEmpty()) return false;
    const QString text = QString::fromUtf8(raw);
    if (!mostlyPrintable(text)) return false;
    *out = text;
    return true;
}

// bytes.decode("utf-16-le", "replace")
QString utf16le(const QByteArray& raw) {
    QString out;
    const qsizetype units = raw.size() / 2;
    for (qsizetype i = 0; i < units; ++i) {
        const char16_t u = char16_t(uchar(raw[2 * i]) | (uchar(raw[2 * i + 1]) << 8));
        if (QChar::isHighSurrogate(u) && i + 1 < units) {
            const char16_t lo = char16_t(uchar(raw[2 * i + 2]) | (uchar(raw[2 * i + 3]) << 8));
            if (QChar::isLowSurrogate(lo)) {
                out += QChar(u);
                out += QChar(lo);
                ++i;
                continue;
            }
        }
        out += QChar::isSurrogate(u) ? QChar(QChar::ReplacementCharacter) : QChar(u);
    }
    if (raw.size() % 2) out += QChar(QChar::ReplacementCharacter);
    return out;
}

bool decodeUtf16Text(const QByteArray& data, QString* out) {
    if (data.size() < 2) return false;
    QByteArray raw = data;
    for (qsizetype idx = 0; idx < std::max<qsizetype>(0, data.size() - 1); idx += 2) {
        if (data[idx] == '\0' && data[idx + 1] == '\0') {
            raw = data.left(idx);
            break;
        }
    }
    if (raw.isEmpty()) return false;
    const QString text = utf16le(raw);
    if (!mostlyPrintable(text)) return false;
    *out = text;
    return true;
}

}  // namespace

QString normalizeTypeHint(const QString& typeHint) {
    QString hint = typeHint.trimmed().toLower();
    if (typeHint.isEmpty()) hint = QStringLiteral("auto");
    return hint.replace(QLatin1String("uint"), QLatin1String("u")).replace(QLatin1String("int"), QLatin1String("i"));
}

int bytesNeededForType(const QString& typeHint, int pointerSize, qint64 requestedSize) {
    const QString hint = normalizeTypeHint(typeHint);
    static const QStringList textual{"string", "str", "utf8", "utf-8", "utf16", "utf-16", "bytes", "auto"};
    if (textual.contains(hint)) return int(std::max<qint64>(1, std::min<qint64>(requestedSize ? requestedSize : 64, 256)));
    bool found = false;
    const int size = typeSize(hint, &found);
    if (!found) return int(std::max<qint64>(1, std::min<qint64>(requestedSize ? requestedSize : pointerSize, 256)));
    if (size == 0) return std::max(1, pointerSize);
    return size;
}

QString formatGlobalValue(uint64_t address, const QByteArray& data, int pointerSize, const QString& typeHint,
                          const QString& name, const std::function<QString(uint64_t)>& resolvePointer) {
    const QString hint = normalizeTypeHint(typeHint);
    const QString label = name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name);
    QStringList lines;
    lines << QStringLiteral("Global value at 0x%1%2").arg(QString::number(address, 16), label);
    lines << QStringLiteral("Type hint: %1").arg(hint);
    lines << QStringLiteral("Bytes: %1").arg(hexBytes(data));

    if (hint == QLatin1String("bytes")) {
        lines << QStringLiteral("ASCII: %1").arg(printableAscii(data));
        return lines.join(QLatin1Char('\n'));
    }

    static const QStringList utf8Hints{"string", "str", "utf8", "utf-8", "auto"};
    static const QStringList utf16Hints{"utf16", "utf-16", "auto"};
    QString text;
    if (utf8Hints.contains(hint) && decodeUtf8Text(data, &text)) lines << QStringLiteral("UTF-8 string: %1").arg(pyRepr(text));
    if (utf16Hints.contains(hint) && decodeUtf16Text(data, &text))
        lines << QStringLiteral("UTF-16LE string: %1").arg(pyRepr(text));

    std::vector<int> widths;
    if (hint == QLatin1String("auto")) widths = {1, 2, 4, 8};
    bool found = false;
    const int explicitWidth = typeSize(hint, &found);
    if (explicitWidth) widths = {explicitWidth};
    else if (hint == QLatin1String("ptr") || hint == QLatin1String("pointer")) widths = {pointerSize};

    static const QStringList unsignedNames{"byte", "word", "dword", "qword", "auto"};
    std::vector<int> seen;
    for (int width : widths) {
        if (std::find(seen.begin(), seen.end(), width) != seen.end() || width <= 0 || data.size() < width) continue;
        seen.push_back(width);
        uint64_t value = 0;
        for (int i = 0; i < width; ++i) value |= uint64_t(uchar(data[i])) << (8 * i);
        const int bits = width * 8;
        if (hint.startsWith(QLatin1Char('i')) || hint == QLatin1String("auto")) {
            qint64 sv = qint64(value);
            if (bits < 64 && (value & (uint64_t(1) << (bits - 1)))) sv = qint64(value) - (qint64(1) << bits);
            lines << QStringLiteral("%1%2: %3")
                         .arg(QLatin1String(hint.startsWith(QLatin1Char('i')) ? "i" : "s"))
                         .arg(bits)
                         .arg(sv);
        }
        if (hint.startsWith(QLatin1Char('u')) || unsignedNames.contains(hint))
            lines << QStringLiteral("u%1: %2 (0x%3)").arg(bits).arg(value).arg(QString::number(value, 16));
    }

    if (data.size() >= pointerSize && (pointerSize == 4 || pointerSize == 8)) {
        uint64_t ptr = 0;
        for (int i = 0; i < pointerSize; ++i) ptr |= uint64_t(uchar(data[i])) << (8 * i);
        const QString target = resolvePointer ? resolvePointer(ptr) : QString();
        const QString suffix = target.isEmpty() ? QString() : QStringLiteral(" -> %1").arg(target);
        lines << QStringLiteral("ptr%1: 0x%2%3").arg(pointerSize * 8).arg(QString::number(ptr, 16), suffix);
    }

    if (lines.size() <= 3) lines << QStringLiteral("ASCII: %1").arg(printableAscii(data));
    return lines.join(QLatin1Char('\n'));
}

}  // namespace rk::ida
