// Settings' "Test connection" for a custom connection against a local model server (HTTP and HTTPS with a
// self-signed certificate): the model list of each API kind with its key, and in plain words a refused key, a wrong
// base URL, nothing listening and an untrusted certificate, which the connection's own certificate fixes.
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include "TestEnv.h"
#include "common/config/ConnectionTest.h"
#include "common/util/Files.h"

namespace {

// self-signed for IP 127.0.0.1, valid until 2126
const char kCert[] = R"(-----BEGIN CERTIFICATE-----
MIIBpDCCAUugAwIBAgIUOFEa8Acq9wecK/jr/auPWHGwBKYwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MTEyNjE5MDUxNVoYDzIxMjYxMTAy
MTkwNTE1WjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAQap4C4d26yrXmfFul451J3jGVvKpopYO3/1zX4xhQeaVgZ0JFORfrc
fa3Ztg8fxE6fjoZz/5CLKrDKhdZ3wLxlo3kwdzAdBgNVHQ4EFgQUCbwVKpief18m
m+P//Q+pXXP54dUwHwYDVR0jBBgwFoAUCbwVKpief18mm+P//Q+pXXP54dUwDwYD
VR0TAQH/BAUwAwEB/zAPBgNVHREECDAGhwR/AAABMBMGA1UdJQQMMAoGCCsGAQUF
BwMBMAoGCCqGSM49BAMCA0cAMEQCIEP7Qtioy6pzmSdB6WCmvB/AtfGA8IW2lFds
dfVn9IT1AiBlavE/75uAIIauV28uDat9Sjv55E3Zr5uriDhOGc+EqQ==
-----END CERTIFICATE-----
)";

const char kKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQglWPWK4tI8iPSEsiX
A9mgi/rLVjYN7DK78TZSM3oqChGhRANCAAQap4C4d26yrXmfFul451J3jGVvKpop
YO3/1zX4xhQeaVgZ0JFORfrcfa3Ztg8fxE6fjoZz/5CLKrDKhdZ3wLxl
-----END PRIVATE KEY-----
)";

// The model lists of OpenAI-compatible servers (Bearer key), Anthropic (x-api-key plus anthropic-version) and
// Ollama (no key); everything else is 404. Prints its HTTP and HTTPS ports.
const char kServer[] = R"PY(
import http.server, json, ssl, sys, threading
cert, key = sys.argv[1], sys.argv[2]
KEY = 'the-key'
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def reply(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path == '/ok/v1/models':
            if self.headers.get('Authorization') != 'Bearer ' + KEY:
                return self.reply(401, {'error': 'bad key'})
            return self.reply(200, {'data': [{'id': 'm1'}, {'id': 'm2'}]})
        if self.path == '/anthropic/v1/models':
            if self.headers.get('x-api-key') != KEY or not self.headers.get('anthropic-version'):
                return self.reply(401, {'error': 'bad key'})
            return self.reply(200, {'data': [{'id': 'claude-x'}]})
        if self.path == '/ollama/api/tags':
            return self.reply(200, {'models': [{'name': 'llama3'}]})
        self.reply(404, {'error': 'not found'})
class Quiet(http.server.ThreadingHTTPServer):
    def handle_error(self, *a):
        pass
plain = Quiet(('127.0.0.1', 0), H)
tls = Quiet(('127.0.0.1', 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
threading.Thread(target=tls.serve_forever, daemon=True).start()
print(plain.server_address[1], tls.server_address[1], flush=True)
plain.serve_forever()
)PY";

}  // namespace

class ConnectionTestTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    QProcess server_;
    int http_ = 0, https_ = 0, closed_ = 0;

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        QVERIFY(rk::writeFileAtomic(dir_.filePath("cert.pem"), kCert));
        QVERIFY(rk::writeFileAtomic(dir_.filePath("key.pem"), kKey));
        QVERIFY(rk::writeFileAtomic(dir_.filePath("server.py"), kServer));
        server_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        server_.start(rktest::python(), {"-I", dir_.filePath("server.py"), dir_.filePath("cert.pem"), dir_.filePath("key.pem")});
        QVERIFY(server_.waitForStarted(10000));
        QVERIFY(server_.waitForReadyRead(60000));  // a Python start can take seconds on a busy CI runner
        const QList<QByteArray> ports = server_.readLine().trimmed().split(' ');
        QCOMPARE(ports.size(), 2);
        http_ = ports[0].toInt();
        https_ = ports[1].toInt();
        QTcpServer probe;  // a port nothing listens on
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        closed_ = probe.serverPort();
    }

    void cleanupTestCase() {
        server_.kill();
        server_.waitForFinished(5000);
    }

    void reportsWhatTheServerSays_data() {
        QTest::addColumn<QString>("api");
        QTest::addColumn<QString>("base");  // %1 HTTP port, %2 HTTPS port, %3 a port nothing listens on
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("ca");    // "cert": the server's certificate
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QString>("says");
        const QString oa = QStringLiteral("openai-completions");
        QTest::newRow("models") << oa << "http://127.0.0.1:%1/ok/v1/" << "the-key" << "" << true << "lists 2 model(s): m1, m2.";
        QTest::newRow("refused key") << oa << "http://127.0.0.1:%1/ok/v1" << "other" << "" << false << "refused the API key (HTTP 401)";
        QTest::newRow("anthropic") << "anthropic-messages" << "http://127.0.0.1:%1/anthropic" << "the-key" << "" << true << "claude-x";
        QTest::newRow("ollama") << "ollama-chat" << "http://127.0.0.1:%1/ollama" << "" << "" << true << "llama3";
        QTest::newRow("wrong base URL") << oa << "http://127.0.0.1:%1/v2" << "the-key" << "" << false << "/v2/models does not exist (HTTP 404)";
        QTest::newRow("nothing listening") << oa << "http://127.0.0.1:%3/v1" << "" << "" << false << "Cannot connect to 127.0.0.1:%3";
        QTest::newRow("untrusted certificate") << oa << "https://127.0.0.1:%2/ok/v1" << "the-key" << "" << false << "its certificate is not trusted";
        QTest::newRow("trusted certificate") << oa << "https://127.0.0.1:%2/ok/v1" << "the-key" << "cert" << true << "m1, m2";
        QTest::newRow("missing certificate") << oa << "https://127.0.0.1:%2/ok/v1" << "the-key" << "missing" << false << "does not exist";
        QTest::newRow("not a URL") << oa << "127.0.0.1:%1/v1" << "" << "" << false << "Enter a base URL";
    }

    void reportsWhatTheServerSays() {
        QFETCH(QString, api);
        QFETCH(QString, base);
        QFETCH(QString, key);
        QFETCH(QString, ca);
        QFETCH(bool, ok);
        QFETCH(QString, says);
        auto ports = [&](QString s) { return s.replace("%1", QString::number(http_)).replace("%2", QString::number(https_)).replace("%3", QString::number(closed_)); };
        rk::CustomEndpoint e;
        e.id = QStringLiteral("private");
        e.api = api;
        e.baseUrl = ports(base);
        e.apiKey = key;
        if (ca == QLatin1String("cert")) e.caFile = dir_.filePath("cert.pem");
        else if (ca == QLatin1String("missing")) e.caFile = dir_.filePath("missing.pem");
        const rk::ConnectionTestResult r = rk::testConnection(e);
        QVERIFY2(r.message.contains(ports(says)), qPrintable(r.message));
        QCOMPARE(r.ok, ok);
    }
};

RK_TEST(ConnectionTestTest)
#include "test_connection.moc"
