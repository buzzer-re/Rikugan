#include "common/omc/LoginFlow.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>

#include "common/config/Paths.h"
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

}  // namespace

LoginFlow::LoginFlow(QObject* parent) : QObject(parent) {}

LoginFlow::~LoginFlow() { closeSession(); }

void LoginFlow::start(const QString& providerId) {
    closeSession();
    const int gen = ++generation_;
    running_ = true;
    sawUrlWithoutLaunch_ = false;
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
        const QString launch = f.value(QStringLiteral("launchUrl")).toString();
        sawUrlWithoutLaunch_ = launch.isEmpty();
        Q_EMIT openUrl(f.value(QStringLiteral("url")).toString(), launch, f.value(QStringLiteral("instructions")).toString());
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

void LoginFlow::cancel() {
    if (!running_) return;
    finish(false, QStringLiteral("cancelled"));
}

void LoginFlow::finish(bool ok, const QString& error) {
    running_ = false;
    inputId_.clear();
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
