// IDA navigation tools.
#include "ida/host/IdaTools.h"

namespace rk::ida {

void registerNavigationTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "get_cursor_position", "navigation", "Get the current cursor address in the IDA disassembly view.", {}, 0,
            [](ToolCall&) { return ToolResult::ok(hx(get_screen_ea())); });

    addTool(reg, "get_current_function", "navigation", "Get information about the function at the current cursor position.",
            {}, 0, [](ToolCall&) {
                func_t* pfn = get_func(get_screen_ea());
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at current address"));
                return ToolResult::ok(QStringLiteral("Name: ") + nameAt(pfn->start_ea) + QStringLiteral("\nStart: ") +
                                      hx(pfn->start_ea) + QStringLiteral("\nEnd: ") + hx(pfn->end_ea) +
                                      QStringLiteral("\nSize: %1 bytes").arg(qint64(pfn->end_ea - pfn->start_ea)));
            });

    addTool(reg, "jump_to", "navigation", "Jump the IDA disassembly view to the specified address.",
            {strParam("address", "Address to jump to (hex string, e.g. '0x401000')")}, 0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const bool ok = jumpto(ea);
                return ToolResult::ok((ok ? QStringLiteral("Jumped to ") : QStringLiteral("Failed to jump to ")) + hx(ea));
            });

    addTool(reg, "get_name_at", "navigation", "Get the name/label at the specified address.",
            {strParam("address", "Address to query (hex string)")}, 0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString name = nameAt(ea);
                return ToolResult::ok(name.isEmpty() ? QStringLiteral("No name at ") + hx(ea) : name);
            });

    addTool(reg, "get_address_of", "navigation", "Get the address of a named symbol.",
            {strParam("name", "Symbol name to look up")}, 0, [](ToolCall& call) {
                const QString name = call.str("name");
                const ea_t ea = get_name_ea(0, name.toUtf8().constData());
                if (ea == BADADDR) return ToolResult::ok(QStringLiteral("Symbol not found: ") + name);
                return ToolResult::ok(hx(ea));
            });
}

}  // namespace rk::ida
