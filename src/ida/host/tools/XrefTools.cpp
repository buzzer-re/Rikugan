// IDA cross-reference tools.
#include <set>

#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

QString xrefTypeName(int type) {
    switch (type) {
    case 0: return QStringLiteral("Data_Unknown");
    case 1: return QStringLiteral("dr_O");
    case 2: return QStringLiteral("dr_W");
    case 3: return QStringLiteral("dr_R");
    case 4: return QStringLiteral("dr_T");
    case 5: return QStringLiteral("dr_I");
    case 16: return QStringLiteral("fl_CF");
    case 17: return QStringLiteral("fl_CN");
    case 18: return QStringLiteral("fl_JF");
    case 19: return QStringLiteral("fl_JN");
    case 20: return QStringLiteral("fl_US");
    case 21: return QStringLiteral("fl_F");
    default: return QStringLiteral("type_%1").arg(type);
    }
}

// format_callers_callees
QString callersCallees(const QString& fname, ea_t start, const std::set<QString>& callers, const std::set<QString>& callees) {
    QStringList parts{QStringLiteral("Function: ") + fname + QStringLiteral(" (") + hx(start) + QLatin1Char(')')};
    parts << QStringLiteral("\nCallers (%1):").arg(callers.size());
    for (const QString& c : callers) parts << QStringLiteral("  ") + c;
    parts << QStringLiteral("\nCallees (%1):").arg(callees.size());
    for (const QString& c : callees) parts << QStringLiteral("  ") + c;
    return parts.join(QLatin1Char('\n'));
}

}  // namespace

void registerXrefTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "xrefs_to", "xrefs", "Get all cross-references to the given address.",
            {strParam("address", "Target address (hex string)"), intParam("limit", "Max results", 30)}, 0,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const qint64 limit = call.integer("limit");
                const QString targetName = nameAt(ea);
                QStringList lines{QStringLiteral("Cross-references to ") + hx(ea) +
                                  (targetName.isEmpty() ? QString() : QStringLiteral(" (") + targetName + QLatin1Char(')')) +
                                  QLatin1Char(':')};
                qint64 count = 0;
                xrefblk_t xb;
                for (bool ok = xb.first_to(ea, XREF_FLOW); ok; ok = xb.next_to()) {
                    if (count >= limit) {
                        lines << QStringLiteral("  ... (truncated at %1)").arg(limit);
                        break;
                    }
                    func_t* pfn = get_func(xb.from);
                    const QString fname = pfn ? nameAt(pfn->start_ea) : QStringLiteral("?");
                    lines << QStringLiteral("  ") + hx(xb.from) + QStringLiteral("  [") + ljust(xrefTypeName(xb.type), 12) +
                                 QStringLiteral("]  in ") + fname;
                    ++count;
                }
                if (count == 0) lines << QStringLiteral("  (none)");
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "xrefs_from", "xrefs", "Get all cross-references from the given address.",
            {strParam("address", "Source address (hex string)"), intParam("limit", "Max results", 30)}, 0,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const qint64 limit = call.integer("limit");
                QStringList lines{QStringLiteral("Cross-references from ") + hx(ea) + QLatin1Char(':')};
                qint64 count = 0;
                xrefblk_t xb;
                for (bool ok = xb.first_from(ea, XREF_FLOW); ok; ok = xb.next_from()) {
                    if (count >= limit) {
                        lines << QStringLiteral("  ... (truncated at %1)").arg(limit);
                        break;
                    }
                    lines << QStringLiteral("  ") + hx(xb.to) + QStringLiteral("  [") + ljust(xrefTypeName(xb.type), 12) +
                                 QStringLiteral("]  ") + nameAt(xb.to);
                    ++count;
                }
                if (count == 0) lines << QStringLiteral("  (none)");
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "function_xrefs", "xrefs", "Get cross-references to and from a function (callers + callees).",
            {strParam("address", "Function address (hex string)")}, Cacheable, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                func_t* pfn = get_func(ea);
                if (pfn == nullptr) return ToolResult::ok(QStringLiteral("No function at ") + hx(ea));
                std::set<QString> callers, callees;
                for (ea_t ref = get_first_fcref_to(pfn->start_ea); ref != BADADDR; ref = get_next_fcref_to(pfn->start_ea, ref)) {
                    func_t* cf = get_func(ref);
                    if (cf && cf->start_ea != pfn->start_ea) callers.insert(nameAt(cf->start_ea));
                }
                func_item_iterator_t fii;
                for (bool ok = fii.set(pfn); ok; ok = fii.next_code()) {
                    const ea_t item = fii.current();
                    for (ea_t ref = get_first_fcref_from(item); ref != BADADDR; ref = get_next_fcref_from(item, ref)) {
                        func_t* cf = get_func(ref);
                        if (cf && cf->start_ea != pfn->start_ea) callees.insert(nameAt(cf->start_ea));
                    }
                    call.throwIfCancelled();
                }
                return ToolResult::ok(callersCallees(nameAt(pfn->start_ea), pfn->start_ea, callers, callees));
            });
}

}  // namespace rk::ida
