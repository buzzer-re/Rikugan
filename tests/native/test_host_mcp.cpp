// The host's own MCP server (Binary Ninja's): the client survives a server restart and reads streamed replies, the
// server's tools replace Rikugan's analysis tools only while the user has them on, a tool that may change the
// database is gated and recorded like a mutation, every call first selects the chat's database, and the panel
// remembers the user's answer per database.
#include <atomic>
#include <thread>

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSocket = SOCKET;
constexpr TestSocket kNoSocket = INVALID_SOCKET;
inline void closeSocket(TestSocket s) { ::closesocket(s); }
#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL 0
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSocket = int;
constexpr TestSocket kNoSocket = -1;
inline void closeSocket(TestSocket s) { ::close(s); }
#endif

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatContext.h"
#include "common/agent/ChatTools.h"
#include "common/agent/DbContext.h"
#include "common/agent/HostMcp.h"
#include "common/agent/RecentSessions.h"
#include "common/mcp/McpClient.h"
#include "common/tools/ToolDispatcher.h"
#include "common/ui/console/HostMcpBar.h"

namespace {

// An MCP server like Binary Ninja's: initialize hands out a session, a request with a session it does not know gets
// 400, tools/call answers "called <tool> <args>".
class FakeMcpServer {
public:
    struct Call {
        QString tool;
        QJsonObject args;
    };

    FakeMcpServer() {
#ifdef _WIN32
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 8) != 0) qFatal("fake MCP server");
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeMcpServer() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        closeSocket(fd_);
        thread_.join();
    }

    rk::McpEndpoint endpoint() const {
        rk::McpEndpoint e;
        e.host = QStringLiteral("127.0.0.1");
        e.port = port_;
        e.path = QStringLiteral("/mcp");
        return e;
    }
    // a restart: every session is forgotten
    void restart() {
        std::lock_guard<std::mutex> g(mu_);
        sessions_.clear();
    }
    void setStreamed(bool on) { streamed_ = on; }
    QList<Call> calls() {
        std::lock_guard<std::mutex> g(mu_);
        return calls_;
    }
    int initializations() const { return inits_; }

private:
    void serve() {
        while (!stop_) {
            const TestSocket c = ::accept(fd_, nullptr, nullptr);
            if (c == kNoSocket) return;
            handle(c);
            closeSocket(c);
        }
    }

    void handle(TestSocket c) {
        QByteArray in;
        char buf[4096];
        qsizetype end = -1, length = 0;
        while (end < 0 || in.size() < end + 4 + length) {
            const auto n = ::recv(c, buf, int(sizeof buf), 0);
            if (n <= 0) return;
            in.append(buf, n);
            if (end < 0 && (end = in.indexOf("\r\n\r\n")) >= 0) {
                for (const QByteArray& line : in.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
            }
        }
        QByteArray session;
        for (const QByteArray& line : in.left(end).split('\n'))
            if (line.toLower().startsWith("mcp-session-id:")) session = line.mid(15).trimmed();
        const QJsonObject req = QJsonDocument::fromJson(in.mid(end + 4, length)).object();
        const QString method = req.value(QStringLiteral("method")).toString();
        const QJsonValue id = req.value(QStringLiteral("id"));

        if (method == QLatin1String("initialize")) {
            ++inits_;
            const QByteArray sid = "s" + QByteArray::number(inits_.load());
            {
                std::lock_guard<std::mutex> g(mu_);
                sessions_ << sid;
            }
            reply(c, 200, {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"protocolVersion", "2025-11-25"}}}}, sid);
            return;
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!sessions_.contains(session)) {
                const QByteArray body = "Missing or invalid MCP-Session-Id";
                writeAll(c, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) +
                                "\r\n\r\n" + body);
                return;
            }
        }
        if (method.startsWith(QLatin1String("notifications/"))) {
            writeAll(c, "HTTP/1.1 202 Accepted\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        QJsonObject result;
        if (method == QLatin1String("tools/list")) {
            auto tool = [](const char* name) {
                return QJsonObject{{"name", name},
                                   {"description", QStringLiteral("%1 from the host").arg(QLatin1String(name))},
                                   {"inputSchema", QJsonObject{{"type", "object"},
                                                               {"properties", QJsonObject{{"address", QJsonObject{{"type", "string"}}}}}}}};
            };
            result = QJsonObject{{"tools", QJsonArray{tool("bn_function_list"), tool("bn_symbol_rename"),
                                                      tool("bn_binary_view_set_active")}}};
        } else if (method == QLatin1String("tools/call")) {
            const QJsonObject p = req.value(QStringLiteral("params")).toObject();
            const QString tool = p.value(QStringLiteral("name")).toString();
            const QJsonObject args = p.value(QStringLiteral("arguments")).toObject();
            {
                std::lock_guard<std::mutex> g(mu_);
                calls_ << Call{tool, args};
            }
            const QString text = QStringLiteral("called %1 %2").arg(tool, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
            result = QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}, {"isError", false}};
        }
        const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        if (streamed_) {
            // an event stream, chunked, with a progress notification ahead of the reply
            const QByteArray events = "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n"
                                      "event: message\ndata: " +
                                      QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n\n";
            QByteArray chunked;
            for (qsizetype i = 0; i < events.size(); i += 40) {
                const QByteArray part = events.mid(i, 40);
                chunked += QByteArray::number(part.size(), 16) + "\r\n" + part + "\r\n";
            }
            chunked += "0\r\n\r\n";
            writeAll(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                            chunked);
            return;
        }
        reply(c, 200, msg, {});
    }

    static void writeAll(TestSocket c, const QByteArray& data) {
        qsizetype off = 0;
        while (off < data.size()) {
            const auto n = ::send(c, data.constData() + off, int(data.size() - off), MSG_NOSIGNAL);
            if (n <= 0) return;
            off += n;
        }
    }
    static void reply(TestSocket c, int status, const QJsonObject& msg, const QByteArray& session) {
        const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " OK\r\nConnection: close\r\nContent-Type: application/json\r\n";
        if (!session.isEmpty()) head += "MCP-Session-Id: " + session + "\r\n";
        writeAll(c, head + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    }

    TestSocket fd_ = kNoSocket;
    quint16 port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> streamed_{false};
    std::atomic<int> inits_{0};
    std::mutex mu_;
    QList<QByteArray> sessions_;
    QList<Call> calls_;
};

struct Fixture {
    FakeMcpServer server;
    QStringList selected;  // hosts the select hook pointed the server at, in order (with the next call)
    std::mutex mu;

    Fixture() {
        rk::HostMcp::Hooks h;
        h.serverName = QStringLiteral("Binary Ninja");
        const rk::McpEndpoint e = server.endpoint();
        h.locate = [e] {
            rk::HostMcp::Location loc;
            loc.endpoint = e;
            loc.enabled = true;
            return loc;
        };
        h.select = [this](rk::Host& host, rk::McpClient&, const rk::CancelToken*) {
            std::lock_guard<std::mutex> g(mu);
            selected << host.databaseInstanceId();
        };
        h.readOnly = [](const QJsonObject& t) { return t.value(QStringLiteral("name")).toString().endsWith(QLatin1String("_list")); };
        h.hidden = [](const QString& n) { return n == QLatin1String("bn_binary_view_set_active"); };
        rk::HostMcp::instance().setHooks(std::move(h));
    }
    ~Fixture() { rk::HostMcp::instance().setHooks({}); }

    // what HostMcp::probe found, after it reported on the UI thread
    rk::HostMcp::Probe probe() {
        std::optional<rk::HostMcp::Probe> got;
        rk::HostMcp::instance().probe([&](const rk::HostMcp::Probe& p) { got = p; });
        if (!QTest::qWaitFor([&] { return got.has_value(); }, 10000)) qFatal("probe did not finish");
        return *got;
    }
};

class DbHost : public rktest::FakeHost {
public:
    explicit DbHost(QString id) : id_(std::move(id)) {}
    QString databaseInstanceId() override { return id_; }

private:
    QString id_;
};

std::shared_ptr<rk::DbContext> makeDb(const QString& id, const QString& path) {
    auto h = std::make_unique<DbHost>(id);
    h->dbPath = path;
    return rk::DbContext::create(std::move(h));
}

void bind(rk::ChatContext& chat, const std::shared_ptr<rk::DbContext>& db) {
    chat.dbId = db->dbId;
    chat.db = db;
    chat.host = db->host.get();
    chat.registry = &db->registry;
    chat.cache = &db->cache;
}

QStringList toolNames(const rk::ChatContext& chat) {
    QStringList out;
    for (const QJsonValue& t : rk::chatHostTools(chat)) out << t.toObject().value(QStringLiteral("name")).toString();
    return out;
}

}  // namespace

class HostMcpTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

private Q_SLOTS:
    void restartedServerGetsANewSession() {
        FakeMcpServer server;
        rk::McpClient client(server.endpoint());
        client.initialize(nullptr, 3000);
        server.restart();
        const QJsonObject r = client.callTool(QStringLiteral("bn_function_list"), QJsonObject{{"address", "0x10"}}, nullptr, 3000);
        QCOMPARE(rk::McpClient::resultText(r), QStringLiteral("called bn_function_list {\"address\":\"0x10\"}"));
        QCOMPARE(server.initializations(), 2);
    }

    void streamedReplyIsRead() {
        FakeMcpServer server;
        server.setStreamed(true);
        rk::McpClient client(server.endpoint());
        const QJsonObject r = client.callTool(QStringLiteral("bn_function_list"), QJsonObject{{"address", "0x20"}}, nullptr, 3000);
        QCOMPARE(rk::McpClient::resultText(r), QStringLiteral("called bn_function_list {\"address\":\"0x20\"}"));
    }

    void hostToolsReplaceAnalysisToolsOnlyWhileOn() {
        Fixture f;
        const rk::HostMcp::Probe p = f.probe();
        QVERIFY2(p.ok, qPrintable(p.error));
        QCOMPARE(p.tools, 2);  // the view switch is hidden
        const auto db = makeDb(QStringLiteral("mcp-vis"), dir.filePath(QStringLiteral("vis.bndb")));
        rk::HostMcp::instance().install(db->registry);
        rk::ChatContext chat;
        bind(chat, db);

        QStringList names = toolNames(chat);
        QVERIFY(names.contains(QStringLiteral("get_cursor_position")));
        QVERIFY(!names.contains(QStringLiteral("bn_function_list")));

        db->hostMcp = true;
        names = toolNames(chat);
        QVERIFY(names.contains(QStringLiteral("bn_function_list")));
        QVERIFY(names.contains(QStringLiteral("bn_symbol_rename")));
        QVERIFY(!names.contains(QStringLiteral("bn_binary_view_set_active")));
        QVERIFY(!names.contains(QStringLiteral("get_cursor_position")));
        QVERIFY(!names.contains(QStringLiteral("rename_function")));
        QVERIFY(names.contains(QStringLiteral("ask_user")));  // chat tools stay
        // the host's schema reaches the model as the host wrote it
        for (const QJsonValue& t : rk::chatHostTools(chat))
            if (t.toObject().value(QStringLiteral("name")) == QLatin1String("bn_function_list"))
                QVERIFY(t.toObject().value(QStringLiteral("parameters")).toObject().value(QStringLiteral("properties")).toObject().contains(QStringLiteral("address")));
    }

    void hostChangeIsGatedAndRecorded() {
        Fixture f;
        QVERIFY(f.probe().ok);
        const auto db = makeDb(QStringLiteral("mcp-gate"), dir.filePath(QStringLiteral("gate.bndb")));
        rk::HostMcp::instance().install(db->registry);
        db->hostMcp = true;
        rk::ChatContext chat;
        bind(chat, db);
        auto token = std::make_shared<rk::CancelToken>();
        auto run = [&](const char* tool) {
            return rk::ToolDispatcher::execute(chat, QString::fromLatin1(tool), QJsonObject{{"address", "0x401000"}},
                                               QStringLiteral("call"), token, {});
        };

        chat.setPhase(QStringLiteral("explore"), true);
        QVERIFY(run("bn_symbol_rename").isError);
        QVERIFY(!run("bn_function_list").isError);
        chat.setPhase(QString(), false);

        QVERIFY(!run("bn_function_list").isError);
        const rk::ToolResult renamed = run("bn_symbol_rename");
        QVERIFY2(!renamed.isError, qPrintable(renamed.text));
        QCOMPARE(chat.mutations->size(), 1);
        const rk::MutationRecord rec = chat.mutations->records().first();
        QVERIFY(!rec.reversible);
        QVERIFY(rec.irreversibleReason.contains(QStringLiteral("MCP")));

        // each call reached the server after selecting this chat's database
        QStringList called;
        for (const auto& c : f.server.calls()) called << c.tool;
        QCOMPARE(called, (QStringList{"bn_function_list", "bn_function_list", "bn_symbol_rename"}));
        QCOMPARE(f.selected, (QStringList{"mcp-gate", "mcp-gate", "mcp-gate"}));
    }

    void answerIsRememberedPerDatabase() {
        Fixture f;
        const auto yes = makeDb(QStringLiteral("mcp-yes"), dir.filePath(QStringLiteral("yes.bndb")));
        const auto unasked = makeDb(QStringLiteral("mcp-unasked"), dir.filePath(QStringLiteral("unasked.bndb")));
        rk::setHostMcpConsent(yes->sessionsDir(), true);
        // recording where the database lives keeps the answer
        rk::recordDatabaseLocation(yes->sessionsDir(), rk::DatabaseLocation{"binja", "/somewhere/yes.bndb", "/somewhere/yes"});
        QCOMPARE(rk::hostMcpConsent(yes->sessionsDir()), std::optional<bool>(true));
        QCOMPARE(rk::hostMcpConsent(unasked->sessionsDir()), std::nullopt);

        rk::ui::HostMcpBar bar;
        bar.setDatabase(yes);
        QTRY_VERIFY_WITH_TIMEOUT(yes->hostMcp.load(), 10000);
        QVERIFY(bar.isHidden());

        bar.setDatabase(unasked);
        QTRY_VERIFY_WITH_TIMEOUT(!bar.busy(), 10000);
        QVERIFY(!bar.isHidden());  // asks
        QVERIFY(!unasked->hostMcp.load());
    }
};

RK_TEST(HostMcpTest)

#include "test_host_mcp.moc"
