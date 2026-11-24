// Settings dialog against the mock environment: provider → model choice, API keys in auth.json, custom
// connections, mcp.json, and Settings on a fresh install with no credentials at all.
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QtTest>

#include "TestEnv.h"
#include "common/config/Paths.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AgentHome.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/settings/SettingsDialog.h"
#include "common/util/Files.h"

class SettingsTest : public QObject {
    Q_OBJECT

    static QString agentFile(const char* name) { return rk::paths::omcAgentDir() + QLatin1Char('/') + QLatin1String(name); }

    static void clickOk(rk::SettingsDialog& dlg) {
        auto* box = dlg.findChild<QDialogButtonBox*>();
        QVERIFY(box);
        box->button(QDialogButtonBox::Ok)->click();
    }

    // Selects provider `id` the way a click does (activated, not just currentIndex).
    static bool pickProvider(QComboBox* providers, const QString& id) {
        if (!rktest::waitFor([&] { return providers->findData(id) >= 0; }, 30000)) return false;
        const int idx = providers->findData(id);
        providers->setCurrentIndex(idx);
        Q_EMIT providers->activated(idx);
        return true;
    }

private Q_SLOTS:
    // The bug a fresh install hit: Settings could not list models or providers, so there was no way to log in.
    // The child inherits this process's environment, which main.cpp already stripped of provider keys.
    void freshInstallListsProviders() {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--fresh-settings-probe")});
        QVERIFY(child.waitForFinished(60000));
        const QByteArray out = child.readAll();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, out.constData());
    }

    void providerPicksItsModel() {
        const rk::RikuganConfig original = rk::RikuganConfig::instance();
        {
            rk::SettingsDialog dlg;
            auto* providers = dlg.findChild<QComboBox*>(QStringLiteral("providerCombo"));
            auto* model = dlg.findChild<QComboBox*>(QStringLiteral("modelCombo"));
            QVERIFY(providers && model);
            // the configured model's provider and model are selected
            QCOMPARE(providers->currentData().toString(), QStringLiteral("mock"));
            QCOMPARE(model->currentText(), QStringLiteral("mock-1"));
            // another provider offers its own models, and its first one becomes the choice
            QVERIFY(pickProvider(providers, QStringLiteral("modemock")));
            QCOMPARE(model->count(), 1);
            QCOMPARE(model->currentText(), QStringLiteral("m1"));
            clickOk(dlg);
        }
        QCOMPARE(rk::RikuganConfig::instance().model, QStringLiteral("modemock/m1"));
        rk::RikuganConfig::instance() = original;
        QVERIFY(original.save());
    }

    void apiKeyWritesAuthJson() {
        const QString path = agentFile("auth.json");
        QVERIFY(rk::writeFileAtomic(path, R"({"other":{"type":"api_key","key":"K1"}})", 0644));
        {
            rk::SettingsDialog dlg;
            auto* providers = dlg.findChild<QComboBox*>(QStringLiteral("providerCombo"));
            auto* key = dlg.findChild<QLineEdit*>(QStringLiteral("apiKeyEdit"));
            auto* save = dlg.findChild<QPushButton*>(QStringLiteral("saveApiKeyButton"));
            auto* account = dlg.findChild<QLabel*>(QStringLiteral("accountStatus"));
            QVERIFY(providers && key && save && account);
            QVERIFY(pickProvider(providers, QStringLiteral("anthropic")));
            QCOMPARE(account->text(), QStringLiteral("Not set up"));
            key->setText(QStringLiteral("sk-test"));
            save->click();
            QVERIFY(key->text().isEmpty());
            QCOMPARE(account->text(), QStringLiteral("API key saved"));
        }
        bool ok = false;
        const QJsonObject auth = rk::readJsonObject(path, &ok);
        QVERIFY(ok);
        QCOMPARE(auth.value("anthropic").toObject(), (QJsonObject{{"type", "api_key"}, {"key", "sk-test"}}));
        QCOMPARE(auth.value("other").toObject().value("key").toString(), QStringLiteral("K1"));
#ifndef _WIN32  // Windows: the profile directory's ACL, not a mode, keeps auth.json private
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
#endif

        QFile::remove(path);
        rk::ServiceSession::reloadCredentials();
    }

    void customConnectionRoundTrip() {
        const rk::RikuganConfig original = rk::RikuganConfig::instance();
        const int n = int(original.customEndpoints.size());  // the endpoints tests/native/main.cpp registered
        {
            rk::SettingsDialog dlg;
            auto* providers = dlg.findChild<QComboBox*>(QStringLiteral("providerCombo"));
            auto* add = dlg.findChild<QPushButton*>(QStringLiteral("addConnectionButton"));
            QVERIFY(providers && add);
            add->click();
            auto* conn = dlg.findChild<QDialog*>(QStringLiteral("addConnectionDialog"));
            QVERIFY(conn);
            conn->findChild<QLineEdit*>(QStringLiteral("connectionNameEdit"))->setText(QStringLiteral("mylocal"));
            conn->findChild<QLineEdit*>(QStringLiteral("connectionUrlEdit"))->setText(QStringLiteral("http://127.0.0.1:9/v1"));
            conn->findChild<QComboBox*>(QStringLiteral("connectionApiCombo"))->setCurrentText(QStringLiteral("anthropic-messages"));
            conn->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(providers->currentData().toString(), QStringLiteral("mylocal"));
            QTest::keyClicks(dlg.findChild<QLineEdit*>(QStringLiteral("connectionModelsEdit")), QStringLiteral("small, big"));
            QTest::keyClicks(dlg.findChild<QLineEdit*>(QStringLiteral("apiKeyEdit")), QStringLiteral("LOCAL_KEY"));
            auto* model = dlg.findChild<QComboBox*>(QStringLiteral("modelCombo"));
            QCOMPARE(model->count(), 2);
            model->setCurrentIndex(model->findText(QStringLiteral("small")));
            QSignalSpy changed(&dlg, &rk::SettingsDialog::settingsChanged);
            clickOk(dlg);
            QCOMPARE(changed.count(), 1);
            QCOMPARE(dlg.result(), int(QDialog::Accepted));
        }

        const rk::RikuganConfig loaded = rk::RikuganConfig::load();
        QCOMPARE(int(loaded.customEndpoints.size()), n + 1);
        const rk::CustomEndpoint& e = loaded.customEndpoints[n];
        QCOMPARE(e.id, QStringLiteral("mylocal"));
        QCOMPARE(e.baseUrl, QStringLiteral("http://127.0.0.1:9/v1"));
        QCOMPARE(e.api, QStringLiteral("anthropic-messages"));
        QCOMPARE(e.apiKey, QStringLiteral("LOCAL_KEY"));
        QCOMPARE(e.models, (QStringList{"small", "big"}));
        QCOMPARE(loaded.customEndpoints[0].id, QStringLiteral("mock"));
        QCOMPARE(loaded.model, QStringLiteral("mylocal/small"));

        const QString yml = QString::fromUtf8(rk::readFile(agentFile("models.yml")));
        QVERIFY2(yml.contains("mylocal:") && yml.contains("http://127.0.0.1:9/v1") && yml.contains("anthropic-messages") &&
                     yml.contains("LOCAL_KEY") && yml.contains("small") && yml.contains("big") && yml.contains("mock:"),
                 qPrintable(yml));

        // reopening shows what was saved
        {
            rk::SettingsDialog dlg;
            auto* providers = dlg.findChild<QComboBox*>(QStringLiteral("providerCombo"));
            QCOMPARE(providers->currentData().toString(), QStringLiteral("mylocal"));
            QCOMPARE(dlg.findChild<QLineEdit*>(QStringLiteral("connectionModelsEdit"))->text(), QStringLiteral("small, big"));
        }

        rk::RikuganConfig::instance() = original;
        QVERIFY(original.save());
        rk::AgentHome::writeModelsYaml();
    }

    void mcpUnknownKeysSurvive() {
        const QString path = agentFile("mcp.json");
        const QJsonObject fixture{
            {"$schema", "custom-schema"},
            {"futureTopLevel", QJsonObject{{"a", 1}}},
            {"mcpServers",
             QJsonObject{{"srv", QJsonObject{{"command", "echo"},
                                             {"args", QJsonArray{"hi"}},
                                             {"customField", 42},
                                             {"env", QJsonObject{{"X", "1"}}}}},
                         {"web", QJsonObject{{"type", "http"},
                                             {"url", "https://example.com/mcp"},
                                             {"headers", QJsonObject{{"H", "v"}}},
                                             {"auth", QJsonObject{{"k", "v"}}}}}}}};
        QVERIFY(rk::writeFileAtomic(path, QJsonDocument(fixture).toJson()));
        {
            rk::SettingsDialog dlg;
            auto* list = dlg.findChild<QListWidget*>(QStringLiteral("mcpServers"));
            QVERIFY(list);
            QCOMPARE(list->count(), 2);
            QListWidgetItem* srv = list->item(0);
            QVERIFY(srv->text().startsWith("srv"));
            QCOMPARE(srv->checkState(), Qt::Checked);
            srv->setCheckState(Qt::Unchecked);
            clickOk(dlg);
        }
        bool ok = false;
        const QJsonObject o = rk::readJsonObject(path, &ok);
        QVERIFY(ok);
        QCOMPARE(o.value("$schema").toString(), QStringLiteral("custom-schema"));
        QCOMPARE(o.value("futureTopLevel").toObject().value("a").toInt(), 1);
        const QJsonObject servers = o.value("mcpServers").toObject();
        const QJsonObject srv = servers.value("srv").toObject();
        QCOMPARE(srv.value("customField").toInt(), 42);
        QCOMPARE(srv.value("enabled").toBool(true), false);
        QCOMPARE(srv.value("command").toString(), QStringLiteral("echo"));
        QCOMPARE(srv.value("env").toObject().value("X").toString(), QStringLiteral("1"));
        const QJsonObject web = servers.value("web").toObject();
        QCOMPARE(web.value("auth").toObject().value("k").toString(), QStringLiteral("v"));
        QVERIFY(!web.contains("enabled"));
        QFile::remove(path);
    }
};

RK_TEST(SettingsTest)
#include "test_settings.moc"
