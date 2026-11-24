#include "common/agent/Subagents.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <optional>

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatTools.h"
#include "common/agent/DbContext.h"
#include "common/agent/SessionListener.h"
#include "common/agent/SystemPrompt.h"
#include "common/config/Paths.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/OmcSession.h"
#include "common/util/Embedded.h"

namespace rk {

QString subagentStatusName(SubagentStatus s) {
    switch (s) {
    case SubagentStatus::Pending: return QStringLiteral("pending");
    case SubagentStatus::Running: return QStringLiteral("running");
    case SubagentStatus::Completed: return QStringLiteral("completed");
    case SubagentStatus::Failed: return QStringLiteral("failed");
    case SubagentStatus::Cancelled: return QStringLiteral("cancelled");
    }
    return {};
}

// ---- registry ----

SubagentRegistry& SubagentRegistry::instance() {
    static SubagentRegistry r;
    return r;
}

QString SubagentRegistry::add(SubagentInfo info) {
    if (info.id.isEmpty()) info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!info.createdAt) info.createdAt = QDateTime::currentMSecsSinceEpoch();
    const QString id = info.id;
    {
        std::lock_guard<std::mutex> g(mu_);
        items_.push_back(std::move(info));
    }
    notify();
    return id;
}

void SubagentRegistry::update(const QString& id, const std::function<void(SubagentInfo&)>& fn) {
    {
        std::lock_guard<std::mutex> g(mu_);
        for (SubagentInfo& i : items_)
            if (i.id == id) fn(i);
    }
    notify();
}

std::vector<SubagentInfo> SubagentRegistry::all() const {
    std::lock_guard<std::mutex> g(mu_);
    return items_;
}

std::optional<SubagentInfo> SubagentRegistry::get(const QString& id) const {
    std::lock_guard<std::mutex> g(mu_);
    for (const SubagentInfo& i : items_)
        if (i.id == id) return i;
    return std::nullopt;
}

void SubagentRegistry::removeFinished() {
    {
        std::lock_guard<std::mutex> g(mu_);
        std::erase_if(items_, [this](const SubagentInfo& i) {
            const bool done = i.status != SubagentStatus::Running && i.status != SubagentStatus::Pending;
            if (done) frames_.erase(i.id);
            return done;
        });
    }
    notify();
}

void SubagentRegistry::appendFrame(const QString& id, const std::string& json) {
    std::vector<std::function<void(const std::string&)>> fns;
    {
        std::lock_guard<std::mutex> g(mu_);
        frames_[id].push_back(json);
        for (const auto& [h, o] : frameObservers_)
            if (o.first == id) fns.push_back(o.second);
    }
    for (const auto& fn : fns) fn(json);
}

std::vector<std::string> SubagentRegistry::frames(const QString& id) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = frames_.find(id);
    return it == frames_.end() ? std::vector<std::string>{} : it->second;
}

int SubagentRegistry::observeFrames(const QString& id, std::function<void(const std::string&)> fn,
                                     std::vector<std::string>* snapshot) {
    std::lock_guard<std::mutex> g(mu_);
    if (snapshot) {
        auto it = frames_.find(id);
        *snapshot = it == frames_.end() ? std::vector<std::string>{} : it->second;
    }
    const int h = nextObserver_++;
    frameObservers_[h] = {id, std::move(fn)};
    return h;
}

void SubagentRegistry::removeFrameObserver(int handle) {
    std::lock_guard<std::mutex> g(mu_);
    frameObservers_.erase(handle);
}

void SubagentRegistry::cancel(const QString& id) {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = cancellers_.find(id);
        if (it != cancellers_.end()) fn = it->second;
    }
    if (fn) fn();
}

void SubagentRegistry::cancelChat(const QString& chatId) {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const SubagentInfo& i : items_)
            if ((chatId.isEmpty() || i.chatId == chatId) && cancellers_.count(i.id)) fns.push_back(cancellers_[i.id]);
    }
    for (const auto& fn : fns) fn();
}

void SubagentRegistry::setCanceller(const QString& id, std::function<void()> fn) {
    std::lock_guard<std::mutex> g(mu_);
    if (fn) cancellers_[id] = std::move(fn);
    else cancellers_.erase(id);
}

bool SubagentRegistry::steer(const QString& id, const QString& text) {
    std::function<void(const QString&)> fn;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = steerers_.find(id);
        if (it != steerers_.end()) fn = it->second;
    }
    if (!fn) return false;
    fn(text);
    return true;
}

void SubagentRegistry::setSteerer(const QString& id, std::function<void(const QString&)> fn) {
    std::lock_guard<std::mutex> g(mu_);
    if (fn) steerers_[id] = std::move(fn);
    else steerers_.erase(id);
}

int SubagentRegistry::addObserver(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(mu_);
    const int h = nextObserver_++;
    observers_[h] = std::move(fn);
    return h;
}

void SubagentRegistry::removeObserver(int handle) {
    std::lock_guard<std::mutex> g(mu_);
    observers_.erase(handle);
}

void SubagentRegistry::notify() {
    std::vector<std::function<void()>> obs;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [h, fn] : observers_) obs.push_back(fn);
    }
    for (auto& fn : obs) fn();
}

// ---- running ----

// The subagent's system addendum: network_recon = preset + perks header + its default perks (user perks
// ignored); report_writer = preset only; custom = perks header + the chosen perks (empty without perks).
QString subagentAddendum(const QString& agentType, const QStringList& perks) {
    auto text = [](const QString& rel) { return embeddedText(("prompts/agents/" + rel).toStdString()).trimmed(); };
    auto perkBlock = [&](const QStringList& keys) {
        QStringList bodies;
        for (const QString& p : keys) {
            const QString t = text(QStringLiteral("perks/") + p + QStringLiteral(".md"));
            if (!t.isEmpty()) bodies << t;
        }
        if (bodies.isEmpty()) return QString();
        return text(QStringLiteral("perks/header.md")) + QStringLiteral("\n\n") + bodies.join(QStringLiteral("\n\n"));
    };
    if (agentType == QLatin1String("network_recon")) {
        const QString perksText = perkBlock({"import_mapping", "string_harvesting", "deep_decompilation"});
        return text(QStringLiteral("network_recon.md")) + (perksText.isEmpty() ? QString() : QStringLiteral("\n\n") + perksText);
    }
    if (agentType == QLatin1String("report_writer")) return text(QStringLiteral("report_writer.md"));
    return perkBlock(perks);
}

namespace {

// The argument a log line shows for a tool call: its path, command, pattern, address ... (first one present).
QString subagentArgSummary(const QJsonObject& args) {
    for (const char* k : {"path", "command", "pattern", "query", "address", "function_name", "name", "new_name", "url", "task"}) {
        const QJsonValue v = args.value(QLatin1String(k));
        if (v.isString() && !v.toString().isEmpty()) return v.toString().section(QLatin1Char('\n'), 0, 0).left(120);
        if (v.isDouble()) return QStringLiteral("0x%1").arg(qulonglong(v.toDouble()), 0, 16);
    }
    return {};
}

}  // namespace

SubagentOutcome runSubagent(ChatContext& parent, const SubagentSpec& spec, const std::shared_ptr<CancelToken>& token) {
    SubagentOutcome out;
    SubagentInfo info;
    info.name = spec.name.isEmpty() ? spec.task.left(40) : spec.name;
    info.task = spec.task;
    info.agentType = spec.agentType;
    info.category = spec.category;
    info.status = SubagentStatus::Running;
    info.parentId = parent.subagentId;
    info.chatId = parent.chatId;
    info.perks = spec.perks;
    const QString id = SubagentRegistry::instance().add(info);
    out.id = id;

    // ask_database: the child works on another open database with that database's tools, and only reads it
    const std::shared_ptr<DbContext> db = spec.database ? spec.database : parent.db;
    const bool peer = spec.database && spec.database != parent.db;
    auto child = std::make_shared<ChatContext>();
    child->chatId = parent.chatId;
    child->dbId = db->dbId;
    child->db = db;
    child->host = db->host.get();
    child->registry = &db->registry;
    child->cache = &db->cache;
    child->artifactsDir = peer ? db->artifactsDir() : parent.artifactsDir;
    child->profile = parent.profile;
    // a change in another database would land in this chat's undo list, which acts on this chat's database
    if (!peer) child->mutations = parent.mutations;
    child->ui = parent.ui;
    child->alwaysAllowScripts = parent.alwaysAllowScripts.load();
    child->permissions = parent.permissions;  // grants given in the chat cover its subagents, and back
    child->depth = parent.depth + 1;
    child->mode = peer ? nullptr : parent.mode;
    child->subagentId = id;
    if (peer) child->setPhase(QStringLiteral("other database"), true);  // Mutating and Script tools refused
    for (const QString& t : spec.extraTools) child->setToolShown(t, true);

    const RikuganConfig& cfg = RikuganConfig::instance();
    OmcOptions o;
    o.cwd = db->scratchDir();
    o.sessionDir = parent.db->sessionsDir() + QStringLiteral("/subagents");
    o.model = cfg.model;
    o.thinking = cfg.thinking;
    o.title = QStringLiteral("subagent: ") + info.name;
    if (spec.noTools) {
        o.noTools = true;
        o.systemPrompt = buildSystemPrompt(*db, *child, {});
    } else {
        o.builtinTools = builtinTools(child->profile, true);
        o.systemPrompt = buildSystemPrompt(*db, *child, o.builtinTools);
        const auto specs = db->registry.visible(child->profile, db->host->hasDecompiler(),
                                                [child](const QString& n) { return chatToolVisible(*child, n); });
        o.hostTools = ToolRegistry::hostToolsJson(specs);
    }

    std::shared_ptr<OmcSession> session;
    // shared with the frame callback, which may still run on an omc thread after this function returns
    struct Watch {
        std::atomic<int> turns{0};
        std::atomic<bool> overLimit{false};
        std::atomic<bool> stopped{false};  // Stop in the agents panel
        std::mutex mu;
        std::weak_ptr<OmcSession> session;
    };
    auto watch = std::make_shared<Watch>();
    std::shared_ptr<ChatUi> ui = spec.silent ? nullptr : parent.ui;
    auto listener = std::make_shared<SessionListener>(child, nullptr);
    listener->setFrameFn([watch, id, maxTurns = spec.maxTurns, ui](std::string_view type, const std::string& json) {
        SubagentRegistry::instance().appendFrame(id, json);  // its transcript, for "Open chat"
        if (type == "turn_start") {
            const int n = ++watch->turns;
            SubagentRegistry::instance().update(id, [n](SubagentInfo& i) { i.turnCount = n; });
            if (n > maxTurns && !watch->overLimit.exchange(true)) {
                std::shared_ptr<OmcSession> s;
                {
                    std::lock_guard<std::mutex> g(watch->mu);
                    s = watch->session.lock();
                }
                if (s) s->abort();
            }
        }
        const bool toolStart = type == "tool_execution_start", toolEnd = type == "tool_execution_end";
        if (!toolStart && !toolEnd && type != "message_end" && type != "turn_start") return;
        const QJsonObject f = QJsonDocument::fromJson(QByteArray::fromRawData(json.data(), qsizetype(json.size()))).object();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (toolStart || toolEnd) {
            const QString tool = f.value(QStringLiteral("toolName")).toString();
            const QString callId = f.value(QStringLiteral("toolCallId")).toString();
            if (toolStart) {
                const QString argText = subagentArgSummary(f.value(QStringLiteral("args")).toObject());
                SubagentRegistry::instance().update(id, [&](SubagentInfo& i) {
                    i.currentTool = tool;
                    i.currentArgs = argText;
                    i.currentToolStartMs = now;
                    ++i.toolCount;
                    i.log << SubagentLogEntry{now, QStringLiteral("tool"), tool, argText, false, true};
                    if (i.log.size() > SubagentInfo::kMaxLog) i.log.removeFirst();
                });
            } else {
                const bool isError = f.value(QStringLiteral("isError")).toBool();
                QString resultLine;
                for (const QJsonValue& c : f.value(QStringLiteral("result")).toObject().value(QStringLiteral("content")).toArray())
                    if (resultLine.isEmpty()) resultLine = c.toObject().value(QStringLiteral("text")).toString().section(QLatin1Char('\n'), 0, 0);
                SubagentRegistry::instance().update(id, [&](SubagentInfo& i) {
                    if (i.currentTool == tool) i.currentTool.clear();
                    // the newest running entry of this tool ends
                    for (auto it = i.log.rbegin(); it != i.log.rend(); ++it) {
                        if (it->running && it->tool == tool) {
                            it->running = false;
                            it->isError = isError;
                            if (!resultLine.isEmpty()) it->summary += QStringLiteral(" \u2192 ") + resultLine.left(160);
                            break;
                        }
                    }
                });
            }
            if (ui)
                ui->postUiEvent(QJsonObject{{"type", "rikugan_subagent_event"},
                                            {"subagentId", id},
                                            {"event", QString::fromUtf8(type.data(), qsizetype(type.size()))},
                                            {"toolName", tool},
                                            {"toolCallId", callId},
                                            {"isError", f.value(QStringLiteral("isError"))}});
        } else if (type == "message_end") {
            const QJsonObject m = f.value(QStringLiteral("message")).toObject();
            if (m.value(QStringLiteral("role")).toString() != QLatin1String("assistant")) return;
            QString text;
            for (const QJsonValue& c : m.value(QStringLiteral("content")).toArray())
                if (c.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
                    text += c.toObject().value(QStringLiteral("text")).toString();
            text = text.trimmed();
            const qint64 used = qint64(m.value(QStringLiteral("usage")).toObject().value(QStringLiteral("totalTokens")).toDouble());
            const bool error = m.value(QStringLiteral("stopReason")).toString() == QLatin1String("error");
            SubagentRegistry::instance().update(id, [&](SubagentInfo& i) {
                i.tokens += used;
                if (error) {
                    i.log << SubagentLogEntry{now, QStringLiteral("error"), {}, m.value(QStringLiteral("errorMessage")).toString(), true, false};
                } else if (!text.isEmpty()) {
                    const QString preview = text.section(QLatin1Char('\n'), 0, 2).left(400);
                    i.recentOutput << preview;
                    while (i.recentOutput.size() > 3) i.recentOutput.removeFirst();
                    i.log << SubagentLogEntry{now, QStringLiteral("text"), {}, preview, false, false};
                }
                while (i.log.size() > SubagentInfo::kMaxLog) i.log.removeFirst();
            });
        } else if (ui) {
            ui->postUiEvent(QJsonObject{{"type", "rikugan_subagent_event"}, {"subagentId", id}, {"event", "turn_start"}});
        }
    });
    session = OmcSession::open(o, listener);
    {
        std::lock_guard<std::mutex> g(watch->mu);
        watch->session = session;
    }
    std::weak_ptr<OmcSession> weak = session;
    listener->setSession(session);
    SubagentRegistry::instance().setCanceller(id, [weak, watch] {
        watch->stopped = true;
        if (auto s = weak.lock()) s->abort();
    });
    // a steering message reaches the child's running turn (omc's steer queues it for the next model call)
    SubagentRegistry::instance().setSteerer(id, [weak](const QString& text) {
        if (auto s = weak.lock()) s->sendControl(QJsonObject{{"type", "steer"}, {"message", text}});
    });

    QString task = QStringLiteral("[SUBAGENT TASK — max %1 turns]\n%2\n\nWhen done, provide a concise summary of your findings. "
                                  "Focus on actionable results, not the process.")
                       .arg(spec.maxTurns)
                       .arg(spec.task);
    if (!spec.systemAddendum.isEmpty()) task = spec.systemAddendum + QStringLiteral("\n\n") + task;

    // the job may outlive this function (a cancel returns at once): what it writes lives on the heap
    struct Run {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        OmcSession::PromptOutcome result;
        QString sessionFile;
    };
    auto run = std::make_shared<Run>();
    session->post([run, task](OmcSession& s) {
        // also when the session failed to open: prompt() then reports why
        OmcSession::PromptOutcome r = s.prompt(task);
        std::lock_guard<std::mutex> g(run->mu);
        run->result = std::move(r);
        run->sessionFile = s.sessionFile();
        run->done = true;
        run->cv.notify_all();
    });
    OmcSession::PromptOutcome result;
    QString sessionFile;
    {
        std::unique_lock<std::mutex> lk(run->mu);
        // a cancel ends the run whenever it comes: while the session opens, before or during the prompt
        auto userWrote = [&] { return spec.yieldsToUser && parent.steerPending.load() > 0; };
        while (!run->done && !(token && token->cancelled()) && !userWrote()) run->cv.wait_for(lk, std::chrono::milliseconds(100));
        if (run->done) {
            result = run->result;
            sessionFile = run->sessionFile;
        } else if (userWrote()) {
            // what it has so far goes back with the call; the chat's agent reads the user's message next
            result.status = QStringLiteral("interrupted");
            if (const auto info = SubagentRegistry::instance().get(id)) result.lastText = info->recentOutput.join(QStringLiteral("\n\n"));
        } else {
            result.status = QStringLiteral("aborted");
        }
    }
    SubagentRegistry::instance().setCanceller(id, nullptr);
    SubagentRegistry::instance().setSteerer(id, nullptr);
    listener->setFrameFn({});
    session->close();
    if (child->alwaysAllowScripts) parent.alwaysAllowScripts = true;

    out.status = result.status;
    out.text = result.lastText;
    out.error = result.error;
    if (watch->overLimit && out.status == QLatin1String("aborted")) out.status = QStringLiteral("completed");
    const bool cancelled = (token && token->cancelled()) || watch->stopped;
    SubagentRegistry::instance().update(id, [&](SubagentInfo& i) {
        i.status = cancelled || out.status == QLatin1String("interrupted") ? SubagentStatus::Cancelled
                   : out.status == QLatin1String("error")          ? SubagentStatus::Failed
                                                                   : SubagentStatus::Completed;
        i.completedAt = QDateTime::currentMSecsSinceEpoch();
        i.summary = out.text.isEmpty() ? out.error : out.text;
        i.sessionFile = sessionFile;
        i.currentTool.clear();
        for (SubagentLogEntry& e : i.log) e.running = false;
    });
    return out;
}

namespace {

QString textParts(const QJsonArray& content) {
    QStringList out;
    for (const QJsonValue& v : content) {
        const QJsonObject p = v.toObject();
        if (p.value(QStringLiteral("type")).toString() == QLatin1String("text")) out << p.value(QStringLiteral("text")).toString();
    }
    return out.join(QLatin1Char('\n'));
}

constexpr qsizetype kTranscriptCap = 60000;

}  // namespace

QString subagentTranscript(const QJsonArray& messages) {
    QStringList parts;
    for (const QJsonValue& v : messages) {
        const QJsonObject m = v.toObject();
        const QString role = m.value(QStringLiteral("role")).toString();
        const QJsonArray content = m.value(QStringLiteral("content")).toArray();
        if (role == QLatin1String("user")) {
            const QString t = stripContextPrefix(textParts(content)).trimmed();
            if (!t.isEmpty()) parts << QStringLiteral("User: ") + t;
        } else if (role == QLatin1String("assistant")) {
            QStringList lines;
            for (const QJsonValue& pv : content) {
                const QJsonObject p = pv.toObject();
                const QString type = p.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("text")) {
                    const QString t = p.value(QStringLiteral("text")).toString().trimmed();
                    if (!t.isEmpty()) lines << t;
                } else if (type == QLatin1String("toolCall")) {
                    lines << QStringLiteral("[tool call] %1 %2").arg(
                        p.value(QStringLiteral("name")).toString(),
                        QString::fromUtf8(QJsonDocument(p.value(QStringLiteral("arguments")).toObject()).toJson(QJsonDocument::Compact)));
                }
            }
            if (!lines.isEmpty()) parts << QStringLiteral("Rikugan: ") + lines.join(QLatin1Char('\n'));
        } else if (role == QLatin1String("toolResult")) {
            const QString t = textParts(content).trimmed();
            parts << QStringLiteral("Tool result (%1): %2").arg(m.value(QStringLiteral("toolName")).toString(), t);
        }
    }
    QString text = parts.join(QStringLiteral("\n\n"));
    if (text.size() > kTranscriptCap) {
        const qsizetype head = kTranscriptCap * 3 / 10;
        text = text.left(head) + QStringLiteral("\n\n[... transcript truncated ...]\n\n") + text.right(kTranscriptCap - head);
    }
    return text;
}

QJsonArray fetchSessionMessages(const std::shared_ptr<OmcSession>& session) {
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        QJsonArray messages;
    };
    auto st = std::make_shared<State>();
    session->post([st](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "get_messages"}});
        std::lock_guard<std::mutex> g(st->mu);
        st->messages = r.value(QStringLiteral("data")).toObject().value(QStringLiteral("messages")).toArray();
        st->done = true;
        st->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(st->mu);
    // the chat may be busy with a turn: the job runs after it; a closed session drops the job
    while (!st->done && !session->closed()) st->cv.wait_for(lk, std::chrono::milliseconds(200));
    return st->messages;
}

}  // namespace rk
