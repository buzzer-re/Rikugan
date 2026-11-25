#include "common/agent/CommandRouter.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>

#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/agent/ModeDriver.h"
#include "common/agent/Skills.h"
#include "common/config/Paths.h"
#include "common/omc/AgentHome.h"
#include "common/omc/OmcSession.h"
#include "common/tools/ToolDispatcher.h"
#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

#include <omc.h>

namespace rk {

namespace {

// markdown: Rikugan's own text in Markdown (shown rendered); otherwise preformatted, like omc's command output
QJsonObject output(const QString& text, bool error = false, bool markdown = false) {
    return QJsonObject{{"type", "rikugan_command_output"}, {"text", text}, {"isError", error}, {"markdown", markdown}};
}

void undo(ChatSession& chat, const QString& arg) {
    int count = 1;
    if (!arg.isEmpty()) {
        bool ok = false;
        count = arg.toInt(&ok);
        if (!ok) {
            chat.postLocal(output(QStringLiteral("Invalid undo count: %1. Usage: /undo [N]").arg(arg), true));
            return;
        }
    }
    std::shared_ptr<ChatContext> ctx = chat.context();
    // reverse operations marshal to the main thread: run them off the UI thread
    spawnThread([ctx, count] {
        const MutationLog::UndoOutcome u = ctx->mutations->undo(count, [&](const QString& t, const QJsonObject& a) {
            return ToolDispatcher::runDirect(*ctx->host, *ctx->registry, ctx.get(), t, a);
        });
        if (!u.undone.isEmpty()) {
            if (ctx->cache) ctx->cache->invalidate();
            ctx->host->databaseChanged();
        }
        if (ctx->ui) {
            for (const QString& d : u.undone) ctx->ui->postUiEvent(QJsonObject{{"type", "rikugan_mutation_undone"}, {"description", d}});
            ctx->ui->postUiEvent(output(u.text));
        }
    });
}

void memory(ChatSession& chat) {
    const QString path = chat.db()->memoryPath();
    if (chat.db()->host->databasePath().isEmpty()) {
        chat.postLocal(output(QStringLiteral("No IDB/BNDB path set — persistent memory is not available.")));
        return;
    }
    if (!QFileInfo::exists(path)) {
        chat.postLocal(output(QStringLiteral("No persistent memory file found.\n\nThe agent can save facts with the `save_memory` "
                                             "tool, and they will be stored in `%1`.").arg(path), false, true));
        return;
    }
    bool ok = false;
    const QString content = QString::fromUtf8(readFile(path, &ok)).trimmed();
    if (!ok) chat.postLocal(output(QStringLiteral("Failed to read RIKUGAN.md: %1").arg(path), true));
    else if (content.isEmpty()) chat.postLocal(output(QStringLiteral("RIKUGAN.md exists but is empty.")));
    else chat.postLocal(output(QStringLiteral("**Persistent memory** (`%1`)\n\n%2").arg(path, content), false, true));
}

void mcp(ChatSession& chat) {
    const QJsonObject cfg = readJsonObject(paths::omcAgentDir() + QStringLiteral("/mcp.json"));
    const QJsonObject servers = cfg.value(QStringLiteral("mcpServers")).toObject();
    if (servers.isEmpty()) {
        chat.postLocal(output(QStringLiteral("No MCP servers configured or connected.")));
        return;
    }
    QStringList lines{QStringLiteral("**MCP Server Status**\n")};
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const QJsonObject s = it.value().toObject();
        const bool disabled = s.value(QStringLiteral("disabled")).toBool() || s.value(QStringLiteral("enabled")).toBool(true) == false;
        const QString target = s.contains(QStringLiteral("url")) ? s.value(QStringLiteral("url")).toString()
                                                               : s.value(QStringLiteral("command")).toString();
        lines << QStringLiteral("- **%1** (%2)%3").arg(it.key(), target, disabled ? QStringLiteral(" — disabled") : QString());
    }
    chat.postLocal(output(lines.join(QLatin1Char('\n'))));
}

void doctor(ChatSession& chat) {
    std::shared_ptr<DbContext> db = chat.db();
    const int tools = int(db->registry.visible(chat.context()->profile, db->host->hasDecompiler()).size());
    const int skills = int(listSkills().size());
    const bool decomp = db->host->hasDecompiler();
    const QString dbPath = db->host->databasePath();
    QPointer<ChatSession> self(&chat);
    auto state = std::make_shared<QJsonObject>();
    auto accounts = std::make_shared<QJsonObject>();
    chat.runJob(
        [state, accounts](OmcSession& s) {
            *state = s.refreshState();
            // the models omc can run right now: what having credentials means here
            *accounts = s.request(QJsonObject{{"type", "get_available_models"}});
        },
        [self, state, accounts, tools, skills, decomp, dbPath] {
            if (!self) return;
            QStringList ok, issues;
            ok << QStringLiteral("Rikugan %1 (agent engine libomc %2, API %3)")
                      .arg(QStringLiteral(RIKUGAN_VERSION), QString::fromUtf8(omc_version()))
                      .arg(omc_api_version());
            const QJsonObject model = state->value(QStringLiteral("model")).toObject();
            if (!model.isEmpty())
                ok << QStringLiteral("Model: %1/%2").arg(model.value(QStringLiteral("provider")).toString(),
                                                         model.value(QStringLiteral("id")).toString());
            else issues << QStringLiteral("No model configured (Settings → Provider)");
            QSet<QString> providers;
            for (const QJsonValue& v : accounts->value(QStringLiteral("data")).toObject().value(QStringLiteral("models")).toArray())
                providers.insert(v.toObject().value(QStringLiteral("provider")).toString());
            providers.remove(AgentHome::serviceProvider());
            if (!providers.isEmpty()) ok << QStringLiteral("Connected: %1").arg(QStringList(providers.begin(), providers.end()).join(QStringLiteral(", ")));
            else issues << QStringLiteral("No model connected (Ctrl+K → Connect a model)");
            if (tools > 0) ok << QStringLiteral("Tools: %1 registered").arg(tools);
            else issues << QStringLiteral("No tools registered — check plugin initialization");
            ok << QStringLiteral("Skills: %1 loaded").arg(skills);
            ok << (decomp ? QStringLiteral("Decompiler: available") : QStringLiteral("Decompiler: not available"));
            if (!dbPath.isEmpty()) ok << QStringLiteral("IDB/BNDB: %1").arg(dbPath);
            else issues << QStringLiteral("No IDB/BNDB path — persistent memory disabled");
            QStringList lines{QStringLiteral("**Rikugan Doctor**\n")};
            if (!ok.isEmpty()) {
                lines << QStringLiteral("**OK:**");
                for (const QString& i : ok) lines << QStringLiteral("  - %1").arg(i);
            }
            if (!issues.isEmpty()) {
                lines << QStringLiteral("\n**Issues:**");
                for (const QString& i : issues) lines << QStringLiteral("  - %1").arg(i);
            } else {
                lines << QStringLiteral("\nNo issues found.");
            }
            self->postLocal(output(lines.join(QLatin1Char('\n'))));
        });
}

// Runs `job` on the chat's session without waiting behind a running prompt: on the worker when the chat is idle
// (ordered after the session opens), else on a helper thread (omc answers these commands beside a running turn).
void sideJob(ChatSession& chat, std::function<void(OmcSession&, ChatUi*)> job) {
    chat.ensureOpen();
    std::shared_ptr<ChatContext> ctx = chat.context();
    std::shared_ptr<OmcSession> s = ctx->session.lock();
    if (!s) return;
    std::shared_ptr<ChatUi> ui = ctx->ui;
    if (!chat.busy()) {
        s->post([ui, job = std::move(job)](OmcSession& os) { job(os, ui.get()); });
        return;
    }
    spawnThread([s, ui, job = std::move(job)] { job(*s, ui.get()); });
}

void post(ChatUi* ui, const QString& text, bool error = false) {
    if (ui) ui->postUiEvent(output(text, error));
}

QString errorOf(const QJsonObject& r) {
    const QString e = r.value(QStringLiteral("error")).toString();
    return e.isEmpty() ? QStringLiteral("request failed") : e;
}

QString describeGoal(const QJsonObject& g) {
    if (g.isEmpty()) return QStringLiteral("No goal is set. Start one with `/goal <objective>`.");
    const qint64 secs = qint64(g.value(QStringLiteral("timeUsedSeconds")).toDouble());
    QString line = QStringLiteral("**Goal** (%1): %2\n\n%3 tokens, %4 min")
                       .arg(g.value(QStringLiteral("status")).toString(), g.value(QStringLiteral("objective")).toString())
                       .arg(qint64(g.value(QStringLiteral("tokensUsed")).toDouble()))
                       .arg(secs / 60);
    const qint64 budget = qint64(g.value(QStringLiteral("tokenBudget")).toDouble());
    if (budget > 0) line += QStringLiteral(" (budget %1 tokens)").arg(budget);
    return line;
}

// Commands that run a job on the chat's session (a mode, /undo, /doctor, a new or resumed goal, a skill with a mode).
bool startsJob(const QString& cmd, const QString& rest) {
    static const QStringList jobs{"plan", "explore", "modify", "research", "undo", "doctor"};
    if (jobs.contains(cmd)) return true;
    if (cmd == QLatin1String("goal")) {
        static const QStringList side{"show", "pause", "drop", "clear"};
        return !rest.isEmpty() && !side.contains(rest.section(QLatin1Char(' '), 0, 0));
    }
    for (const SkillInfo& s : listSkills())
        if (s.slug == cmd) return s.mode == QLatin1String("plan") || s.mode == QLatin1String("exploration");
    return false;
}

void goal(ChatSession& chat, const QString& arg) {
    const QString op = arg.section(QLatin1Char(' '), 0, 0);
    static const QStringList kOps{"pause", "resume", "drop", "show", "clear"};
    if (arg.isEmpty() || op == QLatin1String("show")) {
        sideJob(chat, [](OmcSession& s, ChatUi* ui) {
            const QJsonObject r = s.request(QJsonObject{{"type", "goal"}, {"op", "get"}});
            if (!r.value(QStringLiteral("success")).toBool()) return post(ui, errorOf(r), true);
            post(ui, describeGoal(r.value(QStringLiteral("data")).toObject().value(QStringLiteral("goal")).toObject()));
        });
        return;
    }
    if (op == QLatin1String("pause") || op == QLatin1String("drop") || op == QLatin1String("clear")) {
        const QString wire = op == QLatin1String("pause") ? QStringLiteral("pause") : QStringLiteral("drop");
        sideJob(chat, [wire](OmcSession& s, ChatUi* ui) {
            const QJsonObject r = s.request(QJsonObject{{"type", "goal"}, {"op", wire}});
            if (!r.value(QStringLiteral("success")).toBool()) return post(ui, errorOf(r), true);
            post(ui, wire == QLatin1String("pause") ? QStringLiteral("Goal paused. `/goal resume` continues it.")
                                                    : QStringLiteral("Goal cleared."));
        });
        return;
    }
    std::shared_ptr<ChatUi> ui = chat.context()->ui;
    if (op == QLatin1String("resume")) {
        chat.runJob([ui](OmcSession& s) {
            const QJsonObject r = s.request(QJsonObject{{"type", "goal"}, {"op", "resume"}});
            if (!r.value(QStringLiteral("success")).toBool()) return post(ui.get(), errorOf(r), true);
            s.prompt(QStringLiteral("Continue working toward the goal."));
        });
        return;
    }
    // a new goal: omc keeps running continuation turns until the model completes it, the budget runs out or the
    // user pauses or stops it
    const QString objective = arg;
    chat.runJob([ui, objective](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "goal"}, {"op", "create"}, {"objective", objective}});
        if (!r.value(QStringLiteral("success")).toBool()) return post(ui.get(), errorOf(r), true);
        s.prompt(objective);
    });
}

void btw(ChatSession& chat, const QString& question) {
    if (question.isEmpty()) {
        chat.postLocal(output(QStringLiteral("Usage: /btw <question>"), true));
        return;
    }
    // answered beside the running turn: btw_delta / btw_record frames stream the answer
    sideJob(chat, [question](OmcSession& s, ChatUi* ui) {
        const QJsonObject r = s.request(QJsonObject{{"type", "btw"}, {"question", question}});
        if (!r.value(QStringLiteral("success")).toBool()) post(ui, errorOf(r), true);
    });
}

void exportHtml(ChatSession& chat) {
    const QString dbPath = chat.db()->host->databasePath();
    const QString dir = dbPath.isEmpty() ? chat.db()->scratchDir() : QFileInfo(dbPath).absolutePath();
    const QString path = QStringLiteral("%1/rikugan-%2-%3.html")
                             .arg(dir, chat.chatId().left(8), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    sideJob(chat, [path](OmcSession& s, ChatUi* ui) {
        const QJsonObject r = s.request(QJsonObject{{"type", "export_html"}, {"outputPath", path}});
        if (!r.value(QStringLiteral("success")).toBool()) return post(ui, errorOf(r), true);
        const QString written = r.value(QStringLiteral("data")).toObject().value(QStringLiteral("path")).toString();
        post(ui, QStringLiteral("Exported the chat to `%1`.").arg(written.isEmpty() ? path : written));
    });
}

// omc commands that would act on the host process or global state, or that need omc's terminal UI.
const QHash<QString, QString>& blocked() {
    static const QString host = QStringLiteral("it would change the host process's working directory or files outside Rikugan");
    static const QString global = QStringLiteral("it changes settings shared by every chat; use Settings instead");
    static const QString tui = QStringLiteral("it needs omc's terminal interface");
    static const QHash<QString, QString> b{
        {"move", host},         {"wt", host},          {"worktree", host},   {"add-dir", host},  {"remove-dir", host},
        {"dirs", host},         {"init", host},        {"ssh", host},        {"local", tui},     {"loop", tui},
        {"dump", QStringLiteral("it writes the raw model request, credentials included, to a temporary file")},
        {"usage", QStringLiteral("resetting usage spends a provider credit")},
        {"modelpreset", global}, {"slow", global},     {"extended-context", global}, {"forget-approvals", global},
        {"settings", global},   {"setup", global},     {"session", tui},     {"resume", tui},    {"delete", tui},
        {"clear", tui},         {"quit", tui},         {"exit", tui},        {"q", tui},         {"restart", tui},
        {"hotkeys", tui},       {"copy", tui},         {"open", tui},        {"pause", tui},     {"record", tui},
        {"debug", tui},         {"tree", tui},         {"branch", tui},      {"rewind", tui},    {"jobs", tui},
        {"hub", tui},           {"subagents", tui},    {"switch", tui},      {"plan-review", tui}, {"guided-goal", tui},
    };
    return b;
}

}  // namespace

QList<CommandInfo> CommandRouter::catalog() {
    QList<CommandInfo> c{
        {"explore", "rikugan", QStringLiteral("\u25C9"), "Autonomous read-only investigation with findings", "<goal>"},
        {"modify", "rikugan", QStringLiteral("\u270E"), "Explore, plan, patch and save, with every patch undoable", "<change>"},
        {"research", "rikugan", QStringLiteral("\u2756"), "Explore, then write research notes next to the database", "<topic>"},
        {"plan", "rikugan", QStringLiteral("\u25A4"), "Read-only plan first, approve it, then execute step by step", "<task>"},
        {"undo", "rikugan", QStringLiteral("\u21B6"), "Revert the last N database changes", "[N]"},
        {"memory", "rikugan", QStringLiteral("\u2726"), "Show RIKUGAN.md, the memory kept for this database", {}},
        {"doctor", "rikugan", QStringLiteral("\u271A"), "Check model, credentials, tools and decompiler", {}},
        {"agents", "agent", QStringLiteral("\u22D4"), "Running and finished subagents", {}},
        {"goal", "agent", QStringLiteral("\u25CE"), "Work toward a goal until it is met; pause, resume, drop or show it",
         "<objective> | pause | resume | drop | show"},
        {"btw", "agent", QStringLiteral("\u21AA"), "Ask a side question without derailing the running turn", "<question>"},
        {"todo", "agent", QStringLiteral("\u2611"), "Show the agent's live todo list", {}},
        {"compact", "session", QStringLiteral("\u21F2"), "Summarize older context to free the window", "[focus]"},
        {"handoff", "session", QStringLiteral("\u21C9"), "Hand the work to a fresh context with a summary", "[focus]"},
        {"retry", "session", QStringLiteral("\u21BB"), "Retry the last turn", {}},
        {"fork", "session", QStringLiteral("\u2442"), "Branch this chat into a new one", {}},
        {"new", "session", QStringLiteral("\uFF0B"), "Start a new chat", {}},
        {"rename", "session", QStringLiteral("\u270F"), "Rename this chat", "<title>"},
        {"model", "session", QStringLiteral("\u2B22"), "Switch the model for this chat", "[provider/model]"},
        {"effort", "session", QStringLiteral("\u25D4"), "Set the thinking effort", "<off|low|medium|high|xhigh>"},
        {"fast", "session", QStringLiteral("\u26A1"), "Toggle the provider's fast tier", {}},
        {"advisor", "session", QStringLiteral("\u2696"), "Turn the second-model reviewer on or off", "[on|off]"},
        {"export", "session", QStringLiteral("\u21E5"), "Export this chat as HTML next to the database", {}},
        {"login", "session", QStringLiteral("\u26BF"), "Sign in to a provider", "[provider]"},
        {"logout", "session", QStringLiteral("\u23CF"), "Sign out of a provider", "[provider]"},
        {"mcp", "session", QStringLiteral("\u29C9"), "MCP servers", {}},
    };
    for (const SkillInfo& s : listSkills())
        c << CommandInfo{s.slug, QStringLiteral("skill"), QStringLiteral("\u2605"), s.description, QStringLiteral("[request]")};
    return c;
}

void CommandRouter::submit(ChatSession& chat, const QString& raw, const QJsonArray& images) {
    const QString text = raw.trimmed();
    if (!text.startsWith(QLatin1Char('/'))) {
        chat.sendPrompt(raw, images);
        return;
    }
    const QString cmd = text.section(QRegularExpression(QStringLiteral("\\s")), 0, 0).mid(1);
    const QString rest = text.mid(cmd.size() + 1).trimmed();
    // a command that runs a job of its own waits for the running turn: it goes out from the queue when that ends
    if (chat.busy() && startsJob(cmd, rest)) {
        chat.queueCommand(text);
        return;
    }
    if (cmd == QLatin1String("plan")) return ModeDriver::start(chat, Mode::Plan, rest);
    if (cmd == QLatin1String("explore")) return ModeDriver::start(chat, Mode::Explore, rest);
    if (cmd == QLatin1String("modify")) return ModeDriver::start(chat, Mode::Modify, rest);
    if (cmd == QLatin1String("research")) return ModeDriver::start(chat, Mode::Research, rest);
    if (cmd == QLatin1String("undo")) return undo(chat, rest);
    if (cmd == QLatin1String("memory")) return memory(chat);
    if (cmd == QLatin1String("mcp")) return mcp(chat);
    if (cmd == QLatin1String("doctor")) return doctor(chat);
    if (cmd == QLatin1String("goal")) return goal(chat, rest);
    if (cmd == QLatin1String("btw")) return btw(chat, rest);
    if (cmd == QLatin1String("export")) return exportHtml(chat);
    if (cmd == QLatin1String("rename")) {
        if (rest.isEmpty()) return chat.postLocal(output(QStringLiteral("Usage: /rename <title>"), true));
        chat.setTitle(rest);
        return chat.postLocal(output(QStringLiteral("Renamed the chat to \"%1\".").arg(rest)));
    }
    // the panel acts on these (agents panel, login sheet, new chat, fork, todo list)
    static const QStringList ui{"agents", "login", "logout", "new", "fork", "todo"};
    if (ui.contains(cmd)) return chat.postLocal(QJsonObject{{"type", "rikugan_ui_action"}, {"action", cmd}, {"arg", rest}});
    // omc's own commands, run by omc (output arrives as command_output)
    static const QStringList forwarded{"compact", "handoff", "retry", "effort", "fast", "advisor"};
    if (forwarded.contains(cmd) || (cmd == QLatin1String("model") && !rest.isEmpty())) return chat.sendPrompt(text);
    if (cmd == QLatin1String("model")) return chat.postLocal(QJsonObject{{"type", "rikugan_ui_action"}, {"action", "model"}});
    for (const SkillInfo& s : listSkills()) {
        if (s.slug != cmd) continue;
        if (s.mode == QLatin1String("plan")) return ModeDriver::start(chat, Mode::Plan, rest, s.slug);
        if (s.mode == QLatin1String("exploration")) return ModeDriver::start(chat, Mode::Modify, rest, s.slug);
        chat.sendPrompt(QStringLiteral("/skill:%1%2").arg(s.slug, rest.isEmpty() ? QString() : QStringLiteral(" ") + rest));
        return;
    }
    if (const auto it = blocked().constFind(cmd); it != blocked().constEnd())
        return chat.postLocal(output(QStringLiteral("/%1 is not available in Rikugan: %2.").arg(cmd, it.value()), true));
    chat.postLocal(output(QStringLiteral("Unknown command: /%1").arg(cmd), true));
}

QStringList CommandRouter::completions() {
    QStringList c;
    for (const CommandInfo& i : catalog()) c << QStringLiteral("/") + i.name;
    return c;
}

}  // namespace rk
