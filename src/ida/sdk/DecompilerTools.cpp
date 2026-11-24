// IDA decompiler tools (a ctree_visitor_t subclass: built without RTTI)
#include <algorithm>
#include <cstdlib>

#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

// _decompile: the cfunc, or the user-facing text in *error
cfuncptr_t decompileOrText(IdaHost& host, ea_t ea, QString* error) {
    if (!host.hasDecompiler()) throw ToolError(QStringLiteral("Hex-Rays decompiler is not available"));
    cfuncptr_t cfunc = decompileAt(ea);
    if (cfunc == nullptr) *error = QStringLiteral("Decompilation returned None for ") + hx(ea);
    return cfunc;
}

QStringList pseudocodeLines(cfunc_t* cfunc, bool withLineNumbers) {
    QStringList lines;
    const strvec_t& sv = cfunc->get_pseudocode();
    for (size_t i = 0; i < sv.size(); ++i) {
        qstring text;
        tag_remove(&text, sv[i].line);
        lines << (withLineNumbers ? rjust(qint64(i) + 1, 4) + QStringLiteral("  ") + qs(text) : qs(text));
    }
    return lines;
}

struct StatementCollector : public ctree_visitor_t {
    StatementCollector() : ctree_visitor_t(CV_FAST) {}
    int idaapi visit_insn(cinsn_t* ins) override {
        if (ins->ea != BADADDR) eas.push_back(ins->ea);
        return 0;
    }
    std::vector<ea_t> eas;
};

// The statement address for `target`: `target` itself when a statement starts there, else the nearest statement
// at or below it, else the closest one. No eamap lookup: on IDA 9's API it never succeeds (cinsn_t has no is_cinsn).
bool resolveCtreeEa(cfunc_t* cfunc, ea_t target, ea_t* out) {
    StatementCollector v;
    v.apply_to(&cfunc->body, nullptr);
    if (v.eas.empty()) return false;
    if (std::find(v.eas.begin(), v.eas.end(), target) != v.eas.end()) {
        *out = target;
        return true;
    }
    bool found = false;
    ea_t best = 0;
    for (ea_t ea : v.eas)
        if (ea <= target && (!found || ea > best)) {
            best = ea;
            found = true;
        }
    if (found) {
        *out = best;
        return true;
    }
    std::stable_sort(v.eas.begin(), v.eas.end(), [target](ea_t a, ea_t b) {
        const uint64_t da = a > target ? a - target : target - a;
        const uint64_t db = b > target ? b - target : target - b;
        return da < db;
    });
    *out = v.eas.front();
    return true;
}

treeloc_t locFor(cfunc_t* cfunc, ea_t target) {
    treeloc_t tl;
    if (!resolveCtreeEa(cfunc, target, &tl.ea)) tl.ea = target;
    tl.itp = ITP_SEMI;
    return tl;
}

}  // namespace

void registerDecompilerTools(ToolRegistry& reg, IdaHost& host) {
    addTool(reg, "decompile_function", "decompiler",
            "Decompile the function at the given address and return paginated pseudocode.",
            {strParam("address", "Function address (hex string)"),
             intParam("offset", "Start pseudocode line for pagination", 0),
             intParam("limit", "Max pseudocode lines to return", 160),
             boolParam("with_line_numbers", "Include pseudocode line numbers", true)},
            RequiresDecompiler | Cacheable, [&host](ToolCall& call) {
                const ea_t ea = call.address("address");
                QString error;
                cfuncptr_t cfunc = decompileOrText(host, ea, &error);
                if (cfunc == nullptr) return ToolResult::ok(error);
                const QStringList rows = pseudocodeLines(cfunc, call.boolean("with_line_numbers"));
                const qint64 offset = call.integer("offset"), limit = call.integer("limit");
                const auto [pageOffset, pageLimit] = normalizePage(offset, limit);
                const qint64 next = std::min<qint64>(rows.size(), pageOffset + pageLimit);
                return ToolResult::ok(formatPage(rows, offset, limit, QStringLiteral("Pseudocode for ") + hx(ea),
                                                 QStringLiteral("  (none)"),
                                                 QStringLiteral("Call decompile_function with offset=%1 limit=%2.").arg(next).arg(pageLimit)));
            });

    addTool(reg, "get_decompiler_variables", "decompiler", "List local variables from the decompiler output.",
            {strParam("address", "Function address (hex string)")}, RequiresDecompiler, [&host](ToolCall& call) {
                QString error;
                cfuncptr_t cfunc = decompileOrText(host, call.address("address"), &error);
                if (cfunc == nullptr) return ToolResult::ok(error);
                QStringList lines{QStringLiteral("Local variables:")};
                for (const lvar_t& lv : *cfunc->get_lvars()) {
                    const QString kind = lv.is_arg_var() ? QStringLiteral("arg") : QStringLiteral("local");
                    lines << QStringLiteral("  ") + ljust(kind, 5) + QLatin1Char(' ') + ljust(typeString(lv.type()), 20) +
                                 QLatin1Char(' ') + qs(lv.name);
                }
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "set_pseudocode_comment", "decompiler",
            "Insert a comment into the Hex-Rays pseudocode view at a specific line.",
            {strParam("func_address", "Function address (hex string)"),
             strParam("target_address", "Address of the pseudocode line to comment (hex string)"),
             strParam("comment", "Comment text to insert above the line")},
            RequiresDecompiler | Mutating, [&host](ToolCall& call) {
                const ea_t funcEa = call.address("func_address");
                const ea_t targetEa = call.address("target_address");
                const QString comment = call.str("comment");
                QString error;
                cfuncptr_t cfunc = decompileOrText(host, funcEa, &error);
                if (cfunc == nullptr) return ToolResult::ok(error);
                func_t* pfn = get_func(funcEa);
                if (pfn && !(pfn->start_ea <= targetEa && targetEa < pfn->end_ea))
                    return ToolResult::ok(QStringLiteral("Address ") + hx(targetEa) + QStringLiteral(" is outside function ") +
                                          hx(pfn->start_ea) + QStringLiteral("\u2013") + hx(pfn->end_ea));
                const treeloc_t tl = locFor(cfunc, targetEa);
                cfunc->set_user_cmt(tl, comment.toUtf8().constData());
                cfunc->save_user_cmts();
                return ToolResult::ok(QStringLiteral("Set pseudocode comment at ") + hx(tl.ea) + QStringLiteral(" in function ") +
                                      hx(funcEa) + QStringLiteral(":\n") + comment);
            });

    addTool(reg, "get_pseudocode_comment", "decompiler", "Read the Hex-Rays pseudocode comment at a specific line.",
            {strParam("func_address", "Function address (hex string)"),
             strParam("target_address", "Address of the pseudocode line (hex string)")},
            RequiresDecompiler, [&host](ToolCall& call) {
                const ea_t funcEa = call.address("func_address");
                const ea_t targetEa = call.address("target_address");
                QString error;
                cfuncptr_t cfunc = decompileOrText(host, funcEa, &error);
                if (cfunc == nullptr) return ToolResult::ok(error);
                const char* cmt = cfunc->get_user_cmt(locFor(cfunc, targetEa), RETRIEVE_ALWAYS);
                return ToolResult::ok(qs(cmt));
            });
}

}  // namespace rk::ida
