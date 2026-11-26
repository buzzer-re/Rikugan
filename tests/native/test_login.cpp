// LoginFlow against omc's real login code: the browser step arrives as openUrl (no network needed to build an
// OAuth URL), and cancelling ends the login at once by freeing its session.
#include <QJsonArray>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

#include "TestEnv.h"
#include "common/config/Paths.h"
#include "common/omc/LoginFlow.h"
#include "common/omc/OmcSession.h"
#include "common/omc/ServiceSession.h"
#include "common/util/Env.h"
#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

class LoginTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void browserLoginShowsUrlAndCancels() {
        rk::LoginFlow flow;
        QSignalSpy url(&flow, &rk::LoginFlow::openUrl);
        QSignalSpy finished(&flow, &rk::LoginFlow::finished);
        flow.start(QStringLiteral("anthropic"));
        QVERIFY(flow.running());
        QVERIFY(rktest::waitFor([&] { return url.count() > 0 || finished.count() > 0; }, 15000));
        QCOMPARE(finished.count(), 0);
        QVERIFY2(url.first().at(0).toString().startsWith("https://"), qPrintable(url.first().at(0).toString()));
        flow.cancel();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("cancelled"));
        QVERIFY(!flow.running());
        // nothing from the cancelled login arrives later
        rktest::waitFor([] { return false; }, 300);
        QCOMPARE(finished.count(), 1);
    }

    // Cancel stops omc's login too: started again soon after, it gets its callback port and shows a URL again
    void cancelledLoginCanStartAgain() {
        for (int round = 0; round < 2; ++round) {
            rk::LoginFlow flow;
            QSignalSpy url(&flow, &rk::LoginFlow::openUrl);
            QSignalSpy finished(&flow, &rk::LoginFlow::finished);
            flow.start(QStringLiteral("openai-codex"));
            QVERIFY(rktest::waitFor([&] { return url.count() > 0 || finished.count() > 0; }, 15000));
            QVERIFY2(finished.count() == 0, qPrintable(QString("round %1: %2").arg(round).arg(finished.isEmpty() ? QString() : finished.first().at(1).toString())));
            flow.cancel();
            rktest::waitFor([] { return false; }, 2000);  // the freed session's login thread lets the port go
        }
    }

    // A browser that cannot reach this machine (IDA in a VM): the user pastes where it ended, and the login goes on
    // as if the browser had come back. omc then posts the code to the token endpoint; a local proxy records that
    // attempt (so omc took the code and its state) and refuses it, which ends the login at once.
    void pastedAddressOrCodeReachesTheLogin_data() {
        QTest::addColumn<bool>("bareCode");
        QTest::newRow("browser address") << false;
        QTest::newRow("bare code") << true;  // the state comes from the authorize URL
    }
    void pastedAddressOrCodeReachesTheLogin() {
        QFETCH(bool, bareCode);
        QTcpServer proxy;
        QVERIFY(proxy.listen(QHostAddress::LocalHost, 0));
        QByteArray request;
        connect(&proxy, &QTcpServer::newConnection, &proxy, [&] {
            QTcpSocket* s = proxy.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [&, s] {
                request += s->readAll();
                s->disconnectFromHost();
            });
        });
        rk::setEnv("HTTPS_PROXY", QStringLiteral("http://127.0.0.1:%1").arg(proxy.serverPort()));
        auto unset = qScopeGuard([] { rk::unsetEnv("HTTPS_PROXY"); });

        rk::LoginFlow flow;
        QSignalSpy url(&flow, &rk::LoginFlow::openUrl);
        QSignalSpy finished(&flow, &rk::LoginFlow::finished);
        QSignalSpy failed(&flow, &rk::LoginFlow::pastedCodeFailed);
        QVERIFY(!flow.acceptsPastedCode());
        flow.start(QStringLiteral("anthropic"));
        QVERIFY(rktest::waitFor([&] { return url.count() > 0 || finished.count() > 0; }, 15000));
        QCOMPARE(finished.count(), 0);
        QVERIFY(flow.acceptsPastedCode());

        flow.submitCode(QStringLiteral("  "));
        QCOMPARE(failed.count(), 1);  // nothing to deliver: the login keeps waiting
        QVERIFY(flow.running());

        const QUrlQuery authorize(QUrl(url.first().at(0).toString()));
        const QUrl redirect(authorize.queryItemValue("redirect_uri", QUrl::FullyDecoded));
        QCOMPARE(redirect.host(), QStringLiteral("localhost"));
        const QString state = authorize.queryItemValue("state", QUrl::FullyDecoded);
        QVERIFY(!state.isEmpty());
        QUrl landed = redirect;
        landed.setQuery(QStringLiteral("code=the-code&state=") + QString::fromLatin1(QUrl::toPercentEncoding(state)));
        flow.submitCode(bareCode ? QStringLiteral("the-code") : landed.toString());

        QVERIFY(rktest::waitFor([&] { return finished.count() > 0; }, 20000));
        QVERIFY2(request.startsWith("CONNECT api.anthropic.com:443"),
                 qPrintable(QStringLiteral("proxy saw: %1; login: %2").arg(QString::fromLatin1(request.left(80)), finished.first().at(1).toString())));
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!flow.acceptsPastedCode());
    }

    void providersCarryTheirCredentialSource() {
        QList<rk::LoginProviderInfo> providers;
        QString error;
        bool done = false;
        QObject guard;
        rk::fetchLoginProviders(&guard, [&](const QList<rk::LoginProviderInfo>& p, const QString& e) {
            providers = p;
            error = e;
            done = true;
        });
        QVERIFY(rktest::waitFor([&] { return done; }, 15000));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        auto anthropic = std::find_if(providers.cbegin(), providers.cend(), [](const auto& p) { return p.id == "anthropic"; });
        QVERIFY(anthropic != providers.cend());
        QVERIFY(!anthropic->authenticated);  // the test environment has no credentials
        QVERIFY(anthropic->source.isEmpty());
    }

    // A device login (Kimi Code, against omc's auth mock) that succeeds makes that provider's models available to
    // the service session the model lists come from.
    void deviceLoginMakesModelsAvailable() {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        const int port = probe.serverPort();
        probe.close();
        QProcess mock;
        mock.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        mock.start(rktest::python(), {"-I", qEnvironmentVariable("RK_OMC_DIR") + "/tests/e2e/mock_auth.py", QString::number(port)});
        QVERIFY(mock.waitForStarted());
        QVERIFY(rktest::waitFor([&] {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, quint16(port));
            return s.waitForConnected(100);
        }, 60000));  // a Python start can take seconds on a busy CI runner
        rk::setEnv("KIMI_CODE_OAUTH_HOST", QStringLiteral("http://127.0.0.1:%1/kimi").arg(port));
        rk::setEnv("OMC_AUTH_POLL_SCALE_MS", "20");
        rk::setEnv("OMC_NO_BROWSER", "1");

        rk::LoginFlow flow;
        QSignalSpy finished(&flow, &rk::LoginFlow::finished);
        flow.start(QStringLiteral("kimi-code"));
        QVERIFY(rktest::waitFor([&] { return finished.count() > 0; }, 30000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));

        auto kimiModels = [] {
            int n = -1;
            bool done = false;
            rk::ServiceSession::get()->post([&](rk::OmcSession& s) {
                const QJsonObject r = s.request(QJsonObject{{"type", "get_available_models"}});
                int count = 0;
                for (const QJsonValue& v : r.value("data").toObject().value("models").toArray())
                    if (v.toObject().value("provider").toString() == QLatin1String("kimi-code")) ++count;
                QMetaObject::invokeMethod(rk::uiAnchor(), [&, count] { n = count; done = true; }, Qt::QueuedConnection);
            });
            rktest::waitFor([&] { return done; }, 15000);
            return n;
        };
        const int models = kimiModels();

        QJsonObject auth = rk::readJsonObject(rk::paths::omcAgentDir() + "/auth.json");
        auth.remove("kimi-code");
        rk::writeFileAtomic(rk::paths::omcAgentDir() + "/auth.json", QJsonDocument(auth).toJson());
        rk::ServiceSession::reloadCredentials();
        for (const char* k : {"KIMI_CODE_OAUTH_HOST", "OMC_AUTH_POLL_SCALE_MS", "OMC_NO_BROWSER"}) rk::unsetEnv(k);
        mock.kill();
        mock.waitForFinished();
        QVERIFY2(models > 0, qPrintable(QStringLiteral("kimi-code models after login: %1").arg(models)));
    }
};

RK_TEST(LoginTest)
#include "test_login.moc"
