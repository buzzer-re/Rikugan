// rikugan_ida_tooltest: runs every model-visible IDA tool against a fresh database of /bin/ls through idalib, with
// the cases of tests/ida/tool_cases.json, then checks that /undo restores a rename_function.
//
//   rikugan_ida_tooltest [--dump DIR]   (DIR gets one <n>_<tool>.txt per case and the resolved arguments)
#include <cstdio>
#include <map>
#include <set>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "common/tools/MutationLog.h"
#include "common/tools/ToolDispatcher.h"
#include "common/tools/ToolRegistry.h"
#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"

#include <idalib.hpp>

using namespace rk;
using namespace rk::ida;

namespace {

void out(const QString& s) {
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Values found in the database: the placeholders ${name} of tool_cases.json.
std::map<QString, QString> findPlaceholders() {
    std::map<QString, QString> vars;
    // a mid-sized function that decompiles, has a named local and a caller
    const size_t qty = get_func_qty();
    for (size_t i = 0; i < qty && vars.empty(); ++i) {
        func_t* pfn = getn_func(i);
        if (pfn == nullptr || (pfn->flags & (FUNC_LIB | FUNC_THUNK)) != 0) continue;
        const asize_t size = pfn->end_ea - pfn->start_ea;
        if (size < 0x80 || size > 0x400) continue;
        if (get_first_fcref_to(pfn->start_ea) == BADADDR) continue;
        cfuncptr_t cfunc = decompileAt(pfn->start_ea);
        if (cfunc == nullptr) continue;
        const lvar_t* local = nullptr;
        for (const lvar_t& lv : *cfunc->get_lvars())
            if (!lv.is_arg_var() && !lv.name.empty() && lv.type().get_size() == 4) {
                local = &lv;
                break;
            }
        if (local == nullptr) continue;
        if (cfunc->mba == nullptr || cfunc->mba->qty < 3) continue;
        vars["func"] = hx(pfn->start_ea);
        vars["func_name"] = nameAt(pfn->start_ea);
        vars["func_proto"] = typeAt(pfn->start_ea);
        tinfo_t ftif;
        qstring decl;
        if (get_tinfo(&ftif, pfn->start_ea)) ftif.print(&decl, "rk_proto", PRTYPE_1LINE);
        vars["func_decl"] = qs(decl);
        vars["lvar"] = qs(local->name);
        vars["lvar_type"] = typeString(local->type());
        ea_t insn = next_head(pfn->start_ea, pfn->end_ea);
        insn = next_head(insn, pfn->end_ea);
        vars["func_insn"] = hx(insn);
    }
    // a printable C string of at least 8 characters
    build_strlist();
    string_info_t si;
    for (size_t i = 0; i < get_strlist_qty(); ++i) {
        if (!get_strlist_item(&si, i) || si.type != STRTYPE_C || si.length < 9) continue;
        qstring text;
        if (get_strlit_contents(&text, si.ea, size_t(si.length), si.type) <= 0) continue;
        const QString s = qs(text);
        static const QRegularExpression plain(QStringLiteral("^[A-Za-z][A-Za-z ]{7,}"));
        const QRegularExpressionMatch m = plain.match(s);
        if (!m.hasMatch()) continue;
        vars["string_ea"] = hx(si.ea);
        vars["string_query"] = m.captured(0).left(8);
        break;
    }
    // writable data
    for (int n = 0; n < get_segm_qty(); ++n) {
        segment_t* seg = getnseg(n);
        qstring name;
        get_segm_name(&name, seg);
        if (name == ".bss" || name == ".data") {
            vars["data_ea"] = hx(seg->start_ea + 0x40);
            break;
        }
    }
    return vars;
}

QString substitute(QString text, const std::map<QString, QString>& vars) {
    for (const auto& [k, v] : vars) text.replace(QStringLiteral("${") + k + QLatin1Char('}'), v);
    return text;
}

QJsonValue substitute(const QJsonValue& v, const std::map<QString, QString>& vars) {
    if (v.isString()) return substitute(v.toString(), vars);
    if (v.isObject()) {
        QJsonObject o;
        const QJsonObject src = v.toObject();
        for (auto it = src.begin(); it != src.end(); ++it) o.insert(it.key(), substitute(it.value(), vars));
        return o;
    }
    return v;
}

ToolResult runTool(IdaHost& host, const ToolSpec& spec, const QJsonObject& args) {
    ToolCall call(spec, args, QStringLiteral("tooltest"), host, nullptr, std::make_shared<CancelToken>());
    try {
        return spec.fn(call);
    } catch (const ToolError& e) {
        return ToolResult::error(QStringLiteral("Error: ") + e.message());
    } catch (const std::exception& e) {
        return ToolResult::error(QStringLiteral("Error: ") + QString::fromUtf8(e.what()));
    }
}

QString oneLine(const QString& s, int max = 110) {
    QString t = s;
    t.replace(QLatin1Char('\n'), QStringLiteral(" | "));
    return t.size() > max ? t.left(max) + QStringLiteral("...") : t;
}

}  // namespace

int main(int argc, char** argv) {
    QString dumpDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (QString::fromUtf8(argv[i]) == QLatin1String("--dump")) dumpDir = QString::fromUtf8(argv[i + 1]);

    QFile casesFile(QStringLiteral(RK_TOOL_CASES));
    if (!casesFile.open(QIODevice::ReadOnly)) {
        out(QStringLiteral("cannot read ") + casesFile.fileName());
        return 2;
    }
    const QJsonArray cases = QJsonDocument::fromJson(casesFile.readAll()).object().value(QStringLiteral("cases")).toArray();

    QTemporaryDir tmp;
    const QString binary = tmp.filePath(QStringLiteral("ls"));
    if (!QFile::copy(QStringLiteral("/bin/ls"), binary)) {
        out(QStringLiteral("cannot copy /bin/ls"));
        return 2;
    }
    if (init_library() != 0) {
        out(QStringLiteral("init_library failed"));
        return 2;
    }
    enable_console_messages(false);
    if (open_database(binary.toUtf8().constData(), true) != 0) {
        out(QStringLiteral("open_database failed"));
        return 2;
    }
    auto_wait();
    if (!init_hexrays_plugin()) {
        out(QStringLiteral("Hex-Rays is not available"));
        close_database(false);
        return 2;
    }

    int failures = 0;
    {
        IdaHost host;
        ToolRegistry registry;
        host.registerTools(registry);

        const std::map<QString, QString> vars = findPlaceholders();
        for (const char* key : {"func", "string_ea", "data_ea"})
            if (!vars.count(QLatin1String(key))) {
                out(QStringLiteral("no ${%1} found in the database").arg(QLatin1String(key)));
                close_database(false);
                return 2;
            }
        for (const auto& [k, v] : vars) out(QStringLiteral("${%1} = %2").arg(k, v));
        set_screen_ea(ea_t(parseAddress(vars.at(QStringLiteral("func")))));
        if (!dumpDir.isEmpty()) QDir().mkpath(dumpDir);

        std::set<QString> covered;
        int n = 0;
        for (const QJsonValue& cv : cases) {
            const QJsonObject c = cv.toObject();
            const QString tool = c.value(QStringLiteral("tool")).toString();
            const QJsonObject args = substitute(c.value(QStringLiteral("args")).toObject(), vars).toObject();
            const bool expectError = c.value(QStringLiteral("expect_error")).toBool();
            const QString contains = substitute(c.value(QStringLiteral("expect_contains")).toString(), vars);
            ++n;
            const ToolSpec* spec = registry.find(tool);
            if (spec == nullptr) {
                out(QStringLiteral("[FAIL] %1: no such tool").arg(tool));
                ++failures;
                continue;
            }
            covered.insert(tool);
            const ToolResult r = runTool(host, *spec, args);
            QString why;
            if (r.isError != expectError) why = expectError ? QStringLiteral("expected an error") : QStringLiteral("error result");
            else if (!contains.isEmpty() && !r.text.contains(contains)) why = QStringLiteral("missing \"%1\"").arg(oneLine(contains, 60));
            if (!why.isEmpty()) ++failures;
            out(QStringLiteral("%1 %2 %3 (%4 chars): %5")
                    .arg(why.isEmpty() ? QStringLiteral("[ok]  ") : QStringLiteral("[FAIL]"), ljust(tool, 30),
                         why.isEmpty() ? QString() : QStringLiteral("- ") + why)
                    .arg(r.text.size())
                    .arg(oneLine(r.text)));
            if (!dumpDir.isEmpty()) {
                const QString base = QStringLiteral("%1/%2_%3").arg(dumpDir).arg(n, 3, 10, QLatin1Char('0')).arg(tool);
                QFile f(base + QStringLiteral(".txt"));
                if (f.open(QIODevice::WriteOnly)) f.write(r.text.toUtf8());
                QFile a(base + QStringLiteral(".args.json"));
                if (a.open(QIODevice::WriteOnly)) a.write(QJsonDocument(args).toJson(QJsonDocument::Compact));
            }
        }

        for (const ToolSpec* spec : registry.all())
            if (!spec->has(Internal) && !covered.count(spec->name)) {
                out(QStringLiteral("[FAIL] %1: no case in tool_cases.json").arg(spec->name));
                ++failures;
            }

        // rename_function through the mutation log, then /undo
        const QString func = vars.at(QStringLiteral("func"));
        const QString original = vars.at(QStringLiteral("func_name"));
        MutationLog log;
        const MutationLog::Executor run = [&](const QString& tool, const QJsonObject& args) {
            return ToolDispatcher::runDirect(host, registry, nullptr, tool, args);
        };
        const QJsonObject renameArgs{{"address", func}, {"new_name", "rk_undo_probe"}};
        const QJsonObject pre = MutationLog::capturePreState(QStringLiteral("rename_function"), renameArgs, run);
        const QString renamed = run(QStringLiteral("rename_function"), renameArgs);
        if (MutationLog::mutationSucceeded(QStringLiteral("rename_function"), renamed))
            log.push(MutationLog::buildReverseRecord(QStringLiteral("rename_function"), renameArgs, pre));
        const QString during = nameAt(ea_t(parseAddress(func)));
        const MutationLog::UndoOutcome undo = log.undo(1, run);
        const QString after = nameAt(ea_t(parseAddress(func)));
        const bool undoOk = during == QLatin1String("rk_undo_probe") && after == original && log.size() == 0;
        if (!undoOk) ++failures;
        out(QStringLiteral("%1 undo rename_function: %2 -> %3 -> %4 (%5)")
                .arg(undoOk ? QStringLiteral("[ok]  ") : QStringLiteral("[FAIL]"), original, during, after, oneLine(undo.text)));
    }

    close_database(false);
    out(failures == 0 ? QStringLiteral("ALL PASSED") : QStringLiteral("%1 FAILURE(S)").arg(failures));
    return failures == 0 ? 0 : 1;
}
