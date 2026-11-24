#pragma once
// The databases open in this host process, for the chat tools that reach beyond the chat's own one:
// list_databases, open_database (a file on disk in a new tab of the host) and ask_database (a subagent that answers
// about another database with that database's tools). Thread-safe.
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
// How the host opens a file in a tab of its window (a view of the user's, with its own chats): on a tool worker.
// It waits until the tab shows (a large file loads for a while; analysis goes on after) or `token` is cancelled.
// Hosts with one database per window (IDA) set none.
using TabOpener = std::function<std::shared_ptr<DbContext>(const QString& path, const std::shared_ptr<CancelToken>& token,
                                                           QString* error)>;
void setTabOpener(TabOpener opener);
bool canOpenTab();
// Opens `path` in a host tab (or returns the database already open for it).
std::shared_ptr<DbContext> openTab(const QString& path, const std::shared_ptr<CancelToken>& token, QString* error);

}  // namespace databases
}  // namespace rk
