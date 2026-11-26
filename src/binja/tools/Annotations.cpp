// Binary Ninja annotation tools.
#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

// worker threads: the core API is thread-safe, and the UI must not wait on analysis
constexpr uint32_t kMutating = Mutating;

Ref<Function> functionForUndo(const ToolCall& call) {
    Ref<BinaryView> bv = requireView(call);
    const uint64_t ea = call.address("address");
    Ref<Function> func = functionAt(bv, ea);
    if (!func) throw ToolError(QStringLiteral("No function at %1").arg(hex(ea)));
    return func;
}

}  // namespace

void registerAnnotationTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "rename_function", "annotations", "Rename a function.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("new_name", ToolParam::String, "New function name")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString newName = call.str("new_name");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const uint64_t start = func->GetStart();
            const QString oldName = functionName(func);
            withUndo(bv, [&] { renameSymbolAt(bv, start, newName); });
            return ToolResult::ok(QStringLiteral("Renamed %1: %2 \u2192 %3").arg(hex(start), oldName, newName));
        }));

    registry.add(makeTool(
        "rename_variable", "annotations", "Rename a local variable in a function.",
        {param("func_address", ToolParam::String, "Function address (hex string)"),
         param("old_name", ToolParam::String, "Current variable name"),
         param("new_name", ToolParam::String, "New variable name")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("func_address");
            const QString oldName = call.str("old_name");
            const QString newName = call.str("new_name");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            std::optional<Variable> target;
            size_t count = 0;
            BNVariableNameAndType* vars = BNGetFunctionVariables(func->GetObject(), &count);
            for (size_t i = 0; i < count && !target; ++i) {
                const Variable var(vars[i].var);
                if (qs(func->GetVariableNameOrDefault(var)) == oldName) target = var;
            }
            BNFreeVariableNameAndTypeList(vars, count);
            if (!target)
                return ToolResult::ok(QStringLiteral("Variable '%1' not found in function at %2")
                                          .arg(oldName, hex(func->GetStart())));
            // create_user_var(var, var.type, new_name); a variable without a type cannot be recreated
            const Confidence<Ref<Type>> type = func->GetVariableType(*target);
            if (!type.GetValue())
                return ToolResult::ok(QStringLiteral("Variable renaming is not supported by this Binary Ninja API version"));
            withUndo(bv, [&] { func->CreateUserVariable(*target, type, ss(newName)); });
            return ToolResult::ok(QStringLiteral("Renamed variable: %1 \u2192 %2").arg(oldName, newName));
        }));

    registry.add(makeTool(
        "set_comment", "annotations", "Set a comment at the given address.",
        {param("address", ToolParam::String, "Address (hex string)"), param("comment", ToolParam::String, "Comment text"),
         param("repeatable", ToolParam::Boolean, "Set as repeatable comment", false)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString comment = call.str("comment");
            // Binary Ninja comments are effectively repeatable at address granularity.
            withUndo(bv, [&] { bv->SetCommentForAddress(ea, ss(comment)); });
            return ToolResult::ok(QStringLiteral("Set comment at %1: %2").arg(hex(ea), comment));
        }));

    registry.add(makeTool(
        "set_function_comment", "annotations", "Set a function-level comment.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("comment", ToolParam::String, "Comment text"),
         param("repeatable", ToolParam::Boolean, "Set as repeatable comment", false)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString comment = call.str("comment");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            withUndo(bv, [&] { func->SetComment(ss(comment)); });
            return ToolResult::ok(QStringLiteral("Set function comment at %1").arg(hex(func->GetStart())));
        }));

    registry.add(makeTool(
        "rename_address", "annotations", "Set or change the name/label at an address.",
        {param("address", ToolParam::String, "Address (hex string)"),
         param("new_name", ToolParam::String, "New name/label")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString newName = call.str("new_name");
            QString old;
            if (Ref<Symbol> sym = symbolAt(bv, ea)) {
                old = qs(sym->GetFullName());
                if (old.isEmpty()) old = qs(sym->GetRawName());
            }
            withUndo(bv, [&] { renameSymbolAt(bv, ea, newName); });
            return ToolResult::ok(QStringLiteral("Named %1: %2 \u2192 %3")
                                      .arg(hex(ea), old.isEmpty() ? QStringLiteral("(unnamed)") : old, newName));
        }));

    registry.add(makeTool(
        "set_type", "annotations", "Set the type of a function or data item.",
        {param("address", ToolParam::String, "Address (hex string)"),
         param("type_string", ToolParam::String, "C type string (e.g. 'int __fastcall(void*, int)')")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString typeString = call.str("type_string");
            Ref<Type> type;
            try {
                type = parseTypeString(bv, typeString);
            } catch (const ToolError& e) {
                return ToolResult::ok(QStringLiteral("Failed to parse type: %1").arg(e.message()));
            }
            Ref<Function> func = functionAt(bv, ea);
            withUndo(bv, [&] {
                if (func && func->GetStart() == ea) func->SetUserType(type);
                else bv->DefineUserDataVariable(ea, Confidence<Ref<Type>>(type));
            });
            return ToolResult::ok(QStringLiteral("Set type at %1: %2").arg(hex(ea), typeString));
        }));

    // Undo's readers. Internal, so they add nothing to what the model is sent on each request, and raw, because
    // /undo passes the value straight back to the setter.
    registry.add(makeTool("read_function_name", "annotations", "Read the name rename_function would replace.",
                          {param("address", ToolParam::String, "Address in the function (hex string)")}, Internal,
                          [](ToolCall& call) { return ToolResult::ok(functionName(functionForUndo(call))); }));

    registry.add(makeTool(
        "read_comment", "annotations", "Read the comment set_comment would replace.",
        {param("address", ToolParam::String, "Address (hex string)"),
         param("repeatable", ToolParam::Boolean, "Ignored: Binary Ninja has one comment per address", false)},
        Internal, [](ToolCall& call) { return ToolResult::ok(commentAt(requireView(call), call.address("address"))); }));

    registry.add(makeTool(
        "read_function_comment", "annotations", "Read the comment set_function_comment would replace.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("repeatable", ToolParam::Boolean, "Ignored: Binary Ninja has one comment per function", false)},
        Internal, [](ToolCall& call) { return ToolResult::ok(qs(functionForUndo(call)->GetComment())); }));

    registry.add(makeTool(
        "read_function_prototype", "annotations",
        "Read the prototype set_function_prototype would replace. Empty if it has none.",
        {param("address", ToolParam::String, "Function address (hex string)")}, Internal, [](ToolCall& call) {
            Ref<Function> func = functionForUndo(call);
            Ref<Type> type = func->GetType();
            return ToolResult::ok(type ? qs(type->GetString(func->GetPlatform())) : QString());
        }));
}

}  // namespace rk::binja
