// Binary Ninja IL tools.
#include <map>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

const std::map<QString, QString>& ilLevels() {
    static const std::map<QString, QString> levels{
        {"llil", "llil"},          {"mlil", "mlil"},          {"hlil", "hlil"},
        {"MMAT_GENERATED", "llil"}, {"MMAT_PREOPTIMIZED", "llil"}, {"MMAT_LOCOPT", "mlil"},
        {"MMAT_CALLS", "mlil"},    {"MMAT_GLBOPT1", "mlil"},  {"MMAT_GLBOPT2", "mlil"},
        {"MMAT_GLBOPT3", "mlil"},  {"MMAT_LVARS", "hlil"},
    };
    return levels;
}

QString parseLevel(const QString& value) {
    const QString raw = value.trimmed();
    if (ilLevels().count(raw.toUpper())) return raw.toUpper();
    if (ilLevels().count(raw.toLower())) return raw.toLower();
    return QStringLiteral("hlil");
}

QString resolvedLevel(const QString& level) {
    const auto it = ilLevels().find(level);
    return it == ilLevels().end() ? level : it->second;
}

IlLevel toLevel(const QString& name) {
    if (name == QLatin1String("llil")) return IlLevel::Llil;
    if (name == QLatin1String("mlil")) return IlLevel::Mlil;
    return IlLevel::Hlil;
}

// The requested level, then hlil, mlil, llil
IlFunction ilFor(Function* func, const QString& level) {
    for (IlLevel l : {toLevel(resolvedLevel(level)), IlLevel::Hlil, IlLevel::Mlil, IlLevel::Llil})
        if (IlFunction il = ilOf(func, l)) return il;
    return {};
}

QString renderIl(const IlFunction& il) {
    QStringList lines;
    qint64 i = 0;
    for (size_t index : il.instructionOrder()) {
        lines.append(QStringLiteral("[%1] %2  %3")
                         .arg(rjust(i, 4), hexPad(il.instructionAddress(index), 8), il.instructionText(index)));
        ++i;
    }
    return lines.isEmpty() ? QStringLiteral("(no IL output)") : lines.join(QLatin1Char('\n'));
}

bool patchNop(BinaryView* bv, uint64_t ea) {
    Ref<Architecture> arch = bv->GetDefaultArchitecture();
    if (arch && bv->ConvertToNop(arch, ea)) return true;
    const size_t size = std::max<size_t>(1, instructionLength(bv, ea));
    QByteArray nop;
    if (arch) {
        DataBuffer assembled;
        std::string errors;
        if (arch->Assemble("nop", ea, assembled, errors) && assembled.GetLength() > 0)
            nop = QByteArray(static_cast<const char*>(assembled.GetData()), qsizetype(assembled.GetLength()));
    }
    if (nop.isEmpty()) nop = QByteArray("\x90", 1);
    QByteArray data;
    while (size_t(data.size()) < size) data.append(nop);
    data.truncate(qsizetype(size));
    bv->Write(ea, data.constData(), size_t(data.size()));
    return true;
}

}  // namespace

void registerIlTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "get_il", "il", "Get function IL at a given level (LLIL/MLIL/HLIL).",
        {param("address", ToolParam::String, "Function address (hex string, e.g. '0x401000')"),
         param("level", ToolParam::String, "IL level: 'llil', 'mlil', or 'hlil'", "hlil")},
        RequiresDecompiler, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const QString lvl = parseLevel(call.str("level"));
            const IlFunction il = ilFor(func, lvl);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), lvl));
            const QString header = QStringLiteral("=== IL for %1 at %2 ===\nFunction: %3 - %4\n")
                                       .arg(functionName(func), resolvedLevel(lvl), hex(func->GetStart()),
                                            hex(func->GetHighestAddress()));
            return ToolResult::ok(header + renderIl(il));
        }));

    registry.add(makeTool(
        "get_il_block", "il", "Get detailed IL for a single basic block.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("block_index", ToolParam::Integer, "Block index (0-based)"),
         param("level", ToolParam::String, "IL level: 'llil', 'mlil', or 'hlil'", "hlil")},
        RequiresDecompiler, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const qint64 blockIndex = call.integer("block_index");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const QString lvl = parseLevel(call.str("level"));
            const IlFunction il = ilFor(func, lvl);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for this function"));
            const std::vector<Ref<BasicBlock>> blocks = il.basicBlocks();
            if (blockIndex < 0 || blockIndex >= qint64(blocks.size()))
                return ToolResult::ok(QStringLiteral("Block index %1 out of range [0, %2]")
                                          .arg(blockIndex)
                                          .arg(std::max<qint64>(0, qint64(blocks.size()) - 1)));
            const Ref<BasicBlock>& blk = blocks[size_t(blockIndex)];
            const uint64_t start = blk->GetStart();
            const uint64_t end = blk->GetEnd();
            QStringList lines{QStringLiteral("=== Block %1 of %2 at %3 ===").arg(blockIndex).arg(functionName(func), resolvedLevel(lvl)),
                              QString(), QStringLiteral("Range: %1 .. %2").arg(hex(start), hex(end))};
            // insns[start:end] over the function's instruction list
            const std::vector<size_t> insns = il.instructionOrder();
            std::vector<size_t> blockIns;
            if (start < insns.size())
                blockIns.assign(insns.begin() + qsizetype(start), insns.begin() + qsizetype(std::min<uint64_t>(std::max(end, start), insns.size())));
            lines.append(QString());
            lines.append(QStringLiteral("Instructions:"));
            qint64 i = 0;
            for (size_t index : blockIns) {
                lines.append(QStringLiteral("  [%1] %2  %3")
                                 .arg(rjust(i, 3), hexPad(il.instructionAddress(index), 8), il.instructionText(index)));
                ++i;
            }
            lines.append(QStringLiteral("\nTotal: %1 instructions").arg(blockIns.size()));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "nop_instructions", "il", "Patch selected instructions to NOP bytes and update analysis.",
        {param("func_address", ToolParam::String, "Address of the function to patch"),
         param("instruction_addresses", ToolParam::String,
               "Comma-separated hex addresses of instructions to NOP (e.g. '0x401004,0x401008,0x40100c')")},
        RequiresDecompiler | Mutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("func_address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            QStringList rawAddrs;
            for (const QString& part : call.str("instruction_addresses").split(QLatin1Char(',')))
                if (!part.trimmed().isEmpty()) rawAddrs.append(part.trimmed());
            if (rawAddrs.isEmpty()) return ToolResult::ok(QStringLiteral("No instruction addresses provided."));
            int patched = 0;
            QStringList failed;
            withUndo(bv, [&] {
                for (const QString& raw : rawAddrs) {
                    qint64 target = 0;
                    if (!parsePythonInt(raw, &target)) {
                        failed.append(raw);
                        continue;
                    }
                    if (patchNop(bv, uint64_t(target))) ++patched;
                    else failed.append(raw);
                }
                updateAnalysisAndWait(bv, func);
            });
            QString summary = QStringLiteral("Patched %1/%2 instruction(s) with NOP.").arg(patched).arg(rawAddrs.size());
            if (!failed.isEmpty()) summary += QStringLiteral(" Failed: %1.").arg(failed.join(QStringLiteral(", ")));
            return ToolResult::ok(summary);
        }));

    registry.add(makeTool(
        "redecompile_function", "il", "Refresh analysis and return current pseudocode for a function.",
        {param("address", ToolParam::String, "Function address to redecompile (hex string)")}, RequiresDecompiler,
        [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            updateAnalysisAndWait(bv, func);
            return ToolResult::ok(QStringLiteral("=== Redecompiled %1 ===\n\n%2")
                                      .arg(functionName(func), pseudocode(bv, func->GetStart(), false)));
        }));
}

}  // namespace rk::binja
