#include "common/agent/Actions.h"

#include "common/tools/Tool.h"
#include "common/util/Embedded.h"

namespace rk {

namespace {

QString file(const QString& id) { return embeddedText(("prompts/actions/" + id + ".md").toStdString()).trimmed(); }

}  // namespace

const std::vector<ActionDef>& idaActionDefs() {
    static const std::vector<ActionDef> ida{
        {"send_to", "Send to Rikugan", "Send selection or address to Rikugan input", false, "Ctrl+Shift+A", true, true},
        {"explain", "Explain this", "Explain the current function with Rikugan", true, {}, true, true},
        {"rename", "Rename with Rikugan", "Analyze and rename the current function", true, {}, true, true},
        {"deobfuscate", "Deobfuscate with Rikugan", "Deobfuscate the current function", true, {}, true, true},
        {"vuln_audit", "Find vulnerabilities", "Audit the current function for security bugs", true, {}, true, true},
        {"suggest_types", "Suggest types", "Infer and apply types for the current function", true, {}, true, true},
        {"annotate", "Annotate function", "Add comments to the current function", true, {}, false, true},
        {"clean_mcode", "Clean microcode", "Clean the microcode for the current function", true, {}, false, true},
        {"xref_analysis", "Xref analysis", "Deep cross-reference analysis on the current function", true, {}, true, true},
    };
    return ida;
}

const std::vector<ActionDef>& binjaActionDefs() {
    static const std::vector<ActionDef> binja{
        {"send_to", "Send to Rikugan", "Send selection or address to Rikugan input", false, {}, true, true},
        {"explain", "Explain this", "Explain the current function with Rikugan", true, {}, true, true},
        {"rename", "Rename with Rikugan", "Analyze and rename the current function", true, {}, true, true},
        {"deobfuscate", "Deobfuscate with Rikugan", "Deobfuscate the current function", true, {}, true, true},
        {"vuln_audit", "Find vulnerabilities", "Audit the current function for security bugs", true, {}, true, true},
        {"suggest_types", "Suggest types", "Infer and apply types for the current function", true, {}, true, true},
        {"annotate", "Annotate function", "Add comments to the current function", true, {}, true, true},
        {"clean_il", "Clean IL", "Clean IR IL for the current function", true, {}, true, true},
        {"xref_analysis", "Xref analysis", "Deep cross-reference analysis on the current function", true, {}, true, true},
        {"smart_patch", "Smart Patch", "Modify function behavior with natural language and apply binary patches", false, {}, true, true},
    };
    return binja;
}

QString actionPrompt(const QString& host, const QString& id, const ActionContext& ctx) {
    const bool ida = host == QLatin1String("ida");
    if (id == QLatin1String("send_to")) {
        if (!ctx.selectedText.isEmpty()) return ctx.selectedText;
        if (!ctx.funcName.isEmpty())
            return QString(file("send_to")).replace("{{func_name}}", ctx.funcName).replace("{{address}}", hex(ctx.ea));
        return QString(file("send_to_address")).replace("{{address}}", hex(ctx.ea));
    }
    const QString name = ctx.funcName.isEmpty() ? QStringLiteral("sub_%1").arg(ctx.ea, 0, 16) : ctx.funcName;
    const uint64_t ea = ctx.funcEa.value_or(ctx.ea);
    QString key = id;
    if (id == QLatin1String("clean_mcode") || id == QLatin1String("clean_il")) key = QStringLiteral("clean");
    QString t = file(key);
    t.replace("{{func_name}}", name).replace("{{address}}", hex(ea));
    t.replace("{{optimizer_term}}", ida ? QStringLiteral("microcode") : QStringLiteral("IL"));
    t.replace("{{ir_term}}", ida ? QStringLiteral("microcode") : QStringLiteral("IL"));
    // smart_patch ends with "Desired behavior: " (trailing space kept for the user to type after it)
    if (key == QLatin1String("smart_patch")) t += QLatin1Char(' ');
    return t;
}

}  // namespace rk
