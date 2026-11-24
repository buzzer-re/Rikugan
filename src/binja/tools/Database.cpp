// Binary Ninja database tools and the global-value formatting they use.
#include <algorithm>
#include <map>

#include <QStringDecoder>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

// ---- value formatting ----

const std::map<QString, int>& typeSizes() {
    static const std::map<QString, int> sizes{
        {"u8", 1},  {"i8", 1},     {"byte", 1}, {"u16", 2},    {"i16", 2},   {"word", 2},
        {"u32", 4}, {"i32", 4},    {"dword", 4}, {"float", 4}, {"u64", 8},   {"i64", 8},
        {"qword", 8}, {"double", 8}, {"ptr", 0},  {"pointer", 0},
    };
    return sizes;
}

std::optional<int> typeSize(const QString& hint) {
    const auto it = typeSizes().find(hint);
    if (it == typeSizes().end()) return std::nullopt;
    return it->second;
}

QString normalizeTypeHint(const QString& typeHint) {
    QString hint = typeHint.isEmpty() ? QStringLiteral("auto") : typeHint;
    hint = hint.trimmed().toLower();
    hint.replace(QStringLiteral("uint"), QStringLiteral("u"));
    hint.replace(QStringLiteral("int"), QStringLiteral("i"));
    return hint;
}

bool isOneOf(const QString& s, std::initializer_list<const char*> options) {
    for (const char* o : options)
        if (s == QLatin1String(o)) return true;
    return false;
}

qint64 bytesNeededForType(const QString& typeHint, qint64 pointerSize, qint64 requested) {
    const QString hint = normalizeTypeHint(typeHint);
    if (isOneOf(hint, {"string", "str", "utf8", "utf-8", "utf16", "utf-16", "bytes", "auto"}))
        return std::max<qint64>(1, std::min<qint64>(requested ? requested : 64, 256));
    const std::optional<int> size = typeSize(hint);
    if (!size) return std::max<qint64>(1, std::min<qint64>(requested ? requested : pointerSize, 256));
    if (*size == 0) return std::max<qint64>(1, pointerSize);
    return *size;
}

QString printableAscii(const QByteArray& data) {
    QString out;
    out.reserve(data.size());
    for (char c : data) {
        const uint8_t b = uint8_t(c);
        out.append(b >= 0x20 && b < 0x7F ? QChar(b) : QChar(u'.'));
    }
    return out;
}

// str.isprintable() for one code point
bool pyPrintable(char32_t cp) {
    if (cp == U' ') return true;
    switch (QChar::category(cp)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Space:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph: return false;
    default: return true;
    }
}

// repr(str)
QString pyRepr(const QString& text) {
    const bool useDouble = text.contains(QLatin1Char('\'')) && !text.contains(QLatin1Char('"'));
    const QChar quote = useDouble ? QLatin1Char('"') : QLatin1Char('\'');
    QString out(quote);
    for (char32_t cp : text.toUcs4()) {
        if (cp == quote.unicode() || cp == U'\\') {
            out.append(QLatin1Char('\\'));
            out.append(QChar(char16_t(cp)));
        } else if (cp == U'\t') {
            out.append(QStringLiteral("\\t"));
        } else if (cp == U'\n') {
            out.append(QStringLiteral("\\n"));
        } else if (cp == U'\r') {
            out.append(QStringLiteral("\\r"));
        } else if (cp < 0x20 || cp == 0x7F) {
            out.append(QStringLiteral("\\x%1").arg(uint(cp), 2, 16, QLatin1Char('0')));
        } else if (cp < 0x7F || pyPrintable(cp)) {
            out.append(QString::fromUcs4(&cp, 1));
        } else if (cp < 0x100) {
            out.append(QStringLiteral("\\x%1").arg(uint(cp), 2, 16, QLatin1Char('0')));
        } else if (cp < 0x10000) {
            out.append(QStringLiteral("\\u%1").arg(uint(cp), 4, 16, QLatin1Char('0')));
        } else {
            out.append(QStringLiteral("\\U%1").arg(uint(cp), 8, 16, QLatin1Char('0')));
        }
    }
    out.append(quote);
    return out;
}

bool mostlyPrintable(const QString& text) {
    const QList<uint> cps = text.toUcs4();
    qint64 printable = 0;
    for (char32_t cp : cps)
        if (pyPrintable(cp) || cp == U'\r' || cp == U'\n' || cp == U'\t') ++printable;
    return printable >= std::max<qint64>(1, qint64(double(cps.size()) * 0.8));
}

std::optional<QString> decodeUtf8(const QByteArray& data) {
    const qsizetype nul = data.indexOf('\0');
    const QByteArray raw = nul < 0 ? data : data.left(nul);
    if (raw.isEmpty()) return std::nullopt;
    const QString text = QString::fromUtf8(raw);
    if (!mostlyPrintable(text)) return std::nullopt;
    return text;
}

std::optional<QString> decodeUtf16(const QByteArray& data) {
    if (data.size() < 2) return std::nullopt;
    QByteArray raw = data;
    for (qsizetype idx = 0; idx < std::max<qsizetype>(0, data.size() - 1); idx += 2) {
        if (data[idx] == '\0' && data[idx + 1] == '\0') {
            raw = data.left(idx);
            break;
        }
    }
    if (raw.isEmpty()) return std::nullopt;
    QStringDecoder dec(QStringDecoder::Utf16LE);
    QString text = dec.decode(raw.left(raw.size() & ~qsizetype(1)));
    if (raw.size() & 1) text.append(QChar(0xFFFD));  // truncated data, errors='replace'
    if (!mostlyPrintable(text)) return std::nullopt;
    return text;
}

uint64_t littleEndian(const QByteArray& data, int width) {
    uint64_t v = 0;
    for (int i = width - 1; i >= 0; --i) v = (v << 8) | uint8_t(data[i]);
    return v;
}

int64_t signedValue(uint64_t value, int bits) {
    if (bits >= 64) return int64_t(value);
    const uint64_t sign = uint64_t(1) << (bits - 1);
    return (value & sign) ? int64_t(value) - (int64_t(1) << bits) : int64_t(value);
}

QString formatGlobalValue(uint64_t address, const QByteArray& data, qint64 pointerSize, const QString& typeHint,
                          const QString& name, const std::function<QString(uint64_t)>& resolvePointer) {
    const QString hint = normalizeTypeHint(typeHint);
    QStringList lines{QStringLiteral("Global value at %1%2")
                          .arg(hex(address), name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name)),
                      QStringLiteral("Type hint: %1").arg(hint), QStringLiteral("Bytes: %1").arg(hexBytes(data))};
    if (hint == QLatin1String("bytes")) {
        lines.append(QStringLiteral("ASCII: %1").arg(printableAscii(data)));
        return lines.join(QLatin1Char('\n'));
    }
    if (isOneOf(hint, {"string", "str", "utf8", "utf-8", "auto"}))
        if (const auto text = decodeUtf8(data)) lines.append(QStringLiteral("UTF-8 string: %1").arg(pyRepr(*text)));
    if (isOneOf(hint, {"utf16", "utf-16", "auto"}))
        if (const auto text = decodeUtf16(data))
            lines.append(QStringLiteral("UTF-16LE string: %1").arg(pyRepr(*text)));

    std::vector<qint64> widths;
    if (hint == QLatin1String("auto")) widths = {1, 2, 4, 8};
    const std::optional<int> explicitWidth = typeSize(hint);
    if (explicitWidth && *explicitWidth) widths = {*explicitWidth};
    else if (isOneOf(hint, {"ptr", "pointer"})) widths = {pointerSize};

    std::vector<qint64> seen;
    const bool isSigned = hint.startsWith(QLatin1Char('i'));
    for (qint64 width : widths) {
        if (std::find(seen.begin(), seen.end(), width) != seen.end() || width <= 0 || data.size() < width) continue;
        seen.push_back(width);
        const int bits = int(width * 8);
        const uint64_t value = littleEndian(data, int(width));
        if (isSigned || hint == QLatin1String("auto"))
            lines.append(QStringLiteral("%1%2: %3")
                             .arg(isSigned ? QLatin1Char('i') : QLatin1Char('s'))
                             .arg(bits)
                             .arg(signedValue(value, bits)));
        if (hint.startsWith(QLatin1Char('u')) || isOneOf(hint, {"byte", "word", "dword", "qword", "auto"}))
            lines.append(QStringLiteral("u%1: %2 (%3)").arg(bits).arg(value).arg(hex(value)));
    }

    if (data.size() >= pointerSize && (pointerSize == 4 || pointerSize == 8)) {
        const uint64_t ptr = littleEndian(data, int(pointerSize));
        const QString target = resolvePointer ? resolvePointer(ptr) : QString();
        lines.append(QStringLiteral("ptr%1: %2%3")
                         .arg(pointerSize * 8)
                         .arg(hex(ptr), target.isEmpty() ? QString() : QStringLiteral(" -> %1").arg(target)));
    }
    if (lines.size() <= 3) lines.append(QStringLiteral("ASCII: %1").arg(printableAscii(data)));
    return lines.join(QLatin1Char('\n'));
}

// ---- database tools ----

QString segmentNameForAddr(BinaryView* bv, uint64_t ea) {
    for (const Ref<Section>& sec : bv->GetSections())
        if (sec->GetStart() <= ea && ea < sec->GetEnd()) return qs(sec->GetName());
    return {};
}

bool isImportSymbol(Symbol* sym) {
    switch (sym->GetType()) {
    case ImportAddressSymbol:
    case ImportedFunctionSymbol:
    case ImportedDataSymbol:
    case ExternalSymbol: return true;
    default: return false;
    }
}

bool isExportSymbol(Symbol* sym) {
    const BNSymbolType type = sym->GetType();
    const BNSymbolBinding binding = sym->GetBinding();
    return (type == FunctionSymbol || type == DataSymbol) && (binding == GlobalBinding || binding == WeakBinding);
}

QStringList symbolRows(BinaryView* bv, bool (*keep)(Symbol*)) {
    std::vector<std::pair<uint64_t, QString>> rows;
    for (const Ref<Symbol>& sym : bv->GetSymbols())
        if (keep(sym)) rows.emplace_back(sym->GetAddress(), symbolName(sym));
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QStringList out;
    for (const auto& [ea, name] : rows) out.append(QStringLiteral("  %1  %2").arg(hex(ea), name));
    return out;
}

qint64 pointerSizeOf(BinaryView* bv) {
    Ref<Architecture> arch = bv->GetDefaultArchitecture();
    const size_t size = arch ? arch->GetAddressSize() : 0;
    return size > 0 ? qint64(size) : 8;
}

uint64_t resolveAddrOrName(BinaryView* bv, const QJsonValue& value, const QString& text) {
    if (value.isDouble()) return parseAddress(value);
    qint64 n = 0;
    if (parsePythonInt(text, &n)) return uint64_t(n);
    for (const Ref<Symbol>& sym : symbolsByName(bv, text)) return sym->GetAddress();
    throw ToolError(QStringLiteral("Unknown address or name: %1").arg(text));
}

}  // namespace

QString binaryInfoText(BinaryView* bv) {
    QStringList lines;
    QString filename;
    if (Ref<FileMetadata> file = bv->GetFile()) {
        filename = qs(file->GetFilename());
        if (filename.isEmpty()) filename = qs(file->GetOriginalFilename());
    }
    if (filename.isEmpty()) filename = QStringLiteral("<unknown>");
    lines.append(QStringLiteral("File: %1").arg(filename));
    if (Ref<Architecture> arch = bv->GetDefaultArchitecture()) {
        const QString name = qs(arch->GetName());
        if (!name.isEmpty()) lines.append(QStringLiteral("Processor: %1").arg(name));
        if (const size_t size = arch->GetAddressSize(); size > 0) lines.append(QStringLiteral("Bits: %1").arg(size * 8));
    }
    lines.append(QStringLiteral("Entry point: %1").arg(hex(bv->GetEntryPoint())));
    lines.append(QStringLiteral("Min address: %1").arg(hex(bv->GetStart())));
    lines.append(QStringLiteral("Max address: %1").arg(hex(bv->GetEnd())));
    if (const QString viewType = qs(bv->GetTypeName()); !viewType.isEmpty())
        lines.append(QStringLiteral("File type: %1").arg(viewType));
    lines.append(QStringLiteral("Functions: %1").arg(bv->GetAnalysisFunctionList().size()));
    return lines.join(QLatin1Char('\n'));
}

void registerDatabaseTools(ToolRegistry& registry) {
    registry.add(makeTool("list_segments", "database", "List all segments in the binary.", {}, 0, [](ToolCall& call) {
        Ref<BinaryView> bv = requireView(call);
        QStringList lines{QStringLiteral("Segments:")};
        for (const Ref<Segment>& seg : bv->GetSegments()) {
            const uint64_t start = seg->GetStart();
            const uint64_t end = seg->GetEnd();
            const uint32_t flags = seg->GetFlags();
            QString perms;
            if (flags & SegmentReadable) perms += QLatin1Char('R');
            if (flags & SegmentWritable) perms += QLatin1Char('W');
            if (flags & SegmentExecutable) perms += QLatin1Char('X');
            const QString name = segmentNameForAddr(bv, start);
            const uint64_t size = end > start ? end - start : 0;
            lines.append(QStringLiteral("  %1  %2\u2013%3  (%4 bytes)  %5")
                             .arg(ljust(name.isEmpty() ? QStringLiteral("<segment>") : name, 16), hex(start),
                                  hex(end), hex(size), perms));
        }
        return ToolResult::ok(lines.join(QLatin1Char('\n')));
    }));

    registry.add(makeTool("list_imports", "database", "List imported functions with pagination.",
                          {param("offset", ToolParam::Integer, "Start index for pagination", 0),
                           param("limit", ToolParam::Integer, "Max imports to return", 80)},
                          0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              return ToolResult::ok(formatToolPage(symbolRows(bv, isImportSymbol),
                                                                   call.integer("offset"), call.integer("limit"),
                                                                   QStringLiteral("Imports"), "list_imports"));
                          }));

    registry.add(makeTool("list_exports", "database", "List exported functions/symbols with pagination.",
                          {param("offset", ToolParam::Integer, "Start index for pagination", 0),
                           param("limit", ToolParam::Integer, "Max exports to return", 80)},
                          0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              return ToolResult::ok(formatToolPage(symbolRows(bv, isExportSymbol),
                                                                   call.integer("offset"), call.integer("limit"),
                                                                   QStringLiteral("Exports"), "list_exports"));
                          }));

    registry.add(makeTool("get_binary_info", "database", "Get general information about the loaded binary.", {},
                          Cacheable,
                          [](ToolCall& call) { return ToolResult::ok(binaryInfoText(requireView(call))); }));

    registry.add(makeTool(
        "read_bytes", "database", "Read raw bytes at an address and return as hex dump.",
        {param("address", ToolParam::String, "Start address (hex string)"),
         param("size", ToolParam::Integer, "Number of bytes to read", 64)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const qint64 size = std::clamp<qint64>(call.integer("size"), 0, 1024);
            const QByteArray data = readSafe(bv, ea, size_t(size));
            QStringList lines;
            for (qint64 off = 0; off < size; off += 16) {
                const QByteArray chunk = data.mid(qsizetype(off), 16);
                QStringList hexParts;
                for (char c : chunk) hexParts.append(QString::number(uint8_t(c), 16).rightJustified(2, QLatin1Char('0')));
                while (hexParts.size() < 16) hexParts.append(QStringLiteral("  "));
                const QString hexStr = hexParts.mid(0, 8).join(QLatin1Char(' ')) + QStringLiteral("  ") +
                                       hexParts.mid(8).join(QLatin1Char(' '));
                lines.append(QStringLiteral("  %1  %2  |%3|")
                                 .arg(hexPad(ea + uint64_t(off), 8), hexStr, ljust(printableAscii(chunk), 16)));
            }
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "read_global_value", "database", "Read and interpret a global variable or data value.",
        {param("address", ToolParam::String, "Global/data address or symbol name"),
         param("type_hint", ToolParam::String, "auto, u8/i8/u16/i16/u32/i32/u64/i64, ptr, string, utf16, or bytes",
               "auto"),
         param("size", ToolParam::Integer, "Bytes to inspect for auto/string/bytes; 0 selects a sensible default", 0)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = resolveAddrOrName(bv, call.value("address"), call.str("address"));
            const qint64 pointerSize = pointerSizeOf(bv);
            const QString typeHint = call.str("type_hint");
            const qint64 readSize = bytesNeededForType(typeHint, pointerSize, call.integer("size"));
            const QByteArray data = readSafe(bv, ea, size_t(readSize));
            return ToolResult::ok(formatGlobalValue(ea, data, pointerSize, typeHint, resolveNameAt(bv, ea),
                                                    [&](uint64_t ptr) { return resolveNameAt(bv, ptr); }));
        }));
}

}  // namespace rk::binja
