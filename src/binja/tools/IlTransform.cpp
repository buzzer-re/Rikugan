// Binary Ninja IL transform tools.
#include <QJsonDocument>
#include <QJsonObject>

#include "binja/host/BnHost.h"
#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

// worker threads: updateAnalysisAndWait would freeze the UI on the main thread
constexpr uint32_t kIlMutating = RequiresDecompiler | Mutating;

// One IL instruction (il_func[index])
struct IlInstr {
    size_t instrIndex = 0;
    size_t exprIndex = 0;
    size_t size = 0;
    uint64_t address = 0;
    bool isIf = false;
};

IlInstr instrAt(const IlFunction& il, size_t index) {
    IlInstr out;
    out.instrIndex = index;
    if (il.llil) {
        const LowLevelILInstruction i = il.llil->GetInstruction(index);
        out = {index, i.exprIndex, i.size, i.address, i.operation == LLIL_IF};
    } else if (il.mlil) {
        const MediumLevelILInstruction i = il.mlil->GetInstruction(index);
        out = {index, i.exprIndex, i.size, i.address, i.operation == MLIL_IF};
    } else if (il.hlil) {
        const HighLevelILInstruction i = il.hlil->GetInstruction(index);
        out = {index, i.exprIndex, i.size, i.address, i.operation == HLIL_IF};
    }
    return out;
}

// il_func[index], nullopt when out of range
std::optional<IlInstr> instrAtIndex(const IlFunction& il, qint64 index) {
    const qint64 count = qint64(il.instructionCount());
    if (index < 0) index += count;
    if (index < 0 || index >= count) return std::nullopt;
    return instrAt(il, size_t(index));
}

// condition operand of an IF: (expression index, size)
std::pair<size_t, size_t> conditionOf(const IlFunction& il, size_t index) {
    if (il.llil) {
        const LowLevelILInstruction c = il.llil->GetInstruction(index).GetConditionExpr();
        return {c.exprIndex, c.size};
    }
    if (il.mlil) {
        const MediumLevelILInstruction c = il.mlil->GetInstruction(index).GetConditionExpr();
        return {c.exprIndex, c.size};
    }
    const HighLevelILInstruction c = il.hlil->GetInstruction(index).GetConditionExpr();
    return {c.exprIndex, c.size};
}

ExprId ilConst(const IlFunction& il, size_t size, uint64_t value) {
    if (il.llil) return il.llil->Const(size, value);
    if (il.mlil) return il.mlil->Const(size, value);
    return il.hlil->Const(size, value);
}

ExprId ilNop(const IlFunction& il) {
    if (il.llil) return il.llil->Nop();
    if (il.mlil) return il.mlil->Nop();
    return il.hlil->Nop();
}

ExprId ilCopy(const IlFunction& il, size_t index) {
    if (il.llil) return il.llil->GetInstruction(index).CopyTo(il.llil);
    if (il.mlil) return il.mlil->GetInstruction(index).CopyTo(il.mlil);
    return il.hlil->GetInstruction(index).CopyTo(il.hlil);
}

bool replaceExpr(const IlFunction& il, size_t oldExpr, ExprId newExpr) {
    try {
        if (il.llil) il.llil->ReplaceExpr(oldExpr, newExpr);
        else if (il.mlil) il.mlil->ReplaceExpr(oldExpr, newExpr);
        else il.hlil->ReplaceExpr(oldExpr, newExpr);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void finalizeIl(const IlFunction& il) {
    if (il.llil) {
        il.llil->Finalize();
        il.llil->GenerateSSAForm();
    } else if (il.mlil) {
        il.mlil->Finalize();
        il.mlil->GenerateSSAForm();
    } else if (il.hlil) {
        il.hlil->Finalize();
        il.hlil->GenerateSSAForm();
    }
}

std::optional<QByteArray> patchX86Branch(const QByteArray& orig, const QString& action, size_t length) {
    const uint8_t opcode = uint8_t(orig[0]);
    const QByteArray nops(qsizetype(length), '\x90');
    if (opcode >= 0x70 && opcode <= 0x7F) {
        if (action == QLatin1String("force_true") || action == QLatin1String("unconditional"))
            return QByteArray(1, '\xEB') + orig.mid(1);
        if (action == QLatin1String("force_false")) return nops;
        if (action == QLatin1String("invert")) return QByteArray(1, char(opcode ^ 0x01)) + orig.mid(1);
    }
    if (opcode == 0x0F && length >= 2 && uint8_t(orig[1]) >= 0x80 && uint8_t(orig[1]) <= 0x8F) {
        if (action == QLatin1String("force_true") || action == QLatin1String("unconditional"))
            return QByteArray(1, '\xE9') + orig.mid(2, 4) + QByteArray(1, '\x90');
        if (action == QLatin1String("force_false")) return nops;
        if (action == QLatin1String("invert")) return QByteArray(1, '\x0F') + QByteArray(1, char(uint8_t(orig[1]) ^ 0x01)) + orig.mid(2);
    }
    return std::nullopt;
}

int hexDigit(QChar c) {
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9') return u - u'0';
    if (u >= u'a' && u <= u'f') return u - u'a' + 10;
    if (u >= u'A' && u <= u'F') return u - u'A' + 10;
    return -1;
}

bool asciiSpace(QChar c) {
    const char16_t u = c.unicode();
    return u == u' ' || u == u'\t' || u == u'\n' || u == u'\r' || u == u'\v' || u == u'\f';
}

// bytes.fromhex
QByteArray fromHex(const QString& s, QString* error) {
    QByteArray out;
    qsizetype i = 0;
    while (i < s.size()) {
        if (asciiSpace(s[i])) {
            ++i;
            continue;
        }
        const int top = hexDigit(s[i]);
        const int bot = i + 1 < s.size() ? hexDigit(s[i + 1]) : -1;
        if (top < 0 || bot < 0) {
            *error = QStringLiteral("non-hexadecimal number found in fromhex() arg at position %1").arg(top < 0 ? i : i + 1);
            return {};
        }
        out.append(char(top * 16 + bot));
        i += 2;
    }
    return out;
}

constexpr const char* kInstallWorkflow = R"PY(
import base64 as _rk_b64
import builtins as _rk_builtins
import json as _rk_json
import binaryninja as bn


def _rk_install(name, description, il_level, python_code):
    installed = __rk_state.setdefault('installed_workflows', {})
    if name in installed:
        return f"Workflow '{name}' already exists. Remove it first."

    il_level = il_level.strip().lower()
    if il_level not in ("llil", "mlil"):
        return "il_level must be 'llil' or 'mlil'"

    removed = {"__import__", "exec", "eval", "compile", "breakpoint", "exit", "quit"}
    safe = {k: v for k, v in vars(_rk_builtins).items() if k not in removed}
    ns = {"__builtins__": safe, "transform": None}
    try:
        exec(python_code, ns)
    except Exception as e:
        return f"Failed to compile transform code: {e}"

    transform_fn = ns["transform"]
    if not callable(transform_fn):
        return "python_code must define a transform(analysis_context, il_func) function"

    Workflow = getattr(bn, "Workflow", None)
    Activity = getattr(bn, "Activity", None)
    if Workflow is None or Activity is None:
        return "Binary Ninja Workflow/Activity API not available (requires BN 3.x+)"

    try:
        base_workflow = Workflow("core.function.metaAnalysis")
        workflow_name = f"rikugan.{name}"
        cloned = base_workflow.clone(workflow_name)

        insert_point = "core.function.generateMediumLevelIL"
        insert_method = "insert_before" if il_level == "llil" else "insert_after"

        def _activity_wrapper(analysis_context):
            try:
                il_func = analysis_context.llil if il_level == "llil" else analysis_context.mlil
                if il_func is not None:
                    transform_fn(analysis_context, il_func)
            except Exception as e:
                bn.log_error(f"[Rikugan] Workflow activity '{name}' failed: {e}")

        activity_name = f"rikugan.activity.{name}"
        activity = Activity(activity_name, action=_activity_wrapper)
        cloned.register_activity(activity)

        inserter = getattr(cloned, insert_method, None)
        if callable(inserter):
            inserter(insert_point, activity_name)
        else:
            return f"Workflow does not support {insert_method}()"

        cloned.register()

        installed[name] = {
            "workflow": cloned,
            "workflow_name": workflow_name,
            "activity": activity,
            "activity_name": activity_name,
            "description": description,
            "il_level": il_level,
        }

        return (
            f"Workflow activity '{name}' installed successfully.\n"
            f"  IL level: {il_level}\n"
            f"  Pipeline: {insert_method.replace('_', ' ')} {insert_point}\n"
            f"  Description: {description}\n"
            f"Note: New and re-analyzed functions will use this transform. "
            f"Existing functions need re-analysis to apply."
        )
    except Exception as e:
        return f"Failed to install workflow: {e}"


__rk_result = _rk_install(**_rk_json.loads(_rk_b64.b64decode('@ARGS@').decode('utf-8')))
)PY";

}  // namespace

void registerIlTransformTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "il_replace_expr", "il", "Replace an IL expression at a given index with a new expression.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("expr_index", ToolParam::Integer, "IL expression index to replace"),
         param("replacement_type", ToolParam::String,
               "Replacement type: 'const' (constant value), 'nop', or 'copy_from' (copy from another index)"),
         param("value", ToolParam::Integer, "Value for 'const' type, or source index for 'copy_from'", 0),
         param("size", ToolParam::Integer, "Size in bytes for the replacement expression", 0),
         param("level", ToolParam::String, "IL level: 'llil' or 'mlil'", "mlil")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const qint64 exprIndex = call.integer("expr_index");
            const QString replacementType = call.str("replacement_type");
            const qint64 value = call.integer("value");
            const qint64 size = call.integer("size");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), level));
            const std::optional<IlInstr> old = instrAtIndex(il, exprIndex);
            if (!old) return ToolResult::ok(QStringLiteral("No expression at index %1").arg(exprIndex));
            const size_t exprSize = size > 0 ? size_t(size) : old->size;
            ExprId newExpr = 0;
            if (replacementType == QLatin1String("const")) {
                newExpr = ilConst(il, exprSize, uint64_t(value));
            } else if (replacementType == QLatin1String("nop")) {
                newExpr = ilNop(il);
            } else if (replacementType == QLatin1String("copy_from")) {
                const std::optional<IlInstr> src = instrAtIndex(il, value);
                if (!src) return ToolResult::ok(QStringLiteral("No expression at source index %1").arg(value));
                newExpr = ilCopy(il, src->instrIndex);
            } else {
                return ToolResult::ok(QStringLiteral("Unknown replacement type: %1. Use 'const', 'nop', or 'copy_from'.")
                                          .arg(replacementType));
            }
            const QString oldStr = pyLeft(il.instructionText(old->instrIndex), 80);
            const bool replaced = withUndo(bv, [&] {
                if (!replaceExpr(il, old->exprIndex, newExpr)) return false;
                finalizeIl(il);
                updateAnalysisAndWait(bv, func);
                return true;
            });
            if (!replaced)
                return ToolResult::ok(QStringLiteral("Failed to replace expression at index %1: %2").arg(exprIndex).arg(oldStr));
            return ToolResult::ok(QStringLiteral("Replaced expression at index %1: %2 -> %3(%4)")
                                      .arg(exprIndex)
                                      .arg(oldStr, replacementType)
                                      .arg(value));
        }));

    registry.add(makeTool(
        "il_set_condition", "il",
        "Force a conditional branch true/false by replacing the condition expr with a const.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("condition_address", ToolParam::String, "Address of the conditional branch (hex string)"),
         param("force", ToolParam::String, "'true' or 'false' \u2014 force the condition to this value"),
         param("level", ToolParam::String, "IL level: 'llil' or 'mlil'", "mlil")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString force = call.str("force");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const uint64_t condEa = call.address("condition_address");
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), level));
            std::optional<IlInstr> target;
            for (size_t index : il.instructionOrder()) {
                const IlInstr instr = instrAt(il, index);
                if (instr.address == condEa && instr.isIf) {
                    target = instr;
                    break;
                }
            }
            if (!target) return ToolResult::ok(QStringLiteral("No conditional branch found at %1").arg(hex(condEa)));
            const auto [condExpr, condSize] = conditionOf(il, target->instrIndex);
            const QString forceVal = force.trimmed().toLower();
            if (forceVal != QLatin1String("true") && forceVal != QLatin1String("false"))
                return ToolResult::ok(QStringLiteral("force must be 'true' or 'false'"));
            const int constVal = forceVal == QLatin1String("true") ? 1 : 0;
            const QString oldStr = pyLeft(il.exprText(condExpr), 80);
            const bool replaced = withUndo(bv, [&] {
                if (!replaceExpr(il, condExpr, ilConst(il, condSize, uint64_t(constVal)))) return false;
                finalizeIl(il);
                updateAnalysisAndWait(bv, func);
                return true;
            });
            if (!replaced)
                return ToolResult::ok(QStringLiteral("Failed to replace condition at %1: %2").arg(hex(condEa), oldStr));
            return ToolResult::ok(QStringLiteral("Forced condition at %1 to %2: %3 -> const(%4)")
                                      .arg(hex(condEa), forceVal, oldStr)
                                      .arg(constVal));
        }));

    registry.add(makeTool(
        "il_nop_expr", "il", "Replace an IL expression with a NOP at IL level.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("expr_index", ToolParam::Integer, "IL expression index to NOP"),
         param("level", ToolParam::String, "IL level: 'llil' or 'mlil'", "mlil")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const qint64 exprIndex = call.integer("expr_index");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), level));
            const std::optional<IlInstr> old = instrAtIndex(il, exprIndex);
            if (!old) return ToolResult::ok(QStringLiteral("No expression at index %1").arg(exprIndex));
            const QString oldStr = pyLeft(il.instructionText(old->instrIndex), 80);
            const bool replaced = withUndo(bv, [&] {
                if (!replaceExpr(il, old->exprIndex, ilNop(il))) return false;
                finalizeIl(il);
                updateAnalysisAndWait(bv, func);
                return true;
            });
            if (!replaced)
                return ToolResult::ok(QStringLiteral("Failed to NOP expression at index %1: %2").arg(exprIndex).arg(oldStr));
            return ToolResult::ok(QStringLiteral("NOPed expression at index %1: %2").arg(exprIndex).arg(oldStr));
        }));

    registry.add(makeTool(
        "il_remove_block", "il", "Remove a basic block by NOPing all its instructions.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("block_index", ToolParam::Integer, "Block index to remove (0-based)"),
         param("level", ToolParam::String, "IL level: 'llil' or 'mlil'", "mlil")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const qint64 blockIndex = call.integer("block_index");
            const QString level = call.str("level");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction il = ilWithFallback(func, level);
            if (!il) return ToolResult::ok(QStringLiteral("IL not available for %1 at %2").arg(functionName(func), level));
            const std::vector<Ref<BasicBlock>> blocks = il.basicBlocks();
            if (blockIndex < 0 || blockIndex >= qint64(blocks.size()))
                return ToolResult::ok(QStringLiteral("Block index %1 out of range [0, %2]")
                                          .arg(blockIndex)
                                          .arg(qint64(blocks.size()) - 1));
            const uint64_t blockStart = blocks[size_t(blockIndex)]->GetStart();
            const uint64_t blockEnd = blocks[size_t(blockIndex)]->GetEnd();
            int nopped = 0;
            withUndo(bv, [&] {
                for (size_t index : il.instructionOrder()) {
                    const IlInstr inst = instrAt(il, index);
                    // `instr_index or expr_index`: instruction 0 falls back to its expression index
                    const uint64_t idx = inst.instrIndex ? inst.instrIndex : inst.exprIndex;
                    const bool inBlock = (blockStart <= idx && idx < blockEnd) ||
                                         (blockStart <= inst.address && inst.address < blockEnd);
                    if (inBlock && replaceExpr(il, inst.exprIndex, ilNop(il))) ++nopped;
                }
                finalizeIl(il);
                updateAnalysisAndWait(bv, func);
            });
            return ToolResult::ok(QStringLiteral("Removed block %1 (%2-%3): NOPed %4 instructions")
                                      .arg(blockIndex)
                                      .arg(hex(blockStart), hex(blockEnd))
                                      .arg(nopped));
        }));

    registry.add(makeTool(
        "patch_branch", "il", "Force conditional branch at byte level (force_true/false/invert/unconditional).",
        {param("address", ToolParam::String, "Address of the conditional branch instruction (hex string)"),
         param("action", ToolParam::String, "Action: 'force_true', 'force_false', 'invert', or 'unconditional'")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Architecture> arch = bv->GetDefaultArchitecture();
            if (!arch) return ToolResult::ok(QStringLiteral("Architecture not available"));
            const QString archName = qs(arch->GetName()).toLower();
            const size_t insnLen = bv->GetInstructionLength(arch, ea);
            if (insnLen == 0) return ToolResult::ok(QStringLiteral("Cannot determine instruction length at %1").arg(hex(ea)));
            const QByteArray orig = readRaw(bv, ea, insnLen);
            if (orig.isEmpty()) return ToolResult::ok(QStringLiteral("Cannot read bytes at %1").arg(hex(ea)));
            const QString action = call.str("action").trimmed().toLower();
            if (!archName.contains(QLatin1String("x86")))
                return ToolResult::ok(QStringLiteral("Branch patching not implemented for architecture: %1. Use "
                                                     "il_set_condition for IL-level modification instead.")
                                          .arg(archName));
            const std::optional<QByteArray> patched = patchX86Branch(orig, action, insnLen);
            if (!patched)
                return ToolResult::ok(QStringLiteral("Cannot patch instruction at %1 with action '%2': unsupported opcode 0x%3")
                                          .arg(hex(ea), action, QString::number(uint8_t(orig[0]), 16).rightJustified(2, QLatin1Char('0'))));
            withUndo(bv, [&] {
                bv->Write(ea, patched->constData(), size_t(patched->size()));
                Ref<Function> func = functionAt(bv, ea);
                updateAnalysisAndWait(bv, func);
            });
            return ToolResult::ok(QStringLiteral("Patched branch at %1: %2 -> %3 (%4)")
                                      .arg(hex(ea), hexBytes(orig), hexBytes(*patched), action));
        }));

    registry.add(makeTool(
        "write_bytes", "il", "Write arbitrary hex bytes + re-analysis. Low-level escape hatch.",
        {param("address", ToolParam::String, "Target address (hex string)"),
         param("hex_bytes", ToolParam::String, "Hex byte string to write (e.g. '90 90 90' or '909090')")},
        kIlMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            QString cleaned = call.str("hex_bytes");
            cleaned.remove(QLatin1Char(' '));
            cleaned.remove(QLatin1Char(','));
            if (cleaned.size() % 2 != 0)
                return ToolResult::ok(QStringLiteral("Invalid hex string: odd length (%1)").arg(cleaned.size()));
            QString error;
            const QByteArray data = fromHex(cleaned, &error);
            if (!error.isEmpty()) return ToolResult::ok(QStringLiteral("Invalid hex string: %1").arg(error));
            if (data.isEmpty()) return ToolResult::ok(QStringLiteral("No bytes to write"));
            const QByteArray orig = readRaw(bv, ea, size_t(data.size()));
            const QString origHex = orig.isEmpty() ? QStringLiteral("(unreadable)") : hexBytes(orig);
            withUndo(bv, [&] {
                bv->Write(ea, data.constData(), size_t(data.size()));
                Ref<Function> func = functionAt(bv, ea);
                updateAnalysisAndWait(bv, func);
            });
            return ToolResult::ok(QStringLiteral("Wrote %1 bytes at %2: %3 -> %4")
                                      .arg(data.size())
                                      .arg(hex(ea), origHex, hexBytes(data)));
        }));

    registry.add(makeTool(
        "install_il_workflow", "il", "Register a Python function as a workflow activity at a pipeline stage.",
        {param("name", ToolParam::String, "Unique name for this workflow activity"),
         param("description", ToolParam::String, "What this transform does"),
         param("il_level", ToolParam::String, "'llil' or 'mlil' \u2014 the IL level to modify"),
         param("python_code", ToolParam::String,
               "Python code defining a transform(analysis_context, il_func) function. The function receives an "
               "AnalysisContext and the IL function to modify. Use il_func.replace_expr() for modifications, then "
               "call il_func.finalize() and il_func.generate_ssa_form().")},
        kIlMutating | Script, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QJsonObject args{{"name", call.str("name")},
                                   {"description", call.str("description")},
                                   {"il_level", call.str("il_level")},
                                   {"python_code", call.str("python_code")}};
            QString source = QString::fromUtf8(kInstallWorkflow);
            source.replace(QStringLiteral("@ARGS@"),
                           QString::fromLatin1(QJsonDocument(args).toJson(QJsonDocument::Compact).toBase64()));
            const PythonResult result =
                withUndo(bv, [&] { return bnHost(call).runTrustedPython(source, [&call] { return call.cancelled(); }); });
            if (!result.ok) throw ToolError(result.error);
            return ToolResult::ok(result.output);
        }));
}

}  // namespace rk::binja
