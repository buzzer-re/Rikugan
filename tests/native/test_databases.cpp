// The databases a chat can reach beyond its own: open_database asks first and opens the file in a host tab,
// list_databases shows them, and every database is found by id or by file path.
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatContext.h"
#include "common/agent/Databases.h"
#include "common/agent/DbContext.h"
#include "common/tools/ToolDispatcher.h"

namespace {

class NamedHost : public rktest::FakeHost {
public:
    explicit NamedHost(QString id) : id_(std::move(id)) {}
    QString databaseInstanceId() override { return id_; }

private:
    QString id_;
};

// Answers every question with `reply` and remembers what it was asked.
class FakeUi : public rk::ChatUi {
public:
    QString reply;
    QStringList asked;
    rk::ApprovalChoice askApproval(const rk::ApprovalRequest&, const std::shared_ptr<rk::CancelToken>&) override {
        return rk::ApprovalChoice::Deny;
    }
    rk::UserAnswer askUser(const QString& q, const QStringList&, const std::shared_ptr<rk::CancelToken>&) override {
        asked << q;
        return rk::UserAnswer{false, reply};
    }
    rk::UserAnswer askDecision(const QString&, const QJsonObject&, const std::shared_ptr<rk::CancelToken>&) override {
        return rk::UserAnswer{true, {}};
    }
    void postUiEvent(const QJsonObject&) override {}
};

std::shared_ptr<rk::DbContext> makeDb(const QString& id, const QString& path) {
    auto h = std::make_unique<NamedHost>(id);
    h->dbPath = path;
    return rk::DbContext::create(std::move(h));
}

}  // namespace

class DatabasesTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

    QString touch(const QString& name) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly)) qFatal("cannot create %s", qPrintable(f.fileName()));
        return f.fileName();
    }

private Q_SLOTS:
    void openAsksThenOpensATab() {
        const auto mine = makeDb("dbs-mine", touch("mine.bndb"));
        const auto userTab = makeDb("dbs-tab", touch("tab.bndb"));
        const QString other = touch("other.elf");
        std::vector<std::shared_ptr<rk::DbContext>> tabs;  // the host holds its tabs' databases
        rk::databases::setTabOpener([&](const QString& path, const std::shared_ptr<rk::CancelToken>&, QString*) {
            tabs.push_back(makeDb("dbs-opened", path));
            return tabs.back();
        });

        auto ui = std::make_shared<FakeUi>();
        rk::ChatContext chat;
        chat.dbId = mine->dbId;
        chat.db = mine;
        chat.host = mine->host.get();
        chat.registry = &mine->registry;
        chat.cache = &mine->cache;
        chat.ui = ui;
        auto run = [&](const QString& tool, const QJsonObject& args) {
            return rk::ToolDispatcher::runDirect(*mine->host, mine->registry, &chat, tool, args);
        };

        ui->reply = "Don't open";
        QVERIFY(run("open_database", {{"path", other}}).contains("declined"));
        QVERIFY(tabs.empty());
        QCOMPARE(ui->asked.size(), 1);

        ui->reply = "Open it";
        const QString opened = run("open_database", {{"path", other}});
        QVERIFY2(opened.contains("dbs-opened") && opened.contains("new tab"), qPrintable(opened));
        QCOMPARE(tabs.size(), size_t(1));
        // the same file again: already open, no second tab and no second question
        QVERIFY(run("open_database", {{"path", other}}).contains("Already open"));
        QCOMPARE(tabs.size(), size_t(1));
        QCOMPARE(ui->asked.size(), 2);

        const QString list = run("list_databases", {});
        QVERIFY2(list.contains("dbs-mine") && list.contains("this chat's database"), qPrintable(list));
        QVERIFY(list.contains("dbs-tab") && list.contains("dbs-opened"));
        QCOMPARE(rk::databases::find(other)->dbId, QString("dbs-opened"));  // by path too
        QVERIFY_THROWS_EXCEPTION(rk::ToolError, run("ask_database", {{"database", "dbs-mine"}, {"task", "x"}}));

        rk::databases::setTabOpener(nullptr);
    }
};
RK_TEST(DatabasesTest)

#include "test_databases.moc"
