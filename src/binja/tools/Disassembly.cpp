// Binary Ninja disassembly tools.
#include <QRegularExpression>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

QString formatLine(BinaryView* bv, uint64_t ea) {
    const QString text = disassemblyLine(bv, ea);
    if (text.isEmpty()) return {};
    const QString comment = commentAt(bv, ea);
    return QStringLiteral("  %1  %2%3")
        .arg(hexPad(ea, 8), text, comment.isEmpty() ? QString() : QStringLiteral("  ; %1").arg(comment));
}

}  // namespace

void registerDisassemblyTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "read_disassembly", "disassembly", "Read paginated disassembly starting at the given address.",
        {param("address", ToolParam::String, "Start address (hex string)"),
         param("offset", ToolParam::Integer, "Instruction offset from the start address", 0),
         param("limit", ToolParam::Integer, "Max instructions to return", 30),
         param("count", ToolParam::Integer, "Deprecated: use limit instead", 0)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t start = call.address("address");
            const qint64 count = call.integer("count");
            const auto [offset, limit] = normalizePage(call.integer("offset"), count ? count : call.integer("limit"));
            uint64_t ea = start;
            QStringList lines;
            qint64 index = 0;
            while (lines.size() < limit) {
                call.throwIfCancelled();
                const QString line = formatLine(bv, ea);
                if (line.isEmpty()) break;
                if (index >= offset) lines.append(line);
                ++index;
                const size_t step = instructionLength(bv, ea);
                if (step == 0) break;
                ea += step;
            }
            const QString header =
                QStringLiteral("Disassembly from %1 instructions %2-%3:").arg(hex(start)).arg(offset).arg(offset + lines.size());
            if (lines.size() >= limit)
                lines.append(QStringLiteral("  ... continue with offset=%1 limit=%2.").arg(offset + lines.size()).arg(limit));
            if (lines.isEmpty()) lines.append(QStringLiteral("  (none)"));
            lines.prepend(header);
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "read_function_disassembly", "disassembly", "Read paginated disassembly of a function.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("offset", ToolParam::Integer, "Start instruction index for pagination", 0),
         param("limit", ToolParam::Integer, "Max instructions to return", 120)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            QStringList rows;
            for (uint64_t insn : functionInstructionAddresses(bv, func)) {
                call.throwIfCancelled();
                const QString line = formatLine(bv, insn);
                if (!line.isEmpty()) rows.append(line);
            }
            return ToolResult::ok(formatToolPage(
                rows, call.integer("offset"), call.integer("limit"),
                QStringLiteral("Disassembly for %1 at %2").arg(functionName(func), hex(func->GetStart())),
                "read_function_disassembly"));
        }));

    registry.add(makeTool(
        "get_instruction_info", "disassembly", "Get detailed info about a single instruction.",
        {param("address", ToolParam::String, "Instruction address (hex string)")}, 0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString text = disassemblyLine(bv, ea);
            if (text.isEmpty()) return ToolResult::ok(QStringLiteral("No instruction at %1").arg(hex(ea)));
            size_t size = instructionLength(bv, ea);
            if (size == 0) size = 1;
            // text.split(None, 1)[0] and the rest, stripped
            const QString trimmed = text.trimmed();
            const qsizetype ws = trimmed.indexOf(QRegularExpression(QStringLiteral("\\s")));
            const QString mnemonic = trimmed.isEmpty() ? QStringLiteral("?") : ws < 0 ? trimmed : trimmed.left(ws);
            const QString operands =
                text.size() > mnemonic.size() ? text.mid(mnemonic.size()).trimmed() : QString();
            QStringList parts{
                QStringLiteral("Address: %1").arg(hex(ea)),
                QStringLiteral("Mnemonic: %1").arg(mnemonic),
                QStringLiteral("Operands: %1").arg(operands.isEmpty() ? QStringLiteral("(none)") : operands),
                QStringLiteral("Size: %1 bytes").arg(size),
                QStringLiteral("Bytes: %1").arg(hexBytes(readSafe(bv, ea, size))),
            };
            const QString comment = commentAt(bv, ea);
            if (!comment.isEmpty()) parts.append(QStringLiteral("Comment: %1").arg(comment));
            return ToolResult::ok(parts.join(QLatin1Char('\n')));
        }));
}

}  // namespace rk::binja
