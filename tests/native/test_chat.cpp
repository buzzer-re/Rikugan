// Chat core against the mock model: host tools end to end, exclusivity, cancellation, undo, artifacts.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatSession.h"
#include "common/agent/CommandRouter.h"
#include "common/agent/DbContext.h"
#include "common/agent/Subagents.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/OmcSession.h"
#include "common/tools/MutationLog.h"
#include "common/tools/ToolDispatcher.h"

namespace {

struct Finished {
    QString status, error, text;
    bool done = false;
};

}  // namespace

class ChatTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    rktest::FakeHost* host = nullptr;
    std::shared_ptr<rk::DbContext> db;

    std::unique_ptr<rk::ChatSession> newChat() {
        return std::make_unique<rk::ChatSession>(db, db->chats->create());
    }

    Finished run(rk::ChatSession& chat, const QString& prompt, int ms = 30000) {
        Finished f;
        auto c = QObject::connect(&chat, &rk::ChatSession::promptFinished, [&](const QString& s, const QString& e, const QString& t) {
            f = Finished{s, e, t, true};
        });
        chat.sendPrompt(prompt);
        rktest::waitFor([&] { return f.done; }, ms);
        QObject::disconnect(c);
        return f;
    }

private Q_SLOTS:
    void initTestCase() {
        auto h = std::make_unique<rktest::FakeHost>();
        h->dbPath = dir.path() + "/test.bndb";
        host = h.get();
        db = rk::DbContext::create(std::move(h));
    }

    // every session numbers its host calls host-1, host-2 ...: stopping one chat must not touch another chat's call
    void stopReachesOnlyItsOwnChat() {
        auto a = newChat();
        auto b = newChat();
        Finished fa, fb;
        QObject::connect(a.get(), &rk::ChatSession::promptFinished, [&](const QString& s, const QString& e, const QString& t) { fa = {s, e, t, true}; });
        QObject::connect(b.get(), &rk::ChatSession::promptFinished, [&](const QString& s, const QString& e, const QString& t) { fb = {s, e, t, true}; });
        a->sendPrompt(R"(batch:[["slow_tool",{}]])");
        b->sendPrompt(R"(batch:[["slow_tool",{}]])");
        rktest::waitFor([] { return false; }, 1000);  // both calls run (each is its session's host-1)
        a->stop();
        QVERIFY(rktest::waitFor([&] { return fa.done; }, 10000));
        QCOMPARE(fa.status, QStringLiteral("aborted"));
        QVERIFY2(rktest::waitFor([&] { return fb.done; }, 20000), "the other chat's call was cancelled and never answered");
        QCOMPARE(fb.status, QStringLiteral("completed"));
        QVERIFY2(fb.text.contains("slow done"), qPrintable(fb.text));
    }

    // a command that runs a job of its own waits for the running turn, then runs from the queue
    void commandTypedWhileBusyWaitsItsTurn() {
        auto chat = newChat();
        QStringList outputs;
        QObject::connect(chat.get(), &rk::ChatSession::frame, [&](const QJsonObject& f) {
            if (QJsonDocument(f).toJson().contains("Rikugan Doctor")) outputs << f.value("type").toString();
        });
        chat->setCommandRunner([&](const QString& t) { rk::CommandRouter::submit(*chat, t); });
        bool finished = false;
        QObject::connect(chat.get(), &rk::ChatSession::promptFinished, [&](const QString&, const QString&, const QString&) { finished = true; });
        chat->sendPrompt(R"(seq:[["get_cursor_position",{}]])");
        QVERIFY(chat->busy());
        rk::CommandRouter::submit(*chat, "/doctor");
        QCOMPARE(chat->queued(), QStringList{"/doctor"});
        QVERIFY(outputs.isEmpty());
        QVERIFY(rktest::waitFor([&] { return finished; }, 30000));
        QVERIFY2(rktest::waitFor([&] { return !outputs.isEmpty() && !chat->busy(); }, 30000), "the queued /doctor never ran");
        QVERIFY(chat->queued().isEmpty());
    }

    // a subagent the chat's agent started gives way to a message the user typed: it returns at once, marked
    // interrupted, instead of holding the message back until it finishes
    void toolSubagentYieldsToTheUser() {
        auto chat = newChat();
        rk::ChatContext& ctx = *chat->context();
        ctx.steerPending = 1;
        rk::SubagentSpec spec;
        spec.task = "say:still working";
        spec.yieldsToUser = true;
        rk::SubagentOutcome out;
        std::atomic<bool> done{false};
        std::thread t([&] {
            out = rk::runSubagent(ctx, spec, std::make_shared<rk::CancelToken>());
            done = true;
        });
        QVERIFY(rktest::waitFor([&] { return done.load(); }, 10000));
        t.join();
        QCOMPARE(out.status, QStringLiteral("interrupted"));
        // a mode's subagent keeps going
        spec.yieldsToUser = false;
        done = false;
        std::thread t2([&] {
            out = rk::runSubagent(ctx, spec, std::make_shared<rk::CancelToken>());
            done = true;
        });
        QVERIFY(rktest::waitFor([&] { return done.load(); }, 30000));
        t2.join();
        QCOMPARE(out.status, QStringLiteral("completed"));
        ctx.steerPending = 0;
    }

    // a job's next turn (a mode's next step) does not start on a session that is closing
    void closingSessionStartsNoNewTurn() {
        rk::OmcOptions o;
        o.cwd = db->scratchDir();
        o.model = rk::RikuganConfig::instance().model;
        o.noSession = o.noTools = o.noSkills = o.noMcp = true;
        struct Turns : rk::OmcListener {
            std::atomic<int> started{0};
            void onFrame(std::string_view type, const std::string&) override { started += type == "turn_start" ? 1 : 0; }
            void onHostToolCall(const QJsonObject&) override {}
            void onHostToolCancel(const QString&) override {}
        };
        auto turns = std::make_shared<Turns>();
        auto session = rk::OmcSession::open(o, turns);
        std::atomic<bool> release{false};
        std::atomic<int> status{0};  // 1: error (refused), 2: a turn ran
        session->post([&](rk::OmcSession& s) {
            while (!release) QThread::msleep(10);
            status = s.prompt("say:late").status == QLatin1String("error") ? 1 : 2;
        });
        rktest::waitFor([] { return false; }, 300);
        session->close();
        release = true;
        QVERIFY(session->closeAndWait(10000));
        QCOMPARE(status.load(), 1);
        QCOMPARE(turns->started.load(), 0);
    }

    void hostToolRoundTrip() {
        auto chat = newChat();
        const Finished f = run(*chat, R"(seq:[["get_cursor_position",{}]])");
        QVERIFY(f.done);
        QCOMPARE(f.status, QStringLiteral("completed"));
        QVERIFY2(f.text.contains("Tool said:") && f.text.contains("0x401000"), qPrintable(f.text));
        QVERIFY(!chat->entry().title.isEmpty());
    }

    // omc names the chat after its first exchange (the mock's title is "Fix flaky park tests"); a name the user
    // gave stays
    void omcNamesTheChatUnlessRenamed() {
        auto chat = newChat();
        QVERIFY(run(*chat, "say:hello").done);
        QVERIFY2(rktest::waitFor([&] { return chat->entry().title.contains("Fix flaky park tests"); }, 15000),
                 qPrintable(chat->entry().title));
        QCOMPARE(db->chats->find(chat->chatId())->title, chat->entry().title);  // saved in chats.json

        auto mine = newChat();
        mine->setTitle("Config decryption");
        QVERIFY(run(*mine, "say:hello").done);
        QTest::qWait(1500);  // the title side request would have landed by now
        QCOMPARE(mine->entry().title, QString("Config decryption"));
    }

    // a session omc refused to start (here: a model it does not know) is tried again on the next prompt, so a
    // model connected afterwards works without restarting the host
    void failedStartIsRetried() {
        rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
        const QString good = cfg.model;
        cfg.model = "nosuchprovider/nosuchmodel";
        auto chat = newChat();
        const Finished bad = run(*chat, "say:first");
        cfg.model = good;
        QVERIFY(bad.done);
        QCOMPARE(bad.status, QStringLiteral("error"));
        const Finished ok = run(*chat, "say:second");
        QVERIFY(ok.done);
        QCOMPARE(ok.status, QStringLiteral("completed"));
        QVERIFY2(ok.text.contains("second"), qPrintable(ok.text));
    }

    void exclusiveInOrderAndUndo() {
        auto chat = newChat();
        host->events.clear();
        const Finished f = run(*chat, R"(batch:[["rename_function",{"address":"0x401000","new_name":"a"}],["rename_function",{"address":"0x402000","new_name":"b"}]])");
        QVERIFY(f.done);
        std::vector<rktest::FakeHost::Event> ev;
        {
            std::lock_guard<std::mutex> g(host->mu);
            ev = host->events;
        }
        QCOMPARE(int(ev.size()), 4);
        QCOMPARE(ev[0].what, QStringLiteral("start 0x401000"));
        QCOMPARE(ev[1].what, QStringLiteral("end 0x401000"));  // the second starts only after the first ended
        QCOMPARE(ev[2].what, QStringLiteral("start 0x402000"));
        QCOMPARE(host->names.value(0x401000), QStringLiteral("a"));

        auto ctx = chat->context();
        QCOMPARE(ctx->mutations->size(), 2);
        const rk::MutationLog::UndoOutcome u = ctx->mutations->undo(1, [&](const QString& t, const QJsonObject& a) {
            return rk::ToolDispatcher::runDirect(*db->host, db->registry, ctx.get(), t, a);
        });
        QCOMPARE(u.text, QStringLiteral("Undid 1 mutation(s)."));
        QCOMPARE(host->names.value(0x402000), QStringLiteral("sub_402000"));
        QCOMPARE(host->names.value(0x401000), QStringLiteral("a"));
        QCOMPARE(ctx->mutations->size(), 1);
    }

    void cancelReachesTool() {
        auto chat = newChat();
        host->slowCancelled = false;
        Finished f;
        QObject::connect(chat.get(), &rk::ChatSession::promptFinished,
                         [&](const QString& s, const QString& e, const QString& t) { f = Finished{s, e, t, true}; });
        chat->sendPrompt(R"(batch:[["slow_tool",{}]])");
        rktest::waitFor([] { return false; }, 1000);  // let the call start
        QElapsedTimer t;
        t.start();
        chat->stop();
        QVERIFY(rktest::waitFor([&] { return host->slowCancelled.load(); }, 2000));
        QVERIFY2(t.elapsed() < 1000, "the tool saw the cancel late");
        QVERIFY(rktest::waitFor([&] { return f.done; }, 10000));
        QCOMPARE(f.status, QStringLiteral("aborted"));
    }

    // a message typed while the agent works joins that run (no second prompt), and leaves the queue once taken
    void messageTypedWhileWorkingJoinsTheRun() {
        auto chat = newChat();
        int finished = 0;
        bool seen = false;
        QObject::connect(chat.get(), &rk::ChatSession::promptFinished, [&](const QString&, const QString&, const QString&) { ++finished; });
        QObject::connect(chat.get(), &rk::ChatSession::frame, [&](const QJsonObject& f) {
            if (f.value("type").toString() == "message_start" && f.value("message").toObject().value("role").toString() == "user" &&
                QJsonDocument(f).toJson().contains("look at main too"))
                seen = true;
        });
        chat->sendPrompt(R"(batch:[["slow_tool",{}]])");
        rktest::waitFor([] { return false; }, 500);  // the call runs
        chat->sendPrompt("look at main too");
        QCOMPARE(chat->queued(), QStringList{"look at main too"});
        QVERIFY(rktest::waitFor([&] { return finished > 0 && !chat->busy(); }, 30000));
        QTest::qWait(300);  // a second prompt would start now
        QVERIFY(seen);
        QCOMPARE(finished, 1);
        QVERIFY(chat->queued().isEmpty());
    }

    void oversizedResultGoesToArtifact() {
        auto chat = newChat();
        const Finished f = run(*chat, R"(seq:[["big_tool",{}]])");
        QVERIFY(f.done);
        QVERIFY2(f.text.contains("chars elided; full output:"), qPrintable(f.text.left(300)));
        QVERIFY(QDir(db->artifactsDir()).entryList(QDir::Files).size() >= 1);
    }

    void transcriptReplayAfterResume() {
        rk::ChatEntry entry;
        {
            auto chat = newChat();
            QVERIFY(run(*chat, "say:remember me").done);
            entry = chat->entry();
            QVERIFY2(!entry.sessionFile.isEmpty(), "omc wrote no session file");
        }
        rk::ChatSession again(db, entry);
        QSignalSpy spy(&again, &rk::ChatSession::transcriptLoaded);
        again.ensureOpen();
        QVERIFY(rktest::waitFor([&] { return spy.count() > 0; }, 10000));
        const QJsonArray msgs = spy.at(0).at(0).toJsonArray();
        QVERIFY(msgs.size() >= 2);
    }
};
RK_TEST(ChatTest)

#include "test_chat.moc"
