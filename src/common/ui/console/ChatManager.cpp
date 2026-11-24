#include "common/ui/console/ChatManager.h"

#include <algorithm>

#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/agent/ChatIndex.h"
#include "common/agent/ChatSession.h"
#include "common/agent/Databases.h"
#include "common/agent/DbContext.h"
#include "common/ui/ChatHub.h"
#include "common/ui/Theme.h"
#include "common/ui/console/Palette.h"
#include "common/ui/transcript/TranscriptModel.h"

namespace rk::ui {

namespace {

constexpr int kWidth = 280;

QString chatKey(const QString& dbId, const QString& chatId) { return QStringLiteral("chat:") + dbId + QLatin1Char(':') + chatId; }

}  // namespace

QString relativeTime(qint64 ms) {
    if (!ms) return {};
    const qint64 s = (QDateTime::currentMSecsSinceEpoch() - ms) / 1000;
    if (s < 60) return QStringLiteral("just now");
    if (s < 3600) return QStringLiteral("%1m ago").arg(s / 60);
    if (s < 86400) return QStringLiteral("%1h ago").arg(s / 3600);
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("MMM d"));
}

ChatManager::ChatManager(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("rk_manager"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(kWidth);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);
    title_ = new QLabel(tr("Chats"), this);
    lay->addWidget(title_);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Filter chats\u2026"));
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    list_ = new PaletteList(this);
    list_->setMaxRows(10000);
    list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    lay->addWidget(list_, 1);
    open_ = new QPushButton(tr("Open a binary\u2026"), this);
    open_->setCursor(Qt::PointingHandCursor);
    open_->hide();
    lay->addWidget(open_);

    refresh_.setSingleShot(true);
    refresh_.setInterval(0);
    connect(&refresh_, &QTimer::timeout, this, &ChatManager::refresh);
    connect(&ChatHub::instance(), &ChatHub::chatsChanged, this, &ChatManager::scheduleRefresh);
    connect(&ChatHub::instance(), &ChatHub::databasesChanged, this, &ChatManager::scheduleRefresh);
    connect(&ChatHub::instance(), &ChatHub::stateChanged, this, &ChatManager::scheduleRefresh);
    connect(search_, &QLineEdit::textChanged, list_, &PaletteList::setQuery);
    connect(open_, &QPushButton::clicked, this, &ChatManager::openRequested);
    connect(list_, &PaletteList::activated, this, [this](const QString& key) {
        if (!key.startsWith(QLatin1String("chat:"))) return;
        const QString rest = key.mid(5);
        const qsizetype colon = rest.indexOf(QLatin1Char(':'));
        if (colon > 0) Q_EMIT chatActivated(rest.left(colon), rest.mid(colon + 1));
    });
    connect(list_, &PaletteList::sectionActionActivated, this, [this](const QString& section) {
        const QString db = sectionDb_.value(section);
        if (!db.isEmpty()) Q_EMIT newChatRequested(db);
    });
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &ChatManager::restyle);
    restyle();
}

void ChatManager::setCurrent(const QString& dbId, const QString& chatId) {
    if (dbId == dbId_ && chatId == chatId_) return;
    dbId_ = dbId;
    chatId_ = chatId;
    scheduleRefresh();
}

void ChatManager::setCanOpen(bool canOpen) { open_->setVisible(canOpen); }

void ChatManager::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    refresh();
}

void ChatManager::scheduleRefresh() {
    if (isVisible()) refresh_.start();
}

void ChatManager::refresh() {
    refresh_.stop();
    QList<PaletteEntry> entries;
    sectionDb_.clear();
    for (const std::shared_ptr<DbContext>& db : databases::all()) {
        const QString path = db->host->databasePath().isEmpty() ? db->host->inputPath() : db->host->databasePath();
        QString section = QFileInfo(path).fileName();
        if (section.isEmpty()) section = db->dbId;
        // two files of one name: the headings stay apart
        for (int n = 2; sectionDb_.contains(section); ++n) section = QFileInfo(path).fileName() + QStringLiteral(" (%1)").arg(n);
        sectionDb_.insert(section, db->dbId);

        const QHash<QString, LiveChat>& live = ChatHub::instance().chats(db->dbId);
        QList<ChatEntry> chats = db->chats->chats();
        // a chat created and never used is only worth a row while it is the one shown
        chats.removeIf([&](const ChatEntry& e) {
            return e.sessionFile.isEmpty() && e.title.isEmpty() && !(db->dbId == dbId_ && e.chatId == chatId_);
        });
        std::sort(chats.begin(), chats.end(), [](const ChatEntry& a, const ChatEntry& b) {
            return std::max(a.lastActiveAt, a.createdAt) > std::max(b.lastActiveAt, b.createdAt);
        });
        bool first = true;
        auto add = [&](PaletteEntry e) {
            e.section = section;
            if (first) {
                e.sectionAction = tr("+ New chat");
                if (db->dbId == dbId_) e.sectionNote = tr("shown");
            }
            first = false;
            entries << e;
        };
        if (chats.isEmpty()) {
            // a heading needs a row under it: the new chat it offers
            PaletteEntry e;
            e.key = QStringLiteral("new:") + db->dbId;
            e.title = tr("No chats yet");
            e.enabled = false;
            add(e);
            continue;
        }
        for (const ChatEntry& c : std::as_const(chats)) {
            PaletteEntry e;
            e.key = chatKey(db->dbId, c.chatId);
            e.title = c.title.isEmpty() ? tr("Untitled") : c.title;
            e.detail = relativeTime(c.lastActiveAt ? c.lastActiveAt : c.createdAt);
            e.checked = db->dbId == dbId_ && c.chatId == chatId_;
            const LiveChat l = live.value(c.chatId);
            if (l.model && l.model->awaitingUser()) e.hint = tr("\u25C6 waiting");
            else if (l.session && l.session->busy()) e.hint = tr("\u25CF running");
            add(e);
        }
    }
    list_->setEntries(entries);
    list_->setQuery(search_->text());
}

void ChatManager::restyle() {
    const Theme& t = theme();
    title_->setFont(t.uiBold);
    setStyleSheet(QStringLiteral("#rk_manager { background: %1; border-left: 1px solid %2; }"
                                 "#rk_manager QLabel { color: %3; }"
                                 "#rk_manager QLineEdit { background: %4; color: %3; border: 1px solid %2; border-radius: 5px; "
                                 "padding: 3px 6px; }")
                      .arg(t.bg2.name(), t.line.name(), t.fg.name(), t.bg.name()));
}

}  // namespace rk::ui
