#pragma once
// sessions/<db_id>/chats.json: the chats of one database and the omc session file each continues.
#include <QDateTime>
#include <QList>
#include <QString>

namespace rk {

struct ChatEntry {
    QString chatId;
    QString sessionFile;  // empty until omc wrote the session (lazily, after the first answer)
    QString title;            // the first prompt until omc names the session, then omc's title
    bool titleByUser = false; // renamed by the user: omc's titles no longer replace it
    qint64 createdAt = 0;     // ms since epoch
    qint64 lastActiveAt = 0;
    QString forkedFrom;       // chat id
};

class ChatIndex {
public:
    explicit ChatIndex(QString dir);  // sessions/<db_id>
    const QList<ChatEntry>& chats() const { return chats_; }
    const ChatEntry* find(const QString& chatId) const;
    ChatEntry create(const QString& forkedFrom = {});
    void update(const ChatEntry& e);  // by chatId; saves
    void remove(const QString& chatId);
    QString dir() const { return dir_; }

    static QString titleFrom(const QString& firstUserMessage);  // first line, cut at 60 chars

private:
    void load();
    void save() const;
    QString dir_;
    QList<ChatEntry> chats_;
};

}  // namespace rk
