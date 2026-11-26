// Binary Ninja function tools.
#include <algorithm>
#include <set>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

QString formatFunctionSummary(const QString& name, uint64_t start, uint64_t end, uint64_t size, qint64 blocks,
                              qint64 instrs, const QStringList& callers, const QStringList& callees) {
    QStringList parts{
        QStringLiteral("Name: %1").arg(name),
        QStringLiteral("Address: %1 \u2013 %2").arg(hex(start), hex(end)),
        QStringLiteral("Size: %1 bytes").arg(size),
        QStringLiteral("Basic blocks: %1").arg(blocks),
        QStringLiteral("Instructions: %1").arg(instrs),
    };
    if (!callers.isEmpty())
        parts.append(QStringLiteral("Callers (%1): %2").arg(callers.size()).arg(callers.join(QStringLiteral(", "))));
    if (!callees.isEmpty())
        parts.append(QStringLiteral("Callees (%1): %2").arg(callees.size()).arg(callees.join(QStringLiteral(", "))));
    return parts.join(QLatin1Char('\n'));
}

// sorted(set(names))[:10], Python string order
QStringList firstSorted(const std::set<std::string>& names, size_t n) {
    QStringList out;
    for (const std::string& name : names) {
        if (size_t(out.size()) >= n) break;
        out.append(qs(name));
    }
    return out;
}

}  // namespace

std::set<std::string> callerNames(BinaryView* bv, Function* func) {
    // func.callers: functions of bv.get_callers(func.start)
    std::set<std::string> names;
    for (const ReferenceSource& ref : bv->GetCallers(func->GetStart()))
        if (ref.func) names.insert(ss(functionName(ref.func)));
    return names;
}

std::set<std::string> calleeNames(BinaryView* bv, Function* func) {
    // func.callees: bv.get_function_at for every callee address of every call site
    std::set<std::string> names;
    for (const ReferenceSource& site : func->GetCallSites())
        for (uint64_t addr : bv->GetCallees(site))
            if (Ref<Function> callee = functionAt(bv, addr); callee && callee->GetStart() == addr)
                names.insert(ss(functionName(callee)));
    return names;
}

void registerFunctionTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "list_functions", "functions", "List functions in the binary with pagination.",
        {param("offset", ToolParam::Integer, "Start index for pagination", 0),
         param("limit", ToolParam::Integer, "Max number of functions to return", 50)},
        Cacheable, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const qint64 offset = std::max<qint64>(0, call.integer("offset"));
            const qint64 limit = std::max<qint64>(1, call.integer("limit"));
            const std::vector<Ref<Function>> all = bv->GetAnalysisFunctionList();
            const qint64 total = qint64(all.size());
            std::vector<Ref<Function>> page;
            for (qint64 i = offset; i < total && i < offset + limit; ++i) page.push_back(all[size_t(i)]);
            std::stable_sort(page.begin(), page.end(), [](const Ref<Function>& a, const Ref<Function>& b) {
                return a->GetStart() < b->GetStart();
            });
            QStringList lines{QStringLiteral("Functions %1\u2013%2 of %3:").arg(offset).arg(offset + qint64(page.size())).arg(total)};
            for (const Ref<Function>& f : page)
                lines.append(QStringLiteral("  %1  %2").arg(hex(f->GetStart()), functionName(f)));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "get_function_info", "functions", "Get detailed information about a specific function.",
        {param("address", ToolParam::String, "Function address (hex string)")}, 0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const uint64_t start = func->GetStart();
            const uint64_t end = functionEnd(func);
            const uint64_t size = end > start ? end - start : 0;
            const std::vector<Ref<BasicBlock>> blocks = func->GetBasicBlocks();
            qint64 instrs = 0;
            if (!blocks.empty()) {
                // BasicBlock.instruction_count: instructions until an invalid one
                for (const Ref<BasicBlock>& bb : blocks) {
                    Ref<Architecture> arch = bb->GetArchitecture();
                    for (uint64_t cur = bb->GetStart(); cur < bb->GetEnd();) {
                        call.throwIfCancelled();
                        const size_t len = bv->GetInstructionLength(arch, cur);
                        if (len == 0) break;
                        ++instrs;
                        cur += len;
                    }
                }
            } else {
                instrs = qint64(functionInstructionAddresses(bv, func).size());
            }
            return ToolResult::ok(formatFunctionSummary(functionName(func), start, end, size, qint64(blocks.size()),
                                                        instrs, firstSorted(callerNames(bv, func), 10),
                                                        firstSorted(calleeNames(bv, func), 10)));
        }));

    registry.add(makeTool(
        "search_functions", "functions", "Search for functions by name substring.",
        {param("query", ToolParam::String, "Search string (substring match on function name)"),
         param("limit", ToolParam::Integer, "Max results", 20)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString query = call.str("query");
            const QString q = query.toLower();
            const qint64 limit = call.integer("limit");
            QStringList results;
            for (const Ref<Function>& func : sortedFunctions(bv)) {
                const QString name = functionName(func);
                if (name.toLower().contains(q)) {
                    results.append(QStringLiteral("  %1  %2").arg(hex(func->GetStart()), name));
                    if (results.size() >= limit) break;
                }
            }
            if (results.isEmpty()) return ToolResult::ok(QStringLiteral("No functions matching '%1'").arg(query));
            return ToolResult::ok(QStringLiteral("Found %1 function(s):\n").arg(results.size()) +
                                  results.join(QLatin1Char('\n')));
        }));
}

}  // namespace rk::binja
