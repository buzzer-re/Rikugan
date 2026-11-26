// IDA function tools.
#include <algorithm>
#include <set>

#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

// format_function_summary
QString functionSummary(const QString& name, ea_t start, ea_t end, qint64 size, int blocks, int instrs,
                        const QStringList& callers, const QStringList& callees) {
    QStringList parts{QStringLiteral("Name: ") + name,
                      QStringLiteral("Address: ") + hx(start) + QStringLiteral(" \u2013 ") + hx(end),
                      QStringLiteral("Size: %1 bytes").arg(size), QStringLiteral("Basic blocks: %1").arg(blocks),
                      QStringLiteral("Instructions: %1").arg(instrs)};
    if (!callers.isEmpty())
        parts << QStringLiteral("Callers (%1): ").arg(callers.size()) + callers.join(QStringLiteral(", "));
    if (!callees.isEmpty())
        parts << QStringLiteral("Callees (%1): ").arg(callees.size()) + callees.join(QStringLiteral(", "));
    return parts.join(QLatin1Char('\n'));
}

// The first ten distinct names, sorted
QStringList firstTen(const std::set<QString>& names) {
    QStringList out;
    for (const QString& n : names) {
        if (out.size() == 10) break;
        out << n;
    }
    return out;
}

}  // namespace

void registerFunctionTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "list_functions", "functions", "List functions in the binary with pagination.",
            {intParam("offset", "Start index for pagination", 0), intParam("limit", "Max number of functions to return", 50)},
            Cacheable, [](ToolCall& call) {
                const qint64 offset = std::max<qint64>(0, call.integer("offset"));
                const qint64 limit = std::max<qint64>(1, call.integer("limit"));
                const qint64 qty = qint64(get_func_qty());
                const qint64 end = std::min(qty, offset + limit);
                QStringList rows;
                for (qint64 idx = offset; idx < end; ++idx)
                    if (func_t* pfn = getn_func(size_t(idx)))
                        rows << QStringLiteral("  ") + hx(pfn->start_ea) + QStringLiteral("  ") + nameAt(pfn->start_ea);
                QStringList lines{QStringLiteral("Functions %1\u2013%2 of %3:").arg(offset).arg(offset + rows.size()).arg(qty)};
                lines << rows;
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "get_function_info", "functions", "Get detailed information about a specific function.",
            {strParam("address", "Function address (hex string)")}, 0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at ") + hx(ea));
                const QString name = nameAt(pfn->start_ea);
                int blocks = 0, instrs = 0;
                qflow_chart_t fc("", pfn, BADADDR, BADADDR, 0);
                for (int i = 0; i < fc.size(); ++i) {
                    ++blocks;
                    const ea_t blockEnd = fc.blocks[i].end_ea;
                    for (ea_t head = fc.blocks[i].start_ea; head < blockEnd; head = next_head(head, blockEnd)) ++instrs;
                }
                std::set<QString> callers;
                for (ea_t ref = get_first_fcref_to(pfn->start_ea); ref != BADADDR; ref = get_next_fcref_to(pfn->start_ea, ref))
                    if (func_t* cf = get_func(ref)) callers.insert(nameAt(cf->start_ea));
                std::set<QString> callees;
                func_item_iterator_t fii;
                for (bool ok = fii.set(pfn); ok; ok = fii.next_code()) {
                    const ea_t item = fii.current();
                    for (ea_t ref = get_first_fcref_from(item); ref != BADADDR; ref = get_next_fcref_from(item, ref)) {
                        func_t* cf = get_func(ref);
                        if (cf && cf->start_ea != pfn->start_ea) callees.insert(nameAt(cf->start_ea));
                    }
                    call.throwIfCancelled();
                }
                return ToolResult::ok(functionSummary(name, pfn->start_ea, pfn->end_ea, qint64(pfn->end_ea - pfn->start_ea),
                                                      blocks, instrs, firstTen(callers), firstTen(callees)));
            });

    addTool(reg, "search_functions", "functions", "Search for functions by name substring.",
            {strParam("query", "Search string (substring match on function name)"), intParam("limit", "Max results", 20)}, 0,
            [](ToolCall& call) {
                const QString query = call.str("query");
                const QString q = query.toLower();
                const qint64 limit = call.integer("limit");
                QStringList results;
                const size_t qty = get_func_qty();
                for (size_t i = 0; i < qty; ++i) {
                    func_t* pfn = getn_func(i);
                    if (pfn == nullptr) continue;
                    const QString name = nameAt(pfn->start_ea);
                    if (name.toLower().contains(q)) {
                        results << QStringLiteral("  ") + hx(pfn->start_ea) + QStringLiteral("  ") + name;
                        if (results.size() >= limit) break;
                    }
                    if ((i & 0xfff) == 0) call.throwIfCancelled();
                }
                if (results.isEmpty()) return ToolResult::ok(QStringLiteral("No functions matching '%1'").arg(query));
                return ToolResult::ok(QStringLiteral("Found %1 function(s):\n").arg(results.size()) + results.join(QLatin1Char('\n')));
            });
}

}  // namespace rk::ida
