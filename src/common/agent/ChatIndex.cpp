#include "common/agent/ChatIndex.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "common/util/Files.h"

namespace rk {

namespace {
QString indexPath(const QString& dir) { return dir + QStringLiteral("/chats.json"); }
}  // namespace

ChatIndex::ChatIndex(QString dir) : dir_(std::move(dir)) { load(); }

void ChatIndex::load() {
    chats_.clear();
    bool ok = false;
    const QJsonDocument d = QJsonDocument::fromJson(readFile(indexPath(dir_), &ok));
    for (const QJsonValue& v : d.array()) {
        const QJsonObject o = v.toObject();
        ChatEntry e;
        e.chatId = o.value(QStringLiteral("chat_id")).toString();
        if (e.chatId.isEmpty()) continue;
        e.sessionFile = o.value(QStringLiteral("session_file")).toString();
        e.title = o.value(QStringLiteral("title")).toString();
        e.titleByUser = o.value(QStringLiteral("title_by_user")).toBool();
        e.createdAt = qint64(o.value(QStringLiteral("created_at")).toDouble());
        e.lastActiveAt = qint64(o.value(QStringLiteral("last_active_at")).toDouble());
        e.forkedFrom = o.value(QStringLiteral("forked_from")).toString();
        chats_ << e;
    }
}

void ChatIndex::save() const {
    QJsonArray a;
    for (const ChatEntry& e : chats_) {
        QJsonObject o{{"chat_id", e.chatId},
                      {"session_file", e.sessionFile},
                      {"title", e.title},
                      {"created_at", double(e.createdAt)},
                      {"last_active_at", double(e.lastActiveAt)}};
        if (!e.forkedFrom.isEmpty()) o.insert(QStringLiteral("forked_from"), e.forkedFrom);
        if (e.titleByUser) o.insert(QStringLiteral("title_by_user"), true);
        a.append(o);
    }
    writeFileAtomic(indexPath(dir_), QJsonDocument(a).toJson(QJsonDocument::Indented));
}

const ChatEntry* ChatIndex::find(const QString& chatId) const {
    for (const ChatEntry& e : chats_)
        if (e.chatId == chatId) return &e;
    return nullptr;
}

ChatEntry ChatIndex::create(const QString& forkedFrom) {
    ChatEntry e;
    e.chatId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    e.createdAt = e.lastActiveAt = QDateTime::currentMSecsSinceEpoch();
    e.forkedFrom = forkedFrom;
    chats_ << e;
    save();
    return e;
}

void ChatIndex::update(const ChatEntry& e) {
    for (ChatEntry& x : chats_)
        if (x.chatId == e.chatId) {
            x = e;
            save();
            return;
        }
}

void ChatIndex::remove(const QString& chatId) {
    for (qsizetype i = 0; i < chats_.size(); ++i)
        if (chats_[i].chatId == chatId) {
            chats_.removeAt(i);
            save();
            return;
        }
}

QString ChatIndex::titleFrom(const QString& firstUserMessage) {
    QString t = firstUserMessage.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (t.size() > 60) t = t.left(57) + QStringLiteral("...");
    return t;
}

}  // namespace rk
