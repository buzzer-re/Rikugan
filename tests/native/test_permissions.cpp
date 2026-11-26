// Disk / shell tool approvals against omc's mock model: omc asks before each read/write/edit/bash call and Rikugan
// answers from the chat's grants (Permissions.h).
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/config/RikuganConfig.h"

namespace {

struct Run {
    QString text;
    bool done = false;
    QStringList asked;  // "kind:tool" of every permission card, in order
};

}  // namespace

class PermissionsTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    std::shared_ptr<rk::DbContext> db;

    std::unique_ptr<rk::ChatSession> newChat() { return std::make_unique<rk::ChatSession>(db, db->chats->create()); }

    // Runs `prompt`, answering every permission card with `choice`.
    Run run(rk::ChatSession& chat, const QString& prompt, rk::PermissionChoice choice) {
        Run r;
        auto a = QObject::connect(&chat, &rk::ChatSession::frame, [&](const QJsonObject& f) {
            if (f.value("type").toString() != QLatin1String("rikugan_permission")) return;
            r.asked << f.value("kind").toString() + ':' + f.value("tool").toString();
            chat.answerPermission(f.value("key").toString(), choice);
        });
        auto b = QObject::connect(&chat, &rk::ChatSession::promptFinished, [&](const QString&, const QString&, const QString& t) {
            r.text = t;
            r.done = true;
        });
        chat.sendPrompt(prompt);
        rktest::waitFor([&] { return r.done; }, 30000);
        QObject::disconnect(a);
        QObject::disconnect(b);
        return r;
    }

private Q_SLOTS:
    void initTestCase() {
        auto h = std::make_unique<rktest::FakeHost>();
        h->dbPath = dir.path() + "/perm.bndb";
        db = rk::DbContext::create(std::move(h));
        QFile f(db->scratchDir() + "/notes.md");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("config key at 0x40a000\n");
    }

    void init() {
        rk::RikuganConfig::instance().autoAllowFiles = false;
        rk::RikuganConfig::instance().autoAllowCommands = false;
    }

    void allowForChatAsksOncePerKind() {
        auto chat = newChat();
        Run r = run(*chat, R"(seq:[["read",{"path":"notes.md"}]])", rk::PermissionChoice::AllowChat);
        QVERIFY(r.done);
        QCOMPARE(r.asked, QStringList{"read:read"});
        QVERIFY2(r.text.contains("config key"), qPrintable(r.text));
        // later reads and greps (same kind) run without a card; bash is its own kind and asks
        r = run(*chat, R"(seq:[["read",{"path":"notes.md"}],["grep",{"pattern":"key"}],["bash",{"command":"echo hi"}]])",
                rk::PermissionChoice::AllowOnce);
        QVERIFY(r.done);
        QCOMPARE(r.asked, QStringList{"bash:bash"});
    }

    void allowOnceAsksAgain() {
        auto chat = newChat();
        Run r = run(*chat, R"(seq:[["read",{"path":"notes.md"}],["read",{"path":"notes.md"}]])", rk::PermissionChoice::AllowOnce);
        QVERIFY(r.done);
        QCOMPARE(r.asked, (QStringList{"read:read", "read:read"}));
    }

    void denyBlocksTheCall() {
        auto chat = newChat();
        const QString target = db->scratchDir() + "/denied.txt";
        QFile::remove(target);
        const Run r = run(*chat, R"(seq:[["write",{"path":"denied.txt","content":"x"}]])", rk::PermissionChoice::Deny);
        QVERIFY(r.done);
        QCOMPARE(r.asked, QStringList{"write:write"});
        QVERIFY2(r.text.contains("denied by user"), qPrintable(r.text));
        QVERIFY(!QFile::exists(target));
    }

    void grantAnswersParallelRequests() {
        // two reads in one message run in parallel: both ask, and allowing the first for the chat answers the second
        auto chat = newChat();
        int cards = 0;
        auto a = QObject::connect(chat.get(), &rk::ChatSession::frame, [&](const QJsonObject& f) {
            if (f.value("type").toString() != QLatin1String("rikugan_permission")) return;
            if (++cards == 1) {
                const QString key = f.value("key").toString();
                // answer only once both requests are pending
                QTimer::singleShot(300, chat.get(), [&, key] { chat->answerPermission(key, rk::PermissionChoice::AllowChat); });
            }
        });
        bool done = false;
        QString text;
        auto b = QObject::connect(chat.get(), &rk::ChatSession::promptFinished, [&](const QString&, const QString&, const QString& t) {
            text = t;
            done = true;
        });
        chat->sendPrompt(R"(batch:[["read",{"path":"notes.md"}],["grep",{"pattern":"key"}]])");
        QVERIFY(rktest::waitFor([&] { return done; }, 30000));
        QObject::disconnect(a);
        QObject::disconnect(b);
        QCOMPARE(cards, 2);
        QVERIFY2(!text.contains("denied"), qPrintable(text));
        QCOMPARE(chat->context()->permissions->pendingCount(), 0);
    }

    void globalSwitchesSkipAsking() {
        rk::RikuganConfig::instance().autoAllowFiles = true;
        auto chat = newChat();
        Run r = run(*chat, R"(seq:[["read",{"path":"notes.md"}],["write",{"path":"ok.txt","content":"y"}]])", rk::PermissionChoice::Deny);
        QVERIFY(r.done);
        QVERIFY(r.asked.isEmpty());
        QVERIFY(QFile::exists(db->scratchDir() + "/ok.txt"));
        // files on does not cover commands
        r = run(*chat, R"(seq:[["bash",{"command":"echo hi"}]])", rk::PermissionChoice::Deny);
        QCOMPARE(r.asked, QStringList{"bash:bash"});
        rk::RikuganConfig::instance().autoAllowCommands = true;
        r = run(*chat, R"(seq:[["bash",{"command":"echo hi"}]])", rk::PermissionChoice::Deny);
        QVERIFY(r.asked.isEmpty());
        QVERIFY2(r.text.contains("hi"), qPrintable(r.text));
    }

    void stopCancelsPendingCards() {
        auto chat = newChat();
        QString resolved;
        auto a = QObject::connect(chat.get(), &rk::ChatSession::frame, [&](const QJsonObject& f) {
            const QString t = f.value("type").toString();
            if (t == QLatin1String("rikugan_permission")) chat->stop();
            if (t == QLatin1String("rikugan_permission_resolved")) resolved = f.value("outcome").toString();
        });
        bool done = false;
        auto b = QObject::connect(chat.get(), &rk::ChatSession::promptFinished, [&] { done = true; });
        chat->sendPrompt(R"(seq:[["read",{"path":"notes.md"}]])");
        QVERIFY(rktest::waitFor([&] { return done; }, 30000));
        QObject::disconnect(a);
        QObject::disconnect(b);
        QCOMPARE(resolved, QStringLiteral("cancelled"));
        QCOMPARE(chat->context()->permissions->pendingCount(), 0);
    }

    void profileFiltersOmcToolsAndFollowsSettings() {
        // omc's own tools pass the profile's IOC filter, and a profile switched in Settings reaches an open chat
        rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
        cfg.autoAllowFiles = true;
        QFile f(db->scratchDir() + "/c2.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("beacon to 203.0.113.7\n");
        f.close();
        cfg.activeProfile = QStringLiteral("default");
        auto chat = newChat();
        Run r = run(*chat, R"(seq:[["read",{"path":"c2.txt"}]])", rk::PermissionChoice::AllowOnce);
        QVERIFY(r.done);
        QVERIFY2(r.text.contains("203.0.113.7"), qPrintable(r.text));

        cfg.activeProfile = QStringLiteral("private");
        chat->settingsChanged();
        r = run(*chat, R"(seq:[["read",{"path":"c2.txt"}],["grep",{"pattern":"beacon"}]])", rk::PermissionChoice::AllowOnce);
        cfg.activeProfile = QStringLiteral("default");
        QVERIFY(r.done);
        QVERIFY2(!r.text.contains("203.0.113.7") && r.text.contains("[IP_REDACTED]"), qPrintable(r.text));
    }

    // "Allow for this chat" on a database-change card covers the chat's later changes; "Allow" covers one
    void databaseChangeAllowedForTheChat() {
        rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
        cfg.approveMutations = true;
        auto* host = static_cast<rktest::FakeHost*>(db->host.get());
        const QString twoRenames =
            R"(batch:[["rename_function",{"address":"0x401000","new_name":"%1_a"}],["rename_function",{"address":"0x402000","new_name":"%1_b"}]])";
        auto cardsFor = [&](rk::ChatSession& chat, const QString& prompt, rk::ApprovalChoice choice) {
            int cards = 0;
            auto c = QObject::connect(&chat, &rk::ChatSession::approvalRequested, [&](int id, const rk::ApprovalRequest&) {
                ++cards;
                chat.answerApproval(id, choice);
            });
            const Run r = run(chat, prompt, rk::PermissionChoice::AllowOnce);
            QObject::disconnect(c);
            return r.done ? cards : -1;
        };
        auto chat = newChat();
        const int once = cardsFor(*chat, twoRenames.arg("once"), rk::ApprovalChoice::Allow);
        const int forChat = cardsFor(*chat, twoRenames.arg("chat"), rk::ApprovalChoice::AlwaysAllow);
        const int after = cardsFor(*chat, twoRenames.arg("after"), rk::ApprovalChoice::Deny);
        cfg.approveMutations = false;
        QCOMPARE(once, 2);
        QCOMPARE(forChat, 1);
        QCOMPARE(after, 0);
        std::lock_guard<std::mutex> g(host->mu);
        QCOMPARE(host->names.value(0x401000), QStringLiteral("after_a"));
        QCOMPARE(host->names.value(0x402000), QStringLiteral("after_b"));
    }

    // A script that changes the database (execute_python) shows its code on one card: that answer is its approval,
    // with or without approve_mutations, and it is never asked again as a database change.
    void scriptAsksOnce() {
        rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
        std::atomic<int> runs{0};
        db->registry.add(rk::ToolSpec{"run_db_script", "test", "Runs a script that may change the database.",
                                      {rk::ToolParam{"code", rk::ToolParam::String, "Code", {}, {}, {}}},
                                      rk::Mutating | rk::Script, 20000, [&runs](rk::ToolCall&) {
                                          ++runs;
                                          return rk::ToolResult::ok(QStringLiteral("done"));
                                      }});
        auto chat = newChat();
        QList<rk::ApprovalRequest::Kind> kinds;
        auto c = QObject::connect(chat.get(), &rk::ChatSession::approvalRequested, [&](int id, const rk::ApprovalRequest& r) {
            kinds << r.kind;
            chat->answerApproval(id, rk::ApprovalChoice::Allow);
        });
        cfg.approveMutations = true;
        const Run r = run(*chat, R"js(batch:[["run_db_script",{"code":"print(1)"}]])js", rk::PermissionChoice::AllowOnce);
        cfg.approveMutations = false;
        QObject::disconnect(c);
        QVERIFY(r.done);
        QCOMPARE(runs.load(), 1);
        QCOMPARE(kinds, (QList<rk::ApprovalRequest::Kind>{rk::ApprovalRequest::Script}));
    }

    void cleanupTestCase() {
        rk::RikuganConfig::instance().autoAllowFiles = false;
        rk::RikuganConfig::instance().autoAllowCommands = false;
    }
};

RK_TEST(PermissionsTest)
#include "test_permissions.moc"
