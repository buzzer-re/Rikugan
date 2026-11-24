// IDA disassembly tools.
#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

// mnemonic plus the first three operands
QString operandsText(ea_t ea) {
    QString ops = operandAt(ea, 0);
    const QString op2 = operandAt(ea, 1);
    if (!op2.isEmpty()) ops += QStringLiteral(", ") + op2;
    const QString op3 = operandAt(ea, 2);
    if (!op3.isEmpty()) ops += QStringLiteral(", ") + op3;
    return ops;
}

QString row(ea_t ea, const QString& mnem, const QString& ops, const QString& comment) {
    const QString cmt = comment.isEmpty() ? QString() : QStringLiteral("  ; ") + comment;
    return QStringLiteral("  ") + hexPad(ea, 8) + QStringLiteral("  ") + ljust(mnem, 8) + QLatin1Char(' ') + ops + cmt;
}

}  // namespace

void registerDisassemblyTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "read_disassembly", "disassembly", "Read paginated disassembly starting at the given address.",
            {strParam("address", "Start address (hex string)"),
             intParam("offset", "Instruction offset from the start address", 0),
             intParam("limit", "Max instructions to return", 30), intParam("count", "Deprecated: use limit instead", 0)},
            0, [](ToolCall& call) {
                const ea_t start = call.address("address");
                const qint64 count = call.integer("count");
                const auto [offset, limit] = normalizePage(call.integer("offset"), count ? count : call.integer("limit"));
                ea_t ea = start;
                QStringList lines;
                qint64 index = 0;
                while (lines.size() < limit) {
                    const QString mnem = mnemonicAt(ea);
                    if (mnem.isEmpty()) break;
                    const QString ops = operandsText(ea);
                    QString comment = commentAt(ea, false);
                    if (comment.isEmpty()) comment = commentAt(ea, true);
                    if (index >= offset) lines << row(ea, mnem, ops, comment);
                    ++index;
                    ea = next_head(ea, ea + 0x1000);
                    if (ea == BADADDR) break;
                    if ((index & 0xfff) == 0) call.throwIfCancelled();
                }
                const QString header =
                    QStringLiteral("Disassembly from ") + hx(start) +
                    QStringLiteral(" instructions %1-%2:").arg(offset).arg(offset + lines.size());
                if (ea != BADADDR)
                    lines << QStringLiteral("  ... continue with offset=%1 limit=%2.").arg(offset + lines.size()).arg(limit);
                if (lines.isEmpty()) lines << QStringLiteral("  (none)");
                return ToolResult::ok(header + QLatin1Char('\n') + lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "read_function_disassembly", "disassembly", "Read paginated disassembly of a function.",
            {strParam("address", "Function address (hex string)"),
             intParam("offset", "Start instruction index for pagination", 0),
             intParam("limit", "Max instructions to return", 120)},
            0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at ") + hx(ea));
                QStringList rows;
                func_item_iterator_t fii;
                for (bool ok = fii.set(pfn); ok; ok = fii.next_code()) {
                    const ea_t head = fii.current();
                    const QString mnem = mnemonicAt(head);
                    if (mnem.isEmpty()) continue;
                    QString comment = commentAt(head, false);
                    if (comment.isEmpty()) comment = commentAt(head, true);
                    rows << row(head, mnem, operandsText(head), comment);
                    if ((rows.size() & 0xfff) == 0) call.throwIfCancelled();
                }
                const qint64 offset = call.integer("offset"), limit = call.integer("limit");
                const auto [pageOffset, pageLimit] = normalizePage(offset, limit);
                const qint64 next = std::min<qint64>(rows.size(), pageOffset + pageLimit);
                return ToolResult::ok(formatPage(
                    rows, offset, limit, QStringLiteral("Disassembly for function ") + hx(pfn->start_ea), QStringLiteral("  (none)"),
                    QStringLiteral("Call read_function_disassembly with offset=%1 limit=%2.").arg(next).arg(pageLimit)));
            });

    addTool(reg, "get_instruction_info", "disassembly", "Get detailed info about a single instruction.",
            {strParam("address", "Instruction address (hex string)")}, 0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString mnem = mnemonicAt(ea);
                if (mnem.isEmpty()) return ToolResult::ok(QStringLiteral("No instruction at ") + hx(ea));
                const qint64 size = qint64(get_item_end(ea) - ea);
                QStringList ops;
                for (int i = 0; i < 6; ++i) {
                    const QString op = operandAt(ea, i);
                    if (!op.isEmpty()) ops << op;
                }
                QStringList bytes;
                for (qint64 i = 0; i < size; ++i)
                    bytes << QString::number(get_wide_byte(ea + ea_t(i)), 16).rightJustified(2, QLatin1Char('0'));
                QStringList parts{QStringLiteral("Address: ") + hx(ea), QStringLiteral("Mnemonic: ") + mnem,
                                  QStringLiteral("Operands: ") + (ops.isEmpty() ? QStringLiteral("(none)") : ops.join(QStringLiteral(", "))),
                                  QStringLiteral("Size: %1 bytes").arg(size), QStringLiteral("Bytes: ") + bytes.join(QLatin1Char(' '))};
                const QString comment = commentAt(ea, false);
                if (!comment.isEmpty()) parts << QStringLiteral("Comment: ") + comment;
                return ToolResult::ok(parts.join(QLatin1Char('\n')));
            });
}

}  // namespace rk::ida
