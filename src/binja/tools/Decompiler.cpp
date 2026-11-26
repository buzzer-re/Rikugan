// Binary Ninja decompiler tools.
#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

QStringList pySplitLines(const QString& text) {
    // str.splitlines()
    QStringList out;
    QString cur;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        switch (c) {
        case u'\r':
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) ++i;
            [[fallthrough]];
        case u'\n':
        case u'\v':
        case u'\f':
        case 0x1c:
        case 0x1d:
        case 0x1e:
        case 0x85:
        case 0x2028:
        case 0x2029:
            out.append(cur);
            cur.clear();
            break;
        default: cur.append(QChar(c));
        }
    }
    if (!cur.isEmpty()) out.append(cur);
    return out;
}

// str() of every HLIL instruction, optionally numbered
QString renderHlil(const IlFunction& hlil, bool withLineNumbers) {
    QStringList lines;
    qint64 i = 0;
    for (size_t index : hlil.instructionOrder()) {
        const QString line = hlil.instructionText(index);
        lines.append(withLineNumbers ? QStringLiteral("%1  %2").arg(rjust(i + 1, 4), line) : line);
        ++i;
    }
    return lines.join(QLatin1Char('\n'));
}

// Pseudocode of the function at `ea` (also used by redecompile_function)
QString pseudocode(BinaryView* bv, uint64_t ea, bool withLineNumbers) {
    Ref<Function> func = functionAt(bv, ea);
    if (!func) return QStringLiteral("No function at %1").arg(hex(ea));
    const IlFunction hlil = ilOf(func, IlLevel::Hlil);
    if (!hlil) return QStringLiteral("HLIL not available for function at %1").arg(hex(func->GetStart()));
    const QString text = renderHlil(hlil, withLineNumbers);
    return text.isEmpty() ? QStringLiteral("(no pseudocode)") : text;
}

void registerDecompilerTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "decompile_function", "decompiler",
        "Decompile the function at the given address and return paginated pseudocode.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("offset", ToolParam::Integer, "Start pseudocode line for pagination", 0),
         param("limit", ToolParam::Integer, "Max pseudocode lines to return", 160),
         param("with_line_numbers", ToolParam::Boolean, "Include pseudocode line numbers", true)},
        RequiresDecompiler | Cacheable, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction hlil = ilOf(func, IlLevel::Hlil);
            if (!hlil)
                return ToolResult::ok(QStringLiteral("HLIL not available for function at %1").arg(hex(func->GetStart())));
            const QStringList rows = pySplitLines(renderHlil(hlil, call.boolean("with_line_numbers")));
            return ToolResult::ok(formatToolPage(rows, call.integer("offset"), call.integer("limit"),
                                                 QStringLiteral("Pseudocode for %1").arg(hex(func->GetStart())),
                                                 "decompile_function", QStringLiteral("  (no pseudocode)")));
        }));

    registry.add(makeTool(
        "get_decompiler_variables", "decompiler", "List local variables from the HLIL output.",
        {param("address", ToolParam::String, "Function address (hex string)")}, RequiresDecompiler,
        [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            QStringList lines{QStringLiteral("Local variables:")};
            Ref<Platform> platform = func->GetPlatform();
            size_t count = 0;
            BNVariableNameAndType* vars = BNGetFunctionVariables(func->GetObject(), &count);
            for (size_t i = 0; i < count; ++i) {
                const Variable var(vars[i].var);
                const QString name = qs(func->GetVariableNameOrDefault(var));
                const Confidence<Ref<Type>> type = func->GetVariableType(var);
                const QString tname = type.GetValue() ? qs(type.GetValue()->GetString(platform)) : QStringLiteral("?");
                // str(VariableSourceType) is the IntEnum's value
                const QString kind = QString::number(int(vars[i].var.type));
                lines.append(QStringLiteral("  %1 %2 %3").arg(ljust(kind, 8), ljust(tname, 20), name));
            }
            BNFreeVariableNameAndTypeList(vars, count);
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));
}

}  // namespace rk::binja
