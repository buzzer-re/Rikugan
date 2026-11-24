#include "common/omc/AuthSync.h"

#include <mutex>

#include <QFileInfo>
#include <QJsonDocument>

#include "common/config/Paths.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AgentHome.h"
#include "common/omc/ServiceSession.h"
#include "common/util/Files.h"
#include "common/util/Log.h"

namespace rk {

namespace {

double expires(const QJsonObject& e) { return e.value(QStringLiteral("expires")).toDouble(0); }
bool isOauth(const QJsonObject& e) { return e.value(QStringLiteral("type")).toString() == QLatin1String("oauth"); }

// one writer of Rikugan's auth.json at a time
std::mutex& authMutex() {
    static std::mutex mu;
    return mu;
}

QString rikuganAuthPath() { return paths::omcAgentDir() + QStringLiteral("/auth.json"); }

}  // namespace

AuthMergeResult mergeAuth(QJsonObject& r, QJsonObject& o) {
    AuthMergeResult res;
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject oe = it.value().toObject();
        if (!r.contains(it.key())) {
            r.insert(it.key(), oe);
            res.rChanged = true;
            continue;
        }
        const QJsonObject re = r.value(it.key()).toObject();
        if (!isOauth(re) || !isOauth(oe) || re == oe) continue;
        if (expires(oe) > expires(re)) {
            r.insert(it.key(), oe);
            res.rChanged = true;
        } else if (expires(re) > expires(oe)) {
            it.value() = re;
            res.oChanged = true;
        }
    }
    return res;
}

bool AuthSync::syncFiles(const QString& rPath, const QString& oPath) {
    bool rOk = false, oOk = false;
    QJsonObject r = readJsonObject(rPath, &rOk);
    QJsonObject o = readJsonObject(oPath, &oOk);
    if (!oOk) {
        if (QFileInfo::exists(oPath)) logOnce(QStringLiteral("authsync-o"), QStringLiteral("Rikugan: cannot read %1").arg(oPath));
        return false;
    }
    if (!rOk && QFileInfo::exists(rPath)) {
        logOnce(QStringLiteral("authsync-r"), QStringLiteral("Rikugan: cannot read %1").arg(rPath));
        return false;
    }
    const AuthMergeResult m = mergeAuth(r, o);
    if (m.oChanged) writeFileAtomic(oPath, QJsonDocument(o).toJson(QJsonDocument::Indented), 0600);
    if (m.rChanged) writeFileAtomic(rPath, QJsonDocument(r).toJson(QJsonDocument::Indented), 0600);
    return m.rChanged;
}

void AuthSync::run() {
    if (RikuganConfig::instance().useOmcLogins) syncNow();
}

bool AuthSync::syncNow() {
    std::lock_guard<std::mutex> g(authMutex());
    const QString oPath = AgentHome::standaloneAgentDir() + QStringLiteral("/auth.json");
    if (!syncFiles(rikuganAuthPath(), oPath)) return false;
    ServiceSession::reloadCredentials();
    return true;
}

QString storedCredentialType(const QString& provider) {
    std::lock_guard<std::mutex> g(authMutex());
    bool ok = false;
    return readJsonObject(rikuganAuthPath(), &ok).value(provider).toObject().value(QStringLiteral("type")).toString();
}

QString storeApiKey(const QString& provider, const QString& key) {
    if (provider.isEmpty() || key.trimmed().isEmpty()) return QStringLiteral("Enter the API key first.");
    {
        std::lock_guard<std::mutex> g(authMutex());
        const QString path = rikuganAuthPath();
        bool ok = false;
        QJsonObject auth = readJsonObject(path, &ok);
        if (!ok && QFileInfo::exists(path)) return QStringLiteral("Cannot read %1; the key was not saved.").arg(path);
        auth.insert(provider, QJsonObject{{"type", "api_key"}, {"key", key.trimmed()}});
        QString err;
        if (!writeFileAtomic(path, QJsonDocument(auth).toJson(QJsonDocument::Indented), 0600, &err))
            return QStringLiteral("Could not save the key: %1").arg(err);
    }
    ServiceSession::reloadCredentials();
    return {};
}

}  // namespace rk
