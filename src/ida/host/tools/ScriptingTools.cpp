// IDA scripting tools.
#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"

namespace rk::ida {

void registerScriptingTools(ToolRegistry& reg, IdaHost& host) {
    addTool(reg, "execute_python", "scripting", "Execute arbitrary Python code in IDA's context and return stdout/stderr.",
            {strParam("code", "Python code to execute in IDA's scripting environment")}, Mutating | Script,
            [&host](ToolCall& call) {
                const PythonResult r = host.runPython(call.str("code"), {});
                return ToolResult::ok(r.ok ? r.output : QStringLiteral("Error: ") + r.error);
            });
}

}  // namespace rk::ida
