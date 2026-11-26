#pragma once
// One open database: its host, tools, result cache and chats.
#include <atomic>
#include <memory>

#include <QString>

#include "common/agent/ChatIndex.h"
#include "common/host/Host.h"
#include "common/tools/ResultCache.h"
#include "common/tools/ToolRegistry.h"

namespace rk {

struct DbContext {
    std::unique_ptr<Host> host;
    QString dbId;
    ToolRegistry registry;
    ResultCache cache;
    std::unique_ptr<ChatIndex> chats;
    // The host's MCP tools replace Rikugan's analysis tools in this database's chats (HostMcp). Set on the UI
    // thread, read by tool visibility.
    std::atomic<bool> hostMcp{false};

    // UI thread. Registers the host's tools and Rikugan's chat tools.
    static std::shared_ptr<DbContext> create(std::unique_ptr<Host> host);
    // Saves where the database lives (RecentSessions), for reopening its chats. UI thread: at create and after
    // the host saves the database somewhere new.
    void recordLocation() const;

    QString memoryPath() const;    // <dir of the database>/RIKUGAN.md
    QString notesDir() const;      // <dir of the database>/notes
    QString binaryDir() const;     // directory of the database
    QString scratchDir() const;    // session cwd
    QString artifactsDir() const;
    QString sessionsDir() const;
};

}  // namespace rk
