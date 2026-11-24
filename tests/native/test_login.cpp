// LoginFlow against omc's real login code: the browser step arrives as openUrl (no network needed to build an
// OAuth URL), and cancelling ends the login at once by freeing its session.
#include <QSignalSpy>
#include <QtTest>

#include "TestEnv.h"
#include "common/omc/LoginFlow.h"

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
};

RK_TEST(LoginTest)
#include "test_login.moc"
