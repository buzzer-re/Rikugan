#pragma once
// The databases open in this host process, for the chat tools that reach beyond the chat's own one:
// list_databases, open_database (a file on disk as one more database, without touching the current view) and
// ask_database (a subagent that answers about another database with that database's tools). Thread-safe.
#include <functional>
#include <memory>
#include <vector>

#include <QString>

#include "common/tools/Tool.h"

namespace rk {

struct DbContext;

namespace databases {

// DbContext::create registers every database; the registry holds them weakly.
void add(const std::shared_ptr<DbContext>& db);
// The open ones, the oldest first.
std::vector<std::shared_ptr<DbContext>> all();
// By database id, or by the path of its database or input file.
std::shared_ptr<DbContext> find(const QString& idOrPath);
// Opened by open_database (no tab in the host): Rikugan keeps them alive until close().
bool openedByRikugan(const QString& dbId);

// How the host opens a file as an extra database: on a tool worker; may take long (analysis). Null result plus
// *error when it cannot. Hosts that cannot hold two databases (IDA) set none.
using Loader = std::function<std::shared_ptr<DbContext>(const QString& path, const std::shared_ptr<CancelToken>& token,
                                                        QString* error)>;
void setLoader(Loader loader);
bool canLoad();
// Loads `path` (or returns the database already open for it) and keeps it alive.
std::shared_ptr<DbContext> load(const QString& path, const std::shared_ptr<CancelToken>& token, QString* error);
// Releases a database load() opened; false for any other.
bool close(const QString& dbId);
// Plugin shutdown: releases every loaded database.
void releaseAll();

}  // namespace databases
}  // namespace rk
