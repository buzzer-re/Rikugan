// Binary Ninja cross-reference tools.
#include <algorithm>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

struct RefRow {
    uint64_t addr;
    const char* kind;
    QString function;
};

QString functionLabel(BinaryView* bv, uint64_t ea) {
    Ref<Function> func = functionAt(bv, ea);
    return func ? functionName(func) : QStringLiteral("?");
}

std::vector<RefRow> codeRefsTo(BinaryView* bv, uint64_t ea) {
    std::vector<RefRow> out;
    for (const ReferenceSource& ref : bv->GetCodeReferences(ea)) out.push_back({ref.addr, "code", functionLabel(bv, ref.addr)});
    return out;
}

std::vector<RefRow> dataRefsTo(BinaryView* bv, uint64_t ea) {
    std::vector<RefRow> out;
    for (uint64_t src : bv->GetDataReferences(ea)) out.push_back({src, "data", functionLabel(bv, src)});
    return out;
}

std::vector<std::pair<uint64_t, const char*>> refsFrom(BinaryView* bv, uint64_t ea) {
    std::vector<std::pair<uint64_t, const char*>> out;
    // bv.get_code_refs_from: references from every function containing ea
    for (const Ref<Function>& func : bv->GetAnalysisFunctionsContainingAddress(ea)) {
        ReferenceSource src{func, func->GetArchitecture(), ea};
        for (uint64_t dst : bv->GetCodeReferencesFrom(src)) out.emplace_back(dst, "code");
    }
    for (uint64_t dst : bv->GetDataReferencesFrom(ea)) out.emplace_back(dst, "data");
    return out;
}

QString formatCallersCallees(const QString& fname, uint64_t start, const std::set<std::string>& callers,
                             const std::set<std::string>& callees) {
    QStringList parts{QStringLiteral("Function: %1 (%2)").arg(fname, hex(start))};
    parts.append(QStringLiteral("\nCallers (%1):").arg(callers.size()));
    for (const std::string& c : callers) parts.append(QStringLiteral("  %1").arg(qs(c)));
    parts.append(QStringLiteral("\nCallees (%1):").arg(callees.size()));
    for (const std::string& c : callees) parts.append(QStringLiteral("  %1").arg(qs(c)));
    return parts.join(QLatin1Char('\n'));
}

}  // namespace

void registerXrefTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "xrefs_to", "xrefs", "Get cross-references to the given address with pagination.",
        {param("address", ToolParam::String, "Target address (hex string)"),
         param("offset", ToolParam::Integer, "Start index for pagination", 0),
         param("limit", ToolParam::Integer, "Max results", 30)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString targetName = resolveNameAt(bv, ea);
            std::vector<RefRow> refs = codeRefsTo(bv, ea);
            for (RefRow& r : dataRefsTo(bv, ea)) refs.push_back(std::move(r));
            std::stable_sort(refs.begin(), refs.end(), [](const RefRow& a, const RefRow& b) { return a.addr < b.addr; });
            QStringList rows;
            for (const RefRow& r : refs)
                rows.append(QStringLiteral("  %1  [%2]  in %3").arg(hex(r.addr), ljust(QLatin1String(r.kind), 12), r.function));
            const QString title = QStringLiteral("Cross-references to %1").arg(hex(ea)) +
                                  (targetName.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(targetName));
            return ToolResult::ok(formatToolPage(rows, call.integer("offset"), call.integer("limit"), title, "xrefs_to"));
        }));

    registry.add(makeTool(
        "xrefs_from", "xrefs", "Get cross-references from the given address with pagination.",
        {param("address", ToolParam::String, "Source address (hex string)"),
         param("offset", ToolParam::Integer, "Start index for pagination", 0),
         param("limit", ToolParam::Integer, "Max results", 30)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            QStringList rows;
            for (const auto& [dst, kind] : refsFrom(bv, ea))
                rows.append(QStringLiteral("  %1  [%2]  %3").arg(hex(dst), ljust(QLatin1String(kind), 12), resolveNameAt(bv, dst)));
            return ToolResult::ok(formatToolPage(rows, call.integer("offset"), call.integer("limit"),
                                                 QStringLiteral("Cross-references from %1").arg(hex(ea)), "xrefs_from"));
        }));

    registry.add(makeTool(
        "function_xrefs", "xrefs", "Get cross-references to and from a function (callers + callees).",
        {param("address", ToolParam::String, "Function address (hex string)")}, Cacheable, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const uint64_t start = func->GetStart();
            std::set<std::string> callers = callerNames(bv, func);
            if (callers.empty()) {
                for (const RefRow& r : codeRefsTo(bv, start)) {
                    Ref<Function> cf = functionAt(bv, r.addr);
                    if (cf && cf->GetStart() != start) callers.insert(ss(functionName(cf)));
                }
            }
            return ToolResult::ok(formatCallersCallees(functionName(func), start, callers, calleeNames(bv, func)));
        }));
}

}  // namespace rk::binja
