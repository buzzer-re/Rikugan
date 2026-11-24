// omc bridge: sessions against the mock model, event filtering, managed config, AuthSync.
#include <mutex>
#include <vector>

#include <QJsonDocument>
#include <QtTest>

#include "TestEnv.h"
#include "common/config/Paths.h"
#include "common/agent/Skills.h"
#include "common/omc/AuthSync.h"
#include "common/omc/OmcSession.h"
#include "common/omc/YamlWriter.h"
#include "common/util/Files.h"

namespace {

class Recorder : public rk::OmcListener {
public:
    void onFrame(std::string_view type, const std::string& json) override {
        std::lock_guard<std::mutex> g(mu);
        frames.emplace_back(std::string(type), json);
    }
    void onHostToolCall(const QJsonObject&) override {}
    void onHostToolCancel(const QString&) override {}
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> frames;
};

rk::OmcOptions mockOptions() {
    rk::OmcOptions o;
    o.cwd = rk::paths::scratchDir("test");
    o.model = rktest::env().model;
    o.builtinTools = {"read"};
    o.noSession = true;
    return o;
}

}  // namespace

class BridgeTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void promptAndFilter() {
        auto rec = std::make_shared<Recorder>();
        auto s = rk::OmcSession::open(mockOptions(), rec);
        rk::OmcSession::PromptOutcome out;
        std::atomic<bool> done{false};
        s->post([&](rk::OmcSession& ss) {
            out = ss.prompt("say:hello from the mock");
            done = true;
        });
        QVERIFY(rktest::waitFor([&] { return done.load(); }, 30000));
        QCOMPARE(out.status, QStringLiteral("completed"));
        QCOMPARE(out.lastText.trimmed(), QStringLiteral("hello from the mock"));
        std::lock_guard<std::mutex> g(rec->mu);
        bool sawUpdate = false;
        for (auto& [type, json] : rec->frames) {
            QVERIFY2(type != "agent_end", "agent_end must be filtered out");
            if (type == "message_update") {
                sawUpdate = true;
                QVERIFY2(json.find("\"partial\"") == std::string::npos, json.c_str());
            }
        }
        QVERIFY(sawUpdate);
        QVERIFY(s->closeAndWait(10000));
    }

    void managedConfigIsYaml() {
        // omc started sessions above with this file; it must also round-trip through our writer unchanged
        const QByteArray y = rk::toYaml(QJsonObject{{"a", QJsonObject{{"b", "x: \"y\""}, {"c", false}}},
                                                    {"list", QJsonArray{QJsonObject{{"id", "m1"}, {"n", 2}}, "s"}}});
        QCOMPARE(QString::fromUtf8(y).section('\n', 1),
                 QStringLiteral("a:\n  b: \"x: \\\"y\\\"\"\n  c: false\nlist:\n  - id: \"m1\"\n    n: 2\n  - \"s\"\n"));
        QVERIFY(QFile::exists(rk::paths::omcAgentDir() + "/config.yml"));
    }

    void skillsReachOmc() {
        bool deob = false;
        for (const rk::SkillInfo& s : rk::listSkills())
            if (s.slug == "deobfuscation") deob = s.builtin && s.description.startsWith("Systematic") && s.mode == "plan";
        QVERIFY(deob);
        auto s = rk::OmcSession::open(mockOptions(), std::make_shared<Recorder>());
        QJsonObject r;
        std::atomic<bool> done{false};
        s->post([&](rk::OmcSession& ss) {
            r = ss.request(QJsonObject{{"type", "get_available_commands"}});
            done = true;
        });
        QVERIFY(rktest::waitFor([&] { return done.load(); }, 20000));
        QVERIFY2(QJsonDocument(r).toJson().contains("skill:ctf"), QJsonDocument(r).toJson().left(600).constData());
        QVERIFY(s->closeAndWait(10000));
    }


    void authMerge() {
        QJsonObject r{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "r"}, {"expires", 100}}},
                      {"openai", QJsonObject{{"type", "api_key"}, {"key", "r-key"}}},
                      {"ronly", QJsonObject{{"type", "api_key"}, {"key", "mine"}}}};
        QJsonObject o{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "o"}, {"expires", 200}}},
                      {"openai", QJsonObject{{"type", "api_key"}, {"key", "o-key"}}},
                      {"google", QJsonObject{{"type", "oauth"}, {"access", "g"}, {"expires", 5}}}};
        const rk::AuthMergeResult m = rk::mergeAuth(r, o);
        QVERIFY(m.rChanged);
        QVERIFY(!m.oChanged);
        QCOMPARE(r["anthropic"].toObject()["access"].toString(), QStringLiteral("o"));  // later expiry wins
        QCOMPARE(r["openai"].toObject()["key"].toString(), QStringLiteral("r-key"));     // API keys: R stays
        QCOMPARE(r["google"].toObject()["access"].toString(), QStringLiteral("g"));      // O-only copied
        QVERIFY(!o.contains("ronly"));                                                     // never R -> O

        QJsonObject r2{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "new"}, {"expires", 900}}}};
        QJsonObject o2{{"anthropic", QJsonObject{{"type", "oauth"}, {"access", "old"}, {"expires", 1}}}};
        const rk::AuthMergeResult m2 = rk::mergeAuth(r2, o2);
        QVERIFY(!m2.rChanged);
        QVERIFY(m2.oChanged);
        QCOMPARE(o2["anthropic"].toObject()["access"].toString(), QStringLiteral("new"));
    }
};
RK_TEST(BridgeTest)

#include "test_bridge.moc"
