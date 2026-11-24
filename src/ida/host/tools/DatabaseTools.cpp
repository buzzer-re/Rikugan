// IDA database tools.
#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

ea_t resolveAddrOrName(const QJsonValue& value) {
    if (value.isDouble()) return ea_t(qint64(value.toDouble()));
    const QString text = value.isString() ? value.toString() : value.toVariant().toString();
    qint64 n = 0;
    if (parsePythonInt(text, &n)) return ea_t(n);
    const ea_t ea = get_name_ea(BADADDR, text.toUtf8().constData());
    if (ea == BADADDR) throw ToolError(QStringLiteral("Unknown address or name: ") + text);
    return ea;
}

int pointerSize() { return inf_is_64bit() ? 8 : inf_is_32bit_exactly() ? 4 : 2; }

QString segmentName(ea_t ea) {
    segment_t* seg = getseg(ea);
    if (seg == nullptr) return {};
    qstring name;
    if (get_segm_name(&name, seg) <= 0) return {};
    return qs(name);
}

struct ImportRows {
    QStringList* rows;
    QString module;
};

int idaapi importCallback(ea_t ea, const char* name, uval_t ordinal, void* param) {
    auto* ctx = static_cast<ImportRows*>(param);
    if (name != nullptr && *name != '\0')
        *ctx->rows << QStringLiteral("  [") + ctx->module + QStringLiteral("]  ") + hx(ea) + QStringLiteral("  ") + qs(name);
    else
        *ctx->rows << QStringLiteral("  [") + ctx->module + QStringLiteral("]  ") + hx(ea) +
                          QStringLiteral("  ordinal #%1").arg(qint64(ordinal));
    return 1;
}

}  // namespace

QString binaryInfoText() {
    char root[QMAXPATH];
    if (get_root_filename(root, sizeof(root)) < 0) root[0] = '\0';
    QStringList lines{QStringLiteral("File: ") + QString::fromUtf8(root)};
    lines << QStringLiteral("Processor: ") + qs(inf_get_procname());
    if (inf_is_64bit()) lines << QStringLiteral("Bits: 64");
    else if (inf_is_32bit_exactly()) lines << QStringLiteral("Bits: 32");
    else lines << QStringLiteral("Bits: 16");
    lines << QStringLiteral("Entry point: ") + hx(inf_get_start_ea());
    lines << QStringLiteral("Min address: ") + hx(inf_get_min_ea());
    lines << QStringLiteral("Max address: ") + hx(inf_get_max_ea());
    char ftype[MAXSTR];
    get_file_type_name(ftype, sizeof(ftype));
    lines << QStringLiteral("File type: ") + QString::fromUtf8(ftype);
    lines << QStringLiteral("Functions: %1").arg(qint64(get_func_qty()));
    return lines.join(QLatin1Char('\n'));
}

void registerDatabaseTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "list_segments", "database", "List all segments in the binary.", {}, 0, [](ToolCall&) {
        QStringList lines{QStringLiteral("Segments:")};
        const int qty = get_segm_qty();
        for (int n = 0; n < qty; ++n) {
            segment_t* seg = getnseg(n);
            if (seg == nullptr) continue;
            const ea_t start = seg->start_ea, end = seg->end_ea;
            QString perms;
            if (seg->perm & SEGPERM_READ) perms += QLatin1Char('R');
            if (seg->perm & SEGPERM_WRITE) perms += QLatin1Char('W');
            if (seg->perm & SEGPERM_EXEC) perms += QLatin1Char('X');
            lines << QStringLiteral("  ") + ljust(segmentName(start), 16) + QStringLiteral("  ") + hx(start) +
                         QStringLiteral("\u2013") + hx(end) + QStringLiteral("  (") + altHex(qint64(end - start)) +
                         QStringLiteral(" bytes)  ") + perms;
        }
        return ToolResult::ok(lines.join(QLatin1Char('\n')));
    });

    addTool(reg, "list_imports", "database", "List imported functions with pagination.",
            {intParam("offset", "Start index for pagination", 0), intParam("limit", "Max imports to return", 80)}, 0,
            [](ToolCall& call) {
                QStringList rows;
                const uint nimps = get_import_module_qty();
                for (uint i = 0; i < nimps; ++i) {
                    qstring module;
                    get_import_module_name(&module, int(i));
                    ImportRows ctx{&rows, qs(module)};
                    enum_import_names(int(i), importCallback, &ctx);
                }
                const qint64 offset = call.integer("offset"), limit = call.integer("limit");
                const auto [pageOffset, pageLimit] = normalizePage(offset, limit);
                const qint64 next = std::min<qint64>(rows.size(), pageOffset + pageLimit);
                return ToolResult::ok(formatPage(rows, offset, limit, QStringLiteral("Imports"), QStringLiteral("  (none)"),
                                                 QStringLiteral("Call list_imports with offset=%1 limit=%2.").arg(next).arg(pageLimit)));
            });

    addTool(reg, "list_exports", "database", "List exported functions/symbols with pagination.",
            {intParam("offset", "Start index for pagination", 0), intParam("limit", "Max exports to return", 80)}, 0,
            [](ToolCall& call) {
                QStringList rows;
                const size_t n = get_entry_qty();
                for (size_t i = 0; i < n; ++i) {
                    const uval_t ordinal = get_entry_ordinal(i);
                    qstring name;
                    get_entry_name(&name, ordinal);
                    rows << QStringLiteral("  ") + hx(get_entry(ordinal)) + QStringLiteral("  ") + qs(name);
                }
                const qint64 offset = call.integer("offset"), limit = call.integer("limit");
                const auto [pageOffset, pageLimit] = normalizePage(offset, limit);
                const qint64 next = std::min<qint64>(rows.size(), pageOffset + pageLimit);
                return ToolResult::ok(formatPage(rows, offset, limit, QStringLiteral("Exports"), QStringLiteral("  (none)"),
                                                 QStringLiteral("Call list_exports with offset=%1 limit=%2.").arg(next).arg(pageLimit)));
            });

    addTool(reg, "get_binary_info", "database", "Get general information about the loaded binary.", {}, Cacheable,
            [](ToolCall&) { return ToolResult::ok(binaryInfoText()); });

    addTool(reg, "read_bytes", "database", "Read raw bytes at an address and return as hex dump.",
            {strParam("address", "Start address (hex string)"), intParam("size", "Number of bytes to read", 64)}, 0,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const qint64 size = std::min<qint64>(call.integer("size"), 1024);
                QStringList lines;
                for (qint64 off = 0; off < size; off += 16) {
                    const ea_t rowEa = ea + ea_t(off);
                    QStringList hexParts;
                    QString ascii;
                    for (int j = 0; j < 16; ++j) {
                        if (off + j >= size) {
                            hexParts << QStringLiteral("  ");
                            ascii += QLatin1Char(' ');
                        } else {
                            const uint64 b = get_wide_byte(rowEa + j);
                            hexParts << QString::number(b, 16).rightJustified(2, QLatin1Char('0'));
                            ascii += b >= 0x20 && b < 0x7f ? QChar(char16_t(b)) : QChar(u'.');
                        }
                    }
                    const QString hexStr = hexParts.mid(0, 8).join(QLatin1Char(' ')) + QStringLiteral("  ") +
                                           hexParts.mid(8).join(QLatin1Char(' '));
                    lines << QStringLiteral("  ") + hexPad(rowEa, 8) + QStringLiteral("  ") + hexStr + QStringLiteral("  |") +
                                 ascii + QLatin1Char('|');
                }
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "read_global_value", "database", "Read and interpret a global variable or data value.",
            {strParam("address", "Global/data address or symbol name"),
             strParam("type_hint", "auto, u8/i8/u16/i16/u32/i32/u64/i64, ptr, string, utf16, or bytes", "auto"),
             intParam("size", "Bytes to inspect for auto/string/bytes; 0 selects a sensible default", 0)},
            0, [](ToolCall& call) {
                const ea_t ea = resolveAddrOrName(call.value("address"));
                const QString typeHint = call.str("type_hint");
                const int psize = pointerSize();
                const int readSize = bytesNeededForType(typeHint, psize, call.integer("size"));
                QByteArray data;
                data.reserve(readSize);
                for (int i = 0; i < readSize; ++i) data.append(char(get_wide_byte(ea + i) & 0xff));
                return ToolResult::ok(formatGlobalValue(ea, data, psize, typeHint, nameAt(ea), [](uint64_t p) {
                    if (p == 0 || ea_t(p) == BADADDR) return QString();
                    return nameAt(ea_t(p));
                }));
            });
}

}  // namespace rk::ida
