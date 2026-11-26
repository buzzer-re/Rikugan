#include "common/agent/Databases.h"

#include <algorithm>
#include <mutex>

#include <QFileInfo>

#include "common/agent/DbContext.h"

namespace rk::databases {

namespace {

std::mutex g_mu;
std::vector<std::weak_ptr<DbContext>> g_all;
TabOpener g_tabOpener;

QString canonical(const QString& path) {
    const QFileInfo fi(path);
    const QString c = fi.canonicalFilePath();
    return c.isEmpty() ? fi.absoluteFilePath() : c;
}

bool matches(const DbContext& db, const QString& idOrPath) {
    if (db.dbId == idOrPath) return true;
    const QString want = canonical(idOrPath);
    for (const QString& p : {db.host->databasePath(), db.host->inputPath()})
        if (!p.isEmpty() && canonical(p) == want) return true;
    return false;
}

}  // namespace

void add(const std::shared_ptr<DbContext>& db) {
    std::lock_guard<std::mutex> g(g_mu);
    g_all.erase(std::remove_if(g_all.begin(), g_all.end(), [](const std::weak_ptr<DbContext>& w) { return w.expired(); }),
                g_all.end());
    g_all.push_back(db);
}

std::vector<std::shared_ptr<DbContext>> all() {
    std::lock_guard<std::mutex> g(g_mu);
    std::vector<std::shared_ptr<DbContext>> out;
    for (const std::weak_ptr<DbContext>& w : g_all)
        if (auto db = w.lock()) out.push_back(db);
    return out;
}

std::shared_ptr<DbContext> find(const QString& idOrPath) {
    if (idOrPath.trimmed().isEmpty()) return nullptr;
    for (const std::shared_ptr<DbContext>& db : all())
        if (matches(*db, idOrPath.trimmed())) return db;
    return nullptr;
}

void setTabOpener(TabOpener opener) {
    std::lock_guard<std::mutex> g(g_mu);
    g_tabOpener = std::move(opener);
}

bool canOpenTab() {
    std::lock_guard<std::mutex> g(g_mu);
    return bool(g_tabOpener);
}

std::shared_ptr<DbContext> openTab(const QString& path, const std::shared_ptr<CancelToken>& token, QString* error) {
    if (auto open = find(path)) return open;
    TabOpener opener;
    {
        std::lock_guard<std::mutex> g(g_mu);
        opener = g_tabOpener;
    }
    if (!opener) {
        if (error) *error = QStringLiteral("this host cannot open another tab");
        return nullptr;
    }
    if (!QFileInfo(path).isFile()) {
        if (error) *error = QStringLiteral("no such file: %1").arg(path);
        return nullptr;
    }
    return opener(path, token, error);
}

}  // namespace rk::databases
