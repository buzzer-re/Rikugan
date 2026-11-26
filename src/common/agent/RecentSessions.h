#pragma once
// Chats that can be reopened while no database is open. Each sessions/<db_id>/ keeps a database.json with the
// host and the files it came from, so the panel can open the file again and continue the chat.
#include <optional>

#include <QList>
#include <QString>

namespace rk {

struct DatabaseLocation {
    QString host;   // Host::hostName()
    QString path;   // the database (.bndb, .i64) or the binary when it was never saved
    QString input;  // the original binary
};

// Writes <sessionsDir>/database.json when it changed.
void recordDatabaseLocation(const QString& sessionsDir, const DatabaseLocation& location);

// The user's answer to using the host's MCP server with this database (database.json). nullopt: never asked.
std::optional<bool> hostMcpConsent(const QString& sessionsDir);
void setHostMcpConsent(const QString& sessionsDir, bool allowed);

struct RecentChat {
    QString dbId;
    QString chatId;
    QString title;
    QString file;  // what to open: the recorded database, or the binary when the database is gone
    qint64 lastActiveAt = 0;
};

// The newest chats of `host` with a saved transcript and a file that still exists.
QList<RecentChat> recentChats(const QString& host, int limit);

// The panel opens `file` to continue a chat of `dbId`. A file that carries no Rikugan id yet (a binary that was
// never saved as a database) takes this one, so its chats come back. One expectation at a time.
void expectSession(const QString& file, const QString& dbId);
QString takeExpectedSession(const QString& file);  // the id for `file`, consumed; empty when none
void clearExpectedSession();

}  // namespace rk
