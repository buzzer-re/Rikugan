// Slash commands against omc's mock model: commands unsafe in a host process never reach the model, panel
// commands become UI actions, and /goal runs omc's goal mode until it settles.
#include <QTemporaryDir>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatSession.h"
#include "common/agent/CommandRouter.h"
#include "common/agent/DbContext.h"

class CommandsTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    std::shared_ptr<rk::DbContext> db;

    std::unique_ptr<rk::ChatSession> newChat() { return std::make_unique<rk::ChatSession>(db, db->chats->create()); }

    // Frames the chat emits within `ms` after submitting `text`.
    QList<QJsonObject> framesAfter(rk::ChatSession& chat, const QString& text, int ms) {
        QList<QJsonObject> frames;
        auto c = QObject::connect(&chat, &rk::ChatSession::frame, [&](const QJsonObject& f) { frames << f; });
        rk::CommandRouter::submit(chat, text);
        rktest::waitFor([] { return false; }, ms);
        QObject::disconnect(c);
        return frames;
    }

    static QStringList types(const QList<QJsonObject>& frames) {
        QStringList t;
        for (const QJsonObject& f : frames) t << f.value("type").toString();
        return t;
    }

private Q_SLOTS:
    void initTestCase() {
        auto h = std::make_unique<rktest::FakeHost>();
        h->dbPath = dir.path() + "/cmd.bndb";
        db = rk::DbContext::create(std::move(h));
    }

    void unsafeCommandsNeverReachTheModel() {
        auto chat = newChat();
        for (const QString& cmd : {QStringLiteral("/move /tmp"), QStringLiteral("/dump"), QStringLiteral("/usage reset"),
                                   QStringLiteral("/nonsense")}) {
            const QList<QJsonObject> frames = framesAfter(*chat, cmd, 200);
            QCOMPARE(types(frames), QStringList{"rikugan_command_output"});
            QVERIFY(frames.first().value("isError").toBool());
            QVERIFY(!chat->busy());
        }
    }

    void panelCommandsBecomeUiActions() {
        auto chat = newChat();
        const QList<QJsonObject> frames = framesAfter(*chat, QStringLiteral("/login anthropic"), 100);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.first().value("action").toString(), QStringLiteral("login"));
        QCOMPARE(frames.first().value("arg").toString(), QStringLiteral("anthropic"));
    }

    void goalRunsUntilSettled() {
        auto chat = newChat();
        QList<QJsonObject> goals;
        auto c = QObject::connect(chat.get(), &rk::ChatSession::frame, [&](const QJsonObject& f) {
            if (f.value("type").toString() == QLatin1String("goal_updated")) goals << f;
        });
        rk::CommandRouter::submit(*chat, QStringLiteral("/goal say:working on it"));
        QVERIFY(chat->busy());
        // omc continues the goal with hidden turns until it stops making progress, then the session settles
        QVERIFY(rktest::waitFor([&] { return !chat->busy(); }, 60000));
        QObject::disconnect(c);
        QVERIFY(!goals.isEmpty());
        QCOMPARE(goals.first().value("goal").toObject().value("objective").toString(), QStringLiteral("say:working on it"));
        // the goal is still there to inspect, then it is dropped
        const QList<QJsonObject> shown = framesAfter(*chat, QStringLiteral("/goal show"), 500);
        QVERIFY(!shown.isEmpty());
        QVERIFY2(shown.last().value("text").toString().contains("say:working on it"), qPrintable(shown.last().value("text").toString()));
        framesAfter(*chat, QStringLiteral("/goal drop"), 500);
        const QList<QJsonObject> after = framesAfter(*chat, QStringLiteral("/goal"), 500);
        QVERIFY(!after.isEmpty());
        QVERIFY2(after.last().value("text").toString().contains("No goal"), qPrintable(after.last().value("text").toString()));
    }
};

RK_TEST(CommandsTest)
#include "test_commands.moc"
