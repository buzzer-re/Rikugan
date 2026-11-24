// The transcript reducer: what a stream of omc frames and a replayed session turn into — coalesced streaming,
// tool rows that survive providers reusing call ids, hidden internal prompts, cards that block the composer,
// cancellation, /btw answers and the goal shown in the activity strip.
#include <QSignalSpy>
#include <QtTest>

#include "TestEnv.h"
#include "common/ui/transcript/TranscriptModel.h"

using namespace rk::ui;

namespace {

QJsonObject delta(const char* type, const QString& d, int index = 0) {
    return QJsonObject{{"type", "message_update"},
                       {"assistantMessageEvent", QJsonObject{{"type", type}, {"delta", d}, {"contentIndex", index}}}};
}

QJsonObject message(const char* type, const char* role, const QJsonObject& extra = {}) {
    QJsonObject m = extra;
    m.insert("role", role);
    return QJsonObject{{"type", type}, {"message", m}};
}

QJsonObject toolStart(const QString& id, const QString& name, const QJsonObject& args) {
    return QJsonObject{{"type", "tool_execution_start"}, {"toolCallId", id}, {"toolName", name}, {"args", args}};
}

QJsonObject toolEnd(const QString& id, const QString& text, bool isError = false, const QJsonObject& details = {}) {
    return QJsonObject{{"type", "tool_execution_end"},
                       {"toolCallId", id},
                       {"isError", isError},
                       {"result", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}},
                                              {"details", details}}}};
}

QList<ItemKind> kinds(const TranscriptModel& m) {
    QList<ItemKind> out;
    for (int i = 0; i < m.rowCount(); ++i) out << m.at(i).kind;
    return out;
}

int firstRow(const TranscriptModel& m, ItemKind k) {
    for (int i = 0; i < m.rowCount(); ++i)
        if (m.at(i).kind == k) return i;
    return -1;
}

}  // namespace

class TranscriptModelTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // a burst of deltas flows in over a few frames: one repaint per frame (not per delta), all text shown soon,
    // and the end of the message shows whatever still waits
    void streamingRevealsTextOverFrames() {
        TranscriptModel m;
        m.applyFrame(message("message_start", "assistant"));
        m.applyFrame(delta("text_delta", "x"));
        const int row = firstRow(m, ItemKind::Text);
        QVERIFY(row >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(m.at(row).text, QString("x"), 500);
        QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
        for (int i = 0; i < 50; ++i) m.applyFrame(delta("text_delta", "y"));
        QCOMPARE(changed.count(), 0);  // nothing repaints per delta
        QTRY_COMPARE_WITH_TIMEOUT(m.at(row).text, QString("x") + QString(50, 'y'), 500);
        QVERIFY2(changed.count() >= 2 && changed.count() <= 8, qPrintable(QString::number(changed.count())));
        QCOMPARE(m.rowCount(), row + 1);

        for (int i = 0; i < 40; ++i) m.applyFrame(delta("text_delta", "z"));
        m.applyFrame(message("message_end", "assistant"));
        QCOMPARE(m.at(row).text, QString("x") + QString(50, 'y') + QString(40, 'z'));
    }

    void textAfterAToolCallIsANewRowBelowIt() {
        TranscriptModel m;
        m.applyFrame(message("message_start", "assistant"));
        m.applyFrame(delta("text_delta", "Looking at sub_401000."));
        QJsonObject start = delta("toolcall_start", {}, 1);
        start.insert("block", QJsonObject{{"id", "call_1"}, {"name", "decompile_function"}});
        m.applyFrame(start);
        m.applyFrame(toolStart("call_1", "decompile_function", {{"address", "0x401000"}}));
        m.applyFrame(toolEnd("call_1", "int f() {\n return 0;\n}"));
        m.applyFrame(delta("text_delta", "It returns 0.", 2));
        QCOMPARE(kinds(m), (QList<ItemKind>{ItemKind::Step, ItemKind::Text, ItemKind::Tool, ItemKind::Text}));
        QCOMPARE(m.at(1).text, QString("Looking at sub_401000."));
        QVERIFY(!m.at(1).streaming);
        QCOMPARE(m.at(1).chips.size(), 1);
        QCOMPARE(m.at(1).chips.first().value, QString("0x401000"));
        QCOMPARE(m.at(2).status, ToolStatus::Done);
        QCOMPARE(m.at(2).label, QString("0x401000"));
        QCOMPARE(m.at(2).summary, QString("3 lines"));
        QTRY_COMPARE_WITH_TIMEOUT(m.at(3).text, QString("It returns 0."), 500);
    }

    // a run that ends (here: stopped) with a card still open must not leave the composer locked behind it
    void cardsCloseWhenTheRunEnds() {
        TranscriptModel m;
        m.setBusy(true);
        m.addCard(ItemKind::Decision, 7, QJsonObject{{"kind", "plan"}, {"payload", QJsonObject{{"steps", QJsonArray{"a"}}}}});
        QVERIFY(m.awaitingButtons());
        m.setBusy(false);
        QVERIFY(!m.awaitingButtons());
        QCOMPARE(m.at(m.rowCount() - 1).resolution, QString("cancelled"));
    }

    void reusedCallIdsInALaterTurnGetTheirOwnRow() {
        TranscriptModel m;
        m.applyFrame(QJsonObject{{"type", "turn_start"}});
        m.applyFrame(toolStart("call_1", "rename_function", {{"address", "0x401000"}, {"new_name", "parse_hdr"}}));
        m.applyFrame(toolEnd("call_1", "Renamed sub_401000 to parse_hdr"));
        m.applyFrame(QJsonObject{{"type", "turn_start"}});
        m.applyFrame(message("message_start", "assistant"));
        m.applyFrame(toolStart("call_1", "list_strings", {}));
        m.applyFrame(toolEnd("call_1", "boom", true));
        QList<int> tools;
        for (int i = 0; i < m.rowCount(); ++i)
            if (m.at(i).kind == ItemKind::Tool) tools << i;
        QCOMPARE(tools.size(), 2);
        QCOMPARE(m.at(tools[0]).toolName, QString("rename_function"));
        QCOMPARE(m.at(tools[0]).status, ToolStatus::Done);
        QCOMPARE(m.at(tools[0]).summary, QString("renamed"));
        QCOMPARE(m.at(tools[1]).toolName, QString("list_strings"));
        QCOMPARE(m.at(tools[1]).status, ToolStatus::Error);
    }

    void renamedSymbolsBecomeChipsOutsideCode() {
        TranscriptModel m;
        m.applyFrame(message("message_start", "assistant"));
        m.applyFrame(toolStart("r", "rename_function", {{"address", "0x401000"}, {"new_name", "parse_hdr"}}));
        m.applyFrame(toolEnd("r", "Renamed sub_401000 to parse_hdr"));
        m.applyFrame(QJsonObject{{"type", "turn_start"}});
        m.applyFrame(message("message_start", "assistant"));
        const QString text = "parse_hdr reads `0x402000` via `parse_hdr(x)`.\n```\nparse_hdr();\n```";
        m.applyFrame(delta("text_delta", text));
        m.applyFrame(message("message_end", "assistant"));
        const TranscriptItem& t = m.at(m.rowCount() - 1);
        QCOMPARE(t.kind, ItemKind::Text);
        QCOMPARE(t.chips.size(), 2);
        QCOMPARE(t.chips[0].value, QString("0x401000"));
        QCOMPARE(text.mid(t.chips[0].from, t.chips[0].to - t.chips[0].from), QString("parse_hdr"));
        // an inline code span that is one address is the chip, backticks included
        QCOMPARE(t.chips[1].kind, QString("addr"));
        QCOMPARE(text.mid(t.chips[1].from, t.chips[1].to - t.chips[1].from), QString("`0x402000`"));
    }

    void internalPromptsAndHiddenToolsStayOut() {
        TranscriptModel m;
        const QJsonArray userContent{QJsonObject{{"type", "text"}, {"text", "[SYSTEM] Phase: explore"}}};
        m.applyFrame(message("message_start", "user", {{"content", userContent}}));
        m.applyFrame(message("message_start", "assistant"));
        m.applyFrame(toolStart("q", "ask_user", {{"question", "Which?"}}));
        m.applyFrame(toolEnd("q", "A"));
        const QJsonArray phases{QJsonObject{{"name", "Recon"}, {"tasks", QJsonArray{QJsonObject{{"content", "list imports"}, {"status", "pending"}}}}}};
        m.applyFrame(toolStart("t", "todo", {{"ops", QJsonArray{}}}));
        m.applyFrame(toolEnd("t", "ok", false, {{"phases", phases}}));
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.activity().todoPhases, phases);
    }

    void permissionCardBlocksUntilResolved() {
        TranscriptModel m;
        m.applyFrame(QJsonObject{{"type", "rikugan_permission"},
                                 {"key", "p1"},
                                 {"tool", "bash"},
                                 {"kind", "bash"},
                                 {"details", "ls /tmp"}});
        const int row = m.rowOfKey(ItemKind::Permission, "p1");
        QVERIFY(row >= 0);
        QVERIFY(m.awaitingButtons());
        m.applyFrame(QJsonObject{{"type", "rikugan_permission_resolved"}, {"key", "p1"}, {"outcome", "allowed_chat"}});
        QVERIFY(m.at(row).resolved);
        QCOMPARE(m.at(row).resolution, QString("allowed_chat"));
        QVERIFY(!m.awaitingUser());
        // a denied call shows as denied, not as a failure
        m.applyFrame(toolStart("c", "bash", {{"command", "rm -rf /"}}));
        m.applyFrame(toolEnd("c", "Tool call denied by user: bash", true));
        const TranscriptItem& t = m.at(m.rowCount() - 1);
        QCOMPARE(t.status, ToolStatus::Denied);
        QCOMPARE(t.summary, QString("denied"));
    }

    void cancelledCardsStopAsking() {
        TranscriptModel m;
        m.addCard(ItemKind::Question, 7, QJsonObject{{"question", "Which?"}, {"options", QJsonArray{"a", "b"}}});
        m.applyFrame(QJsonObject{{"type", "extension_ui_request"}, {"id", "d1"}, {"method", "select"}, {"title", "Pick"}});
        QVERIFY(m.awaitingButtons());
        m.cancelCard(7);
        QVERIFY(m.awaitingUser());
        m.applyFrame(QJsonObject{{"type", "extension_ui_request"}, {"method", "cancel"}, {"targetId", "d1"}});
        QVERIFY(!m.awaitingUser());
        QCOMPARE(m.at(m.rowOfKey(ItemKind::Dialog, "d1")).resolution, QString("cancelled"));
    }

    void abortStopsRunningTools() {
        TranscriptModel m;
        m.setBusy(true);
        QVERIFY(m.activity().running);
        m.applyFrame(toolStart("a", "decompile_function", {{"address", "0x401000"}}));
        QCOMPARE(m.activity().currentTool, QString("decompile_function 0x401000"));
        m.setBusy(false);
        m.promptFinished("aborted", {});
        QCOMPARE(m.at(firstRow(m, ItemKind::Tool)).status, ToolStatus::Cancelled);
        QCOMPARE(m.activity().lastOutcome, QString("cancelled"));
        QVERIFY(m.activity().currentTool.isEmpty());
    }

    void btwAnswerStreamsIntoOneRow() {
        TranscriptModel m;
        m.applyFrame(QJsonObject{{"type", "btw_delta"}, {"recordId", "b1"}, {"delta", "It is "}});
        m.applyFrame(QJsonObject{{"type", "btw_delta"}, {"recordId", "b1"}, {"delta", "RC4."}});
        m.applyFrame(QJsonObject{
            {"type", "btw_record"},
            {"record", QJsonObject{{"id", "b1"}, {"question", "cipher?"}, {"answer", "It is RC4."}, {"status", "done"}}}});
        QCOMPARE(m.rowCount(), 1);
        QCOMPARE(m.at(0).title, QString("cipher?"));
        QCOMPARE(m.at(0).text, QString("It is RC4."));
        QVERIFY(!m.at(0).streaming);
    }

    void goalShownUntilDone() {
        TranscriptModel m;
        const QJsonObject goal{{"id", "g"}, {"objective", "map the C2 protocol"}, {"status", "active"}};
        m.applyFrame(QJsonObject{{"type", "goal_updated"}, {"goal", goal}});
        QCOMPARE(m.activity().goal.value("objective").toString(), QString("map the C2 protocol"));
        QJsonObject done = goal;
        done.insert("status", "complete");
        m.applyFrame(QJsonObject{{"type", "goal_updated"}, {"goal", done}});
        QVERIFY(m.activity().goal.isEmpty());
    }

    void replayRebuildsTheConversation() {
        TranscriptModel m;
        m.applyFrame(QJsonObject{{"type", "notice"}, {"message", "stale"}});
        const QJsonArray messages{
            QJsonObject{{"role", "user"}, {"content", "[SYSTEM] internal"}},
            QJsonObject{{"role", "user"}, {"content", "rename main"}},
            QJsonObject{{"role", "assistant"},
                        {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", "hmm"}},
                                               QJsonObject{{"type", "toolCall"},
                                                           {"id", "call_1"},
                                                           {"name", "rename_function"},
                                                           {"arguments", QJsonObject{{"address", "0x401000"}, {"new_name", "main"}}}}}}},
            QJsonObject{{"role", "toolResult"},
                        {"toolCallId", "call_1"},
                        {"toolName", "rename_function"},
                        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Renamed sub_401000 to main"}}}},
                        {"details", QJsonObject{{"rikugan", QJsonObject{{"mutation", QJsonObject{{"description", "rename sub_401000 → main"},
                                                                                                 {"reversible", true}}}}}}}},
            QJsonObject{{"role", "assistant"}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Done."}}}}}};
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&m, &QAbstractItemModel::rowsInserted);
        m.applyTranscript(messages);
        QCOMPARE(reset.count(), 1);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(kinds(m), (QList<ItemKind>{ItemKind::User, ItemKind::Step, ItemKind::Thinking, ItemKind::Tool, ItemKind::Step,
                                            ItemKind::Text}));
        const TranscriptItem& tool = m.at(3);
        QCOMPARE(tool.status, ToolStatus::Done);
        QCOMPARE(tool.label, QString("0x401000 \u2192 main"));
        QCOMPARE(tool.mutation, QString("rename sub_401000 → main"));
        QVERIFY(tool.mutationReversible);
        QCOMPARE(tool.mutationIndex, -1);
        // signals flow again after the replay
        QSignalSpy after(&m, &QAbstractItemModel::rowsInserted);
        m.applyFrame(QJsonObject{{"type", "notice"}, {"message", "hello"}});
        QCOMPARE(after.count(), 1);
    }
};

RK_TEST(TranscriptModelTest)
#include "test_transcript_model.moc"
