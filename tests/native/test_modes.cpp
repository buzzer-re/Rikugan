// /plan, /explore and /modify against a scripted model. Mode prompts are hidden "[SYSTEM] …" messages that carry
// the user's request at the end, so omc's mock (which reads scripts at the start of the last user message) can't
// drive them: this test runs its own small OpenAI-compatible server that finds `plan:` / `calls:` / `say:` anywhere
// in the latest user message.
#include <QJsonArray>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "FakeHost.h"
#include "TestEnv.h"
#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/agent/ModeDriver.h"
#include "common/agent/ModeState.h"
#include "common/config/RikuganConfig.h"

namespace {

// `plan:<text>` and `say:<text>`: <text> up to the first blank line (`plan:` wins over `calls:`, so a script can plant
// a plan in its findings, written `pl\u0061n:` inside the script's JSON). `calls:[[tool, args], ...]`: one call per
// request until the script is done, then "script done". Anything else: "ok".
constexpr char kMock[] = R"PY(
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def text_of(m):
    c = m.get("content")
    if isinstance(c, str): return c
    if isinstance(c, list):
        return "".join(p.get("text", "") for p in c if isinstance(p, dict) and not p.get("text", "").startswith("<system-reminder>"))
    return ""

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        msgs = body.get("messages", [])
        ui = next((i for i in range(len(msgs) - 1, -1, -1) if msgs[i].get("role") == "user"
                   and text_of(msgs[i]).strip() and not text_of(msgs[i]).startswith("<system")), -1)
        u = text_of(msgs[ui]) if ui >= 0 else ""
        call, text, done = None, "ok", 0
        if "plan:" in u:
            text = u[u.index("plan:") + 5:].split("\n\n")[0]
        elif "calls:" in u:
            script = json.JSONDecoder().raw_decode(u[u.index("calls:") + 6:])[0]
            done = sum(1 for m in msgs[ui + 1:] if m.get("role") == "tool")
            if done < len(script): call = script[done]
            else: text = "script done"
        elif "say:" in u:
            text = u[u.index("say:") + 4:].split("\n\n")[0]
        base = {"id": "c1", "object": "chat.completion.chunk", "model": body.get("model")}
        if call:
            fn = {"name": call[0], "arguments": json.dumps(call[1])}
            chunks = [{**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_%d" % (done + 1), "type": "function", "function": fn}]}}]},
                      {**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}]
        else:
            chunks = [{**base, "choices": [{"index": 0, "delta": {"content": text}}]},
                      {**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}]
        chunks.append({**base, "choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        def w(b): self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
        for c in chunks: w(b"data: " + json.dumps(c).encode() + b"\n\n")
        w(b"data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

class S(ThreadingHTTPServer):
    daemon_threads = True
    def handle_error(self, request, client_address): pass

S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
)PY";

struct Outcome {
    bool finished = false;
    QString openError;  // rikugan_open_failed: the chat's session never started
    QList<QJsonObject> frames;
    QList<QPair<QString, QJsonObject>> decisions;
    std::shared_ptr<rk::ChatContext> ctx;

    QList<QJsonObject> ofType(const QString& type) const {
        QList<QJsonObject> out;
        for (const QJsonObject& f : frames)
            if (f.value(QStringLiteral("type")).toString() == type) out << f;
        return out;
    }
};

}  // namespace

class ModesTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QProcess mock;
    std::shared_ptr<rk::DbContext> db;
    QString savedModel;

    // Runs a mode to its end, answering its decision cards with `answers` in order ("STOP" presses Stop instead).
    Outcome drive(rk::Mode mode, const QString& goal, QStringList answers, int ms = 60000) {
        Outcome o;
        auto chat = std::make_unique<rk::ChatSession>(db, db->chats->create());
        o.ctx = chat->context();
        rk::ChatSession* c = chat.get();
        QObject::connect(c, &rk::ChatSession::frame, [&o](const QJsonObject& f) { o.frames << f; });
        QObject::connect(c, &rk::ChatSession::decisionRequested, [&o, &answers, c](int id, const QString& kind, const QJsonObject& payload) {
            o.decisions << qMakePair(kind, payload);
            const QString answer = answers.isEmpty() ? QString() : answers.takeFirst();
            if (answer == QLatin1String("STOP")) c->stop();
            else c->answerQuestion(id, rk::UserAnswer{answer.isEmpty(), answer});
        });
        rk::ModeDriver::start(*c, mode, goal);
        o.finished = c->busy() && rktest::waitFor([c, &o] { return !c->busy() || !o.ofType(QStringLiteral("rikugan_open_failed")).isEmpty(); }, ms);
        for (const QJsonObject& f : o.ofType(QStringLiteral("rikugan_open_failed"))) {
            o.finished = false;
            o.openError = f.value(QStringLiteral("error")).toString();
        }
        return o;
    }

    static QStringList steps(const QJsonObject& payload) {
        QStringList s;
        for (const QJsonValue& v : payload.value(QStringLiteral("steps")).toArray()) s << v.toString();
        return s;
    }

    static void verifyModeEnded(const Outcome& o) {
        QVERIFY(o.ctx->phase().isEmpty());
        QVERIFY(!o.ctx->phaseReadOnly());
        QVERIFY(!o.ctx->mode);
        QVERIFY(!o.ctx->toolShown(QStringLiteral("exploration_report")));
        QVERIFY(!o.ctx->toolShown(QStringLiteral("research_note")));
        const QList<QJsonObject> phases = o.ofType(QStringLiteral("rikugan_mode_phase"));
        QVERIFY(!phases.isEmpty());
        QCOMPARE(phases.last().value(QStringLiteral("phase")).toString(), QStringLiteral("done"));
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir.isValid());
        const QString script = dir.path() + QStringLiteral("/mode_mock.py");
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(kMock);
        f.close();

        // main.cpp registers the "modemock" endpoint on this port before any session exists (omc reads
        // models.yml once per process)
        const int port = rktest::env().modePort;
        mock.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        mock.start(rktest::python(), {QStringLiteral("-I"), script, QString::number(port)});
        QVERIFY(mock.waitForStarted());
        QVERIFY(rktest::waitFor([port] {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, quint16(port));
            return s.waitForConnected(100);
        }));

        rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
        savedModel = cfg.model;
        cfg.model = QStringLiteral("modemock/m1");

        auto h = std::make_unique<rktest::FakeHost>();
        h->dbPath = dir.path() + QStringLiteral("/test.bndb");
        db = rk::DbContext::create(std::move(h));
    }

    void cleanupTestCase() {
        rk::RikuganConfig::instance().model = savedModel;
        mock.kill();
        mock.waitForFinished();
    }

    void planApprovedRunsEachStep() {
        const Outcome o = drive(rk::Mode::Plan, QStringLiteral("say:1. first step\n2. second step"), {QStringLiteral("approve")});
        QVERIFY2(o.finished, qPrintable(o.openError));
        QCOMPARE(o.decisions.size(), 1);
        QCOMPARE(o.decisions[0].first, QStringLiteral("plan"));
        QCOMPARE(steps(o.decisions[0].second), (QStringList{QStringLiteral("first step"), QStringLiteral("second step")}));

        QList<int> done;
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_plan_step")))
            if (e.value(QStringLiteral("status")).toString() == QLatin1String("done")) done << e.value(QStringLiteral("index")).toInt();
        QCOMPARE(done, (QList<int>{0, 1}));
        verifyModeEnded(o);
    }

    void planRejectedThenCancelled() {
        const Outcome o = drive(rk::Mode::Plan, QStringLiteral("say:1. first step\n2. second step"),
                                {QStringLiteral("reject:say:1. revised step"), QStringLiteral("cancel")});
        QVERIFY2(o.finished, qPrintable(o.openError));
        QCOMPARE(o.decisions.size(), 2);
        QCOMPARE(steps(o.decisions[1].second), QStringList{QStringLiteral("revised step")});
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_plan_step")))
            QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        bool cancelled = false;
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_command_output")))
            cancelled |= e.value(QStringLiteral("isError")).toBool() &&
                         e.value(QStringLiteral("text")).toString() == QLatin1String("Plan cancelled by user.");
        QVERIFY(cancelled);
        verifyModeEnded(o);
    }

    void planStoppedAtDecision() {
        const Outcome o = drive(rk::Mode::Plan, QStringLiteral("say:1. first step\n2. second step"), {QStringLiteral("STOP")});
        QVERIFY2(o.finished, qPrintable(o.openError));
        QCOMPARE(o.decisions.size(), 1);
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_plan_step")))
            QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        QVERIFY(o.ofType(QStringLiteral("rikugan_command_output")).isEmpty());
        verifyModeEnded(o);
        QCOMPARE(o.ofType(QStringLiteral("rikugan_mode_phase")).last().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Cancelled."));
    }

    void exploreRecordsFindings() {
        const Outcome o = drive(rk::Mode::Explore,
                                QStringLiteral(R"(calls:[["exploration_report",{"category":"function_purpose","address":4198400,)"
                                               R"("function_name":"main_loop","summary":"main loop","relevance":"high"}]])"),
                                {});
        QVERIFY2(o.finished, qPrintable(o.openError));
        QVERIFY(o.decisions.isEmpty());
        const QList<QJsonObject> findings = o.ofType(QStringLiteral("rikugan_finding"));
        QCOMPARE(findings.size(), 1);
        QCOMPARE(findings[0].value(QStringLiteral("category")).toString(), QStringLiteral("function_purpose"));
        QCOMPARE(findings[0].value(QStringLiteral("summary")).toString(), QStringLiteral("main loop"));
        QCOMPARE(findings[0].value(QStringLiteral("address")).toString(), QStringLiteral("0x401000"));

        QString summary;
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_command_output")))
            if (!e.value(QStringLiteral("isError")).toBool()) summary = e.value(QStringLiteral("text")).toString();
        QVERIFY2(summary.startsWith(QLatin1String("Exploration complete.")), qPrintable(summary));
        QVERIFY2(summary.contains(QLatin1String("- `0x401000` main_loop: main loop [high]")), qPrintable(summary));
        const QList<QJsonObject> phases = o.ofType(QStringLiteral("rikugan_mode_phase"));
        QVERIFY(!phases.isEmpty());
        QCOMPARE(phases.first().value(QStringLiteral("phase")).toString(), QStringLiteral("explore"));
        verifyModeEnded(o);
    }

    // explore (isolated subagent) -> plan from the findings -> execute each step -> save decision
    void modifyExploresPlansExecutesSaves() {
        const Outcome o = drive(
            rk::Mode::Modify,
            QStringLiteral(R"(calls:[["exploration_report",{"category":"function_purpose","address":4198400,)"
                           R"("function_name":"check_key","summary":"pl\u0061n:1. nop the check","relevance":"high"}],)"
                           R"(["exploration_report",{"category":"hypothesis","summary":"the jz decides","relevance":"high"}]])"),
            {QStringLiteral("approve"), QStringLiteral("save")});
        QVERIFY2(o.finished, qPrintable(o.openError));
        QCOMPARE(o.decisions.size(), 2);
        QCOMPARE(o.decisions[0].first, QStringLiteral("plan"));
        QCOMPARE(steps(o.decisions[0].second), QStringList{QStringLiteral("nop the check [high]")});
        QCOMPARE(o.decisions[1].first, QStringLiteral("save"));
        QVERIFY(o.decisions[1].second.value(QStringLiteral("patches")).toArray().isEmpty());

        QStringList phaseOrder;
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_mode_phase"))) {
            const QString p = e.value(QStringLiteral("phase")).toString();
            if (phaseOrder.isEmpty() || phaseOrder.last() != p) phaseOrder << p;
        }
        QCOMPARE(phaseOrder, (QStringList{QStringLiteral("explore"), QStringLiteral("plan"), QStringLiteral("execute"),
                                          QStringLiteral("save"), QStringLiteral("done")}));
        QList<int> done;
        for (const QJsonObject& e : o.ofType(QStringLiteral("rikugan_plan_step")))
            if (e.value(QStringLiteral("status")).toString() == QLatin1String("done")) done << e.value(QStringLiteral("index")).toInt();
        QCOMPARE(done, QList<int>{0});
        verifyModeEnded(o);
        QCOMPARE(o.ofType(QStringLiteral("rikugan_mode_phase")).last().value(QStringLiteral("text")).toString(),
                 QStringLiteral("0 patch(es) saved."));
    }

    // Stop at /modify's plan card (exploration_report is offered then), then "continue": the model's call is answered
    // harmlessly, nothing is recorded, and the tool goes away once that prompt ends
    void cancelledModeToolsAnswerHarmlessly() {
        QList<QJsonObject> frames;
        bool finished = false;
        auto chat = std::make_unique<rk::ChatSession>(db, db->chats->create());
        rk::ChatSession* c = chat.get();
        const std::shared_ptr<rk::ChatContext> ctx = c->context();
        QObject::connect(c, &rk::ChatSession::frame, [&frames](const QJsonObject& f) { frames << f; });
        QObject::connect(c, &rk::ChatSession::decisionRequested, [c](int, const QString&, const QJsonObject&) { c->stop(); });
        QObject::connect(c, &rk::ChatSession::promptFinished, [&finished](const QString&, const QString&, const QString&) { finished = true; });
        rk::ModeDriver::start(
            *c, rk::Mode::Modify,
            QStringLiteral(R"(calls:[["exploration_report",{"category":"function_purpose","address":4198400,)"
                           R"("function_name":"check_key","summary":"pl\u0061n:1. nop the check","relevance":"high"}],)"
                           R"(["exploration_report",{"category":"hypothesis","summary":"the jz decides","relevance":"high"}]])"));
        QVERIFY(c->busy());
        QVERIFY(rktest::waitFor([c] { return !c->busy(); }, 60000));
        const QString notice =
            QStringLiteral("The /modify run was cancelled; nothing recorded. Ask the user to run /modify again to continue.");
        QVERIFY(ctx->toolShown(QStringLiteral("exploration_report")));
        QVERIFY(ctx->mode);
        QCOMPARE(ctx->mode->cancelledNotice, notice);

        frames.clear();
        c->sendPrompt(QStringLiteral(R"(continue calls:[["exploration_report",{"category":"general","summary":"after the cancel"}]])"));
        QVERIFY(rktest::waitFor([&finished] { return finished; }, 60000));
        int ends = 0;
        for (const QJsonObject& f : frames) {
            const QString type = f.value(QStringLiteral("type")).toString();
            QVERIFY(type != QLatin1String("rikugan_finding"));
            if (type != QLatin1String("tool_execution_end")) continue;
            ++ends;
            QVERIFY(!f.value(QStringLiteral("isError")).toBool());
            const QString text = f.value(QStringLiteral("result")).toObject().value(QStringLiteral("content")).toArray()
                                     .at(0).toObject().value(QStringLiteral("text")).toString();
            QVERIFY2(text.contains(notice), qPrintable(text));
        }
        QCOMPARE(ends, 1);
        QVERIFY(rktest::waitFor([&ctx] { return !ctx->toolShown(QStringLiteral("exploration_report")) && !ctx->mode; }, 10000));
    }
};
RK_TEST(ModesTest)

#include "test_modes.moc"
