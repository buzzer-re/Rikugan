// Binary Ninja navigation tools.
#include "binja/host/BnHost.h"
#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

void registerNavigationTools(ToolRegistry& registry) {
    registry.add(makeTool("get_cursor_position", "navigation", "Get the current cursor address in the Binary Ninja view.",
                          {}, 0, [](ToolCall& call) {
                              return ToolResult::ok(hex(bnHost(call).currentAddress().value_or(0)));
                          }));

    registry.add(makeTool(
        "get_current_function", "navigation", "Get information about the function at the current cursor position.", {},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = bnHost(call).currentAddress().value_or(0);
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at current address"));
            const uint64_t start = func->GetStart();
            const uint64_t end = func->GetHighestAddress();
            const uint64_t size = end > start ? end - start : 0;
            return ToolResult::ok(QStringLiteral("Name: %1\nStart: %2\nEnd: %3\nSize: %4 bytes")
                                      .arg(functionName(func), hex(start), hex(end))
                                      .arg(size));
        }));

    registry.add(makeTool(
        "jump_to", "navigation", "Jump the Binary Ninja view to the specified address.",
        {param("address", ToolParam::String, "Address to jump to (hex string, e.g. '0x401000')")}, MainThread,
        [](ToolCall& call) {
            const uint64_t ea = call.address("address");
            if (bnHost(call).navigateTo(ea)) return ToolResult::ok(QStringLiteral("Jumped to %1").arg(hex(ea)));
            return ToolResult::ok(QStringLiteral("Could not navigate to %1. Ensure the Rikugan panel was opened from "
                                                 "the active Binary Ninja UI.")
                                      .arg(hex(ea)));
        }));

    registry.add(makeTool("get_name_at", "navigation", "Get the name/label at the specified address.",
                          {param("address", ToolParam::String, "Address to query (hex string)")}, 0,
                          [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const uint64_t ea = call.address("address");
                              const QString name = resolveNameAt(bv, ea);
                              return ToolResult::ok(name.isEmpty() ? QStringLiteral("No name at %1").arg(hex(ea))
                                                                   : name);
                          }));

    registry.add(makeTool("get_address_of", "navigation", "Get the address of a named symbol.",
                          {param("name", ToolParam::String, "Symbol name to look up")}, 0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const QString name = call.str("name");
                              for (const Ref<Symbol>& sym : symbolsByName(bv, name))
                                  return ToolResult::ok(hex(sym->GetAddress()));
                              return ToolResult::ok(QStringLiteral("Symbol not found: %1").arg(name));
                          }));
}

}  // namespace rk::binja
