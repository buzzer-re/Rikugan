#include "common/omc/LoginFlow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QUrlQuery>

#include "common/config/Paths.h"
#include "common/mcp/McpClient.h"
#include "common/omc/AgentHome.h"
#include "common/omc/AuthSync.h"
#include "common/omc/OmcSession.h"
#include "common/omc/ServiceSession.h"
#include "common/omc/UserErrors.h"
#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

const QString LoginFlow::kTerminalOnly =
    QStringLiteral("This provider's sign-in needs omc's terminal login; save an API key for it instead.");

namespace {

// Hands the login's dialogs to the flow on the UI thread.
class LoginListener : public OmcListener {
public:
    explicit LoginListener(QPointer<LoginFlow> flow, std::function<void(LoginFlow*, const QJsonObject&)> fn)
        : flow_(std::move(flow)), fn_(std::move(fn)) {}
    void onFrame(std::string_view type, const std::string& json) override {
        if (type != "extension_ui_request") return;
        const QJsonObject f = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
        QPointer<LoginFlow> flow = flow_;
        auto fn = fn_;
        QMetaObject::invokeMethod(
            uiAnchor(), [flow, fn, f] {
                if (flow) fn(flow.data(), f);
            },
            Qt::QueuedConnection);
    }
    void onHostToolCall(const QJsonObject&) override {}
    void onHostToolCancel(const QString&) override {}

private:
    QPointer<LoginFlow> flow_;
    std::function<void(LoginFlow*, const QJsonObject&)> fn_;
};

bool needsTerminal(const QString& error) {
    return error.contains(QLatin1String("not supported in RPC mode")) || error.contains(QLatin1String("terminal UI"));
}

// The authorize URL's redirect_uri when it is a callback server on this machine, else an invalid URL.
QUrl localCallback(const QUrl& authorize) {
    QUrl cb(QUrlQuery(authorize).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
    const QString host = cb.host();
    if (cb.scheme() != QLatin1String("http") || cb.port() <= 0 ||
        (host != QLatin1String("localhost") && host != QLatin1String("127.0.0.1")))
        return {};
    cb.setHost(QStringLiteral("127.0.0.1"));  // omc listens on IPv4 only; "localhost" may resolve to ::1 first
    return cb;
}

}  // namespace

LoginFlow::LoginFlow(QObject* parent) : QObject(parent) {}

LoginFlow::~LoginFlow() { closeSession(); }

void LoginFlow::start(const QString& providerId) {
    closeSession();
    const int gen = ++generation_;
    running_ = true;
    sawUrlWithoutLaunch_ = false;
    callback_.clear();
    inputId_.clear();
    OmcOptions o;
    o.cwd = paths::scratchDir(QStringLiteral("_login"));
    o.model = AgentHome::serviceModel();
    o.noSession = o.noTools = o.noSkills = o.noMcp = true;
    session_ = OmcSession::open(o, std::make_shared<LoginListener>(QPointer<LoginFlow>(this),
                                                                   [](LoginFlow* f, const QJsonObject& r) { f->onRequest(r); }));
    QPointer<LoginFlow> self(this);
    session_->post([self, gen, providerId](OmcSession& s) {
        // a login waits on the user and the browser: omc's own deadline (5 min for a browser callback) ends it
        const QJsonObject r = s.request(QJsonObject{{"type", "login"}, {"providerId", providerId}}, 30 * 60 * 1000);
        const bool ok = r.value(QStringLiteral("success")).toBool();
        if (ok) AuthSync::run();
        QMetaObject::invokeMethod(
            uiAnchor(), [self, gen, ok, r] {
                if (!self || self->generation_ != gen || !self->running_) return;
                if (ok) {
                    ServiceSession::reloadCredentials();
                    self->finish(true, {});
                    return;
                }
                const QString e = r.value(QStringLiteral("error")).toString();
                self->finish(false, needsTerminal(e) ? kTerminalOnly : userFacingError(e.isEmpty() ? QStringLiteral("Login failed") : e));
            },
            Qt::QueuedConnection);
    });
}

void LoginFlow::onRequest(const QJsonObject& f) {
    if (!running_) return;
    const QString method = f.value(QStringLiteral("method")).toString();
    if (method == QLatin1String("open_url")) {
        const QString url = f.value(QStringLiteral("url")).toString();
        const QString launch = f.value(QStringLiteral("launchUrl")).toString();
        sawUrlWithoutLaunch_ = launch.isEmpty();
        callback_ = localCallback(QUrl(url));
        Q_EMIT openUrl(url, launch, f.value(QStringLiteral("instructions")).toString());
    } else if (method == QLatin1String("notify")) {
        const QString msg = f.value(QStringLiteral("message")).toString();
        // a pasted authorization code can't be delivered over RPC: the login would only time out
        if (sawUrlWithoutLaunch_ && msg.contains(QLatin1String("pasted authorization code"), Qt::CaseInsensitive)) {
            finish(false, kTerminalOnly);
            return;
        }
        Q_EMIT progress(msg);
    } else if (method == QLatin1String("input")) {
        inputId_ = f.value(QStringLiteral("id")).toString();
        Q_EMIT inputRequested(f.value(QStringLiteral("title")).toString(), f.value(QStringLiteral("placeholder")).toString());
    } else if (method == QLatin1String("cancel")) {
        if (f.value(QStringLiteral("targetId")).toString() == inputId_) inputId_.clear();
    } else if (session_) {
        // a login asks only for text: anything else is declined so omc does not wait on it
        session_->sendControl(QJsonObject{{"type", "extension_ui_response"},
                                          {"id", f.value(QStringLiteral("id")).toString()},
                                          {"cancelled", true}});
    }
}

void LoginFlow::answerInput(const QString& value) {
    if (!session_ || inputId_.isEmpty()) return;
    session_->sendControl(QJsonObject{{"type", "extension_ui_response"}, {"id", inputId_}, {"value", value}});
    inputId_.clear();
}

void LoginFlow::submitCode(const QString& text) {
    if (!acceptsPastedCode()) return;
    const QString t = text.trimmed();
    QString code, state;
    if (t.contains(QLatin1String("code="))) {  // the browser's address, or just its query
        QString query = t.contains(QLatin1Char('?')) ? t.mid(t.indexOf(QLatin1Char('?')) + 1) : t;
        query = query.left(query.indexOf(QLatin1Char('#')) < 0 ? query.size() : query.indexOf(QLatin1Char('#')));
        const QUrlQuery q(query);
        code = q.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
        state = q.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
    } else if (t.contains(QLatin1Char('#'))) {  // "code#state", as Claude's page shows it
        code = t.left(t.indexOf(QLatin1Char('#')));
        state = t.mid(t.indexOf(QLatin1Char('#')) + 1);
    } else {
        code = t;
    }
    if (code.isEmpty()) {
        Q_EMIT pastedCodeFailed(tr("That has no authorization code in it. Paste the whole address from the browser's address bar."));
        return;
    }
    QUrl target = callback_;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("code"), code);
    // without a state omc checks none (a bare code); an empty "state=" would not match its own
    if (!state.isEmpty()) q.addQueryItem(QStringLiteral("state"), state);
    target.setQuery(q);
    const QString url = target.toString(QUrl::FullyEncoded);
    QPointer<LoginFlow> self(this);
    const int gen = generation_;
    // the callback answers at once, then omc exchanges the code; finished() comes from the login's own reply
    spawnThread([self, gen, url] {
        QString error;
        try {
            const int status = httpGet(url, 10000);
            if (status != 200) error = tr("The login did not accept it (HTTP %1).").arg(status);
        } catch (const ToolError& e) {
            error = tr("The login is no longer waiting for a code (%1).").arg(e.message());
        }
        if (error.isEmpty()) return;
        QMetaObject::invokeMethod(
            uiAnchor(), [self, gen, error] {
                if (self && self->generation_ == gen && self->running_) Q_EMIT self->pastedCodeFailed(error);
            },
            Qt::QueuedConnection);
    });
}

void LoginFlow::cancel() {
    if (!running_) return;
    finish(false, QStringLiteral("cancelled"));
}

void LoginFlow::finish(bool ok, const QString& error) {
    running_ = false;
    inputId_.clear();
    callback_.clear();
    closeSession();
    Q_EMIT finished(ok, error);
}

void LoginFlow::closeSession() {
    // close() never blocks: freeing the session on its worker aborts the login thread inside omc
    if (session_) session_->close();
    session_.reset();
}

void fetchLoginProviders(QObject* guard, std::function<void(const QList<LoginProviderInfo>&, const QString& error)> done) {
    QPointer<QObject> g(guard);
    ServiceSession::get()->post([g, done = std::move(done)](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "get_login_providers"}});
        QList<LoginProviderInfo> out;
        QString error;
        if (!r.value(QStringLiteral("success")).toBool()) {
            error = userFacingError(r.value(QStringLiteral("error")).toString());
        } else {
            const QJsonObject auth = readJsonObject(paths::omcAgentDir() + QStringLiteral("/auth.json"));
            for (const QJsonValue& v : r.value(QStringLiteral("data")).toObject().value(QStringLiteral("providers")).toArray()) {
                const QJsonObject p = v.toObject();
                LoginProviderInfo i;
                i.id = p.value(QStringLiteral("id")).toString();
                i.name = p.value(QStringLiteral("name")).toString();
                i.authenticated = p.value(QStringLiteral("authenticated")).toBool();
                const QString type = auth.value(i.id).toObject().value(QStringLiteral("type")).toString();
                i.source = type == QLatin1String("oauth")      ? QStringLiteral("login")
                           : !type.isEmpty()                    ? QStringLiteral("api key")
                           : i.authenticated                    ? QStringLiteral("env")
                                                                : QString();
                out << i;
            }
        }
        QMetaObject::invokeMethod(
            uiAnchor(), [g, done, out, error] {
                if (g) done(out, error);
            },
            Qt::QueuedConnection);
    });
}

}  // namespace rk
