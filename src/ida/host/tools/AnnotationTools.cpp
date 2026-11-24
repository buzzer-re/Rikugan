// IDA annotation tools.
#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"

namespace rk::ida {

void registerAnnotationTools(ToolRegistry& reg, IdaHost& host) {
    addTool(reg, "rename_function", "annotations", "Rename a function.",
            {strParam("address", "Function address (hex string)"), strParam("new_name", "New function name")}, Mutating,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at ") + hx(ea));
                const ea_t start = pfn->start_ea;
                const QString oldName = nameAt(start);
                const QString newName = call.str("new_name");
                if (set_name(start, newName.toUtf8().constData(), SN_NOWARN | SN_NOCHECK))
                    return ToolResult::ok(QStringLiteral("Renamed ") + hx(start) + QStringLiteral(": ") + oldName +
                                          QStringLiteral(" \u2192 ") + newName);
                return ToolResult::ok(QStringLiteral("Failed to rename function at ") + hx(start));
            });

    addTool(reg, "rename_variable", "annotations", "Rename a local variable in a decompiled function.",
            {strParam("func_address", "Function address (hex string)"), strParam("old_name", "Current variable name"),
             strParam("new_name", "New variable name")},
            Mutating, [&host](ToolCall& call) {
                if (!host.hasDecompiler()) return ToolResult::ok(QStringLiteral("Hex-Rays decompiler not available"));
                const ea_t ea = call.address("func_address");
                cfuncptr_t cfunc = decompileAt(ea);
                if (cfunc == nullptr) return ToolResult::ok(QStringLiteral("No decompilation for ") + hx(ea));
                const QString oldName = call.str("old_name"), newName = call.str("new_name");
                const QByteArray oldUtf8 = oldName.toUtf8();
                lvars_t* lvars = cfunc->get_lvars();
                for (const lvar_t& lv : *lvars) {
                    if (lv.name != oldUtf8.constData()) continue;
                    if (rename_lvar(cfunc->entry_ea, lv.name.c_str(), newName.toUtf8().constData()))
                        return ToolResult::ok(QStringLiteral("Renamed variable: ") + oldName + QStringLiteral(" \u2192 ") + newName);
                    return ToolResult::ok(QStringLiteral("Failed to rename variable ") + oldName);
                }
                return ToolResult::ok(QStringLiteral("Variable '") + oldName + QStringLiteral("' not found in function at ") + hx(ea));
            });

    addTool(reg, "set_comment", "annotations", "Set a comment at the given address.",
            {strParam("address", "Address (hex string)"), strParam("comment", "Comment text"),
             boolParam("repeatable", "Set as repeatable comment", false)},
            Mutating, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString comment = call.str("comment");
                const bool repeatable = call.boolean("repeatable");
                if (set_cmt(ea, comment.toUtf8().constData(), repeatable))
                    return ToolResult::ok(QStringLiteral("Set ") + (repeatable ? QStringLiteral("repeatable ") : QString()) +
                                          QStringLiteral("comment at ") + hx(ea) + QStringLiteral(": ") + comment);
                return ToolResult::ok(QStringLiteral("Failed to set comment at ") + hx(ea));
            });

    addTool(reg, "set_function_comment", "annotations", "Set a function-level comment.",
            {strParam("address", "Function address (hex string)"), strParam("comment", "Comment text"),
             boolParam("repeatable", "Set as repeatable comment", false)},
            Mutating, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at ") + hx(ea));
                if (set_func_cmt(pfn, call.str("comment").toUtf8().constData(), call.boolean("repeatable")))
                    return ToolResult::ok(QStringLiteral("Set function comment at ") + hx(pfn->start_ea));
                return ToolResult::ok(QStringLiteral("Failed to set function comment at ") + hx(pfn->start_ea));
            });

    addTool(reg, "rename_address", "annotations", "Set or change the name/label at an address.",
            {strParam("address", "Address (hex string)"), strParam("new_name", "New name/label")}, Mutating,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString old = nameAt(ea);
                const QString newName = call.str("new_name");
                if (set_name(ea, newName.toUtf8().constData(), SN_NOWARN | SN_NOCHECK))
                    return ToolResult::ok(QStringLiteral("Named ") + hx(ea) + QStringLiteral(": ") +
                                          (old.isEmpty() ? QStringLiteral("(unnamed)") : old) + QStringLiteral(" \u2192 ") + newName);
                return ToolResult::ok(QStringLiteral("Failed to set name at ") + hx(ea));
            });

    addTool(reg, "set_type", "annotations", "Set the type of a function or data item.",
            {strParam("address", "Address (hex string)"),
             strParam("type_string", "C type string (e.g. 'int __fastcall(void*, int)')")},
            Mutating, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString typeString = call.str("type_string");
                if (setTypeAt(ea, typeString)) return ToolResult::ok(QStringLiteral("Set type at ") + hx(ea) + QStringLiteral(": ") + typeString);
                return ToolResult::ok(QStringLiteral("Failed to set type at ") + hx(ea) + QStringLiteral(". Check syntax: ") + typeString);
            });

    // Undo's readers: internal, and raw because /undo passes the value straight back to the setter.
    addTool(reg, "read_function_name", "annotations", "Read the name rename_function would replace.",
            {strParam("address", "Address in the function (hex string)")}, Internal, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) throw ToolError(QStringLiteral("No function at ") + hx(ea));
                return ToolResult::ok(nameAt(pfn->start_ea));
            });

    addTool(reg, "read_comment", "annotations", "Read the comment set_comment would replace.",
            {strParam("address", "Address (hex string)"), boolParam("repeatable", "Read the repeatable comment", false)}, Internal,
            [](ToolCall& call) { return ToolResult::ok(commentAt(call.address("address"), call.boolean("repeatable"))); });

    addTool(reg, "read_function_comment", "annotations", "Read the comment set_function_comment would replace.",
            {strParam("address", "Function address (hex string)"), boolParam("repeatable", "Read the repeatable comment", false)},
            Internal, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) throw ToolError(QStringLiteral("No function at ") + hx(ea));
                qstring buf;
                if (get_func_cmt(&buf, pfn, call.boolean("repeatable")) < 0) return ToolResult::ok(QString());
                return ToolResult::ok(qs(buf));
            });

    addTool(reg, "read_function_prototype", "annotations",
            "Read the prototype set_function_prototype would replace. Empty if it has none.",
            {strParam("address", "Function address (hex string)")}, Internal,
            [](ToolCall& call) { return ToolResult::ok(typeAt(call.address("address"))); });
}

}  // namespace rk::ida
