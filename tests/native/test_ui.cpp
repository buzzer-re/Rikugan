// The panel against the mock model (offscreen): a prompt's answer on screen, an approval card and a permission card
// answered with their buttons, the transcript of a reopened chat, and a recent chat opened with no database open.
#include <atomic>

#include <QCheckBox>
#include <QClipboard>
#include <QMimeData>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/agent/Subagents.h"
#include "common/config/RikuganConfig.h"
#include "common/ui/PanelCore.h"
#include "common/ui/WelcomeView.h"
#include "common/ui/console/AgentsPanel.h"
#include "common/ui/console/Palette.h"
#include "common/ui/transcript/Cards.h"
#include "common/ui/transcript/TranscriptView.h"
#include "common/config/Paths.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/console/ActivityStrip.h"
#include "common/ui/console/Composer.h"
#include "common/ui/console/GetStarted.h"
#include "common/ui/console/ModelPicker.h"
#include "common/ui/transcript/TranscriptModel.h"
#include "common/util/Files.h"

using namespace rk::ui;

namespace {

// Its own database id: the chat tests' chats must not show up in this panel's list.
class UiHost : public rktest::FakeHost {
public:
    QString databaseInstanceId() override { return QStringLiteral("ui-test-db"); }
};

TranscriptView* transcript(QWidget* root) { return root->findChild<TranscriptView*>(); }

TranscriptModel* shown(QWidget* root) {
    TranscriptView* v = transcript(root);
    return v ? v->transcriptModel() : nullptr;
}

// Every row of a kind, as the model has it.
QStringList texts(QWidget* root, ItemKind kind) {
    QStringList out;
    if (TranscriptModel* m = shown(root))
        for (int i = 0; i < m->rowCount(); ++i)
            if (m->at(i).kind == kind) out << m->at(i).text;
    return out;
}

bool idle(QWidget* root) {
    TranscriptModel* m = shown(root);
    return m && m->chat() && !m->chat()->busy();
}

// The first unresolved card of `kind` with its live widget.
TranscriptCard* pendingCard(QWidget* root, ItemKind kind) {
    TranscriptModel* m = shown(root);
    if (!m) return nullptr;
    for (int i = 0; i < m->rowCount(); ++i)
        if (m->at(i).kind == kind && !m->at(i).resolved) return transcript(root)->cardFor(m->at(i).id);
    return nullptr;
}

QPushButton* buttonOf(QWidget* card, const QString& text) {
    for (QPushButton* b : card->findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}

// A header button, by its tooltip.
void clickTool(QWidget* root, const QString& tip) {
    for (QToolButton* b : root->findChildren<QToolButton*>())
        if (b->toolTip() == tip && b->isVisibleTo(root)) {
            b->click();
            return;
        }
    QFAIL(qPrintable("no button " + tip));
}

}  // namespace

class UiTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    std::shared_ptr<rk::DbContext> db;
    std::atomic<int> scriptRuns{0};
    QString helloChat;

private Q_SLOTS:
    void initTestCase() {
        auto h = std::make_unique<UiHost>();
        h->dbPath = dir.path() + "/ui.bndb";
        QFile bndb(h->dbPath);  // recent chats list only files that exist
        QVERIFY(bndb.open(QIODevice::WriteOnly));
        db = rk::DbContext::create(std::move(h));
        db->registry.add(rk::ToolSpec{"run_test_script", "test", "Runs a test script.",
                                      {rk::ToolParam{"code", rk::ToolParam::String, "Code", {}, {}, {}}}, rk::Script, 20000,
                                      [this](rk::ToolCall& c) {
                                          ++scriptRuns;
                                          return rk::ToolResult::ok(QStringLiteral("ran %1").arg(c.str("code")));
                                      }});
        rk::RikuganConfig::instance().restoreSessionsOnStart = true;
        rk::RikuganConfig::instance().dontAutoLoadChats = false;
    }

    void promptShowsAnswer() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        panel.submitToCurrentChat("say:hello");
        QVERIFY2(rktest::waitFor([&] { return texts(&panel, ItemKind::Text).join('\n').contains("hello"); }, 30000),
                 qPrintable(texts(&panel, ItemKind::Text).join('\n')));
        QVERIFY(rktest::waitFor([&] { return idle(&panel); }, 30000));
        TranscriptModel* m = shown(&panel);
        helloChat = m->chat()->chatId();
        QVERIFY(!m->chat()->entry().sessionFile.isEmpty());
        QVERIFY(texts(&panel, ItemKind::User).contains("say:hello"));
        // every row has a laid-out place in the view
        TranscriptView* v = transcript(&panel);
        for (int i = 0; i < m->rowCount(); ++i) QVERIFY2(v->visualRect(m->index(i)).height() > 0, qPrintable(QString::number(i)));
    }

    void approvalCardRoundTrip() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        // a fresh chat: the most recent one was restored and is open
        clickTool(&panel, "New chat");
        QVERIFY(shown(&panel) && shown(&panel)->rowCount() == 0);
        scriptRuns = 0;
        panel.submitToCurrentChat(R"x(seq:[["run_test_script",{"code":"print(1)"}]])x");
        TranscriptCard* card = nullptr;
        QVERIFY(rktest::waitFor([&] { return (card = pendingCard(&panel, ItemKind::Approval)) != nullptr; }, 30000));
        QCOMPARE(scriptRuns.load(), 0);  // nothing runs before the user answers
        QPushButton* allow = buttonOf(card, "Allow");
        QVERIFY(allow);
        allow->click();
        QVERIFY(rktest::waitFor([&] { return scriptRuns.load() == 1; }, 30000));
        QVERIFY(rktest::waitFor([&] { return idle(&panel); }, 30000));
        QVERIFY(!pendingCard(&panel, ItemKind::Approval));
        // the call's row shows its result
        TranscriptModel* m = shown(&panel);
        bool done = false;
        for (int i = 0; i < m->rowCount(); ++i)
            if (m->at(i).kind == ItemKind::Tool && m->at(i).toolName == "run_test_script")
                done = m->at(i).status == ToolStatus::Done && m->at(i).result.contains("ran print(1)");
        QVERIFY(done);
    }

    void permissionCardAllowsForTheChat() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        clickTool(&panel, "New chat");
        panel.submitToCurrentChat(R"x(seq:[["bash",{"command":"echo one"}]])x");
        TranscriptCard* card = nullptr;
        QVERIFY(rktest::waitFor([&] { return (card = pendingCard(&panel, ItemKind::Permission)) != nullptr; }, 30000));
        QPushButton* chat = buttonOf(card, "Allow for this chat");
        QVERIFY(chat);
        chat->click();
        QVERIFY(rktest::waitFor([&] { return idle(&panel); }, 30000));
        // the second command of the chat runs without asking
        panel.submitToCurrentChat(R"x(seq:[["bash",{"command":"echo two"}]])x");
        QVERIFY2(rktest::waitFor([&] { return idle(&panel) && texts(&panel, ItemKind::User).size() == 2; }, 30000),
                 qPrintable(texts(&panel, ItemKind::User).join(" | ")));
        TranscriptModel* m = shown(&panel);
        int permissions = 0, ran = 0;
        for (int i = 0; i < m->rowCount(); ++i) {
            permissions += m->at(i).kind == ItemKind::Permission ? 1 : 0;
            if (m->at(i).kind == ItemKind::Tool && m->at(i).toolName == "bash" && m->at(i).status == ToolStatus::Done) ++ran;
        }
        QCOMPARE(permissions, 1);
        QCOMPARE(ran, 2);
    }

    void reopenedChatReplaysTranscript() {
        QVERIFY(!helloChat.isEmpty());
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        // the chat switcher lists every chat of the database
        PaletteList* switcher = nullptr;
        clickTool(&panel, "Switch chat");
        for (PaletteList* l : panel.findChildren<PaletteList*>())
            if (l->isVisibleTo(&panel))
                for (const PaletteEntry& e : l->entries())
                    if (e.key == helloChat) switcher = l;
        QVERIFY(switcher);
        Q_EMIT switcher->activated(helloChat);
        QCOMPARE(shown(&panel)->chat()->chatId(), helloChat);
        QVERIFY2(rktest::waitFor([&] { return texts(&panel, ItemKind::Text).join('\n').contains("hello"); }, 30000),
                 qPrintable(texts(&panel, ItemKind::Text).join('\n')));
        QVERIFY(texts(&panel, ItemKind::User).contains("say:hello"));
    }

    void recentChatReopensItsDatabase() {
        QVERIFY(!helloChat.isEmpty());
        rk::RikuganConfig::instance().dontAutoLoadChats = true;  // only the picked chat may open by itself
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        QString openedFile, openedId;
        panel.setFileOpener("binja", [&](const QString& file, const QString& id) {
            openedFile = file;
            openedId = id;
            panel.setDatabase(db);
            return true;
        });
        const QString title = db->chats->find(helloChat)->title;
        SuggestionRow* row = nullptr;
        for (SuggestionRow* r : panel.findChildren<WelcomeView*>().value(0)->findChildren<SuggestionRow*>()) {
            QStringList labels;
            for (QLabel* l : r->findChildren<QLabel*>()) labels << l->text();
            if (labels.contains(title) && labels.join(' ').contains("ui.bndb")) row = r;
        }
        QVERIFY2(row, qPrintable(title));
        QTest::mouseClick(row, Qt::LeftButton);
        rk::RikuganConfig::instance().dontAutoLoadChats = false;
        QCOMPARE(openedFile, dir.path() + "/ui.bndb");
        QCOMPARE(openedId, QStringLiteral("ui-test-db"));
        QVERIFY(shown(&panel)->chat());
        QCOMPARE(shown(&panel)->chat()->chatId(), helloChat);
    }

    // switching to another database (a Binary Ninja tab) leaves the chat running; back on it, the same chat shows
    // its finished answer
    void chatKeepsRunningWhileAnotherDatabaseShows() {
        auto h2 = std::make_unique<UiHost>();
        h2->dbPath = dir.path() + "/other.bndb";
        const auto other = rk::DbContext::create(std::move(h2));
        auto* host = static_cast<UiHost*>(db->host.get());
        host->slowCancelled = false;
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        clickTool(&panel, "New chat");
        panel.submitToCurrentChat(R"(batch:[["slow_tool",{}]])");
        rk::ChatSession* running = shown(&panel)->chat();
        QVERIFY(running && running->busy());
        QPointer<rk::ChatSession> alive(running);
        panel.setDatabase(other);
        QVERIFY(rktest::waitFor([&] { return alive && !alive->busy(); }, 20000));
        QVERIFY(!host->slowCancelled.load());  // nobody stopped the call
        panel.setDatabase(db);
        QCOMPARE(shown(&panel)->chat(), running);
        QVERIFY2(texts(&panel, ItemKind::Text).join('\n').contains("slow done"), qPrintable(texts(&panel, ItemKind::Text).join('\n')));
    }

    // a subagent's conversation opens like a chat (its run so far, then live), the switcher lists it under its chat,
    // and Back returns to that chat
    void subagentOpensLikeAChat() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        clickTool(&panel, "New chat");
        const QString parent = shown(&panel)->chat()->chatId();
        rk::SubagentInfo info;
        info.name = "Recon";
        info.task = "Map the callers";
        info.chatId = parent;
        info.status = rk::SubagentStatus::Running;
        auto& reg = rk::SubagentRegistry::instance();
        const QString id = reg.add(info);
        auto frame = [&](const QJsonObject& f) { reg.appendFrame(id, QJsonDocument(f).toJson(QJsonDocument::Compact).toStdString()); };
        frame({{"type", "message_start"}, {"message", QJsonObject{{"role", "assistant"}}}});
        frame({{"type", "message_update"}, {"assistantMessageEvent", QJsonObject{{"type", "text_delta"}, {"delta", "found main"}, {"contentIndex", 0}}}});

        Q_EMIT panel.findChild<AgentsPanel*>()->openChatRequested(id);
        QTRY_VERIFY_WITH_TIMEOUT(texts(&panel, ItemKind::Text).join(' ').contains("found main"), 2000);
        frame({{"type", "message_update"}, {"assistantMessageEvent", QJsonObject{{"type", "text_delta"}, {"delta", " and its callers"}, {"contentIndex", 0}}}});
        QTRY_VERIFY_WITH_TIMEOUT(texts(&panel, ItemKind::Text).join(' ').contains("found main and its callers"), 2000);

        clickTool(&panel, "Switch chat");
        bool listed = false;
        for (PaletteList* l : panel.findChildren<PaletteList*>())
            if (l->isVisibleTo(&panel))
                for (const PaletteEntry& e : l->entries()) listed |= e.key == "agent:" + id && e.title.contains("Recon");
        QVERIFY(listed);

        QPushButton* back = nullptr;
        for (QPushButton* b : panel.findChildren<QPushButton*>())
            if (b->text().contains("Back to chat")) back = b;
        QVERIFY(back);
        back->click();
        QCOMPARE(shown(&panel)->chat()->chatId(), parent);
        reg.update(id, [](rk::SubagentInfo& i) { i.status = rk::SubagentStatus::Completed; });
        reg.removeFinished();
    }

    // Connect a model → Paste an API key: once the key is saved, that provider's models are offered.
    void setupKeyShowsItsModels() {
        QWidget area;
        area.resize(600, 600);
        auto* setup = new rk::ui::GetStarted(&area);
        setup->resize(600, 600);
        rk::ui::ModelPicker picker(&area);
        QObject::connect(setup, &rk::ui::GetStarted::refreshModels, &picker, [&picker] { picker.refresh(); });
        QObject::connect(&picker, &rk::ui::ModelPicker::refreshed, setup, [&] { setup->setModels(picker.models()); });
        area.show();
        setup->start();
        picker.refresh();  // the panel's first refresh, still in flight when the key is saved
        QPushButton* connectBtn = nullptr;
        for (QPushButton* b : setup->findChildren<QPushButton*>())
            if (b->text() == QLatin1String("Connect")) connectBtn = b;
        QComboBox* providers = nullptr;
        for (QComboBox* c : setup->findChildren<QComboBox*>())
            if (c->findData(QStringLiteral("anthropic")) >= 0) providers = c;
        QLineEdit* key = nullptr;
        for (QLineEdit* e : setup->findChildren<QLineEdit*>())
            if (e->echoMode() == QLineEdit::Password) key = e;
        QVERIFY(connectBtn && providers && key);
        providers->setCurrentIndex(providers->findData(QStringLiteral("anthropic")));
        QVERIFY(providers->currentIndex() >= 0);
        key->setText(QStringLiteral("sk-test-setup"));
        connectBtn->click();
        auto modelRows = [&] {
            int n = 0;
            for (rk::ui::SuggestionRow* r : setup->findChildren<rk::ui::SuggestionRow*>())
                if (r->isVisible()) ++n;
            return n;
        };
        const bool shown = rktest::waitFor([&] { return modelRows() > 0; }, 15000);
        rk::writeFileAtomic(rk::paths::omcAgentDir() + "/auth.json", "{}");
        rk::ServiceSession::reloadCredentials();
        QVERIFY2(shown, "no model rows after saving an Anthropic key");
    }

    // A finished thought folds and unfolds when its header line is clicked.
    void thinkingHeaderToggles() {
        rk::ui::TranscriptModel m;
        m.applyTranscript(QJsonArray{
            QJsonObject{{"role", "user"}, {"content", "hi"}},
            QJsonObject{{"role", "assistant"},
                        {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", "line one\n\nline two\n\nline three"}},
                                               QJsonObject{{"type", "text"}, {"text", "Done."}}}}}});
        int row = -1;
        for (int i = 0; i < m.rowCount(); ++i)
            if (m.at(i).kind == rk::ui::ItemKind::Thinking) row = i;
        QVERIFY(row >= 0);
        TranscriptView v;
        v.resize(600, 500);
        v.setTranscriptModel(&m);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        const QModelIndex idx = m.index(row, 0);
        auto clickHeader = [&] {
            const QRect r = v.visualRect(idx);
            QTest::mouseClick(v.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(r.left() + 40, r.top() + 6));
            QCoreApplication::processEvents();
        };
        const int closedH = v.visualRect(idx).height();
        QVERIFY(!m.at(row).expanded);
        clickHeader();
        QVERIFY(m.at(row).expanded);
        QVERIFY(rktest::waitFor([&] { return v.visualRect(idx).height() > closedH; }, 2000));
        clickHeader();
        QVERIFY(!m.at(row).expanded);
        QVERIFY(rktest::waitFor([&] { return v.visualRect(idx).height() == closedH; }, 2000));
    }

    // An image pasted into the composer goes to omc with the next message: omc echoes the message with it.
    void pastedImageGoesWithTheMessage() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        clickTool(&panel, "New chat");
        QImage img(40, 30, QImage::Format_RGB32);
        img.fill(Qt::red);
        auto* mime = new QMimeData;
        mime->setImageData(img);
        QGuiApplication::clipboard()->setMimeData(mime);
        auto* edit = panel.findChild<rk::ui::ComposerEdit*>();
        auto* composer = panel.findChild<rk::ui::Composer*>();
        QVERIFY(edit && composer);
        edit->setFocus();
        edit->paste();
        QCOMPARE(composer->imageCount(), 1);
        QVERIFY(edit->toPlainText().isEmpty());  // the image is not pasted as text
        composer->setText("say:seen");
        QToolButton* send = panel.findChild<QToolButton*>("rk_send");
        QVERIFY(send && send->isEnabled());
        send->click();
        QCOMPARE(composer->imageCount(), 0);
        QVERIFY2(rktest::waitFor([&] { return texts(&panel, ItemKind::User).contains("say:seen\n[1 image]"); }, 30000),
                 qPrintable(texts(&panel, ItemKind::User).join(" | ")));
        QVERIFY(rktest::waitFor([&] { return idle(&panel); }, 30000));
    }

    // The same in a live chat: the streamed thought is done when the turn ends, and its header folds it.
    void liveThinkingToggles() {
        rk::PanelCore panel;
        panel.resize(900, 700);
        panel.show();
        panel.setDatabase(db);
        clickTool(&panel, "New chat");
        panel.submitToCurrentChat("think:weighing the options\\nat length|the answer");
        QVERIFY(rktest::waitFor([&] { return texts(&panel, ItemKind::Text).join('\n').contains("the answer"); }, 30000));
        QVERIFY(rktest::waitFor([&] { return idle(&panel); }, 30000));
        TranscriptModel* m = shown(&panel);
        int row = -1;
        for (int i = 0; i < m->rowCount(); ++i)
            if (m->at(i).kind == ItemKind::Thinking) row = i;
        QVERIFY(row >= 0);
        QVERIFY2(!m->at(row).streaming, "the thought still streams after the turn");
        TranscriptView* v = transcript(&panel);
        const QModelIndex idx = m->index(row, 0);
        const bool was = m->at(row).expanded;
        const QRect r = v->visualRect(idx);
        QTest::mouseClick(v->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(r.left() + 40, r.top() + 6));
        QCOMPARE(m->at(row).expanded, !was);
        const int h = v->visualRect(idx).height();
        QTest::mouseClick(v->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(r.left() + 40, r.top() + 6));
        QCOMPARE(m->at(row).expanded, was);
        QVERIFY(rktest::waitFor([&] { return v->visualRect(idx).height() != h; }, 2000));
    }
};
RK_TEST(UiTest)

// The plan card on its own (no chat): it builds, an unticked step leaves the plan and Approve says how many run.
class PlanCardTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void untickedStepLeavesThePlan() {
        TranscriptItem item;
        item.kind = ItemKind::Decision;
        item.requestId = 1;
        item.payload = QJsonObject{{"kind", "plan"},
                                   {"payload", QJsonObject{{"steps", QJsonArray{"Find `main`", "Rename it", "Comment it"}}}}};
        std::unique_ptr<TranscriptCard> card(TranscriptCard::create(item, nullptr, nullptr));
        QVERIFY(card);
        const QList<QCheckBox*> boxes = card->findChildren<QCheckBox*>();
        QCOMPARE(boxes.size(), 3);
        QPushButton* approve = nullptr;
        for (QPushButton* b : card->findChildren<QPushButton*>())
            if (b->text().startsWith("Approve")) approve = b;
        QVERIFY(approve);
        QCOMPARE(approve->text(), QString("Approve"));
        boxes[1]->setChecked(false);
        QCOMPARE(approve->text(), QString("Approve 2 step(s)"));
        QString answer;
        QObject::connect(card.get(), &TranscriptCard::answered, [&](const QString& a) { answer = a; });
        approve->click();
        QCOMPARE(answer, QString("Approved 2 edited step(s)"));
    }
};
RK_TEST(PlanCardTest)

QJsonArray todoPhases(const char* first, const char* second) {
    return QJsonArray{QJsonObject{{"name", "Work"},
                                  {"tasks", QJsonArray{QJsonObject{{"content", "one"}, {"status", first}},
                                                       QJsonObject{{"content", "two"}, {"status", second}}}}}};
}

// The todo list folds by itself once every task is done, and unfolds when open tasks come back.
class ActivityStripTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void finishedTodoFolds() {
        const auto phases = &todoPhases;
        rk::ui::ActivityStrip strip;
        rk::ui::ChatActivity a;
        a.todoPhases = phases("completed", "in_progress");
        strip.setActivity(a);
        QVERIFY(strip.todoExpanded());
        a.todoPhases = phases("completed", "completed");
        strip.setActivity(a);
        QVERIFY(!strip.todoExpanded());
        strip.setActivity(a);  // still finished: stays folded
        QVERIFY(!strip.todoExpanded());
        a.todoPhases = phases("completed", "pending");
        strip.setActivity(a);
        QVERIFY(strip.todoExpanded());
    }
};
RK_TEST(ActivityStripTest)

#include "test_ui.moc"
