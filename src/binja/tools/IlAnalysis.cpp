// Binary Ninja IL analysis tools.
#include <algorithm>
#include <set>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

QString branchTypeName(BNBranchType type) {
    switch (type) {
    case UnconditionalBranch: return QStringLiteral("UnconditionalBranch");
    case FalseBranch: return QStringLiteral("FalseBranch");
    case TrueBranch: return QStringLiteral("TrueBranch");
    case CallDestination: return QStringLiteral("CallDestination");
    case FunctionReturn: return QStringLiteral("FunctionReturn");
    case SystemCall: return QStringLiteral("SystemCall");
    case IndirectBranch: return QStringLiteral("IndirectBranch");
    case ExceptionBranch: return QStringLiteral("ExceptionBranch");
    case UnresolvedBranch: return QStringLiteral("UnresolvedBranch");
    case UserDefinedBranch: return QStringLiteral("UserDefinedBranch");
    }
    return QString::number(int(type));
}

// One entry of il.vars / ssa.vars / ssa.ssa_vars
struct VarEntry {
    QString name;
    bool ssa = false;
    BNVariable var{};
    size_t version = 0;
};

QString variableName(Function* func, const BNVariable& var) { return qs(func->GetVariableNameOrDefault(Variable(var))); }

std::vector<VarEntry> mlilHlilVars(Function* func, const IlFunction& il, bool withVersions) {
    std::vector<VarEntry> out;
    size_t count = 0;
    BNVariable* vars = il.mlil ? BNGetMediumLevelILVariables(il.mlil->GetObject(), &count)
                               : BNGetHighLevelILVariables(il.hlil->GetObject(), &count);
    for (size_t i = 0; i < count; ++i) {
        const QString name = variableName(func, vars[i]);
        if (!withVersions) {
            out.push_back({name, false, vars[i], 0});
            continue;
        }
        size_t versionCount = 0;
        size_t* versions = il.mlil ? BNGetMediumLevelILVariableSSAVersions(il.mlil->GetObject(), &vars[i], &versionCount)
                                   : BNGetHighLevelILVariableSSAVersions(il.hlil->GetObject(), &vars[i], &versionCount);
        for (size_t v = 0; v < versionCount; ++v) out.push_back({name, true, vars[i], versions[v]});
        BNFreeILInstructionList(versions);
    }
    BNFreeVariableList(vars);
    return out;
}

// LowLevelILFunction.vars: registers, register stacks and flags
std::vector<VarEntry> llilVars(const IlFunction& il) {
    std::vector<VarEntry> out;
    Ref<Architecture> arch = il.llil->GetArchitecture();
    size_t count = 0;
    uint32_t* regs = BNGetLowLevelRegisters(il.llil->GetObject(), &count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t r = regs[i];
        out.push_back({(r & 0x80000000u) ? QStringLiteral("temp%1").arg(r & 0x7fffffffu)
                                         : (arch ? qs(arch->GetRegisterName(r)) : QString::number(r)),
                       false, {}, 0});
    }
    BNFreeLLILVariablesList(regs);
    uint32_t* stacks = BNGetLowLevelRegisterStacks(il.llil->GetObject(), &count);
    for (size_t i = 0; i < count; ++i)
        out.push_back({arch ? qs(arch->GetRegisterStackName(stacks[i])) : QString::number(stacks[i]), false, {}, 0});
    BNFreeLLILVariablesList(stacks);
    uint32_t* flags = BNGetLowLevelFlags(il.llil->GetObject(), &count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t f = flags[i];
        out.push_back({(f & 0x80000000u) ? QStringLiteral("cond:%1").arg(f & 0x7fffffffu)
                                         : (arch ? qs(arch->GetFlagName(f)) : QString::number(f)),
                       false, {}, 0});
    }
    BNFreeLLILVariablesList(flags);
    return out;
}

// ssa.vars (SSA form: versions for MLIL/HLIL), else il.vars
std::vector<VarEntry> trackableVars(Function* func, const IlFunction& il, const IlFunction& ssa) {
    if (il.llil) {
        std::vector<VarEntry> vars = llilVars(ssa);
        return vars.empty() ? llilVars(il) : vars;
    }
    std::vector<VarEntry> vars = mlilHlilVars(func, ssa, true);
    return vars.empty() ? mlilHlilVars(func, il, false) : vars;
}

void scanVariableUses(const IlFunction& il, const QString& varName, QStringList& lines) {
    for (size_t index : il.instructionOrder()) {
        const QString text = il.instructionText(index);
        if (text.contains(varName))
            lines.append(QStringLiteral("  %1: %2").arg(hex(il.instructionAddress(index)), pyLeft(text, 120)));
    }
}

// An integer CONST/CONST_PTR source operand of a definition
std::optional<int64_t> constantSource(const IlFunction& ssa, size_t defExprOrInstr) {
    if (ssa.mlil) {
        const MediumLevelILInstruction defn = ssa.mlil->GetInstruction(defExprOrInstr);
        size_t operand = 0;
        if (!defn.GetOperandIndexForUsage(SourceExprMediumLevelOperandUsage, operand)) return std::nullopt;
        const MediumLevelILInstruction src = defn.GetSourceExpr();
        if (src.operation == MLIL_CONST || src.operation == MLIL_CONST_PTR) return src.GetConstant();
        return std::nullopt;
    }
    if (ssa.hlil) {
        const HighLevelILInstruction defn = ssa.hlil->GetExpr(defExprOrInstr);
        size_t operand = 0;
        if (!defn.GetOperandIndexForUsage(SourceExprHighLevelOperandUsage, operand)) return std::nullopt;
        const HighLevelILInstruction src = defn.GetSourceExpr();
        if (src.operation == HLIL_CONST || src.operation == HLIL_CONST_PTR) return src.GetConstant();
    }
    return std::nullopt;
}

}  // namespace

void registerIlAnalysisTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "get_cfg", "il", "Get CFG structure: blocks, edges, dominators, back edges, loop detection.",
        {param("address", ToolParam::String, "Function address (hex string, e.g. '0x401000')"),
         param("level", ToolParam::String, "IL level: 'llil', 'mlil', or 'hlil'", "mlil")},
        RequiresDecompiler, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), level));
            const std::vector<Ref<BasicBlock>> blocks = il.basicBlocks();
            if (blocks.empty()) return ToolResult::ok(QStringLiteral("No basic blocks found"));

            QStringList lines{QStringLiteral("=== CFG for %1 at %2 ===").arg(functionName(func), level),
                              QStringLiteral("Blocks: %1").arg(blocks.size()), QString()};
            std::vector<std::pair<uint64_t, uint64_t>> backEdges;
            std::set<uint64_t> loopHeaders;
            for (size_t i = 0; i < blocks.size(); ++i) {
                call.throwIfCancelled();
                const Ref<BasicBlock>& block = blocks[i];
                const uint64_t start = block->GetStart();
                const uint64_t end = block->GetEnd();
                lines.append(QStringLiteral("Block %1: %2-%3 (%4 instructions)").arg(i).arg(hex(start), hex(end)).arg(end - start));
                for (const BasicBlockEdge& edge : block->GetIncomingEdges()) {
                    const uint64_t src = edge.target ? edge.target->GetStart() : 0;
                    lines.append(QStringLiteral("  <- %1 (%2)%3")
                                     .arg(hex(src), branchTypeName(edge.type),
                                          edge.backEdge ? QStringLiteral(" [BACK EDGE]") : QString()));
                    if (edge.backEdge) {
                        backEdges.emplace_back(src, start);
                        loopHeaders.insert(start);
                    }
                }
                for (const BasicBlockEdge& edge : block->GetOutgoingEdges()) {
                    const uint64_t tgt = edge.target ? edge.target->GetStart() : 0;
                    lines.append(QStringLiteral("  -> %1 (%2)%3")
                                     .arg(hex(tgt), branchTypeName(edge.type),
                                          edge.backEdge ? QStringLiteral(" [BACK EDGE]") : QString()));
                    if (edge.backEdge) {
                        backEdges.emplace_back(start, tgt);
                        loopHeaders.insert(tgt);
                    }
                }
                std::vector<uint64_t> doms;
                for (const Ref<BasicBlock>& d : block->GetDominators()) doms.push_back(d->GetStart());
                std::sort(doms.begin(), doms.end());
                if (!doms.empty()) {
                    QStringList parts;
                    for (uint64_t a : doms) parts.append(hex(a));
                    lines.append(QStringLiteral("  dominators: %1").arg(parts.join(QStringLiteral(", "))));
                }
                if (Ref<BasicBlock> idom = block->GetImmediateDominator())
                    lines.append(QStringLiteral("  idom: %1").arg(hex(idom->GetStart())));
                lines.append(QString());
            }
            lines.append(QStringLiteral("--- Summary ---"));
            if (!backEdges.empty()) {
                lines.append(QStringLiteral("Back edges: %1").arg(backEdges.size()));
                for (const auto& [src, tgt] : backEdges) lines.append(QStringLiteral("  %1 -> %2").arg(hex(src), hex(tgt)));
            }
            if (!loopHeaders.empty()) {
                QStringList parts;
                for (uint64_t h : loopHeaders) parts.append(hex(h));
                lines.append(QStringLiteral("Loop headers: %1").arg(parts.join(QStringLiteral(", "))));
            }
            if (backEdges.empty() && loopHeaders.empty()) lines.append(QStringLiteral("No loops detected"));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "track_variable_ssa", "il",
        "Track SSA def-use chains: all versions of a variable, constant values, where used.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("variable_name", ToolParam::String, "Variable name to track"),
         param("level", ToolParam::String, "IL level for SSA: 'mlil' or 'hlil'", "mlil")},
        RequiresDecompiler, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString variableName = call.str("variable_name");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1").arg(functionName(func)));
            const IlFunction ssa = il.ssaForm();
            if (!ssa) return ToolResult::ok(QStringLiteral("SSA form not available at %1 level").arg(level));

            const std::vector<VarEntry> allVars = trackableVars(func, il, ssa);
            const bool found = std::any_of(allVars.begin(), allVars.end(),
                                           [&](const VarEntry& v) { return v.name == variableName; });
            if (!found) {
                std::set<std::string> names;
                for (const VarEntry& v : allVars) names.insert(ss(v.name));
                QStringList shown;
                for (const std::string& n : names) {
                    if (shown.size() >= 30) break;
                    shown.append(qs(n));
                }
                QString msg = QStringLiteral("Variable '%1' not found.\nAvailable variables: %2")
                                  .arg(variableName, shown.join(QStringLiteral(", ")));
                QStringList scan;
                scanVariableUses(il, variableName, scan);
                if (!scan.isEmpty())
                    msg += QStringLiteral("\n\nInstruction scan for '%1':\n").arg(variableName) +
                           scan.mid(0, 30).join(QLatin1Char('\n'));
                return ToolResult::ok(msg);
            }

            QStringList lines{QStringLiteral("=== SSA tracking for '%1' in %2 ===").arg(variableName, functionName(func))};
            int versionCount = 0;
            if (!il.llil) {
                // LLIL SSA registers carry no variable name, so only MLIL/HLIL versions can match
                for (const VarEntry& v : mlilHlilVars(func, ssa, true)) {
                    if (v.name != variableName) continue;
                    call.throwIfCancelled();
                    ++versionCount;
                    lines.append(QStringLiteral("\n--- %1#%2 ---").arg(variableName).arg(v.version));
                    // get_ssa_var_definition: MLIL answers an instruction index, HLIL an expression index
                    if (ssa.mlil) {
                        const size_t def = BNGetMediumLevelILSSAVarDefinition(ssa.mlil->GetObject(), &v.var, v.version);
                        if (def < ssa.instructionCount()) {
                            lines.append(QStringLiteral("  Defined at %1: %2")
                                             .arg(hex(ssa.instructionAddress(def)), ssa.instructionText(def)));
                            if (const auto c = constantSource(ssa, def))
                                lines.append(QStringLiteral("  Constant value: %1 (%2)").arg(*c).arg(signedHex(*c)));
                        }
                    } else {
                        const size_t def = BNGetHighLevelILSSAVarDefinition(ssa.hlil->GetObject(), &v.var, v.version);
                        if (def < ssa.hlil->GetExprCount()) {
                            lines.append(QStringLiteral("  Defined at %1: %2")
                                             .arg(hex(ssa.exprAddress(def)), ssa.exprText(def, true)));
                            if (const auto c = constantSource(ssa, def))
                                lines.append(QStringLiteral("  Constant value: %1 (%2)").arg(*c).arg(signedHex(*c)));
                        }
                    }
                    size_t useCount = 0;
                    size_t* uses = ssa.mlil ? BNGetMediumLevelILSSAVarUses(ssa.mlil->GetObject(), &v.var, v.version, &useCount)
                                            : BNGetHighLevelILSSAVarUses(ssa.hlil->GetObject(), &v.var, v.version, &useCount);
                    lines.append(QStringLiteral("  Uses (%1):").arg(useCount));
                    for (size_t u = 0; u < useCount && u < 20; ++u) {
                        const uint64_t addr = ssa.mlil ? ssa.instructionAddress(uses[u]) : ssa.exprAddress(uses[u]);
                        const QString text = ssa.mlil ? ssa.instructionText(uses[u]) : ssa.exprText(uses[u], true);
                        lines.append(QStringLiteral("    %1: %2").arg(hex(addr), pyLeft(text, 100)));
                    }
                    if (useCount > 20) lines.append(QStringLiteral("    ... and %1 more").arg(useCount - 20));
                    BNFreeILInstructionList(uses);
                }
            }
            if (versionCount == 0) {
                lines.append(QStringLiteral("No SSA versions found \u2014 falling back to instruction scan"));
                scanVariableUses(ssa, variableName, lines);
            }
            lines.append(QStringLiteral("\nTotal SSA versions: %1").arg(versionCount));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));
}

}  // namespace rk::binja
