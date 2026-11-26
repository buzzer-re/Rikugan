// Claude Code's and Codex's logins shared with Rikugan's auth.json (ExternalLogins): imported when Rikugan has none,
// the later expiry winning for one account on both sides (a refresh omc made goes back to the tool's file), another
// account or an API key left alone.
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include "TestEnv.h"
#include "common/omc/ExternalLogins.h"
#include "common/util/Files.h"

namespace {

QString jwt(qint64 expSeconds) {
    auto part = [](const QJsonObject& o) { return QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)); };
    return part({{"alg", "none"}}) + "." + part({{"exp", expSeconds}}) + ".sig";
}

QJsonObject claude(const QString& access, const QString& refresh, qint64 expiresAt, const QString& account = "acct-1") {
    return QJsonObject{{"claudeAiOauth", QJsonObject{{"accessToken", access}, {"refreshToken", refresh}, {"expiresAt", expiresAt},
                                                      {"subscriptionType", "max"}, {"accountUuid", account}}}};
}

}  // namespace

class ExternalLoginsTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString rPath() const { return dir.path() + "/auth.json"; }
    rk::ExternalLogins::Sources src() const { return {dir.path() + "/claude.json", dir.path() + "/codex.json", false}; }
    void write(const QString& name, const QJsonObject& o) { QVERIFY(rk::writeFileAtomic(dir.path() + "/" + name, QJsonDocument(o).toJson())); }
    QJsonObject read(const QString& name) { return rk::readJsonObject(dir.path() + "/" + name); }

private Q_SLOTS:
    void init() {
        for (const char* f : {"auth.json", "claude.json", "codex.json"}) QFile::remove(dir.path() + "/" + f);
    }

    void importsWhenRikuganHasNone() {
        write("claude.json", claude("at-1", "rt-1", 1000));
        write("codex.json", QJsonObject{{"auth_mode", "chatgpt"},
                                        {"tokens", QJsonObject{{"access_token", jwt(2000)}, {"refresh_token", "crt-1"}, {"account_id", "cx-1"}}}});
        QVERIFY(rk::ExternalLogins::syncFiles(rPath(), src(), false));
        const QJsonObject r = read("auth.json");
        QCOMPARE(r["anthropic"].toObject(),
                 (QJsonObject{{"type", "oauth"}, {"access", "at-1"}, {"refresh", "rt-1"}, {"expires", 1000}, {"accountId", "acct-1"}}));
        const QJsonObject cx = r["openai-codex"].toObject();
        QCOMPARE(cx["refresh"].toString(), QString("crt-1"));
        QCOMPARE(cx["expires"].toDouble(), 2000000.0);
        QCOMPARE(cx["accountId"].toString(), QString("cx-1"));
        QCOMPARE(rk::ExternalLogins::find(src()).size(), 2);
        QVERIFY(!rk::ExternalLogins::syncFiles(rPath(), src(), false));  // nothing new
    }

    void laterExpiryWinsForOneAccount() {
        write("auth.json", QJsonObject{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "at-1"}, {"refresh", "rt-1"},
                                                                {"expires", 1000}, {"accountId", "acct-1"}, {"email", "a@b"}}}});
        // Claude Code refreshed: Rikugan takes the new tokens and keeps what it knew
        write("claude.json", claude("at-2", "rt-2", 5000));
        QVERIFY(rk::ExternalLogins::syncFiles(rPath(), src(), false));
        QJsonObject a = read("auth.json")["anthropic"].toObject();
        QCOMPARE(a["refresh"].toString(), QString("rt-2"));
        QCOMPARE(a["email"].toString(), QString("a@b"));
        // omc refreshed (new refresh token, later expiry): Claude Code gets it, its other fields stay
        a["access"] = "at-3";
        a["refresh"] = "rt-3";
        a["expires"] = 9000;
        write("auth.json", QJsonObject{{"anthropic", a}});
        QVERIFY(!rk::ExternalLogins::syncFiles(rPath(), src(), false));
        const QJsonObject c = read("claude.json")["claudeAiOauth"].toObject();
        QCOMPARE(c["refreshToken"].toString(), QString("rt-3"));
        QCOMPARE(c["accessToken"].toString(), QString("at-3"));
        QCOMPARE(c["expiresAt"].toDouble(), 9000.0);
        QCOMPARE(c["subscriptionType"].toString(), QString("max"));
    }

    void otherAccountsAndKeysStay() {
        const QJsonObject r{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "x"}, {"refresh", "y"}, {"expires", 1}, {"accountId", "other"}}},
                            {"openai-codex", QJsonObject{{"type", "api_key"}, {"key", "sk"}}}};
        write("auth.json", r);
        write("claude.json", claude("at-1", "rt-1", 5000));
        write("codex.json", QJsonObject{{"tokens", QJsonObject{{"access_token", jwt(99999)}, {"refresh_token", "c"}}}});
        QVERIFY(!rk::ExternalLogins::syncFiles(rPath(), src(), false));
        QCOMPARE(read("auth.json"), r);
        QCOMPARE(read("claude.json")["claudeAiOauth"].toObject()["refreshToken"].toString(), QString("rt-1"));
    }

    void noLoginNoChange() {
        write("codex.json", QJsonObject{{"auth_mode", "apikey"}, {"OPENAI_API_KEY", "sk"}});
        QVERIFY(rk::ExternalLogins::find(src()).isEmpty());
        QVERIFY(!rk::ExternalLogins::syncFiles(rPath(), src(), false));
        QVERIFY(!QFile::exists(rPath()));
    }
};

RK_TEST(ExternalLoginsTest)
#include "test_external_logins.moc"
