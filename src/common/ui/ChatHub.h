#pragma once
// The live chats of every open database, outside any panel: switching Binary Ninja tabs or closing IDA's dock
// detaches the panel, while the chats keep running, with their undo history, grants and queue. A database's chats
// end when the host closes that database (closeDatabase) or the plugin unloads (closeAll). UI thread only.
#include <memory>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace rk {

class ChatSession;
struct ChatEntry;
struct DbContext;

namespace ui {

class TranscriptModel;

struct LiveChat {
    ChatSession* session = nullptr;
    TranscriptModel* model = nullptr;
    QJsonObject state;  // the newest get_state
};

class ChatHub : public QObject {
    Q_OBJECT
public:
    static ChatHub& instance();

    // A database's live chats by chat id (empty for an unknown id).
    QHash<QString, LiveChat>& chats(const QString& dbId);
    // Whether its saved chats were loaded once already.
    bool restored(const QString& dbId) const { return restored_.contains(dbId); }
    void setRestored(const QString& dbId) { restored_.insert(dbId, true); }
    // The chat the panel showed last for that database ("" for the welcome).
    QString current(const QString& dbId) const { return current_.value(dbId); }
    void setCurrent(const QString& dbId, const QString& chatId) { current_.insert(dbId, chatId); }

    // A chat of `db` (its session, transcript model and queued-command runner).
    LiveChat& add(const std::shared_ptr<DbContext>& db, const ChatEntry& entry);
    // One chat goes (deleted chat): its session closes.
    void remove(const QString& dbId, const QString& chatId);
    // The host closed the database: its chats close now.
    void closeDatabase(const QString& dbId);
    // Plugin unload: every chat closes now.
    void closeAll();

Q_SIGNALS:
    // A chat's state changed (the panel showing it updates its chrome).
    void stateChanged(const QString& chatId);

private:
    explicit ChatHub(QObject* parent);
    QHash<QString, QHash<QString, LiveChat>> chats_;  // dbId -> chatId -> chat
    QHash<QString, bool> restored_;
    QHash<QString, QString> current_;
};

}  // namespace ui
}  // namespace rk
