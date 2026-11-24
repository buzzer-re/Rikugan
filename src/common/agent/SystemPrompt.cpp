#include "common/agent/SystemPrompt.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatTools.h"
#include "common/agent/DbContext.h"
#include "common/agent/HostMcp.h"
#include "common/config/RikuganConfig.h"
#include "common/security/Sanitize.h"
#include "common/tools/ToolRegistry.h"
#include "common/util/Embedded.h"

namespace rk {

namespace {

constexpr int kMaxMemoryLines = 200;

QString piece(const char* path) { return embeddedText(path).trimmed(); }

QString render(QString text, std::initializer_list<std::pair<const char*, QString>> vars) {
    for (const auto& [k, v] : vars) text.replace(QStringLiteral("{{%1}}").arg(QLatin1String(k)), v);
    return text;
}

// Persistent memory: the first 200 lines, then a truncation note
QString loadMemory(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream in(&f);
    QStringList lines;
    int i = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (i++ >= kMaxMemoryLines) {
            lines << QString() << render(piece("prompts/system/memory_truncated.md"), {{"max_lines", QString::number(kMaxMemoryLines)}});
            break;
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

}  // namespace

QStringList builtinTools(const AnalysisProfile& profile, bool subagent) {
    QStringList t{"read", "write", "edit", "ast_edit", "grep", "glob", "bash", "web_search"};
    if (!subagent) t << QStringLiteral("todo");  // omc strips todo from subagents
    t.removeIf([&](const QString& n) { return profile.deniedTools.contains(n); });
    return t;
}

QString buildSystemPrompt(DbContext& db, const ChatContext& chat, const QStringList& builtinTools) {
    const bool ida = db.host->hostName() == QLatin1String("ida");
    const char* intro = ida ? "prompts/system/ida/intro.md" : "prompts/system/binja/intro.md";
    const char* usage = ida ? "prompts/system/ida/tool_usage.md" : "prompts/system/binja/tool_usage.md";
    const char* caps = ida ? "prompts/system/ida/capabilities.md" : "prompts/system/binja/capabilities.md";
    QStringList parts{render(embeddedText("prompts/system/base.md").trimmed(),
                             {{"intro", piece(intro)}, {"tool_usage", piece(usage)}, {"capabilities", piece(caps)}})};
    const AnalysisProfile& profile = chat.profile;

    const QString memory = loadMemory(db.memoryPath());
    if (!memory.isEmpty())
        parts << QString() + render(piece("prompts/system/memory.md"), {{"memory", sanitize::sanitizeMemory(memory)}});

    if (!profile.hideBinaryMetadata) {
        const QString info = onMain(*db.host, MainAccess::Read, [&] { return db.host->binaryInfo(); });
        if (!info.isEmpty())
            parts << QString() + render(piece("prompts/system/current_binary.md"),
                                        {{"binary_info", sanitize::sanitizeBinaryContext(info, QStringLiteral("binary_info"))}});
    }

    QStringList names;
    const auto specs = db.registry.visible(profile, db.host->hasDecompiler(),
                                           [&](const QString& n) { return chatToolVisible(chat, n); });
    for (const ToolSpec* s : specs) names << s->name;
    names << builtinTools;
    parts << QString() + render(piece("prompts/system/available_tools.md"), {{"tool_names", names.join(QStringLiteral(", "))}});
    if (builtinTools.contains(QStringLiteral("write"))) {
        auto path = [](const QString& p) { return sanitize::stripInjectionMarkers(p); };
        parts << render(piece("prompts/system/filesystem.md"), {{"scratch_dir", path(db.scratchDir())},
                                                                {"binary_dir", path(db.binaryDir())},
                                                                {"notes_dir", path(db.notesDir())}});
    }
    if (builtinTools.contains(QStringLiteral("bash"))) parts << piece("prompts/system/shell.md");
    if (chatToolVisible(chat, QStringLiteral("open_database"))) parts << piece("prompts/system/databases.md");
    if (db.hostMcp.load() && HostMcp::instance().toolCount() > 0)
        parts << render(piece("prompts/system/host_mcp.md"), {{"server", HostMcp::instance().serverName()}});

    if (profile.singularAnalysis) parts << QString() + piece("prompts/system/profile/singular_analysis.md");
    if (!profile.customFilters.isEmpty())
        parts << QString() + render(piece("prompts/system/profile/instructions.md"), {{"custom_filters", profile.customFilters.join(QLatin1Char('\n'))}});
    if (!profile.deniedFunctions.isEmpty()) {
        QStringList l;
        for (const QString& f : profile.deniedFunctions) l << QStringLiteral("- ") + f;
        parts << QString() + render(piece("prompts/system/profile/restricted_functions.md"), {{"denied_functions", l.join(QLatin1Char('\n'))}});
    }
    if (profile.name != QLatin1String("default")) {
        QString section = render(piece("prompts/system/profile/active.md"),
                                 {{"profile_name", profile.name},
                                  {"profile_description", profile.description.isEmpty() ? QString() : profile.description + QLatin1Char('\n')}});
        if (profile.hasAnyIocFilter()) {
            QStringList active;
            for (const auto& [key, label] : iocFilterCategories())
                if (profile.iocFilters.value(key)) active << QStringLiteral("- ") + label;
            if (!active.isEmpty())
                section += QStringLiteral("\n\n") + render(piece("prompts/system/profile/ioc_filters.md"),
                                                           {{"ioc_categories", active.join(QLatin1Char('\n'))}});
        }
        parts << QString() + section;
    }
    // "\n".join(parts) with each later section opening on a blank line, as build_system_prompt did
    QString out = parts.takeFirst();
    for (const QString& p : parts) out += (p.isEmpty() ? QString() : QStringLiteral("\n\n") + p.trimmed());
    return out;
}

QString contextPrefix(DbContext& db, const ChatContext& chat) {
    if (!RikuganConfig::instance().autoContext || chat.profile.hideBinaryMetadata) return {};
    const auto ea = db.host->currentAddress();
    if (!ea) return {};
    const QString fn = db.host->currentFunctionLabel();
    QString block = QStringLiteral("<rikugan_context>\nCurrent address: %1\n").arg(hex(*ea));
    if (!fn.isEmpty()) block += QStringLiteral("Current function: %1\n").arg(sanitize::stripInjectionMarkers(fn));
    return block + QStringLiteral("</rikugan_context>\n\n");
}

QString stripContextPrefix(const QString& text) {
    static const QRegularExpression re(QStringLiteral("^<rikugan_context>\\n.*?</rikugan_context>\\n\\n"),
                                       QRegularExpression::DotMatchesEverythingOption);
    QString t = text;
    t.remove(re);
    return t;
}

}  // namespace rk
