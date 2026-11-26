#include "common/ui/ChatHub.h"

#include <QPointer>

#include "common/agent/ChatSession.h"
#include "common/agent/CommandRouter.h"
#include "common/agent/DbContext.h"
#include "common/ui/transcript/TranscriptModel.h"
#include "common/util/ModuleLifetime.h"

namespace rk::ui {

ChatHub::ChatHub(QObject* parent) : QObject(parent) {}

ChatHub& ChatHub::instance() {
    // owned by the module anchor: unload deletes it with the anchor, after closeAll
    static QPointer<ChatHub> hub;
    if (!hub) hub = new ChatHub(uiAnchor());
    return *hub;
}

QHash<QString, LiveChat>& ChatHub::chats(const QString& dbId) { return chats_[dbId]; }

LiveChat& ChatHub::add(const std::shared_ptr<DbContext>& db, const ChatEntry& entry) {
    auto* s = new ChatSession(db, entry, this);
    auto* m = new TranscriptModel(this);
    m->bind(s);
    // a command that waited for a run: shown and routed when its turn comes, as if typed then
    s->setCommandRunner([s, m](const QString& text) {
        m->addLocalUser(text);
        CommandRouter::submit(*s, text);
    });
    const QString dbId = db->dbId, chatId = entry.chatId;
    connect(s, &ChatSession::stateChanged, this, [this, dbId, chatId](const QJsonObject& st) {
        auto it = chats_[dbId].find(chatId);
        if (it == chats_[dbId].end()) return;
        it->state = st;
        Q_EMIT stateChanged(chatId);
    });
    connect(s, &ChatSession::busyChanged, this, &ChatHub::chatsChanged);
    connect(s, &ChatSession::entryChanged, this, &ChatHub::chatsChanged);
    LiveChat& c = chats_[dbId][chatId];
    c = LiveChat{s, m, {}};
    Q_EMIT chatsChanged();
    return c;
}

void ChatHub::remove(const QString& dbId, const QString& chatId) {
    const LiveChat c = chats_[dbId].take(chatId);
    if (current_.value(dbId) == chatId) current_.remove(dbId);
    if (c.model) c.model->deleteLater();
    if (c.session) c.session->deleteLater();
    Q_EMIT chatsChanged();
}

void ChatHub::closeDatabase(const QString& dbId) {
    const QHash<QString, LiveChat> gone = chats_.take(dbId);
    restored_.remove(dbId);
    current_.remove(dbId);
    // now, with their pending events: a session must not outlive its database
    for (const LiveChat& c : gone) {
        delete c.model;
        delete c.session;
    }
    Q_EMIT chatsChanged();
}

void ChatHub::settingsChanged() {
    for (auto& db : chats_)
        for (LiveChat& c : db)
            if (c.session) c.session->settingsChanged();
}

void ChatHub::closeAll() {
    const QStringList ids = chats_.keys();
    for (const QString& id : ids) closeDatabase(id);
}

}  // namespace rk::ui
