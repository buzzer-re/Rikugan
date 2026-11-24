// Recent chats offered while no database is open, and the id a reopened binary takes.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "TestEnv.h"
#include "common/agent/ChatIndex.h"
#include "common/agent/RecentSessions.h"
#include "common/config/Paths.h"

namespace {

QString touch(const QString& path) {
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    return path;
}

void addChat(const QString& dbId, const QString& title, const QString& sessionFile, qint64 lastActiveAt) {
    rk::ChatIndex index(rk::paths::sessionsDir(dbId));
    rk::ChatEntry e = index.create();
    e.title = title;
    e.sessionFile = sessionFile;
    e.lastActiveAt = lastActiveAt;
    index.update(e);
}

}  // namespace

class RecentSessionsTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

private Q_SLOTS:
    void listsOpenableChatsOfTheHost() {
        const QString a = touch(dir.filePath("a.bndb"));
        const QString b = touch(dir.filePath("b.elf"));
        rk::recordDatabaseLocation(rk::paths::sessionsDir("recent-a"), {"binja", a, a});
        addChat("recent-a", "answered", "s1.jsonl", 300);
        addChat("recent-a", "never answered", QString(), 400);
        // the database is gone, the binary is still there
        rk::recordDatabaseLocation(rk::paths::sessionsDir("recent-b"), {"binja", dir.filePath("gone.bndb"), b});
        addChat("recent-b", "from the binary", "s3.jsonl", 500);
        rk::recordDatabaseLocation(rk::paths::sessionsDir("recent-c"), {"ida", a, a});
        addChat("recent-c", "other host", "s4.jsonl", 600);
        rk::recordDatabaseLocation(rk::paths::sessionsDir("recent-d"), {"binja", dir.filePath("x.bndb"), dir.filePath("x")});
        addChat("recent-d", "nothing to open", "s5.jsonl", 700);

        QStringList got;
        for (const rk::RecentChat& r : rk::recentChats("binja", 1000))
            if (r.dbId.startsWith("recent-")) got << r.title + "@" + QFileInfo(r.file).fileName();
        QCOMPARE(got, (QStringList{"from the binary@b.elf", "answered@a.bndb"}));
    }

    void expectedIdGoesToThatFileOnce() {
        const QString a = touch(dir.filePath("e.bin"));
        rk::expectSession(a, "db-1");
        QVERIFY(rk::takeExpectedSession(touch(dir.filePath("f.bin"))).isEmpty());
        QCOMPARE(rk::takeExpectedSession(dir.path() + "/./e.bin"), QStringLiteral("db-1"));
        QVERIFY(rk::takeExpectedSession(a).isEmpty());
        rk::expectSession(a, "db-2");
        rk::clearExpectedSession();
        QVERIFY(rk::takeExpectedSession(a).isEmpty());
    }
};
RK_TEST(RecentSessionsTest)

#include "test_recent_sessions.moc"
