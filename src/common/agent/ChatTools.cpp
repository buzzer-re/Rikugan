#include "common/agent/ChatTools.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include "common/agent/ChatContext.h"
#include "common/agent/Databases.h"
#include "common/agent/DbContext.h"
#include "common/agent/HostMcp.h"
#include "common/agent/ModeState.h"
#include "common/agent/Subagents.h"
#include "common/security/Sanitize.h"
#include "common/tools/ToolRegistry.h"
#include "common/util/Files.h"

namespace rk {

namespace {

}  // namespace

const char* memoryFileHeader() {
    return "# Rikugan Persistent Memory\n\n"
           "This file persists across sessions. "
           "The agent reads the first 200 lines into its system prompt.\n\n";
}

namespace {

ChatContext& chatOf(ToolCall& call) {
    if (!call.chat()) throw ToolError(QStringLiteral("this tool only runs inside a chat"));
    return *call.chat();
}

// NFKD -> ascii, lowercase, drop punctuation, spaces/hyphens -> "-"
QString slugify(const QString& text) {
    QString t = text.normalized(QString::NormalizationForm_KD);
    QString ascii;
    for (QChar c : t)
        if (c.unicode() < 128) ascii += c;
    ascii = ascii.toLower().trimmed();
    ascii.remove(QRegularExpression(QStringLiteral("[^\\w\\s-]")));
    ascii.replace(QRegularExpression(QStringLiteral("[-\\s]+")), QStringLiteral("-"));
    while (ascii.startsWith(QLatin1Char('-'))) ascii.remove(0, 1);
    while (ascii.endsWith(QLatin1Char('-'))) ascii.chop(1);
    return ascii.isEmpty() ? QStringLiteral("untitled") : ascii;
}

ToolResult askUser(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    if (!chat.ui) return ToolResult::error(QStringLiteral("Error: no user is attached to this chat."));
    QStringList options;
    for (const QJsonValue& v : call.array("options")) options << v.toString();
    const UserAnswer a = chat.ui->askUser(call.str("question"), options, call.token());
    if (a.cancelled) return ToolResult::error(QStringLiteral("The user did not answer."));
    return ToolResult::ok(QStringLiteral("User answered: %1").arg(a.text));
}

ToolResult saveMemory(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    const QString fact = sanitize::stripInjectionMarkers(call.str("fact"));
    const QString category = call.has("category") ? call.str("category") : QStringLiteral("general");
    if (fact.isEmpty()) return ToolResult::error(QStringLiteral("Error: 'fact' is required."));
    if (!chat.db || chat.db->host->databasePath().isEmpty())
        return ToolResult::error(QStringLiteral("Error: No IDB/BNDB path set; cannot determine where to save memory."));
    const QString path = chat.db->memoryPath();
    QFile f(path);
    const bool fresh = !QFileInfo::exists(path);
    if (!f.open(QIODevice::Append)) return ToolResult::error(QStringLiteral("Error writing RIKUGAN.md: %1").arg(f.errorString()));
    if (fresh) f.write(memoryFileHeader());
    f.write(QStringLiteral("- [%1] %2\n").arg(category, fact).toUtf8());
    return ToolResult::ok(QStringLiteral("Saved to RIKUGAN.md: [%1] %2").arg(category, fact));
}

// A subagent run the user's message cut short: what it had so far, and what to do about it.
ToolResult interruptedResult(const SubagentOutcome& out) {
    ToolResult r = ToolResult::ok(
        QStringLiteral("Stopped early because the user sent a message: read it first. Do not count this as finished work; "
                       "start the subagent again if its task still matters.\n\nWhat it had so far:\n%1")
            .arg(out.text.isEmpty() ? QStringLiteral("(nothing yet)") : sanitize::stripInjectionMarkers(out.text)));
    r.details.insert(QStringLiteral("subagentId"), out.id);
    return r;
}

ToolResult spawnSubagent(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    const QString task = call.str("task");
    if (task.isEmpty()) return ToolResult::error(QStringLiteral("Error: 'task' is required."));
    SubagentSpec spec;
    spec.task = task;
    spec.maxTurns = int(call.integer("max_turns"));
    spec.yieldsToUser = chat.depth == 0;
    const SubagentOutcome out = runSubagent(chat, spec, call.token());
    if (out.status == QLatin1String("interrupted")) return interruptedResult(out);
    if (out.status == QLatin1String("error") && out.text.isEmpty())
        return ToolResult::error(QStringLiteral("Subagent error: %1").arg(out.error));
    ToolResult r = ToolResult::ok(sanitize::stripInjectionMarkers(out.text.isEmpty() ? QStringLiteral("(Subagent produced no output)") : out.text));
    r.details.insert(QStringLiteral("subagentId"), out.id);
    return r;
}

QString databaseLine(const DbContext& db, const QString& self) {
    const QString path = db.host->databasePath().isEmpty() ? db.host->inputPath() : db.host->databasePath();
    QString line = QStringLiteral("- %1  %2  (%3)").arg(db.dbId, QFileInfo(path).fileName(), path);
    if (db.dbId == self) line += QStringLiteral("  [this chat's database]");
    else if (databases::openedByRikugan(db.dbId)) line += QStringLiteral("  [opened by Rikugan, not shown in a tab]");
    return sanitize::stripInjectionMarkers(line);
}

ToolResult listDatabases(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    QStringList lines{QStringLiteral("Open databases (id, file, path):")};
    for (const auto& db : databases::all()) lines << databaseLine(*db, chat.dbId);
    return ToolResult::ok(lines.join(QLatin1Char('\n')));
}

ToolResult openDatabase(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    const QString path = QFileInfo(expandHome(call.str("path").trimmed())).absoluteFilePath();
    if (call.str("path").trimmed().isEmpty()) return ToolResult::error(QStringLiteral("Error: 'path' is required."));
    const bool inTab = call.boolean("in_tab") && databases::canOpenTab();
    // one loaded in the background still gets its tab when asked for one
    if (auto open = databases::find(path); open && !(inTab && databases::openedByRikugan(open->dbId)))
        return ToolResult::ok(QStringLiteral("Already open:\n%1").arg(databaseLine(*open, chat.dbId)));
    if (!QFileInfo(path).isFile()) return ToolResult::error(QStringLiteral("Error: no such file: %1").arg(path));
    // the path comes from the model: the user says yes first
    if (!chat.ui) return ToolResult::error(QStringLiteral("Error: no user is attached to this chat to approve opening a file."));
    const QString open = QStringLiteral("Open it");
    const QString how = inTab ? QStringLiteral("in a new tab") : QStringLiteral("as another database (analysed in the background, the current view stays as it is)");
    const UserAnswer a = chat.ui->askUser(
        QStringLiteral("Rikugan wants to open %1 %2. Reason: %3")
            .arg(path, how, call.str("reason").isEmpty() ? QStringLiteral("not given") : call.str("reason")),
        {open, QStringLiteral("Don't open")}, call.token());
    if (a.cancelled || a.text != open) return ToolResult::ok(QStringLiteral("The user declined to open %1.").arg(path));
    QString error;
    const auto db = inTab ? databases::openTab(path, call.token(), &error) : databases::load(path, call.token(), &error);
    if (!db) return ToolResult::error(QStringLiteral("Error: could not open %1: %2").arg(path, error));
    if (inTab)
        return ToolResult::ok(QStringLiteral("Opened in a new tab (its analysis runs in the background; the user can chat "
                                             "there too):\n%1\nUse ask_database with this id to work on it.")
                                  .arg(databaseLine(*db, chat.dbId)));
    return ToolResult::ok(QStringLiteral("Opened and analysed:\n%1\nUse ask_database with this id to work on it.")
                              .arg(databaseLine(*db, chat.dbId)));
}

ToolResult askDatabase(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    const auto db = databases::find(call.str("database"));
    if (!db) return ToolResult::error(QStringLiteral("Error: no open database '%1' (see list_databases).").arg(call.str("database")));
    if (db->dbId == chat.dbId) return ToolResult::error(QStringLiteral("Error: that is this chat's own database; use your tools directly."));
    const QString task = call.str("task");
    if (task.isEmpty()) return ToolResult::error(QStringLiteral("Error: 'task' is required."));
    SubagentSpec spec;
    spec.name = QStringLiteral("on %1").arg(QFileInfo(db->host->inputPath()).fileName());
    spec.task = task;
    spec.maxTurns = int(call.integer("max_turns"));
    spec.database = db;
    spec.yieldsToUser = chat.depth == 0;
    const SubagentOutcome out = runSubagent(chat, spec, call.token());
    if (out.status == QLatin1String("interrupted")) return interruptedResult(out);
    if (out.status == QLatin1String("error") && out.text.isEmpty())
        return ToolResult::error(QStringLiteral("Subagent error: %1").arg(out.error));
    ToolResult r = ToolResult::ok(sanitize::stripInjectionMarkers(out.text.isEmpty() ? QStringLiteral("(Subagent produced no output)") : out.text));
    r.details.insert(QStringLiteral("subagentId"), out.id);
    return r;
}

ToolResult closeDatabase(ToolCall& call) {
    const QString id = call.str("database");
    const auto db = databases::find(id);
    if (!db || !databases::openedByRikugan(db->dbId))
        return ToolResult::error(QStringLiteral("Error: '%1' is not a database Rikugan opened (the user's tabs stay open).").arg(id));
    databases::close(db->dbId);
    return ToolResult::ok(QStringLiteral("Closed %1.").arg(db->dbId));
}

ToolResult explorationReport(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    if (!chat.mode) return ToolResult::error(QStringLiteral("Error: exploration_report is only available during /explore, /modify and /research."));
    if (!chat.mode->cancelledNotice.isEmpty()) return ToolResult::ok(chat.mode->cancelledNotice);
    Finding f;
    f.category = call.has("category") ? call.str("category") : QStringLiteral("general");
    if (call.has("address")) {
        qint64 a = 0;
        const QJsonValue v = call.value("address");
        if (v.isDouble()) f.address = uint64_t(qint64(v.toDouble()));
        else if (parsePythonInt(v.toVariant().toString(), &a)) f.address = uint64_t(a);
    }
    f.summary = call.str("summary");
    f.evidence = call.has("evidence") ? call.str("evidence") : QString();
    f.relevance = call.has("relevance") ? call.str("relevance") : QStringLiteral("medium");
    chat.mode->addFinding(f);
    if (f.category == QLatin1String("function_purpose") && f.address) {
        const QString name = call.has("function_name") ? call.str("function_name") : QStringLiteral("sub_%1").arg(*f.address, 0, 16);
        chat.mode->addFunction(FunctionInfo{*f.address, name, f.summary, f.relevance});
    }
    if (f.category == QLatin1String("patch_result") && f.address) {
        auto bytes = [](const QString& h) {
            QString s = h;
            s.remove(QLatin1Char(' '));
            const QByteArray b = QByteArray::fromHex(s.toLatin1());
            return b.toHex().size() == s.size() ? b : QByteArray();
        };
        const QString orig = call.has("original_hex") ? call.str("original_hex") : QString();
        const QString neu = call.has("new_hex") ? call.str("new_hex") : QString();
        const QString ev = f.evidence.toLower();
        chat.mode->addPatch(PatchRecord{*f.address, bytes(orig), bytes(neu), f.summary,
                                        ev.contains(QLatin1String("verif")) || ev.contains(QLatin1String("confirm")), f.evidence});
        if (chat.ui)
            chat.ui->postUiEvent(QJsonObject{{"type", "rikugan_patch_applied"}, {"address", hex(*f.address)}, {"description", f.summary},
                                             {"original_hex", orig}, {"new_hex", neu}});
    }
    if (chat.ui)
        chat.ui->postUiEvent(QJsonObject{{"type", "rikugan_finding"}, {"category", f.category}, {"summary", f.summary},
                                         {"address", f.address ? hex(*f.address) : QString()}, {"relevance", f.relevance}});
    return ToolResult::ok(QStringLiteral("Finding logged: [%1] %2").arg(f.category, f.summary));
}

ToolResult researchNote(ToolCall& call) {
    ChatContext& chat = chatOf(call);
    if (chat.mode && !chat.mode->cancelledNotice.isEmpty()) return ToolResult::ok(chat.mode->cancelledNotice);
    if (!chat.mode || chat.mode->notesDir.isEmpty())
        return ToolResult::error(QStringLiteral("Error: research_note is only available during /research."));
    const QString content = call.str("content");
    if (content.isEmpty()) return ToolResult::error(QStringLiteral("Error: 'content' is required."));
    const QString genre = call.has("genre") ? call.str("genre") : QStringLiteral("general");
    const QString title = call.has("title") ? call.str("title") : QStringLiteral("untitled");
    QStringList related;
    for (const QJsonValue& v : call.array("related_notes")) related << v.toString();
    const QString slug = slugify(title);
    const QString path = chat.mode->notesDir + QLatin1Char('/') + slugify(genre) + QLatin1Char('/') + slug + QStringLiteral(".md");
    QString error;
    if (!writeFileAtomic(path, content.toUtf8(), 0, &error))
        return ToolResult::error(QStringLiteral("Error: cannot write %1: %2").arg(path, error));
    chat.mode->addNote(ResearchNote{genre, title, slug, path, related});
    if (chat.ui)
        chat.ui->postUiEvent(QJsonObject{{"type", "rikugan_research_note"}, {"genre", genre}, {"title", title}, {"path", path}});
    return ToolResult::ok(QStringLiteral("Note saved: %1").arg(path));
}

}  // namespace

void registerChatTools(ToolRegistry& r) {
    r.add(ToolSpec{
        "ask_user", "chat",
        "Ask the user a question and wait for their answer. Use this when you need clarification, confirmation, or a "
        "choice from the user before proceeding.",
        {ToolParam{"question", ToolParam::String, "The question to ask the user.", {}, {}, {}},
         ToolParam{"options", ToolParam::Array, "Optional list of choices for the user.", QJsonArray(), {}, QJsonObject{{"type", "string"}}}},
        Blocking | Exclusive, 20000, askUser});
    r.add(ToolSpec{
        "save_memory", "chat",
        "Save a fact to persistent memory (RIKUGAN.md). Use this to remember important findings across sessions: "
        "function purposes, naming conventions, architecture notes, or analysis results that would be useful in future "
        "sessions.",
        {ToolParam{"fact", ToolParam::String, "The fact or finding to remember.", {}, {}, {}},
         ToolParam{"category", ToolParam::String, "Category of the memory.", {},
                   {"function_purpose", "architecture", "naming_convention", "prior_analysis", "data_structure", "general"}, {}}},
        0, 20000, saveMemory});
    r.add(ToolSpec{
        "spawn_subagent", "chat",
        "Spawn an isolated subagent to handle a complex subtask. The subagent has its own context window and can use all "
        "available tools. It returns a concise summary of its findings. Use this to delegate research-heavy tasks (e.g. "
        "'analyze all functions referencing the score string') without filling your own context with raw tool output.",
        {ToolParam{"task", ToolParam::String, "The task for the subagent to perform.", {}, {}, {}},
         ToolParam{"max_turns", ToolParam::Integer, "Maximum turns for the subagent (default: 20).", 20, {}, {}}},
        Blocking, 20000, spawnSubagent});
    r.add(ToolSpec{
        "exploration_report", "chat",
        "Log a structured finding during binary exploration. Call this whenever you discover something relevant to the "
        "user's goal: a function's purpose, a key constant, a data structure, or a hypothesis about what to change.",
        {ToolParam{"category", ToolParam::String, "Type of finding.", {},
                   {"function_purpose", "data_structure", "constant", "hypothesis", "string_ref", "import_usage", "patch_result", "general"}, {}},
         ToolParam{"address", ToolParam::Integer, "Address related to this finding (hex or decimal).", QJsonValue(QJsonValue::Null), {}, {}},
         ToolParam{"function_name", ToolParam::String, "Name of the function (for function_purpose findings).", QJsonValue(QJsonValue::Null), {}, {}},
         ToolParam{"summary", ToolParam::String, "Brief summary of the finding.", {}, {}, {}},
         ToolParam{"evidence", ToolParam::String, "Supporting evidence (e.g. decompiled code snippet).", QJsonValue(QJsonValue::Null), {}, {}},
         ToolParam{"relevance", ToolParam::String, "How relevant to the user's goal.", QJsonValue(QJsonValue::Null), {"low", "medium", "high"}, {}},
         ToolParam{"original_hex", ToolParam::String,
                   "Original bytes as hex string (for patch_result category). E.g. '74 05'.", QJsonValue(QJsonValue::Null), {}, {}},
         ToolParam{"new_hex", ToolParam::String, "New patched bytes as hex string (for patch_result category). E.g. '75 05'.",
                   QJsonValue(QJsonValue::Null), {}, {}}},
        0, 20000, explorationReport});
    r.add(ToolSpec{
        "research_note", "chat",
        "Write an Obsidian-compatible markdown research note to the notes/ folder. Use this to document findings during "
        "research mode. Notes should include [[wiki-links]] to cross-reference other notes, mermaid diagrams for call "
        "flows, and tables for function/address listings. Write notes progressively as you discover things — don't wait "
        "until the end.",
        {ToolParam{"genre", ToolParam::String,
                   "Folder category for the note: networking, crypto, initialization, data-structures, persistence, "
                   "anti-analysis, command-and-control, general, etc.",
                   {}, {}, {}},
         ToolParam{"title", ToolParam::String, "Note title (becomes the filename slug).", {}, {}, {}},
         ToolParam{"content", ToolParam::String,
                   "Full markdown body with Obsidian conventions: [[wiki-links]], #tags, mermaid diagrams, tables. Include "
                   "addresses, decompiled snippets, and evidence.",
                   {}, {}, {}},
         ToolParam{"related_notes", ToolParam::Array, "Titles of other notes to cross-link via [[wiki-links]].", QJsonArray(), {},
                   QJsonObject{{"type", "string"}}}},
        0, 20000, researchNote});
    r.add(ToolSpec{
        "list_databases", "chat",
        "List the databases open in this host: this chat's own, the user's other tabs, and the ones opened with "
        "open_database. Each has an id for ask_database.",
        {}, 0, 20000, listDatabases});
    r.add(ToolSpec{
        "open_database", "chat",
        "Open a file on disk (a binary or a .bndb) as another database, to compare it with this one or study it on its "
        "own. It opens in a new tab of the host's window, a view the user sees and can chat in; with in_tab false it is "
        "analysed in the background with no tab instead. This chat's database is not touched. The user approves first. "
        "Returns the database id; then use ask_database. Prefer this over loading files with execute_python.",
        {ToolParam{"path", ToolParam::String, "Absolute path of the file to open.", {}, {}, {}},
         ToolParam{"reason", ToolParam::String, "Why you need it (shown to the user).", QString(), {}, {}},
         ToolParam{"in_tab", ToolParam::Boolean,
                   "Open it in a new tab of the host's window (a view the user sees). False: in the background, no tab.", true, {}, {}}},
        Blocking | Exclusive, 20000, openDatabase});
    r.add(ToolSpec{
        "ask_database", "chat",
        "Give a task to a subagent that works on another open database (see list_databases) with that database's own "
        "analysis tools, read-only, and returns its findings. Use it to compare versions, look up a function in a "
        "library, or check how another binary uses a structure.",
        {ToolParam{"database", ToolParam::String, "Database id (or file path) from list_databases.", {}, {}, {}},
         ToolParam{"task", ToolParam::String, "What to find out there, with the names and addresses that matter.", {}, {}, {}},
         ToolParam{"max_turns", ToolParam::Integer, "Maximum turns for the subagent (default: 20).", 20, {}, {}}},
        Blocking, 20000, askDatabase});
    r.add(ToolSpec{
        "close_database", "chat",
        "Close a database you opened with open_database when you no longer need it. The user's own tabs cannot be closed.",
        {ToolParam{"database", ToolParam::String, "Database id from open_database.", {}, {}, {}}},
        0, 20000, closeDatabase});
}

bool chatToolVisible(const ChatContext& chat, const QString& name) {
    // the host's MCP tools, once the user turned them on for this database, stand in for Rikugan's analysis tools:
    // one tool set per request, not two
    const HostMcp& mcp = HostMcp::instance();
    const bool hostMcp = chat.db && chat.db->hostMcp.load() && mcp.toolCount() > 0;
    if (mcp.owns(name)) return hostMcp && mcp.offers(name);
    if (hostMcp) {
        const ToolSpec* spec = chat.registry ? chat.registry->find(name) : nullptr;
        if (spec && spec->category != QLatin1String("chat")) return false;
    }
    if (name == QLatin1String("spawn_subagent")) return chat.depth == 0;
    // a host that opens a second database (Binary Ninja); a subagent on another database does not branch further
    if (name == QLatin1String("list_databases") || name == QLatin1String("open_database") ||
        name == QLatin1String("ask_database") || name == QLatin1String("close_database"))
        return chat.depth == 0 && databases::canLoad();
    if (name == QLatin1String("exploration_report") || name == QLatin1String("research_note")) return chat.toolShown(name);
    return true;
}

}  // namespace rk

namespace rk {

QJsonArray chatHostTools(const ChatContext& chat) {
    const auto specs = chat.registry->visible(chat.profile, chat.host->hasDecompiler(),
                                              [&](const QString& n) { return chatToolVisible(chat, n); });
    return ToolRegistry::hostToolsJson(specs);
}

}  // namespace rk
