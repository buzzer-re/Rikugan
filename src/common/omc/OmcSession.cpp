#include "common/omc/OmcSession.h"

#include <chrono>
#include <cstring>
#include <vector>

#include <QJsonDocument>

#include <omc.h>

#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

QJsonObject parseObject(const char* json, size_t len) {
    QJsonParseError err{};
    QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromRawData(json, qsizetype(len)), &err);
    return d.isObject() ? d.object() : QJsonObject();
}

// "type" of a frame without parsing it: omc writes it first ({"type":"..."}); anything else is parsed
std::string peekType(const char* json, size_t len) {
    static constexpr char kPrefix[] = "{\"type\":\"";
    constexpr size_t n = sizeof kPrefix - 1;
    if (len > n && std::memcmp(json, kPrefix, n) == 0) {
        const char* p = json + n;
        const char* end = static_cast<const char*>(std::memchr(p, '"', len - n));
        if (end) return std::string(p, size_t(end - p));
    }
    return parseObject(json, len).value(QStringLiteral("type")).toString().toStdString();
}

QByteArray utf8(const QString& s) { return s.toUtf8(); }

}  // namespace

std::shared_ptr<OmcSession> OmcSession::open(OmcOptions opts, std::shared_ptr<OmcListener> listener) {
    std::shared_ptr<OmcSession> s(new OmcSession(std::move(opts), std::move(listener)));
    // the worker keeps the session alive until it has freed the omc session
    spawnThread([s] { s->run(); });
    return s;
}

OmcSession::OmcSession(OmcOptions opts, std::shared_ptr<OmcListener> listener)
    : opts_(std::move(opts)), listener_(std::move(listener)) {}

OmcSession::~OmcSession() = default;

std::shared_ptr<OmcListener> OmcSession::listener() const {
    std::lock_guard<std::mutex> g(mu_);
    return listener_;
}

void OmcSession::post(std::function<void(OmcSession&)> job) {
    if (closing_.load()) return;
    {
        std::lock_guard<std::mutex> g(jobsMu_);
        jobs_.push_back(std::move(job));
    }
    jobsCv_.notify_one();
}

void OmcSession::openOnWorker() {
    QByteArray cwd = utf8(opts_.cwd), sdir = utf8(opts_.sessionDir), resume = utf8(opts_.resume),
               model = utf8(opts_.model), thinking = utf8(opts_.thinking), sys = utf8(opts_.systemPrompt);
    std::vector<QByteArray> toolNames;
    std::vector<const char*> tools;
    for (const QString& t : opts_.builtinTools) toolNames.push_back(utf8(t));
    for (const QByteArray& t : toolNames) tools.push_back(t.constData());
    tools.push_back(nullptr);
    auto opt = [](const QByteArray& b) { return b.isEmpty() ? nullptr : b.constData(); };

    if (!opts_.cwd.isEmpty()) ensureDir(opts_.cwd);
    if (!opts_.sessionDir.isEmpty()) ensureDir(opts_.sessionDir);
    omc_options_t o{};
    o.struct_size = sizeof o;
    o.cwd = opt(cwd);
    o.model = opt(model);
    o.thinking = opt(thinking);
    o.system_prompt = opt(sys);
    o.session_dir = opt(sdir);
    o.resume = opt(resume);
    o.no_session = opts_.noSession;
    o.no_tools = opts_.noTools;
    o.tools = opts_.noTools ? nullptr : tools.data();
    o.no_context_files = true;
    o.no_rules = true;
    o.no_skills = opts_.noSkills;
    o.no_mcp = opts_.noMcp;
    char* err = nullptr;
    omc_session* created = nullptr;
    {
        // libomc's session setup rewrites process-wide settings (app_create -> settings_set_runtime_quiet):
        // two sessions starting at once on different workers corrupt them
        static std::mutex createMu;
        std::lock_guard<std::mutex> g(createMu);
        created = omc_session_new(&o, &OmcSession::onEvent, this, &err);
    }
    {
        std::unique_lock<std::shared_mutex> g(sMu_);
        s_ = created;
    }
    if (!s_) {
        openError_ = QString::fromUtf8(err ? err : "omc refused to start a session");
        omc_free(err);
        openFailed_ = true;
        if (auto l = listener()) l->onOpenFailed(openError_);
        return;
    }
    omc_set_result_filter(s_, &OmcSession::onFilterResult, [](void*, char* text) { delete[] text; }, this);
    QJsonArray events;
    for (const char* e : {"agent_start", "turn_start", "turn_end", "message_start", "message_update", "message_end",
                          "tool_execution_start", "tool_execution_update", "tool_execution_end",
                          "auto_compaction_start", "auto_compaction_end", "manual_compaction_start",
                          "manual_compaction_end", "auto_retry_start", "auto_retry_end", "queue_update",
                          "model_changed", "thinking_level_changed", "notice", "goal_updated"})
        events.append(QString::fromLatin1(e));
    request(QJsonObject{{"type", "set_event_filter"}, {"events", events}, {"messageUpdates", "delta"}});
    if (!opts_.hostTools.isEmpty()) request(QJsonObject{{"type", "set_host_tools"}, {"tools", opts_.hostTools}});
    if (!opts_.title.isEmpty()) request(QJsonObject{{"type", "set_session_name"}, {"name", opts_.title}});
    refreshState();
}

void OmcSession::run() {
    openOnWorker();
    for (;;) {
        std::function<void(OmcSession&)> job;
        {
            std::unique_lock<std::mutex> lk(jobsMu_);
            jobsCv_.wait(lk, [&] { return closing_.load() || !jobs_.empty(); });
            if (closing_.load()) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        // also when the session failed to open: prompt()/request() then report the error, so callers waiting on a
        // job's completion are always answered
        job(*this);
    }
    omc_session* s = nullptr;
    {
        // waits for control senders in flight; later ones see closing_ and drop their frame
        std::unique_lock<std::shared_mutex> g(sMu_);
        s = s_;
        s_ = nullptr;
    }
    if (s) omc_session_free(s);  // aborts a running turn, waits for it, persists
    {
        std::lock_guard<std::mutex> g(mu_);
        listener_.reset();
        for (auto& [id, w] : waiters_) w->done = true;
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> g(doneMu_);
        finished_ = true;
    }
    doneCv_.notify_all();
}

void OmcSession::onEvent(void* ud, const char* json) { static_cast<OmcSession*>(ud)->handleEvent(json); }

char* OmcSession::onFilterResult(void* ud, const char* tool, const char* text) {
    const std::shared_ptr<OmcListener> l = static_cast<OmcSession*>(ud)->listener();
    if (!l) return nullptr;
    const std::optional<QString> out = l->filterToolResult(QString::fromUtf8(tool ? tool : ""), QString::fromUtf8(text));
    if (!out) return nullptr;
    const QByteArray u = out->toUtf8();
    char* copy = new char[size_t(u.size()) + 1];
    std::memcpy(copy, u.constData(), size_t(u.size()) + 1);
    return copy;
}

void OmcSession::handleEvent(const char* json) {
    const size_t len = std::strlen(json);
    const std::string type = peekType(json, len);
    if (type == "response" || type == "prompt_result") {
        QJsonObject f = parseObject(json, len);
        std::string id = f.value(QStringLiteral("id")).toString().toStdString();
        if (type == "prompt_result") id = "prompt:" + id;
        std::lock_guard<std::mutex> g(mu_);
        auto it = waiters_.find(id);
        if (it != waiters_.end()) {
            it->second->frame = std::move(f);
            it->second->done = true;
            cv_.notify_all();
        }
        if (type == "response") return;
    } else if (type == "ready") {
        return;
    } else if (type == "session_settled") {
        std::lock_guard<std::mutex> g(mu_);
        ++settled_;
        cv_.notify_all();
    } else if (type == "host_tool_call") {
        if (auto l = listener()) l->onHostToolCall(parseObject(json, len));
        return;
    } else if (type == "host_tool_cancel") {
        if (auto l = listener()) l->onHostToolCancel(parseObject(json, len).value(QStringLiteral("targetId")).toString());
        return;
    }
    if (auto l = listener()) l->onFrame(type, std::string(json, len));
}

QJsonObject OmcSession::request(QJsonObject cmd, int timeoutMs) {
    {
        std::shared_lock<std::shared_mutex> g(sMu_);
        if (!s_) return QJsonObject{{"success", false}, {"error", openError_.isEmpty() ? "session closed" : openError_}};
    }
    const std::string id = "rk-" + std::to_string(nextId_++);
    cmd.insert(QStringLiteral("id"), QString::fromStdString(id));
    auto w = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> g(mu_);
        waiters_[id] = w;
    }
    {
        std::shared_lock<std::shared_mutex> g(sMu_);
        if (s_) omc_send(s_, compactJson(cmd).constData());
    }
    std::unique_lock<std::mutex> lk(mu_);
    // closing ends the wait: a long request (a login waits up to 30 min) must not keep the worker from freeing the
    // session, which is what stops omc's work behind it
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return w->done || closing_.load(); });
    waiters_.erase(id);
    if (closing_.load() && !w->done) return QJsonObject{{"success", false}, {"error", "session closed"}};
    if (!ok || w->frame.isEmpty())
        return QJsonObject{{"success", false}, {"error", QStringLiteral("no response to %1").arg(cmd.value("type").toString())}};
    return w->frame;
}

OmcSession::PromptOutcome OmcSession::prompt(const QString& text, const QJsonArray& images) {
    PromptOutcome out;
    if (!s_ || closing_.load()) {  // a job queued behind close() (a mode's next step) starts no new turn
        out.status = QStringLiteral("error");
        out.error = openError_.isEmpty() ? QStringLiteral("session closed") : openError_;
        return out;
    }
    busy_ = true;
    uint64_t settledBefore = 0;
    const std::string id = "rk-" + std::to_string(nextId_++);
    auto result = std::make_shared<Waiter>();
    auto response = std::make_shared<Waiter>();
    {
        std::lock_guard<std::mutex> g(mu_);
        waiters_["prompt:" + id] = result;
        waiters_[id] = response;
        settledBefore = settled_;
    }
    QJsonObject cmd{{"type", "prompt"}, {"id", QString::fromStdString(id)}, {"message", text}};
    if (!images.isEmpty()) cmd.insert(QStringLiteral("images"), images);
    omc_send(s_, compactJson(cmd).constData());
    // a builtin command that ran without the agent (/compact): its response, sent once it finished, says
    // agentInvoked false and no prompt_result follows
    auto ranLocally = [&] {
        return response->done &&
               response->frame.value(QStringLiteral("data")).toObject().value(QStringLiteral("agentInvoked")) == QJsonValue(false);
    };
    bool local = false;
    {
        std::unique_lock<std::mutex> lk(mu_);
        // a refused prompt gets a failed response and never a prompt_result
        cv_.wait(lk, [&] {
            return result->done || (response->done && !response->frame.value(QStringLiteral("success")).toBool(true)) ||
                   ranLocally() || closing_.load();
        });
        local = !result->done && ranLocally() && response->frame.value(QStringLiteral("success")).toBool(true);
        waiters_.erase(id);
        waiters_.erase("prompt:" + id);
        // goal mode: the first prompt_result comes early (sessionSettled:false) and omc keeps running continuation
        // turns; the run is over at session_settled
        if (result->done && !result->frame.value(QStringLiteral("sessionSettled")).toBool(true))
            cv_.wait(lk, [&] { return settled_ > settledBefore || closing_.load(); });
    }
    if (local) {
        out.status = QStringLiteral("completed");
    } else if (!result->done || result->frame.isEmpty()) {
        out.status = QStringLiteral("error");
        out.error = response->frame.value(QStringLiteral("error")).toString(QStringLiteral("the prompt was refused"));
    } else {
        out.status = result->frame.value(QStringLiteral("status")).toString(QStringLiteral("completed"));
        out.error = result->frame.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        const QJsonObject r = request(QJsonObject{{"type", "get_last_assistant_text"}});
        out.lastText = r.value(QStringLiteral("data")).toObject().value(QStringLiteral("text")).toString();
    }
    refreshState();
    busy_ = false;
    return out;
}

QJsonObject OmcSession::refreshState() {
    const QJsonObject r = request(QJsonObject{{"type", "get_state"}});
    const QJsonObject d = r.value(QStringLiteral("data")).toObject();
    if (!d.isEmpty()) {
        std::lock_guard<std::mutex> g(mu_);
        state_ = d;
    }
    return d;
}

void OmcSession::sendControl(const QJsonObject& frame) {
    const QByteArray text = compactJson(frame);
    std::shared_lock<std::shared_mutex> g(sMu_);
    if (s_) omc_send(s_, text.constData());
}

void OmcSession::refreshStateAsync(std::function<void(const QJsonObject&)> done) {
    std::shared_ptr<OmcSession> self = shared_from_this();
    spawnThread([self, done = std::move(done)] {
        const QJsonObject st = self->refreshState();
        if (done) done(st);
    });
}

void OmcSession::abort() {
    std::shared_lock<std::shared_mutex> g(sMu_);
    if (s_) omc_abort(s_);
}

void OmcSession::close() {
    if (closing_.exchange(true)) return;
    abort();
    jobsCv_.notify_all();
    {
        std::lock_guard<std::mutex> g(mu_);
        cv_.notify_all();  // a prompt waiting for a goal run to settle
    }
}

bool OmcSession::closeAndWait(int ms) {
    close();
    std::unique_lock<std::mutex> lk(doneMu_);
    return doneCv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return finished_.load(); });
}

QString OmcSession::sessionFile() const {
    std::lock_guard<std::mutex> g(mu_);
    return state_.value(QStringLiteral("sessionFile")).toString();
}

QJsonObject OmcSession::cachedState() const {
    std::lock_guard<std::mutex> g(mu_);
    return state_;
}

}  // namespace rk
