#include "common/tools/MutationLog.h"

#include <QDateTime>

#include "common/tools/Tool.h"

namespace rk {

namespace {

QString s(const QJsonObject& o, const char* k) {
    const QJsonValue v = o.value(QLatin1String(k));
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
    return {};
}

bool truthy(const QJsonValue& v) {
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return !v.toString().isEmpty();
    return false;
}

MutationRecord rec(const QString& tool, const QJsonObject& args, const QString& rtool, const QJsonObject& rargs,
                   const QString& desc) {
    MutationRecord r;
    r.toolName = tool;
    r.arguments = args;
    r.reverseTool = rtool;
    r.reverseArguments = rargs;
    r.description = desc;
    r.timestampMs = QDateTime::currentMSecsSinceEpoch();
    return r;
}

std::optional<MutationRecord> reverseComment(const QString& tool, const QString& what, const QJsonObject& args,
                                             const QJsonObject& pre) {
    if (!pre.contains(QStringLiteral("old_comment"))) return std::nullopt;
    const QJsonValue address = args.value(QStringLiteral("address"));
    return rec(tool, args, tool,
               QJsonObject{{"address", address.isUndefined() ? QJsonValue("") : address},
                           {"comment", pre.value(QStringLiteral("old_comment"))},
                           {"repeatable", truthy(args.value(QStringLiteral("repeatable")))}},
               what + QLatin1Char(' ') + s(args, "address"));
}

std::optional<MutationRecord> build(const QString& tool, const QJsonObject& args, const QJsonObject& pre) {
    if (tool == QLatin1String("rename_function")) {
        const QString oldName = s(pre, "old_name");
        if (oldName.isEmpty()) return std::nullopt;
        return rec(tool, args, tool, QJsonObject{{"address", args.value("address")}, {"new_name", oldName}},
                   QStringLiteral("Rename function %1 → %2").arg(oldName, s(args, "new_name")));
    }
    if (tool == QLatin1String("rename_variable")) {
        const QString func = s(args, "func_address"), o = s(args, "old_name"), n = s(args, "new_name");
        return rec(tool, args, tool, QJsonObject{{"func_address", args.value("func_address")}, {"old_name", n}, {"new_name", o}},
                   QStringLiteral("Rename variable %1 → %2 in %3").arg(o, n, func));
    }
    if (tool == QLatin1String("set_comment")) return reverseComment(tool, QStringLiteral("Set comment at"), args, pre);
    if (tool == QLatin1String("set_function_comment"))
        return reverseComment(tool, QStringLiteral("Set function comment at"), args, pre);
    if (tool == QLatin1String("set_pseudocode_comment")) {
        if (!pre.contains(QStringLiteral("old_comment"))) return std::nullopt;
        return rec(tool, args, tool,
                   QJsonObject{{"func_address", args.value("func_address")},
                               {"target_address", args.value("target_address")},
                               {"comment", pre.value("old_comment")}},
                   QStringLiteral("Set pseudocode comment at %1").arg(s(args, "target_address")));
    }
    if (tool == QLatin1String("rename_address")) {
        const QString oldName = s(pre, "old_name");
        if (oldName.isEmpty()) return std::nullopt;
        return rec(tool, args, tool, QJsonObject{{"address", args.value("address")}, {"new_name", oldName}},
                   QStringLiteral("Rename %1 → %2").arg(s(args, "address"), s(args, "new_name")));
    }
    if (tool == QLatin1String("set_function_prototype")) {
        const QString old = s(pre, "old_prototype");
        if (old.isEmpty()) return std::nullopt;
        return rec(tool, args, tool, QJsonObject{{"address", args.value("address")}, {"prototype", old}},
                   QStringLiteral("Set prototype at %1").arg(s(args, "address")));
    }
    return std::nullopt;
}

}  // namespace

QJsonObject MutationRecord::toJson() const {
    return QJsonObject{{"tool", toolName},           {"description", description}, {"reversible", reversible},
                       {"reason", irreversibleReason}, {"timestamp", timestampMs}};
}

const QSet<QString>& MutationLog::reverseBuilders() {
    static const QSet<QString> s{"rename_function", "rename_variable",        "set_comment",           "set_function_comment",
                                 "set_pseudocode_comment", "rename_address", "set_function_prototype"};
    return s;
}

const QSet<QString>& MutationLog::knownIrreversible() {
    static const QSet<QString> s{
        "execute_python",   "set_type",           "propagate_type",       "apply_type_to_variable",
        "apply_struct_to_address", "create_struct", "create_enum",        "create_typedef",
        "modify_struct",    "modify_enum",        "import_c_header",      "import_type_from_library",
        "write_bytes",      "nop_instructions",   "nop_microcode",        "patch_branch",
        "il_replace_expr",  "il_set_condition",   "il_nop_expr",          "il_remove_block",
        "install_il_workflow", "install_microcode_optimizer", "remove_microcode_optimizer", "redecompile_function"};
    return s;
}

bool MutationLog::mutationSucceeded(const QString& tool, const QString& result) {
    if (!reverseBuilders().contains(tool)) return true;
    return result.startsWith(QLatin1String("Renamed")) || result.startsWith(QLatin1String("Named")) ||
           result.startsWith(QLatin1String("Set "));
}

QJsonObject MutationLog::capturePreState(const QString& tool, const QJsonObject& args, const Executor& run) {
    QJsonObject pre;
    const QJsonValue address = args.contains(QStringLiteral("address")) ? args.value(QStringLiteral("address")) : QJsonValue("");
    const bool repeatable = truthy(args.value(QStringLiteral("repeatable")));
    try {
        if (tool == QLatin1String("rename_function")) {
            pre.insert("old_name", run("read_function_name", QJsonObject{{"address", address}}));
        } else if (tool == QLatin1String("set_comment")) {
            pre.insert("old_comment", run("read_comment", QJsonObject{{"address", address}, {"repeatable", repeatable}}));
        } else if (tool == QLatin1String("set_function_comment")) {
            pre.insert("old_comment",
                       run("read_function_comment", QJsonObject{{"address", address}, {"repeatable", repeatable}}));
        } else if (tool == QLatin1String("set_pseudocode_comment")) {
            const QString c = run("get_pseudocode_comment", QJsonObject{{"func_address", args.value("func_address")},
                                                                        {"target_address", args.value("target_address")}});
            // a failed decompile comes back as text; kept, undo would write it as a comment
            if (c.startsWith(QLatin1String("Decompilation "))) throw ToolError(c);
            pre.insert("old_comment", c);
        } else if (tool == QLatin1String("set_function_prototype")) {
            const QString p = run("read_function_prototype", QJsonObject{{"address", address}});
            pre.insert("old_prototype", p);
            if (p.isEmpty()) pre.insert("_capture_failed", QStringLiteral("the function had no prototype to restore"));
        } else if (tool == QLatin1String("rename_address")) {
            const QString n = run("get_name_at", QJsonObject{{"address", address}});
            // its message for "none"; kept as the old name, undo would rename to this text
            if (n.startsWith(QLatin1String("No name at"))) throw ToolError(QStringLiteral("the address had no name to restore"));
            pre.insert("old_name", n);
        }
    } catch (const ToolError& e) {
        pre.insert("_capture_failed", e.message());
    } catch (const std::exception& e) {
        pre.insert("_capture_failed", QString::fromUtf8(e.what()));
    }
    return pre;
}

MutationRecord MutationLog::buildReverseRecord(const QString& tool, const QJsonObject& args, const QJsonObject& pre) {
    if (reverseBuilders().contains(tool))
        if (auto r = build(tool, args, pre)) return *r;
    MutationRecord r = rec(tool, args, QString(), QJsonObject(), QStringLiteral("Call %1").arg(tool));
    r.reversible = false;
    if (knownIrreversible().contains(tool)) r.irreversibleReason = QStringLiteral("this tool has no reverse operation");
    else {
        const QString failed = pre.value(QStringLiteral("_capture_failed")).toString();
        r.irreversibleReason = failed.isEmpty() ? QStringLiteral("the previous value could not be read") : failed;
    }
    return r;
}

void MutationLog::push(MutationRecord r) {
    {
        std::lock_guard<std::mutex> g(mu_);
        log_.append(std::move(r));
    }
    changed();
}

QList<MutationRecord> MutationLog::records() const {
    std::lock_guard<std::mutex> g(mu_);
    return log_;
}

int MutationLog::size() const {
    std::lock_guard<std::mutex> g(mu_);
    return int(log_.size());
}

void MutationLog::setOnChanged(std::function<void()> cb) {
    std::lock_guard<std::mutex> g(mu_);
    onChanged_ = std::move(cb);
}

void MutationLog::changed() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> g(mu_);
        cb = onChanged_;
    }
    if (cb) cb();
}

MutationLog::UndoOutcome MutationLog::undo(int count, const Executor& run) {
    UndoOutcome out;
    if (size() == 0) {
        out.text = QStringLiteral("Nothing to undo — mutation log is empty.");
        return out;
    }
    count = std::min(count, size());
    int undone = 0;
    QStringList errors;
    for (int i = 0; i < count; ++i) {
        MutationRecord r;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (log_.isEmpty()) break;
            r = log_.last();
        }
        auto pop = [&] {
            {
                std::lock_guard<std::mutex> g(mu_);
                if (!log_.isEmpty()) log_.removeLast();
            }
            out.undone << r.description;
            changed();
        };
        if (!r.reversible) {
            pop();
            errors << QStringLiteral("Cannot undo: %1 (%2)")
                          .arg(r.description, r.irreversibleReason.isEmpty() ? QStringLiteral("not reversible") : r.irreversibleReason);
            continue;
        }
        try {
            const QString outcome = run(r.reverseTool, r.reverseArguments);
            if (!mutationSucceeded(r.reverseTool, outcome)) throw ToolError(outcome);
        } catch (const UndoNoLongerPossible& e) {
            // retrying cannot help; holding the record would block every older undo behind it
            pop();
            errors << QStringLiteral("Cannot undo: %1 (%2)").arg(r.description, e.message());
            continue;
        } catch (const ToolError& e) {
            // the mutation is still applied: its record stays, and older ones stay behind it
            errors << QStringLiteral("Failed to undo %1: %2").arg(r.description, e.message());
            break;
        }
        pop();
        ++undone;
    }
    QStringList parts;
    if (undone) parts << QStringLiteral("Undid %1 mutation(s).").arg(undone);
    if (!errors.isEmpty()) parts << errors.join(QLatin1Char('\n'));
    out.text = parts.isEmpty() ? QStringLiteral("Nothing undone.") : parts.join(QLatin1Char('\n'));
    return out;
}

}  // namespace rk
