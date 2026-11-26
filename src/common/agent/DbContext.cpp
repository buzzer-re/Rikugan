#include "common/agent/DbContext.h"

#include <QFileInfo>

#include "common/agent/ChatTools.h"
#include "common/agent/Databases.h"
#include "common/agent/RecentSessions.h"
#include "common/config/Paths.h"
#include "common/util/Files.h"

namespace rk {

std::shared_ptr<DbContext> DbContext::create(std::unique_ptr<Host> host) {
    auto db = std::make_shared<DbContext>();
    db->host = std::move(host);
    db->dbId = db->host->databaseInstanceId();
    db->host->registerTools(db->registry);
    registerChatTools(db->registry);
    db->chats = std::make_unique<ChatIndex>(db->sessionsDir());
    ensureDir(db->scratchDir());
    db->recordLocation();
    databases::add(db);
    return db;
}

void DbContext::recordLocation() const {
    recordDatabaseLocation(sessionsDir(), DatabaseLocation{host->hostName(), host->databasePath(), host->inputPath()});
}

QString DbContext::binaryDir() const {
    const QString p = host->databasePath().isEmpty() ? host->inputPath() : host->databasePath();
    return QFileInfo(p).absolutePath();
}

QString DbContext::memoryPath() const { return binaryDir() + QStringLiteral("/RIKUGAN.md"); }
QString DbContext::notesDir() const { return binaryDir() + QStringLiteral("/notes"); }
QString DbContext::scratchDir() const { return paths::scratchDir(dbId); }
QString DbContext::artifactsDir() const { return paths::artifactsDir(dbId); }
QString DbContext::sessionsDir() const { return paths::sessionsDir(dbId); }

}  // namespace rk
