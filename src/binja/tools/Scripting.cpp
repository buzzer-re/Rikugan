// Binary Ninja scripting tools.
#include "binja/host/BnHost.h"
#include "binja/tools/BnTools.h"

namespace rk::binja {

void registerScriptingTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "execute_python", "scripting",
        "Execute arbitrary Python code in Binary Ninja context and return stdout/stderr.",
        {param("code", ToolParam::String, "Python code to execute in Binary Ninja's scripting environment")},
        // a worker thread: the script runs in Binary Ninja's Python thread, and the panel stays usable meanwhile
        Mutating | Script, [](ToolCall& call) {
            Ref<BinaryNinja::BinaryView> bv = requireView(call);
            const QString code = call.str("code");
            const PythonResult result = withUndo(
                bv, [&] { return bnHost(call).runGuardedPython(code, [&call] { return call.cancelled(); }); });
            return ToolResult::ok(result.ok ? result.output : QStringLiteral("Error: ") + result.error);
        }));
}

}  // namespace rk::binja
