#include "common/ui/PanelCore.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "common/agent/ChatExport.h"
#include "common/agent/ChatSession.h"
#include "common/agent/CommandRouter.h"
#include "common/agent/Databases.h"
#include "common/agent/DbContext.h"
#include "common/agent/RecentSessions.h"
#include "common/agent/Skills.h"
#include "common/agent/Subagents.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/OmcSession.h"
#include "common/tools/ToolDispatcher.h"
#include "common/ui/BinarySummary.h"
#include "common/ui/ChatHub.h"
#include "common/ui/Theme.h"
#include "common/ui/WelcomeView.h"
#include "common/ui/console/ActivityStrip.h"
#include "common/ui/console/AgentsPanel.h"
#include "common/ui/console/Composer.h"
#include "common/ui/console/ChatManager.h"
#include "common/ui/console/ConsoleHeader.h"
#include "common/ui/console/HostMcpBar.h"
#include "common/ui/console/GetStarted.h"
#include "common/ui/console/LoginSheet.h"
#include "common/ui/console/ModelPicker.h"
#include "common/ui/console/Palette.h"
#include "common/ui/console/StatusLine.h"
#include "common/ui/settings/SettingsDialog.h"
#include "common/ui/tools/ToolsPanel.h"
#include "common/ui/transcript/TranscriptModel.h"
#include "common/ui/transcript/TranscriptView.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

using namespace rk::ui;

namespace {

constexpr int kThemeCheckDelayMs = 120;
constexpr int kPaletteWidth = 560;
constexpr int kRecentChats = 6;
constexpr int kSetupPage = 2;  // pages: chat, bulk renamer, connect a model
const QString kOpenKey = QStringLiteral("!open");

QString modelOf(const QJsonObject& state) {
    const QJsonObject m = state.value(QLatin1String("model")).toObject();
    const QString provider = m.value(QLatin1String("provider")).toString();
    const QString id = m.value(QLatin1String("id")).toString();
    if (id.isEmpty()) return {};
    return provider.isEmpty() ? id : provider + QLatin1Char('/') + id;
}

}  // namespace

struct PanelCore::Impl {
    PanelCore* q;
    std::shared_ptr<DbContext> db;

    using Chat = LiveChat;
    // the database's live chats: they belong to the ChatHub, the panel only shows them
    QHash<QString, Chat>& chats() const { return ChatHub::instance().chats(db ? db->dbId : QString()); }
    QString current;  // chat id; empty: no chat (the welcome screen)

    ConsoleHeader* header = nullptr;
    ChatManager* manager = nullptr;  // the full-window panel's list of every database's chats
    ActivityStrip* strip = nullptr;
    QStackedWidget* pages = nullptr;
    QSplitter* split = nullptr;
    TranscriptView* view = nullptr;
    TranscriptModel* emptyModel = nullptr;
    WelcomeView* welcome = nullptr;
    AgentsPanel* agents = nullptr;
    Composer* composer = nullptr;
    StatusLine* status = nullptr;
    ToolsPanel* tools = nullptr;
    GetStarted* setup = nullptr;
    bool setupDismissed = false;  // "Not now": it comes back only when a prompt needs a model
    LoginSheet* login = nullptr;
    ModelPicker* picker = nullptr;
    PalettePopup* palette = nullptr;
    PalettePopup* switcher = nullptr;

    BinarySummary binary;
    std::optional<quint64> address;
    int binaryGeneration = 0;
    QTimer themeTimer;

    // no database open: what the welcome offers
    QString openerHost;
    PanelCore::FileOpener opener;
    QList<RecentChat> recent;
    QString pendingDb, pendingChat;  // the recent chat being opened

    // a subagent's conversation shown like a chat ("Open chat"): its frames from the registry, live
    struct AgentFeed {
        std::mutex mu;
        std::vector<std::string> pending;
        bool posted = false;
    };
    QString agentId;
    TranscriptModel* agentModel = nullptr;
    int agentObserver = 0;
    std::shared_ptr<AgentFeed> agentFeed;
    QWidget* agentBar = nullptr;
    HostMcpBar* mcpBar = nullptr;
    QLabel* agentInfo = nullptr;
    QPushButton* agentStop = nullptr;
    std::shared_ptr<std::atomic<bool>> agentRefreshPosted = std::make_shared<std::atomic<bool>>(false);
    int registryObserver = 0;

    explicit Impl(PanelCore* p) : q(p) {}

    // ---- lookups ----
    ChatSession* chat() const { return chats().value(current).session; }
    TranscriptModel* model() const {
        TranscriptModel* m = chats().value(current).model;
        return m ? m : emptyModel;
    }
    static QString label(const ChatEntry& e) { return e.title.isEmpty() ? QStringLiteral("Untitled") : e.title; }

    QString binaryName() const {
        if (!db) return {};
        const QString p = db->host->databasePath().isEmpty() ? db->host->inputPath() : db->host->databasePath();
        return QFileInfo(p).fileName();
    }

    QString modelKey() const {
        const QString m = modelOf(chats().value(current).state);
        return m.isEmpty() ? RikuganConfig::instance().model : m;
    }

    // ---- chrome for the current chat ----
    void syncChrome() {
        manager->setCurrent(db ? db->dbId : QString(), current);
        if (pages->currentIndex() == kSetupPage) return;  // its own title; the chat's chrome comes back when it closes
        if (!agentId.isEmpty()) return syncAgentChrome();
        ChatSession* c = chat();
        TranscriptModel* m = model();
        const bool running = c && c->busy();
        const bool buttons = m->awaitingButtons();
        composer->setInputEnabled(db && !buttons);
        const QString name = binaryName();
        composer->setPlaceholder(!db        ? QObject::tr("Open a binary to start.")
                                 : buttons  ? QObject::tr("Answer the card above to continue.")
                                 : running ? QObject::tr("Rikugan is working\u2026 Enter queues a follow-up.")
                                           : QObject::tr("Ask about %1\u2026   / for commands").arg(name.isEmpty() ? QObject::tr("this binary") : name));
        composer->setRunning(running);
        header->setTitle(c ? label(c->entry()) : QObject::tr("New chat"));
        header->setChatActionsEnabled(c != nullptr);
        syncActivity();
    }

    void syncActivity() {
        const ChatActivity& a = model()->activity();
        strip->setActivity(a);
        status->setActivity(a);
        status->setModelFallback(RikuganConfig::instance().model);
        composer->setMode(a.mode.isEmpty() ? QString() : a.mode.left(1).toUpper() + a.mode.mid(1));
        ChatSession* c = chat();
        header->setUndoAvailable(c && c->context()->mutations->size() > 0);
        const QString key = modelKey();
        QString thinking = a.thinking.isEmpty() ? RikuganConfig::instance().thinking : a.thinking;
        QString text = key.isEmpty() ? QObject::tr("Choose a model") : picker->displayName(key);
        if (!thinking.isEmpty() && thinking != QLatin1String("off")) text += QStringLiteral(" \u00b7 ") + thinking;
        composer->setModelLabel(text, key.isEmpty() ? QObject::tr("Choose a model (Ctrl+M)") : key + QObject::tr(" (Ctrl+M)"));
    }

    // ---- welcome ----
    void refreshWelcome() {
        if (!db) {
            showNoDatabase();
            return;
        }
        QStringList slugs;
        for (const SkillInfo& s : listSkills()) slugs << s.slug;
        welcome->setBinarySummary(binary);
        welcome->setSuggestions(buildSuggestions(slugs, address));
    }

    void showNoDatabase() {
        recent = opener ? recentChats(openerHost, kRecentChats) : QList<RecentChat>{};
        QList<Suggestion> rows;
        if (opener) rows.append({QString(), QObject::tr("Open a binary\u2026"), kOpenKey, {}});
        for (qsizetype i = 0; i < recent.size(); ++i) {
            const RecentChat& r = recent[i];
            rows.append({QString(), r.title.isEmpty() ? QStringLiteral("Untitled") : r.title, QString::number(i),
                         QFileInfo(r.file).fileName() + QStringLiteral(" \u00b7 ") + relativeTime(r.lastActiveAt)});
        }
        const QString facts = !opener          ? QObject::tr("Open a database to start.")
                              : recent.isEmpty() ? QObject::tr("Open a binary to start.")
                                                 : QObject::tr("Open a binary, or continue a recent chat.");
        welcome->setNoDatabase(facts, rows);
    }

    // A row of the no-database welcome: the file dialog, or a recent chat's file (its chat opens with it).
    void openFromWelcome(const QString& key) {
        if (!opener || db) return;
        if (key == kOpenKey) return openFile();
        bool ok = false;
        const qsizetype i = key.toLongLong(&ok);
        if (!ok || i < 0 || i >= recent.size()) return;
        const RecentChat r = recent[i];
        pendingDb = r.dbId;
        pendingChat = r.chatId;
        expectSession(r.file, r.dbId);
        if (opener(r.file, r.dbId)) return;
        clearExpectedSession();
        pendingDb.clear();
        pendingChat.clear();
    }

    // The welcome's facts, read off the UI thread (the host reads them on its main thread through onMain).
    void loadBinarySummary() {
        binary = BinarySummary{};
        refreshWelcome();
        if (!db) return;
        const int gen = ++binaryGeneration;
        std::shared_ptr<DbContext> d = db;
        QPointer<PanelCore> self(q);
        spawnThread([d, self, gen] {
            QString info;
            std::optional<qint64> strings;
            try {
                info = onMain(*d->host, MainAccess::Read, [&] { return d->host->binaryInfo(); });
                if (d->registry.find(QStringLiteral("list_strings")))
                    strings = parseTotalCount(ToolDispatcher::runDirect(*d->host, d->registry, nullptr, QStringLiteral("list_strings"),
                                                                        QJsonObject{{"offset", 0}, {"limit", 1}}));
            } catch (const std::exception& e) {
                log(QStringLiteral("Rikugan: binary summary failed: %1").arg(QString::fromUtf8(e.what())));
            }
            const BinarySummary summary = buildBinarySummary(info, strings);
            QMetaObject::invokeMethod(
                uiAnchor(),
                [self, gen, summary] {
                    if (!self || self->d->binaryGeneration != gen) return;
                    self->d->binary = summary;
                    self->d->refreshWelcome();
                },
                Qt::QueuedConnection);
        });
    }

    // ---- chats ----
    // A new chat of the database, created in the hub and shown by this panel.
    ChatSession* addChat(const ChatEntry& entry) {
        const Chat& c = ChatHub::instance().add(db, entry);
        attachChat(entry.chatId, c);
        return c.session;
    }

    // This panel follows a hub chat (it shows whichever is current).
    void attachChat(const QString& id, const Chat& c) {
        ChatSession* s = c.session;
        TranscriptModel* m = c.model;
        QObject::connect(s, &ChatSession::busyChanged, q, [this, id](bool) {
            if (id == current) syncChrome();
        });
        QObject::connect(s, &ChatSession::queueChanged, q, [this, id](const QStringList& queued) {
            if (id == current) composer->setQueue(queued);
        });
        QObject::connect(s, &ChatSession::entryChanged, q, [this, id](const ChatEntry& e) {
            if (id == current) header->setTitle(label(e));
        });
        QObject::connect(m, &TranscriptModel::activityChanged, q, [this, id] {
            if (id == current) syncActivity();
        });
        QObject::connect(m, &TranscriptModel::awaitingChanged, q, [this, id] {
            if (id == current) syncChrome();
        });
        QObject::connect(m, &TranscriptModel::uiAction, q, [this, id](const QString& action, const QString& arg) {
            if (id == current) uiAction(action, arg);
        });
    }

    void selectChat(const QString& id) {
        if (!chats().contains(id)) return;
        closeAgentChat();
        current = id;
        ChatHub::instance().setCurrent(db->dbId, id);
        const Chat& c = chats()[id];
        view->setTranscriptModel(c.model);
        agents->setChat(c.session);
        composer->setQueue(c.session->queued());
        // a chat with history: open its session, which replays the transcript
        if (!c.session->entry().sessionFile.isEmpty()) c.session->ensureOpen();
        pages->setCurrentIndex(0);
        syncChrome();
    }

    void showWelcome() {
        closeAgentChat();
        current.clear();
        if (db) ChatHub::instance().setCurrent(db->dbId, QString());
        view->setTranscriptModel(emptyModel);
        agents->setChat(nullptr);
        composer->setQueue({});
        syncChrome();
    }

    ChatSession* newChat() {
        if (!db) return nullptr;
        ChatSession* s = addChat(db->chats->create());
        selectChat(s->chatId());
        composer->focusInput();
        return s;
    }

    // A chat from the manager, of any open database: the panel turns to that database and shows it (empty chatId: a
    // new chat there).
    void showChatOf(const QString& dbId, const QString& chatId) {
        const std::shared_ptr<DbContext> target = databases::find(dbId);
        if (!target) return;
        if (target != db) {
            pendingDb = dbId;
            pendingChat = chatId;  // restoreChats opens it
            q->setDatabase(target);
        } else if (!chatId.isEmpty()) {
            if (!chats().contains(chatId))
                if (const ChatEntry* e = db->chats->find(chatId)) addChat(*e);
            selectChat(chatId);
        }
        if (chatId.isEmpty()) newChat();
    }

    // "Open a binary…" from the manager.
    void openFile() {
        if (!opener) return;
        const QString file = QFileDialog::getOpenFileName(q, QObject::tr("Open a binary"));
        if (!file.isEmpty()) opener(file, {});
    }

    // A deleted chat: it closes for good.
    void removeChatObjects(const QString& id) {
        if (current == id) view->setTranscriptModel(emptyModel);
        ChatHub::instance().remove(db->dbId, id);
    }

    // The panel turns to another database: this one's chats keep running in the hub, the panel stops following them.
    void detachChats() {
        closeAgentChat();
        for (const Chat& c : std::as_const(chats())) {
            QObject::disconnect(c.session, nullptr, q, nullptr);
            QObject::disconnect(c.model, nullptr, q, nullptr);
        }
        view->setTranscriptModel(emptyModel);
        agents->setChat(nullptr);
        current.clear();
    }

    // The database's chats: the live ones when the hub has them (back from another tab), else the saved ones
    // (the latest opens unless dont_auto_load_chats), or the recent chat the database was opened for.
    void restoreChats() {
        const RikuganConfig& cfg = RikuganConfig::instance();
        QString wanted;
        if (db) {
            if (pendingDb == db->dbId) wanted = pendingChat;
            pendingDb.clear();
            pendingChat.clear();
        }
        if (db) {
            // seen before (back from another tab): its live chats, as they are now
            const bool known = ChatHub::instance().restored(db->dbId);
            ChatHub::instance().setRestored(db->dbId);
            if (known) {
                for (auto it = chats().cbegin(); it != chats().cend(); ++it) attachChat(it.key(), it.value());
                if (!wanted.isEmpty() && !chats().contains(wanted))
                    if (const ChatEntry* e = db->chats->find(wanted)) addChat(*e);
                const QString last = wanted.isEmpty() ? ChatHub::instance().current(db->dbId) : wanted;
                if (chats().contains(last)) selectChat(last);
                else showWelcome();
                return;
            }
        }
        if (!db || (!cfg.restoreSessionsOnStart && wanted.isEmpty())) {
            showWelcome();
            return;
        }
        QList<ChatEntry> entries = db->chats->chats();
        // chats created and never used leave empty rows behind
        for (const ChatEntry& e : std::as_const(entries))
            if (e.sessionFile.isEmpty() && e.title.isEmpty()) db->chats->remove(e.chatId);
        entries = db->chats->chats();
        std::sort(entries.begin(), entries.end(), [](const ChatEntry& a, const ChatEntry& b) { return a.lastActiveAt > b.lastActiveAt; });
        for (const ChatEntry& e : std::as_const(entries)) addChat(e);
        if (chats().contains(wanted)) selectChat(wanted);
        else if (entries.isEmpty() || cfg.dontAutoLoadChats) showWelcome();
        else selectChat(entries.first().chatId);
    }

    // ---- actions ----
    void submit(const QString& raw, const QJsonArray& images = {}) {
        const QString text = raw.trimmed();
        if ((text.isEmpty() && images.isEmpty()) || !db) return;
        // a subagent's conversation is open: the text steers that agent (it reads it at its next step)
        if (!agentId.isEmpty()) {
            if (!SubagentRegistry::instance().steer(agentId, text)) composer->setText(raw);
            return;
        }
        // nothing can answer yet: connect a model first, the text stays in the composer
        if (needsSetup() && !text.startsWith(QLatin1Char('/'))) {
            showSetup();
            composer->setText(raw);
            return;
        }
        ChatSession* c = chat();
        if (!c) c = newChat();
        if (!c) return;
        TranscriptModel* m = model();
        if (m->awaitingButtons()) return;  // plan / save / option questions are answered with their buttons
        // a question without options takes the text as its answer
        for (int r = m->rowCount() - 1; r >= 0; --r) {
            const TranscriptItem& it = m->at(r);
            if (it.kind == ItemKind::Question && !it.resolved && it.payload.value(QLatin1String("options")).toArray().isEmpty()) {
                c->answerQuestion(it.requestId, UserAnswer{false, text});
                m->resolveCard(r, text);
                return;
            }
        }
        // omc echoes no command but a skill (whose echo replaces this row): show what was typed (a busy chat shows it
        // queued)
        if (text.startsWith(QLatin1Char('/')) && !c->busy()) m->addLocalUser(text);
        CommandRouter::submit(*c, text, images);
        view->scrollToEnd();
    }

    void stop() {
        if (ChatSession* c = chat()) c->stop();
    }

    void uiAction(const QString& action, const QString& arg) {
        if (action == QLatin1String("agents")) toggleAgents(true);
        // a chat that could not reach a model: the full setup when none is connected, else the model picker
        else if (action == QLatin1String("setup")) needsSetup() ? showSetup() : openModelPicker();
        else if (action == QLatin1String("login")) arg.isEmpty() ? login->showList() : login->startLogin(arg);
        else if (action == QLatin1String("logout")) q->openSettings();
        else if (action == QLatin1String("new")) newChat();
        else if (action == QLatin1String("fork")) forkChat(current);
        else if (action == QLatin1String("todo")) strip->setTodoExpanded(true);
        else if (action == QLatin1String("model")) openModelPicker();
    }

    void forkChat(const QString& id) {
        ChatSession* src = chats().value(id).session;
        if (!src) return;
        QPointer<PanelCore> self(q);
        src->fork(
            [self](const ChatEntry& e) {
                if (!self) return;
                self->d->addChat(e);
                self->d->selectChat(e.chatId);
            },
            [self](const QString& error) {
                if (self) QMessageBox::information(self, QObject::tr("Fork chat"), error);
            });
    }

    void renameChat(const QString& id) {
        ChatSession* c = chats().value(id).session;
        if (!c) return;
        bool ok = false;
        const QString title =
            QInputDialog::getText(q, QObject::tr("Rename chat"), QObject::tr("Name:"), QLineEdit::Normal, c->entry().title, &ok).trimmed();
        if (ok && !title.isEmpty()) c->setTitle(title);
    }

    void deleteChat(const QString& id) {
        ChatSession* c = chats().value(id).session;
        if (!c) return;
        const ChatEntry entry = c->entry();
        if (!entry.sessionFile.isEmpty() &&
            QMessageBox::question(q, QObject::tr("Delete chat"), QObject::tr("Delete '%1' and remove it from saved history?").arg(label(entry)),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        c->stop();
        const bool wasCurrent = current == id;
        removeChatObjects(id);
        db->chats->remove(id);
        if (!entry.sessionFile.isEmpty()) QFile::remove(entry.sessionFile);
        if (!wasCurrent) return;
        current.clear();
        QString next;
        qint64 best = -1;
        for (auto it = chats().cbegin(); it != chats().cend(); ++it)
            if (it->session->entry().lastActiveAt > best) {
                best = it->session->entry().lastActiveAt;
                next = it.key();
            }
        if (next.isEmpty() || RikuganConfig::instance().dontAutoLoadChats) showWelcome();
        else selectChat(next);
    }

    void exportChat(const QString& id) {
        ChatSession* c = chats().value(id).session;
        if (!c) return;
        if (c->entry().sessionFile.isEmpty()) {
            QMessageBox::information(q, QObject::tr("Export chat"), QObject::tr("Nothing to export yet."));
            return;
        }
        if (c->busy()) {
            QMessageBox::information(q, QObject::tr("Export chat"), QObject::tr("The chat is busy."));
            return;
        }
        QString name = label(c->entry());
        name.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('\\'), QLatin1Char('-'));
        const QString path = QFileDialog::getSaveFileName(
            q, QObject::tr("Export chat"),
            QStringLiteral("rikugan-%1-%2.md").arg(name, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))),
            QObject::tr("Markdown (*.md);;Text (*.txt);;All Files (*)"));
        if (path.isEmpty()) return;
        auto messages = std::make_shared<QJsonArray>();
        QPointer<PanelCore> self(q);
        const QString model = modelOf(chats().value(id).state);
        const QString file = binaryName();
        c->runJob(
            [messages](OmcSession& s) {
                const QJsonObject r = s.request(QJsonObject{{"type", "get_messages"}});
                *messages = r.value(QLatin1String("data")).toObject().value(QLatin1String("messages")).toArray();
            },
            [self, messages, path, model, file] {
                if (!self) return;
                QString error;
                if (!writeMarkdownExport(*messages, path, model, file, &error))
                    QMessageBox::warning(self, QObject::tr("Export chat"), QObject::tr("Failed to export the chat: %1").arg(error));
                else log(QStringLiteral("Rikugan: exported chat to %1").arg(path));
            });
    }

    // Undo the change recorded at `index` of the chat's log and every later one (the log reverts newest first).
    void undoFrom(int index) {
        ChatSession* c = chat();
        if (!c) return;
        const int count = int(c->context()->mutations->size()) - index;
        if (count <= 0) return;
        if (count > 1 &&
            QMessageBox::question(q, QObject::tr("Undo"), QObject::tr("Changes are undone newest first: this reverts %1 changes. Continue?").arg(count),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
            return;
        submit(QStringLiteral("/undo %1").arg(count));
    }

    void chooseModel(const QString& key) {
        RikuganConfig& cfg = RikuganConfig::instance();
        cfg.model = key;  // new chats start with it too
        cfg.save();
        if (ChatSession* c = chat()) c->setModel(key);
        syncActivity();
    }

    void chooseThinking(const QString& level) {
        RikuganConfig& cfg = RikuganConfig::instance();
        cfg.thinking = level;
        cfg.save();
        if (ChatSession* c = chat()) c->setThinkingLevel(level);
        syncActivity();
    }

    // ---- connect a model ----
    // No model can answer: the list came back empty (not an error, which may be passing).
    bool needsSetup() const { return picker->loaded() && picker->models().isEmpty() && picker->error().isEmpty(); }

    // The setup is a screen of its own: the chat's strip, composer and status line (another chat's agents, a card's
    // "answer above") are hidden while it shows, whichever way the page changes.
    void showSetup() {
        closePopups();
        if (pages->currentIndex() != kSetupPage) setup->start();
        pages->setCurrentIndex(kSetupPage);
    }

    void leaveSetup() {
        pages->setCurrentIndex(0);
        composer->focusInput();
    }

    void onPageChanged(int index) {
        const bool setupShown = index == kSetupPage;
        strip->setVisible(!setupShown && agentId.isEmpty());
        agentBar->setVisible(!setupShown && !agentId.isEmpty());
        composer->setVisible(!setupShown);
        status->setVisible(!setupShown);
        if (setupShown) {
            mcpBar->hide();
            header->setTitle(QObject::tr("Connect a model"), QStringLiteral("plug"));
            header->setChatActionsEnabled(false);
        } else {
            syncChrome();
        }
    }

    // one picker at a time
    void closePopups() {
        palette->dismiss();
        switcher->dismiss();
        picker->dismiss();
    }

    void openModelPicker() {
        closePopups();
        QStringList levels;
        for (const QJsonValue& v : chats().value(current).state.value(QLatin1String("thinkingLevels")).toArray()) levels << v.toString();
        const ChatActivity& a = model()->activity();
        const QString thinking = a.thinking.isEmpty() ? RikuganConfig::instance().thinking : a.thinking;
        picker->open(composer->modelButton(), modelKey(), thinking, levels);
    }

    void toggleAgents(bool show) {
        if (show == agents->isVisible() && !show) return;
        agents->setVisible(show);
        if (show) {
            pages->setCurrentIndex(0);
            const int total = split->height();
            split->setSizes({total * 3 / 5, total * 2 / 5});
        }
    }

    // ---- a subagent's conversation ----
    // Shows the agent's whole run in the transcript, live, like a chat; the composer steers it. Back (or picking a
    // chat) returns to the chat that started it.
    void openAgentChat(const QString& id) {
        const auto info = SubagentRegistry::instance().get(id);
        if (!info || !db) return;
        closePopups();
        if (current != info->chatId && chats().contains(info->chatId)) selectChat(info->chatId);
        closeAgentChat();
        agentId = id;
        agentModel = new TranscriptModel(q);
        QSet<QString> hostTools;
        for (const QString& n : db->registry.names()) hostTools.insert(n);
        agentModel->setHostTools(hostTools);
        auto feed = std::make_shared<AgentFeed>();
        agentFeed = feed;
        QPointer<PanelCore> self(q);
        std::weak_ptr<AgentFeed> weak = feed;
        std::vector<std::string> history;
        agentObserver = SubagentRegistry::instance().observeFrames(
            id,
            [weak, self](const std::string& json) {
                auto f = weak.lock();
                if (!f) return;
                std::lock_guard<std::mutex> g(f->mu);
                f->pending.push_back(json);
                if (f->posted) return;
                f->posted = true;
                // one posted drain per burst
                QMetaObject::invokeMethod(
                    uiAnchor(),
                    [weak, self] {
                        auto feed = weak.lock();
                        if (!feed || !self || self->d->agentFeed != feed) return;
                        std::vector<std::string> frames;
                        {
                            std::lock_guard<std::mutex> g(feed->mu);
                            frames.swap(feed->pending);
                            feed->posted = false;
                        }
                        for (const std::string& j : frames) self->d->applyAgentFrame(j);
                    },
                    Qt::QueuedConnection);
            },
            &history);
        for (const std::string& j : history) applyAgentFrame(j);
        agentModel->setBusy(info->status == SubagentStatus::Running || info->status == SubagentStatus::Pending);
        view->setTranscriptModel(agentModel);
        view->scrollToEnd();
        onPageChanged(pages->currentIndex());
        syncChrome();
        composer->focusInput();
    }

    void applyAgentFrame(const std::string& json) {
        if (!agentModel) return;
        agentModel->applyFrame(QJsonDocument::fromJson(QByteArray::fromStdString(json)).object());
    }

    void closeAgentChat() {
        if (agentId.isEmpty()) return;
        SubagentRegistry::instance().removeFrameObserver(agentObserver);
        agentObserver = 0;
        agentFeed.reset();
        agentId.clear();
        if (view->transcriptModel() == agentModel) view->setTranscriptModel(model());
        if (agentModel) agentModel->deleteLater();
        agentModel = nullptr;
        onPageChanged(pages->currentIndex());
        syncChrome();
    }

    void syncAgentChrome() {
        const auto info = SubagentRegistry::instance().get(agentId);
        const QString name = info ? (info->name.isEmpty() ? info->id : info->name) : agentId;
        const bool live = info && (info->status == SubagentStatus::Running || info->status == SubagentStatus::Pending);
        header->setTitle(name, QStringLiteral("agent"));
        header->setChatActionsEnabled(false);
        composer->setInputEnabled(live);
        composer->setRunning(false);
        composer->setQueue({});
        composer->setPlaceholder(live ? QObject::tr("Steer %1… it reads this at its next step").arg(name)
                                      : QObject::tr("%1 has finished. Back to chat returns to the conversation.").arg(name));
        if (agentModel) agentModel->setBusy(live);
        QString parent;
        if (info) {
            const ChatSession* c = chats().value(info->chatId).session;
            parent = c ? label(c->entry()) : QString();
        }
        QStringList facts{QStringLiteral("<b>%1</b>").arg(subagentStatusName(info ? info->status : SubagentStatus::Cancelled))};
        if (info) facts << QObject::tr("%n turn(s)", nullptr, info->turnCount) << QObject::tr("%n tool call(s)", nullptr, info->toolCount);
        if (!parent.isEmpty()) facts << QObject::tr("subagent of “%1”").arg(parent.toHtmlEscaped());
        agentInfo->setText(facts.join(QStringLiteral(" · ")));
        agentStop->setVisible(live);
    }

    // ---- palettes ----
    void openSwitcher() {
        closePopups();
        QList<PaletteEntry> entries;
        PaletteEntry add;
        add.key = QStringLiteral("!new");
        add.glyph = QStringLiteral("+");
        add.title = QObject::tr("New chat");
        add.hint = QObject::tr("+");
        entries << add;
        QList<ChatSession*> list;
        for (const Chat& c : std::as_const(chats())) list << c.session;
        std::sort(list.begin(), list.end(), [](ChatSession* a, ChatSession* b) { return a->entry().lastActiveAt > b->entry().lastActiveAt; });
        for (ChatSession* s : std::as_const(list)) {
            const Chat c = chats().value(s->chatId());
            PaletteEntry e;
            e.key = s->chatId();
            e.section = binaryName();
            e.title = label(s->entry());
            e.checked = s->chatId() == current;
            QStringList detail{relativeTime(s->entry().lastActiveAt ? s->entry().lastActiveAt : s->entry().createdAt)};
            if (const int n = int(s->context()->mutations->size())) detail << QObject::tr("%1 changes").arg(n);
            e.detail = detail.join(QStringLiteral(" \u00b7 "));
            if (c.model->awaitingUser()) e.hint = QObject::tr("\u25C6 waiting");
            else if (s->busy()) e.hint = s->queued().isEmpty() ? QObject::tr("\u25CF running") : QObject::tr("queued %1").arg(s->queued().size());
            else if (c.model->activity().lastOutcome == QLatin1String("error")) e.hint = QObject::tr("error");
            entries << e;
            // its agents under it, the ones working on other databases too
            for (const SubagentInfo& a : SubagentRegistry::instance().all()) {
                if (a.chatId != s->chatId()) continue;
                PaletteEntry child;
                child.key = QStringLiteral("agent:") + a.id;
                child.section = e.section;
                child.title = QStringLiteral("   ↳ ") + (a.name.isEmpty() ? a.id : a.name);
                child.search = a.name + QLatin1Char(' ') + a.task.left(200);
                child.detail = a.task.section(QLatin1Char('\n'), 0, 0).left(120);
                child.hint = subagentStatusName(a.status);
                child.checked = a.id == agentId;
                entries << child;
            }
        }
        switcher->list()->setEntries(entries);
        switcher->popupAt(header->titleButton(), false, kPaletteWidth);
    }

    void openCommandPalette() {
        closePopups();
        QList<PaletteEntry> entries;
        auto action = [&](const QString& key, const QString& glyph, const QString& title, const QString& hint = {}, bool enabled = true) {
            PaletteEntry e;
            e.key = QStringLiteral("a:") + key;
            e.section = QObject::tr("Actions");
            e.glyph = glyph;
            e.title = title;
            e.hint = hint;
            e.enabled = enabled;
            entries << e;
        };
        ChatSession* c = chat();
        action(QStringLiteral("agents"), QStringLiteral("\u2442"), agents->isVisible() ? QObject::tr("Hide agents panel") : QObject::tr("Show agents panel"),
               QStringLiteral("Ctrl J"));
        action(QStringLiteral("model"), QStringLiteral("\u2699"), QObject::tr("Change model or thinking"), QStringLiteral("Ctrl M"));
        action(QStringLiteral("new"), QStringLiteral("+"), QObject::tr("New chat"));
        action(QStringLiteral("switch"), QStringLiteral("\u21C4"), QObject::tr("Switch chat"));
        if (c && c->busy()) action(QStringLiteral("stop"), QStringLiteral("\u25A0"), QObject::tr("Stop"), QStringLiteral("Esc"));
        action(QStringLiteral("todo"), QStringLiteral("\u2611"), QObject::tr("Show the todo list"));
        action(QStringLiteral("setup"), QStringLiteral("+"), QObject::tr("Connect a model"));
        if (c) {
            action(QStringLiteral("fork"), QStringLiteral("\u2442"), QObject::tr("Fork chat"));
            action(QStringLiteral("rename"), QStringLiteral("\u270E"), QObject::tr("Rename chat"));
            action(QStringLiteral("export"), QStringLiteral("\u2913"), QObject::tr("Export as Markdown"));
            action(QStringLiteral("delete"), QStringLiteral("\u2715"), QObject::tr("Delete chat"));
            if (c->context()->mutations->size() > 0) action(QStringLiteral("undo"), QStringLiteral("\u21B6"), QObject::tr("Undo last database change"));
        }
        action(QStringLiteral("renamer"), QStringLiteral("\u2630"), QObject::tr("Bulk renamer"));
        action(QStringLiteral("settings"), QStringLiteral("\u2638"), QObject::tr("Settings"));
        for (const CommandInfo& ci : CommandRouter::catalog()) {
            PaletteEntry e;
            e.key = QStringLiteral("c:") + ci.name;
            e.section = ci.group == QLatin1String("rikugan") ? QObject::tr("Reverse engineering")
                        : ci.group == QLatin1String("agent")  ? QObject::tr("Agent")
                        : ci.group == QLatin1String("skill")  ? QObject::tr("Skills")
                                                              : QObject::tr("Session");
            e.glyph = ci.icon;
            e.title = QStringLiteral("/") + ci.name;
            e.detail = ci.description;
            e.mono = true;
            entries << e;
        }
        palette->list()->setEntries(entries);
        palette->popupAt(header, false, kPaletteWidth);
    }

    void paletteActivated(const QString& key) {
        palette->dismiss();
        if (key.startsWith(QLatin1String("c:"))) {
            const QString name = key.mid(2);
            for (const CommandInfo& ci : CommandRouter::catalog())
                if (ci.name == name && ci.argHint.startsWith(QLatin1Char('<'))) {
                    composer->setText(QStringLiteral("/%1 ").arg(name));
                    return;
                }
            submit(QStringLiteral("/") + name);
            return;
        }
        const QString a = key.mid(2);
        if (a == QLatin1String("agents")) toggleAgents(!agents->isVisible());
        else if (a == QLatin1String("model")) openModelPicker();
        else if (a == QLatin1String("new")) newChat();
        else if (a == QLatin1String("switch")) openSwitcher();
        else if (a == QLatin1String("stop")) stop();
        else if (a == QLatin1String("todo")) strip->setTodoExpanded(true);
        else if (a == QLatin1String("setup")) showSetup();
        else if (a == QLatin1String("fork")) forkChat(current);
        else if (a == QLatin1String("rename")) renameChat(current);
        else if (a == QLatin1String("export")) exportChat(current);
        else if (a == QLatin1String("delete")) deleteChat(current);
        else if (a == QLatin1String("undo")) submit(QStringLiteral("/undo 1"));
        else if (a == QLatin1String("renamer")) pages->setCurrentIndex(1);
        else if (a == QLatin1String("settings")) q->openSettings();
    }

    // ---- theme ----
    void applyTheme() { q->setStyleSheet(themeStylesheet()); }

    void build();
};

void PanelCore::Impl::build() {
    q->setObjectName(QStringLiteral("rk_root"));
    q->setAttribute(Qt::WA_StyledBackground);
    // the chat column, and the chat manager at its right (full window only)
    auto* outer = new QHBoxLayout(q);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* column = new QWidget(q);
    outer->addWidget(column, 1);
    manager = new ChatManager(q);
    manager->hide();
    outer->addWidget(manager);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    header = new ConsoleHeader(q);
    layout->addWidget(header);
    // a subagent's conversation is open: who it is, how it is doing, and the way back
    agentBar = new QFrame(q);
    agentBar->setObjectName(QStringLiteral("rk_agentbar"));
    agentBar->setAttribute(Qt::WA_StyledBackground);
    {
        auto* h = new QHBoxLayout(agentBar);
        h->setContentsMargins(10, 4, 8, 4);
        auto* back = new QPushButton(QObject::tr("← Back to chat"), agentBar);
        back->setCursor(Qt::PointingHandCursor);
        h->addWidget(back);
        agentInfo = new QLabel(agentBar);
        agentInfo->setTextFormat(Qt::RichText);
        agentInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        h->addWidget(agentInfo, 1);
        agentStop = new QPushButton(QStringLiteral("■ ") + QObject::tr("Stop agent"), agentBar);
        agentStop->setProperty("rkDanger", true);
        h->addWidget(agentStop);
        QObject::connect(back, &QPushButton::clicked, q, [this] { closeAgentChat(); });
    }
    agentBar->hide();
    layout->addWidget(agentBar);
    mcpBar = new HostMcpBar(q);
    layout->addWidget(mcpBar);
    QObject::connect(mcpBar, &HostMcpBar::stateChanged, q, [this] {
        header->setHostMcp(mcpBar->available(), mcpBar->on(), mcpBar->busy(), mcpBar->tooltip());
    });
    QObject::connect(header, &ConsoleHeader::hostMcpClicked, q, [this] { mcpBar->toggle(); });
    // laid out under the transcript, above the composer: "Thinking…" shows where the answer appears
    strip = new ActivityStrip(q);

    pages = new QStackedWidget(q);
    split = new QSplitter(Qt::Vertical, pages);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(4);
    view = new TranscriptView(split);
    emptyModel = new TranscriptModel(q);
    view->setTranscriptModel(emptyModel);
    welcome = new WelcomeView;
    // a short host pane (Binary Ninja's sidebar) must scroll the welcome, not squeeze it
    auto* welcomePane = new QScrollArea;
    welcomePane->setWidgetResizable(true);
    welcomePane->setFrameShape(QFrame::NoFrame);
    welcomePane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    welcomePane->setWidget(welcome);
    view->setEmptyWidget(welcomePane);
    split->addWidget(view);
    agents = new AgentsPanel(split);
    agents->hide();
    split->addWidget(agents);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    pages->addWidget(split);
    tools = new ToolsPanel(pages);
    pages->addWidget(tools);
    setup = new GetStarted;
    auto* setupPane = new QScrollArea;
    setupPane->setWidgetResizable(true);
    setupPane->setFrameShape(QFrame::NoFrame);
    setupPane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setupPane->setWidget(setup);
    pages->addWidget(setupPane);  // kSetupPage
    QObject::connect(pages, &QStackedWidget::currentChanged, q, [this](int i) { onPageChanged(i); });
    layout->addWidget(pages, 1);
    layout->addWidget(strip);

    composer = new Composer(q);
    composer->setPopupArea(q);
    composer->setCommands(CommandRouter::catalog());
    layout->addWidget(composer);
    status = new StatusLine(q);
    layout->addWidget(status);

    picker = new ModelPicker(q);
    palette = new PalettePopup(q, true, QObject::tr("Type a command or action\u2026"));
    palette->list()->setMaxRows(12);
    switcher = new PalettePopup(q, true, QObject::tr("Switch chat\u2026"));
    login = new LoginSheet(q, true);

    QObject::connect(header, &ConsoleHeader::switcherRequested, q, [this] { openSwitcher(); });
    QObject::connect(header, &ConsoleHeader::paletteRequested, q, [this] { openCommandPalette(); });
    QObject::connect(header, &ConsoleHeader::newChatRequested, q, [this] { newChat(); });
    QObject::connect(header, &ConsoleHeader::forkRequested, q, [this] { forkChat(current); });
    QObject::connect(header, &ConsoleHeader::renameRequested, q, [this] { renameChat(current); });
    QObject::connect(header, &ConsoleHeader::exportRequested, q, [this] { exportChat(current); });
    QObject::connect(header, &ConsoleHeader::exportHtmlRequested, q, [this] { submit(QStringLiteral("/export")); });
    QObject::connect(header, &ConsoleHeader::deleteRequested, q, [this] { deleteChat(current); });
    QObject::connect(header, &ConsoleHeader::undoRequested, q, [this] { submit(QStringLiteral("/undo 1")); });
    QObject::connect(header, &ConsoleHeader::agentsRequested, q, [this] { toggleAgents(!agents->isVisible()); });
    QObject::connect(header, &ConsoleHeader::renamerRequested, q, [this] { pages->setCurrentIndex(1); });
    QObject::connect(header, &ConsoleHeader::settingsRequested, q, [this] { q->openSettings(); });

    QObject::connect(strip, &ActivityStrip::agentsClicked, q, [this] { toggleAgents(!agents->isVisible()); });
    QObject::connect(strip, &ActivityStrip::goalPause, q, [this] { submit(QStringLiteral("/goal pause")); });
    QObject::connect(strip, &ActivityStrip::goalResume, q, [this] { submit(QStringLiteral("/goal resume")); });
    QObject::connect(strip, &ActivityStrip::goalClear, q, [this] { submit(QStringLiteral("/goal drop")); });
    QObject::connect(strip, &ActivityStrip::stopRequested, q, [this] { stop(); });

    QObject::connect(view, &TranscriptView::navigateRequested, q, [this](quint64 ea) {
        if (db) db->host->navigate(ea);
    });
    QObject::connect(view, &TranscriptView::undoRequested, q, [this](int index) { undoFrom(index); });
    QObject::connect(view, &TranscriptView::agentActivated, q, [this](const QString& id) {
        toggleAgents(true);
        agents->showAgent(id);
    });
    QObject::connect(welcome, &WelcomeView::activated, q, [this](const QString& prompt) { composer->setText(prompt); });
    QObject::connect(&ChatHub::instance(), &ChatHub::stateChanged, q, [this](const QString& id) {
        if (id == current) syncActivity();
    });
    QObject::connect(welcome, &WelcomeView::openRequested, q, [this](const QString& key) { openFromWelcome(key); });

    QObject::connect(manager, &ChatManager::chatActivated, q, [this](const QString& dbId, const QString& chatId) {
        showChatOf(dbId, chatId);
    });
    QObject::connect(manager, &ChatManager::newChatRequested, q, [this](const QString& dbId) { showChatOf(dbId, {}); });
    QObject::connect(manager, &ChatManager::openRequested, q, [this] { openFile(); });
    QObject::connect(agents, &AgentsPanel::closeRequested, q, [this] { toggleAgents(false); });
    QObject::connect(agents, &AgentsPanel::summaryForChat, q, [this](const QString& t) { submit(t); });
    QObject::connect(agents, &AgentsPanel::openChatRequested, q, [this](const QString& id) { openAgentChat(id); });
    QObject::connect(agentStop, &QPushButton::clicked, q, [this] {
        if (!agentId.isEmpty()) SubagentRegistry::instance().cancel(agentId);
    });
    // the open agent's status line follows the registry (one refresh per burst of changes)
    {
        QPointer<PanelCore> self(q);
        auto posted = agentRefreshPosted;
        registryObserver = SubagentRegistry::instance().addObserver([self, posted] {
            if (posted->exchange(true)) return;
            QMetaObject::invokeMethod(
                uiAnchor(),
                [self, posted] {
                    posted->store(false);
                    if (self && !self->d->agentId.isEmpty()) self->d->syncChrome();
                },
                Qt::QueuedConnection);
        });
    }

    QObject::connect(composer, &Composer::submitted, q, [this](const QString& t, const QJsonArray& images) { submit(t, images); });
    QObject::connect(composer, &Composer::stopRequested, q, [this] { stop(); });
    QObject::connect(composer, &Composer::modelPickerRequested, q, [this] {
        if (picker->isVisible()) picker->dismiss();
        else openModelPicker();
    });
    QObject::connect(composer, &Composer::removeQueued, q, [this](int i) {
        if (ChatSession* c = chat()) c->removeQueued(i);
    });
    QObject::connect(composer, &Composer::sendQueuedNow, q, [this](int i) {
        if (ChatSession* c = chat()) c->sendQueuedNow(i);
    });

    QObject::connect(status, &StatusLine::locationClicked, q, [this](const QString& a) {
        const QString t = composer->text();
        composer->setText(t.isEmpty() ? a + QLatin1Char(' ') : t + QLatin1Char(' ') + a + QLatin1Char(' '));
    });
    QObject::connect(status, &StatusLine::modelClicked, q, [this] { openModelPicker(); });

    QObject::connect(picker, &ModelPicker::modelChosen, q, [this](const QString& key) { chooseModel(key); });
    QObject::connect(picker, &ModelPicker::thinkingChosen, q, [this](const QString& level) { chooseThinking(level); });
    QObject::connect(picker, &ModelPicker::connectRequested, q, [this](const QString& id) {
        id.isEmpty() ? login->showList() : login->startLogin(id);
    });
    QObject::connect(picker, &ModelPicker::modelsChanged, q, [this] {
        syncActivity();
        // first run (or every login gone): the panel opens on the setup
        if (needsSetup() && !setupDismissed && pages->currentIndex() == 0) showSetup();
    });
    QObject::connect(picker, &ModelPicker::refreshed, q, [this] { setup->setModels(picker->models()); });
    QObject::connect(login, &LoginSheet::connected, q, [this](const QString& id) {
        if (pages->currentIndex() == kSetupPage) setup->loggedIn(id);
        else openModelPicker();
    });
    QObject::connect(setup, &GetStarted::loginRequested, q, [this](const QString& id) { login->startLogin(id); });
    QObject::connect(setup, &GetStarted::refreshModels, q, [this] { picker->refresh(); });
    QObject::connect(setup, &GetStarted::modelChosen, q, [this](const QString& key) {
        chooseModel(key);
        leaveSetup();
    });
    QObject::connect(setup, &GetStarted::dismissed, q, [this] {
        setupDismissed = true;
        leaveSetup();
    });
    QObject::connect(login, &LoginSheet::closed, q, [this] { composer->focusInput(); });

    QObject::connect(palette->list(), &PaletteList::activated, q, [this](const QString& key) { paletteActivated(key); });
    QObject::connect(switcher->list(), &PaletteList::activated, q, [this](const QString& key) {
        switcher->dismiss();
        if (key == QLatin1String("!new")) newChat();
        else if (key.startsWith(QLatin1String("agent:"))) openAgentChat(key.mid(6));
        else selectChat(key);
    });
    QObject::connect(tools, &ToolsPanel::backRequested, q, [this] { pages->setCurrentIndex(0); });
    // a closed picker gives the keyboard back to the composer (the panel's shortcuts need focus inside it)
    for (PalettePopup* p : {palette, switcher, static_cast<PalettePopup*>(picker)})
        QObject::connect(p, &PalettePopup::dismissed, q, [this] {
            if (!login->isVisible()) composer->focusInput();
        });

    // Ctrl+K / M / J / N: shortcuts while the focus is in the panel; the composer claims them first so a host
    // shortcut on the same keys does not make them ambiguous
    auto hotkey = [this](int key) {
        switch (key) {
        case Qt::Key_K: palette->isVisible() ? palette->dismiss() : openCommandPalette(); break;
        case Qt::Key_M: picker->isVisible() ? picker->dismiss() : openModelPicker(); break;
        case Qt::Key_J: toggleAgents(!agents->isVisible()); break;
        case Qt::Key_N: newChat(); break;
        default: break;
        }
    };
    for (const int key : {Qt::Key_K, Qt::Key_M, Qt::Key_J, Qt::Key_N}) {
        auto* s = new QShortcut(QKeySequence(int(Qt::ControlModifier) | key), q);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(s, &QShortcut::activated, q, [hotkey, key] { hotkey(key); });
    }
    QObject::connect(composer, &Composer::panelKey, q, hotkey);

    themeTimer.setSingleShot(true);
    themeTimer.setInterval(kThemeCheckDelayMs);
    QObject::connect(&themeTimer, &QTimer::timeout, q, [this] { refreshTheme(q->window()); });
    QObject::connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, q, [this] { applyTheme(); });
    applyTheme();
    syncChrome();
    refreshWelcome();
    picker->refresh();  // tells whether a model is set up (the setup opens when none is)
}

PanelCore::PanelCore(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>(this)) { d->build(); }

PanelCore::~PanelCore() {
    // the chats live on in the ChatHub (IDA's dock closed, a new panel shows them again): only the panel's
    // connections to them go
    d->detachChats();
    SubagentRegistry::instance().removeObserver(d->registryObserver);
    // the children's signals reach lambdas on Impl: they go while it still exists
    d->themeTimer.stop();
    QList<QPointer<QObject>> kids;
    for (QObject* o : children()) kids << o;
    for (const QPointer<QObject>& o : kids) delete o.data();
}

void PanelCore::setDatabase(std::shared_ptr<DbContext> db) {
    if (db == d->db) return;
    d->detachChats();
    d->db = std::move(db);
    // an expected id the new database did not take must not reach a later file
    if (d->db) clearExpectedSession();
    if (d->db) setThemeHost(d->db->host->hostName());
    refreshTheme(window());
    d->tools->setDatabase(d->db);
    d->composer->setCommands(CommandRouter::catalog());
    d->loadBinarySummary();
    if (d->db) d->picker->refresh();  // friendly model names for the pill
    d->restoreChats();
    d->mcpBar->setDatabase(d->db);
}

std::shared_ptr<DbContext> PanelCore::database() const { return d->db; }

void PanelCore::setFileOpener(const QString& hostName, FileOpener opener) {
    d->openerHost = hostName;
    d->opener = std::move(opener);
    if (!d->db) d->refreshWelcome();
}

void PanelCore::setLocation(uint64_t address, const QString& functionName) {
    const bool first = !d->address;
    d->address = address;
    d->status->setLocation(address, functionName);
    // the /modify suggestion carries the cursor; refresh it only while the welcome shows
    if (first || (d->current.isEmpty() && d->welcome->isVisible())) d->refreshWelcome();
}

void PanelCore::submitToCurrentChat(const QString& text) { d->submit(text); }

void PanelCore::prefillComposer(const QString& text) {
    d->pages->setCurrentIndex(0);
    d->composer->setText(text);
}

void PanelCore::addToBulkRenamer(const QList<quint64>& functionAddresses) {
    d->pages->setCurrentIndex(1);
    d->tools->addToRenamer(functionAddresses);
}

void PanelCore::showTools() { d->pages->setCurrentIndex(1); }

void PanelCore::setManagerShown(bool shown) {
    d->manager->setCanOpen(bool(d->opener));
    d->manager->setVisible(shown);
}

void PanelCore::setWindowToggle(bool fullWindow, std::function<void()> toggle) {
    QObject::disconnect(d->header, &ConsoleHeader::windowToggleRequested, this, nullptr);
    d->header->setWindowToggle(bool(toggle), fullWindow);
    if (toggle) QObject::connect(d->header, &ConsoleHeader::windowToggleRequested, this, std::move(toggle));
}

void PanelCore::openSettings() {
    SettingsDialog dlg(this);
    QObject::connect(&dlg, &SettingsDialog::settingsChanged, this, [this] {
        ChatHub::instance().settingsChanged();
        d->composer->setCommands(CommandRouter::catalog());
        d->refreshWelcome();
        d->picker->refresh();
        d->syncActivity();
    });
    dlg.exec();
}

void PanelCore::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // the theme may have changed while the panel was hidden
    d->themeTimer.start();
    // a server started while the panel was hidden (Binary Ninja's MCP): look again
    d->mcpBar->recheck();
}

void PanelCore::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (!d) return;  // change events arrive from the base constructor
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
        d->themeTimer.start();
        break;
    default:
        break;
    }
}

}  // namespace rk
