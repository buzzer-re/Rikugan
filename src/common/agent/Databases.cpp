#include "common/agent/Databases.h"

#include <algorithm>
#include <mutex>

#include <QFileInfo>

#include "common/agent/DbContext.h"

namespace rk::databases {

namespace {

std::mutex g_mu;
std::vector<std::weak_ptr<DbContext>> g_all;
std::vector<std::shared_ptr<DbContext>> g_loaded;  // open_database: Rikugan's own, no tab
Loader g_loader;
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

bool openedByRikugan(const QString& dbId) {
    std::lock_guard<std::mutex> g(g_mu);
    return std::any_of(g_loaded.begin(), g_loaded.end(), [&](const auto& db) { return db->dbId == dbId; });
}

void setLoader(Loader loader) {
    std::lock_guard<std::mutex> g(g_mu);
    g_loader = std::move(loader);
}

bool canLoad() {
    std::lock_guard<std::mutex> g(g_mu);
    return bool(g_loader);
}

std::shared_ptr<DbContext> load(const QString& path, const std::shared_ptr<CancelToken>& token, QString* error) {
    if (auto open = find(path)) return open;
    Loader loader;
    {
        std::lock_guard<std::mutex> g(g_mu);
        loader = g_loader;
    }
    if (!loader) {
        if (error) *error = QStringLiteral("this host cannot open a second database");
        return nullptr;
    }
    if (!QFileInfo(path).isFile()) {
        if (error) *error = QStringLiteral("no such file: %1").arg(path);
        return nullptr;
    }
    std::shared_ptr<DbContext> db = loader(path, token, error);
    if (!db) return nullptr;
    std::lock_guard<std::mutex> g(g_mu);
    g_loaded.push_back(db);
    return db;
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
    if (auto open = find(path); open && !openedByRikugan(open->dbId)) return open;
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

bool close(const QString& dbId) {
    std::shared_ptr<DbContext> released;  // destroyed outside the lock
    std::lock_guard<std::mutex> g(g_mu);
    auto it = std::find_if(g_loaded.begin(), g_loaded.end(), [&](const auto& db) { return db->dbId == dbId; });
    if (it == g_loaded.end()) return false;
    released = std::move(*it);
    g_loaded.erase(it);
    return true;
}

void releaseAll() {
    std::vector<std::shared_ptr<DbContext>> released;
    {
        std::lock_guard<std::mutex> g(g_mu);
        released.swap(g_loaded);
        g_loader = nullptr;
        g_tabOpener = nullptr;
    }
}

}  // namespace rk::databases
