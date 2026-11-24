#include "common/agent/ChatSession.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QJsonDocument>
#include <QList>
#include <QPointer>

#include "common/agent/ChatTools.h"
#include "common/agent/DbContext.h"
#include "common/agent/SessionListener.h"
#include "common/agent/Subagents.h"
#include "common/agent/SystemPrompt.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AuthSync.h"
#include "common/omc/EventQueue.h"
#include "common/omc/OmcSession.h"
#include "common/util/Files.h"

namespace rk {

namespace {

constexpr int kMaxLiveSessions = 4;
QList<ChatSession*> g_lru;  // UI thread; front = most recent

std::string dump(const QJsonObject& o) { return compactJson(o).toStdString(); }

QJsonObject approvalJson(const ApprovalRequest& r) {
    return QJsonObject{{"kind", r.kind == ApprovalRequest::Mutation ? "mutation" : "script"},
                       {"toolName", r.toolName},
                       {"arguments", r.arguments},
                       {"reason", r.reason}};
}

ApprovalRequest approvalFromJson(const QJsonObject& o) {
    ApprovalRequest r;
    r.kind = o.value(QStringLiteral("kind")).toString() == QLatin1String("mutation") ? ApprovalRequest::Mutation
                                                                                    : ApprovalRequest::Script;
    r.toolName = o.value(QStringLiteral("toolName")).toString();
    r.arguments = o.value(QStringLiteral("arguments")).toObject();
    r.reason = o.value(QStringLiteral("reason")).toString();
    return r;
}

}  // namespace

// Cards asked from tool workers. Requests and cancellations reach the UI through the chat's EventQueue (which
// survives the ChatSession being destroyed); answers come back through answer*() on the UI thread.
class ChatUiBridge : public ChatUi {
public:
    explicit ChatUiBridge(std::shared_ptr<EventQueue> q) : q_(std::move(q)) {}

    ApprovalChoice askApproval(const ApprovalRequest& req, const std::shared_ptr<CancelToken>& token) override {
        const int id = open();
        q_->push(dump(QJsonObject{{"type", "rikugan_approval"}, {"requestId", id}, {"request", approvalJson(req)}}));
        const Answer a = wait(id, token);
        if (a.cancelled) return ApprovalChoice::Deny;
        return a.choice;
    }

    UserAnswer askUser(const QString& question, const QStringList& options,
                       const std::shared_ptr<CancelToken>& token) override {
        const int id = open();
        q_->push(dump(QJsonObject{{"type", "rikugan_question"},
                                  {"requestId", id},
                                  {"question", question},
                                  {"options", QJsonArray::fromStringList(options)}}));
        const Answer a = wait(id, token);
        return UserAnswer{a.cancelled, a.text};
    }

    UserAnswer askDecision(const QString& kind, const QJsonObject& payload, const std::shared_ptr<CancelToken>& token) override {
        const int id = open();
        q_->push(dump(QJsonObject{{"type", "rikugan_decision"}, {"requestId", id}, {"kind", kind}, {"payload", payload}}));
        const Answer a = wait(id, token);
        return UserAnswer{a.cancelled, a.text};
    }

    void postUiEvent(const QJsonObject& event) override { q_->push(dump(event)); }

    void answerApproval(int id, ApprovalChoice c) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        it->second.done = true;
        it->second.choice = c;
        cv_.notify_all();
    }

    void answerQuestion(int id, const UserAnswer& a) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        it->second.done = true;
        it->second.cancelled = a.cancelled;
        it->second.text = a.text;
        cv_.notify_all();
    }

    // The chat is going away: every open card resolves as cancelled.
    void detach() {
        std::lock_guard<std::mutex> g(mu_);
        detached_ = true;
        for (auto& [id, a] : pending_) {
            a.done = true;
            a.cancelled = true;
        }
        cv_.notify_all();
    }

private:
    struct Answer {
        bool done = false;
        bool cancelled = false;
        ApprovalChoice choice = ApprovalChoice::Deny;
        QString text;
    };

    int open() {
        std::lock_guard<std::mutex> g(mu_);
        const int id = next_++;
        pending_[id] = Answer{};
        if (detached_) pending_[id] = Answer{true, true, ApprovalChoice::Deny, {}};
        return id;
    }

    Answer wait(int id, const std::shared_ptr<CancelToken>& token) {
        std::unique_lock<std::mutex> lk(mu_);
        while (!pending_[id].done) {
            cv_.wait_for(lk, std::chrono::milliseconds(100));
            if (!pending_[id].done && token && token->cancelled()) {
                pending_[id].done = true;
                pending_[id].cancelled = true;
                lk.unlock();
                q_->push(dump(QJsonObject{{"type", "rikugan_card_cancelled"}, {"requestId", id}}));
                lk.lock();
            }
        }
        Answer a = pending_[id];
        pending_.erase(id);
        return a;
    }

    std::shared_ptr<EventQueue> q_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<int, Answer> pending_;
    int next_ = 1;
    bool detached_ = false;
};

ChatSession::ChatSession(std::shared_ptr<DbContext> db, ChatEntry entry, QObject* parent)
    : QObject(parent), db_(std::move(db)), entry_(std::move(entry)) {
    static const int reg = [] {
        qRegisterMetaType<rk::ApprovalRequest>();
        qRegisterMetaType<rk::ChatEntry>();
        return 0;
    }();
    (void)reg;
    events_ = std::make_shared<EventQueue>(this);
    bridge_ = std::make_shared<ChatUiBridge>(events_);
    ctx_ = std::make_shared<ChatContext>();
    ctx_->chatId = entry_.chatId;
    ctx_->dbId = db_->dbId;
    ctx_->db = db_;
    ctx_->host = db_->host.get();
    ctx_->registry = &db_->registry;
    ctx_->cache = &db_->cache;
    ctx_->artifactsDir = db_->artifactsDir();
    ctx_->profile = activeProfile();
    ctx_->ui = bridge_;
    std::weak_ptr<EventQueue> wq = events_;
    ctx_->mutations->setOnChanged([wq] {
        if (auto q = wq.lock()) q->push(R"({"type":"rikugan_mutations_changed"})");
    });
    ctx_->permissions->setSink([wq](const QJsonObject& f) {
        if (auto q = wq.lock()) q->push(dump(f));
    });
}

ChatSession::~ChatSession() {
    g_lru.removeAll(this);
    // its mode job and every agent it started end with it (their own sessions do not see this one close)
    if (jobToken_) jobToken_->cancel();
    SubagentRegistry::instance().cancelChat(entry_.chatId);
    ctx_->mutations->setOnChanged({});
    ctx_->permissions->setSink({});
    ctx_->permissions->cancelAll();
    bridge_->detach();
    events_->detach();
    if (listener_) listener_->setFrameFn({});
    if (session_) session_->close();
}

bool ChatSession::isOpen() const { return session_ && !session_->closed(); }

void ChatSession::touch(ChatSession* s) {
    g_lru.removeAll(s);
    g_lru.prepend(s);
    int open = 0;
    for (ChatSession* c : g_lru) open += c->isOpen() ? 1 : 0;
    for (qsizetype i = g_lru.size() - 1; i >= 0 && open > kMaxLiveSessions; --i) {
        ChatSession* c = g_lru[i];
        if (c != s && c->isOpen() && !c->busy()) {
            c->closeSession();
            --open;
        }
    }
}

void ChatSession::ensureOpen() {
    // a session omc refused to start (no model yet) is tried again: a model may have been connected since
    if (session_ && session_->openFailed()) closeSession();
    if (!isOpen()) openSession();
    touch(this);
}

void ChatSession::openSession() {
    const RikuganConfig& cfg = RikuganConfig::instance();
    const QStringList builtin = builtinTools();
    const std::shared_ptr<ChatContext> ctx = ctx_;
    const auto specs = db_->registry.visible(ctx->profile, db_->host->hasDecompiler(),
                                             [ctx](const QString& n) { return chatToolVisible(*ctx, n); });

    OmcOptions o;
    o.cwd = db_->scratchDir();
    o.sessionDir = db_->sessionsDir();
    o.resume = entry_.sessionFile;
    o.model = cfg.model;
    o.thinking = cfg.thinking;
    o.systemPrompt = buildSystemPrompt(*db_, *ctx_, builtin);
    o.builtinTools = builtin;
    o.hostTools = ToolRegistry::hostToolsJson(specs);
    // a name the user chose (or a fork's) is the session's too; otherwise omc names it after the first exchange
    if (entry_.titleByUser) o.title = entry_.title;

    std::weak_ptr<EventQueue> wq = events_;
    std::weak_ptr<ChatContext> wctx = ctx_;
    listener_ = std::make_shared<SessionListener>(ctx_, [wq, wctx](std::string_view type, const std::string& json) {
        if (auto c = wctx.lock()) c->notifyFrame(type);
        if (auto q = wq.lock()) q->push(json);
    });
    session_ = OmcSession::open(o, listener_);
    listener_->setSession(session_);
    ctx_->session = session_;
    if (!entry_.sessionFile.isEmpty()) {
        transcriptPending_ = true;
        session_->post([wq](OmcSession& s) {
            const QJsonObject r = s.request(QJsonObject{{"type", "get_messages"}});
            if (auto q = wq.lock())
                q->push(dump(QJsonObject{{"type", "rikugan_transcript"},
                                         {"messages", r.value(QStringLiteral("data")).toObject().value(QStringLiteral("messages"))}}));
        });
    }
    session_->post([wq](OmcSession& s) {
        if (auto q = wq.lock()) q->push(dump(QJsonObject{{"type", "rikugan_state"}, {"state", s.cachedState()}}));
    });
}

void ChatSession::closeSession() {
    if (!session_) return;
    if (listener_) listener_->setFrameFn({});
    ctx_->permissions->cancelAll();
    session_->close();
    session_.reset();
    listener_.reset();
    ctx_->session.reset();
}

void ChatSession::hostToolsChanged() {
    if (!session_) return;
    if (!busy_) {
        closeSession();
        return;
    }
    const QJsonArray tools = chatHostTools(*ctx_);
    session_->post([tools](OmcSession& s) { s.request(QJsonObject{{"type", "set_host_tools"}, {"tools", tools}}); });
    reopenWhenIdle_ = true;
}

void ChatSession::sendPrompt(const QString& text, const QJsonArray& givenImages) {
    QJsonArray images = givenImages;
    if (images.isEmpty()) {
        // a queued message going out now: the images it was typed with
        for (qsizetype i = 0; i < queuedImages_.size(); ++i)
            if (queuedImages_[i].first == text) {
                images = queuedImages_.takeAt(i).second;
                break;
            }
    }
    if (busy_) {
        if (!images.isEmpty()) queuedImages_.append({text, images});
        // a message typed while the agent works reaches it at its next turn (omc's steer); commands, and a chat
        // whose session never started, wait for the run to end
        if (!text.startsWith(QLatin1Char('/')) && session_ && session_->isOpen() && !session_->openFailed()) {
            steering_ << text;
            QJsonObject steer{{"type", "steer"}, {"message", text}};
            if (!images.isEmpty()) steer.insert(QStringLiteral("images"), images);
            session_->sendControl(steer);
        } else {
            queue_ << text;
        }
        queueUpdated();
        return;
    }
    ensureOpen();
    busy_ = true;
    Q_EMIT busyChanged(true);
    if (entry_.title.isEmpty()) entry_.title = ChatIndex::titleFrom(text);
    entry_.lastActiveAt = QDateTime::currentMSecsSinceEpoch();
    persistEntry();
    std::weak_ptr<EventQueue> wq = events_;
    std::shared_ptr<DbContext> db = db_;
    std::shared_ptr<ChatContext> ctx = ctx_;
    session_->post([wq, db, ctx, text, images](OmcSession& s) {
        // omc recognises its commands (/skill:x, /compact ...) only at the start of the text
        const QString prefix = text.startsWith(QLatin1Char('/')) ? QString() : contextPrefix(*db, *ctx);
        const OmcSession::PromptOutcome out = s.prompt(prefix + text, images);
        AuthSync::run();
        if (auto q = wq.lock())
            q->push(dump(QJsonObject{{"type", "rikugan_prompt_done"},
                                     {"status", out.status},
                                     {"error", out.error},
                                     {"text", out.lastText},
                                     {"state", s.cachedState()}}));
    });
}

void ChatSession::runJob(std::function<void(OmcSession&)> job, std::function<void()> done) {
    runJob([job = std::move(job)](OmcSession& s, const std::shared_ptr<CancelToken>&) { job(s); }, std::move(done));
}

void ChatSession::runJob(std::function<void(OmcSession&, const std::shared_ptr<CancelToken>&)> job,
                         std::function<void()> done) {
    ensureOpen();
    busy_ = true;
    Q_EMIT busyChanged(true);
    const int id = nextJob_++;
    jobDone_[id] = std::move(done);
    auto token = std::make_shared<CancelToken>();
    jobToken_ = token;
    std::weak_ptr<EventQueue> wq = events_;
    session_->post([wq, id, token, job = std::move(job)](OmcSession& s) {
        job(s, token);
        if (auto q = wq.lock()) q->push(dump(QJsonObject{{"type", "rikugan_job_done"}, {"jobId", id}, {"state", s.cachedState()}}));
    });
}

void ChatSession::postLocal(const QJsonObject& event) { Q_EMIT frame(event); }

void ChatSession::stop() {
    if (session_)
        for (const QString& t : std::as_const(steering_))
            session_->sendControl(QJsonObject{{"type", "remove_queued_message"}, {"message", t}, {"queue", "steering"}});
    if (!queue_.isEmpty() || !steering_.isEmpty()) {
        queue_.clear();
        steering_.clear();
        queueUpdated();
    }
    if (jobToken_) jobToken_->cancel();  // a mode waiting on a card or a subagent
    ctx_->permissions->cancelAll();
    if (session_) session_->abort();
}

// index into queued(): the steered messages first, then the ones waiting for the run to end
void ChatSession::removeQueued(int index) {
    if (index < 0 || index >= steering_.size() + queue_.size()) return;
    if (index < steering_.size()) {
        if (session_)
            session_->sendControl(QJsonObject{{"type", "remove_queued_message"}, {"message", steering_[index]}, {"queue", "steering"}});
        steering_.removeAt(index);
    } else {
        queue_.removeAt(index - steering_.size());
    }
    queueUpdated();
}

void ChatSession::sendQueuedNow(int index) {
    QStringList order = queued();
    if (index < 0 || index >= order.size()) return;
    const QString text = order.takeAt(index);
    if (!busy_) {
        removeQueued(index);
        if (text.startsWith(QLatin1Char('/')) && commandRunner_) commandRunner_(text);
        else sendPrompt(text);
        return;
    }
    stop();  // drops both queues (and omc's copy of the steered ones)
    // what waits now goes out when the stopped run ends: this message first, the others after it, all as prompts
    queue_ = QStringList{text} + order;
    queueUpdated();
}

// The run ended before the agent took these (a mode waiting on a card, a run that stopped early): they leave omc's
// queue and go out as the next prompts.
void ChatSession::reclaimSteering() {
    if (steering_.isEmpty()) return;
    if (session_)
        for (const QString& t : std::as_const(steering_))
            session_->sendControl(QJsonObject{{"type", "remove_queued_message"}, {"message", t}, {"queue", "steering"}});
    queue_ = steering_ + queue_;
    steering_.clear();
    queueUpdated();
}

void ChatSession::setTitle(const QString& title) {
    entry_.title = title;
    entry_.titleByUser = true;
    persistEntry();
    // the session file carries the name too, and omc stops naming it
    if (session_ && isOpen()) session_->post([title](OmcSession& s) { s.request(QJsonObject{{"type", "set_session_name"}, {"name", title}}); });
}

void ChatSession::setModel(const QString& model) {
    changeSetting(QJsonObject{{"type", "set_model"}, {"provider", model.section(QLatin1Char('/'), 0, 0)},
                              {"modelId", model.section(QLatin1Char('/'), 1)}});
}

void ChatSession::setThinkingLevel(const QString& level) {
    changeSetting(QJsonObject{{"type", "set_thinking_level"}, {"level", level}});
}

// A session setting: the request, then the state the panel shows.
void ChatSession::changeSetting(const QJsonObject& request) {
    ensureOpen();
    session_->post([request](OmcSession& s) {
        s.request(request);
        s.refreshState();
    });
    std::weak_ptr<EventQueue> wq = events_;
    session_->post([wq](OmcSession& s) {
        if (auto q = wq.lock()) q->push(dump(QJsonObject{{"type", "rikugan_state"}, {"state", s.cachedState()}}));
    });
}

void ChatSession::fork(std::function<void(const ChatEntry&)> done, std::function<void(const QString&)> failed) {
    if (busy_) {
        failed(QStringLiteral("The chat is busy."));
        return;
    }
    if (entry_.sessionFile.isEmpty()) {
        failed(QStringLiteral("Nothing to fork yet."));
        return;
    }
    // a separate session on the same file: `fork` moves that session to the new file, this chat stays
    OmcOptions o;
    o.cwd = db_->scratchDir();
    o.sessionDir = db_->sessionsDir();
    o.resume = entry_.sessionFile;
    o.builtinTools = {QStringLiteral("read")};
    QPointer<ChatSession> self(this);
    auto forker = OmcSession::open(o, std::make_shared<SessionListener>(ctx_, nullptr));
    auto result = std::make_shared<QJsonObject>();
    std::weak_ptr<EventQueue> wq = events_;
    const QString source = entry_.chatId;
    forker->post([wq, result](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "fork"}});
        if (auto q = wq.lock()) q->push(dump(QJsonObject{{"type", "rikugan_fork_done"}, {"response", r}}));
        s.close();
    });
    // the reply is handled in drain(): stash the callbacks
    forkDone_ = [this, done, failed, source](const QJsonObject& r) {
        const QString file = r.value(QStringLiteral("data")).toObject().value(QStringLiteral("sessionFile")).toString();
        if (!r.value(QStringLiteral("success")).toBool() || file.isEmpty()) {
            failed(r.value(QStringLiteral("error")).toString(QStringLiteral("fork failed")));
            return;
        }
        ChatEntry e = db_->chats->create(source);
        e.sessionFile = file;
        e.title = entry_.title.isEmpty() ? QStringLiteral("Fork") : entry_.title + QStringLiteral(" (fork)");
        e.titleByUser = true;  // its own name: the copied session name must not replace it
        db_->chats->update(e);
        done(e);
    };
}

void ChatSession::answerApproval(int requestId, ApprovalChoice choice) { bridge_->answerApproval(requestId, choice); }
void ChatSession::answerQuestion(int requestId, const UserAnswer& answer) { bridge_->answerQuestion(requestId, answer); }
void ChatSession::answerPermission(const QString& key, PermissionChoice choice) { ctx_->permissions->answer(key, choice); }

void ChatSession::answerUiRequest(const QString& id, const QJsonObject& answer) {
    if (!session_) return;
    QJsonObject f = answer;
    f.insert(QStringLiteral("type"), QStringLiteral("extension_ui_response"));
    f.insert(QStringLiteral("id"), id);
    session_->sendControl(f);
}

bool ChatSession::event(QEvent* e) {
    if (e->type() == EventQueue::DrainEvent) {
        drain();
        return true;
    }
    return QObject::event(e);
}

void ChatSession::persistEntry() {
    db_->chats->update(entry_);
    Q_EMIT entryChanged(entry_);
}

void ChatSession::finishPrompt(const QString& status, const QString& error, const QString& text) {
    busy_ = false;
    Q_EMIT busyChanged(false);
    Q_EMIT promptFinished(status, error, text);
    startNext();
}

void ChatSession::startNext() {
    if (busy_) return;
    reclaimSteering();
    if (reopenWhenIdle_) {
        reopenWhenIdle_ = false;
        closeSession();
    }
    // images of messages the agent took while it ran are in omc's hands: only the waiting ones keep theirs
    queuedImages_.removeIf([this](const QPair<QString, QJsonArray>& e) { return !queue_.contains(e.first); });
    if (queue_.isEmpty()) return;
    const QString next = queue_.takeFirst();
    queueUpdated();
    if (next.startsWith(QLatin1Char('/')) && commandRunner_) commandRunner_(next);
    else sendPrompt(next);
}

// The queue changed: the panel shows it, and long tools learn whether a typed message waits for the agent.
void ChatSession::queueUpdated() {
    ctx_->steerPending = int(steering_.size());
    Q_EMIT queueChanged(queued());
}

void ChatSession::queueCommand(const QString& text) {
    queue_ << text;
    queueUpdated();
    if (!busy_) startNext();
}

void ChatSession::drain() {
    for (const std::string& raw : events_->drain()) {
        const QJsonObject f = QJsonDocument::fromJson(QByteArray::fromStdString(raw)).object();
        const QString type = f.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("rikugan_prompt_done")) {
            const QJsonObject st = f.value(QStringLiteral("state")).toObject();
            const QString file = st.value(QStringLiteral("sessionFile")).toString();
            if (!file.isEmpty() && file != entry_.sessionFile) {
                entry_.sessionFile = file;
                persistEntry();
            }
            if (!st.isEmpty()) onState(st);
            finishPrompt(f.value(QStringLiteral("status")).toString(), f.value(QStringLiteral("error")).toString(),
                         f.value(QStringLiteral("text")).toString());
        } else if (type == QLatin1String("rikugan_job_done")) {
            std::function<void()> fn;
            auto jt = jobDone_.find(f.value(QStringLiteral("jobId")).toInt());
            if (jt != jobDone_.end()) {
                fn = std::move(jt->second);
                jobDone_.erase(jt);
            }
            const QJsonObject st = f.value(QStringLiteral("state")).toObject();
            const QString file = st.value(QStringLiteral("sessionFile")).toString();
            if (!file.isEmpty() && file != entry_.sessionFile) {
                entry_.sessionFile = file;
                persistEntry();
            }
            if (!st.isEmpty()) onState(st);
            busy_ = false;
            Q_EMIT busyChanged(false);
            if (fn) fn();
            if (!busy_) startNext();
        } else if (type == QLatin1String("rikugan_state")) {
            const QJsonObject st = f.value(QStringLiteral("state")).toObject();
            if (!st.isEmpty()) onState(st);
        } else if (type == QLatin1String("rikugan_transcript")) {
            transcriptPending_ = false;
            Q_EMIT transcriptLoaded(f.value(QStringLiteral("messages")).toArray());
        } else if (type == QLatin1String("rikugan_approval")) {
            Q_EMIT approvalRequested(f.value(QStringLiteral("requestId")).toInt(),
                                     approvalFromJson(f.value(QStringLiteral("request")).toObject()));
        } else if (type == QLatin1String("rikugan_question")) {
            QStringList opts;
            for (const QJsonValue& v : f.value(QStringLiteral("options")).toArray()) opts << v.toString();
            Q_EMIT questionAsked(f.value(QStringLiteral("requestId")).toInt(), f.value(QStringLiteral("question")).toString(), opts);
        } else if (type == QLatin1String("rikugan_decision")) {
            Q_EMIT decisionRequested(f.value(QStringLiteral("requestId")).toInt(), f.value(QStringLiteral("kind")).toString(),
                                     f.value(QStringLiteral("payload")).toObject());
        } else if (type == QLatin1String("rikugan_card_cancelled")) {
            Q_EMIT cardCancelled(f.value(QStringLiteral("requestId")).toInt());
        } else if (type == QLatin1String("rikugan_fork_done")) {
            if (forkDone_) {
                auto cb = std::move(forkDone_);
                forkDone_ = nullptr;
                cb(f.value(QStringLiteral("response")).toObject());
            }
        } else if (type == QLatin1String("notice") && f.value(QStringLiteral("level")).toString() == QLatin1String("title")) {
            // omc named the session after its first exchange (as omc and omp do)
            adoptTitle(f.value(QStringLiteral("message")).toString());
        } else {
            // the agent took (or still holds) the steered messages
            if (type == QLatin1String("queue_update")) {
                QStringList held;
                for (const QJsonValue& v : f.value(QStringLiteral("steering")).toArray()) held << v.toString();
                if (held != steering_) {
                    steering_ = held;
                    queueUpdated();
                }
            }
            if (type == QLatin1String("turn_end") && session_) {
                std::weak_ptr<EventQueue> wq = events_;
                session_->refreshStateAsync([wq](const QJsonObject& st) {
                    if (auto q = wq.lock()) q->push(dump(QJsonObject{{"type", "rikugan_state"}, {"state", st}}));
                });
            }
            Q_EMIT frame(f);
        }
    }
}

void ChatSession::onState(const QJsonObject& st) {
    // a title omc gave the session while this chat was closed
    adoptTitle(st.value(QStringLiteral("sessionName")).toString());
    Q_EMIT stateChanged(st);
}

void ChatSession::adoptTitle(const QString& title) {
    const QString t = title.trimmed();
    if (t.isEmpty() || entry_.titleByUser || t == entry_.title) return;
    entry_.title = t;
    persistEntry();
}

}  // namespace rk
