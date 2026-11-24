#include "common/agent/RecentSessions.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include "common/agent/ChatIndex.h"
#include "common/config/Paths.h"
#include "common/util/Files.h"

namespace rk {

namespace {

QString locationPath(const QString& dir) { return dir + QStringLiteral("/database.json"); }

QString samePathKey(const QString& file) {
    const QFileInfo fi(file);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

std::mutex g_expectMu;
QString g_expectFile;  // samePathKey
QString g_expectId;

}  // namespace

void recordDatabaseLocation(const QString& sessionsDir, const DatabaseLocation& location) {
    const QJsonObject old = readJsonObject(locationPath(sessionsDir));
    QJsonObject o = old;
    o.insert(QStringLiteral("host"), location.host);
    o.insert(QStringLiteral("path"), location.path);
    o.insert(QStringLiteral("input"), location.input);
    if (o == old) return;
    writeFileAtomic(locationPath(sessionsDir), compactJson(o));
}

std::optional<bool> hostMcpConsent(const QString& sessionsDir) {
    const QJsonValue v = readJsonObject(locationPath(sessionsDir)).value(QStringLiteral("hostMcp"));
    if (!v.isBool()) return std::nullopt;
    return v.toBool();
}

void setHostMcpConsent(const QString& sessionsDir, bool allowed) {
    QJsonObject o = readJsonObject(locationPath(sessionsDir));
    if (o.value(QStringLiteral("hostMcp")) == QJsonValue(allowed)) return;
    o.insert(QStringLiteral("hostMcp"), allowed);
    QDir().mkpath(sessionsDir);
    writeFileAtomic(locationPath(sessionsDir), compactJson(o));
}

QList<RecentChat> recentChats(const QString& host, int limit) {
    QList<RecentChat> out;
    const QDir root(paths::sessionsRoot());
    for (const QString& id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = root.filePath(id);
        const QJsonObject loc = readJsonObject(locationPath(dir));
        if (loc.value(QLatin1String("host")).toString() != host) continue;
        QString file = loc.value(QLatin1String("path")).toString();
        if (file.isEmpty() || !QFileInfo(file).isFile()) file = loc.value(QLatin1String("input")).toString();
        if (file.isEmpty() || !QFileInfo(file).isFile()) continue;
        const ChatIndex index(dir);
        for (const ChatEntry& e : index.chats()) {
            if (e.sessionFile.isEmpty()) continue;
            out.append(RecentChat{id, e.chatId, e.title, file, e.lastActiveAt});
        }
    }
    std::sort(out.begin(), out.end(), [](const RecentChat& a, const RecentChat& b) { return a.lastActiveAt > b.lastActiveAt; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void expectSession(const QString& file, const QString& dbId) {
    std::lock_guard<std::mutex> g(g_expectMu);
    g_expectFile = samePathKey(file);
    g_expectId = dbId;
}

QString takeExpectedSession(const QString& file) {
    std::lock_guard<std::mutex> g(g_expectMu);
    if (g_expectId.isEmpty() || g_expectFile != samePathKey(file)) return {};
    g_expectFile.clear();
    return std::exchange(g_expectId, QString());
}

void clearExpectedSession() {
    std::lock_guard<std::mutex> g(g_expectMu);
    g_expectFile.clear();
    g_expectId.clear();
}

}  // namespace rk
