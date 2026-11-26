#include "common/omc/ServiceSession.h"

#include <mutex>

#include "common/config/Paths.h"
#include "common/omc/AgentHome.h"
#include "common/util/Log.h"

namespace rk {

namespace {

std::mutex g_mu;
std::shared_ptr<OmcSession> g_session;
ServiceSession::FrameHandler g_handler;

class ServiceListener : public OmcListener {
public:
    void onFrame(std::string_view type, const std::string& json) override {
        ServiceSession::FrameHandler h;
        {
            std::lock_guard<std::mutex> g(g_mu);
            h = g_handler;
        }
        if (h) h(type, json);
    }
    void onHostToolCall(const QJsonObject&) override {}
    void onHostToolCancel(const QString&) override {}
};

}  // namespace

std::shared_ptr<OmcSession> ServiceSession::get() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_session || g_session->closed()) {
        OmcOptions o;
        o.cwd = paths::scratchDir(QStringLiteral("_service"));
        o.model = AgentHome::serviceModel();  // works with no credentials yet (see AgentHome::serviceModel)
        o.noSession = o.noTools = o.noSkills = o.noMcp = true;
        g_session = OmcSession::open(o, std::make_shared<ServiceListener>());
    }
    return g_session;
}

void ServiceSession::setFrameHandler(FrameHandler h) {
    std::lock_guard<std::mutex> g(g_mu);
    g_handler = std::move(h);
}

void ServiceSession::reloadCredentials() {
    // omc re-reads auth.json while it lists a provider's accounts; it refuses the request (and skips the reload)
    // without a providerId, and any id will do
    get()->post([](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "get_logout_accounts"}, {"providerId", "anthropic"}});
        if (!r.value(QStringLiteral("success")).toBool())
            log(QStringLiteral("Rikugan: omc did not reload the logins: %1").arg(r.value(QStringLiteral("error")).toString()));
    });
}

void ServiceSession::shutdown(int waitMs) {
    std::shared_ptr<OmcSession> s;
    {
        std::lock_guard<std::mutex> g(g_mu);
        s.swap(g_session);
        g_handler = nullptr;
    }
    if (s) s->closeAndWait(waitMs);
}

}  // namespace rk
