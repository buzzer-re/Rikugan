#pragma once
// One chat's transcript as a flat list of items for the virtualized TranscriptView: timeline nodes at depth 0
// (you, the rikugan step, permission, phase, notices ...) and the rows of a step at depth 1 (thinking, text, tool
// calls, a task card for its subagents). Built from the chat's frames, its replayed transcript and its cards.
//
// Streaming touches only the tail item. Deltas do not land at once: they are revealed a few characters per display
// frame (all of a burst within ~100 ms), one dataChanged per growing row per frame, so text flows instead of jumping
// by chunks. The end of a block shows the rest at once. Timers run only while something streams. UI thread only.
#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatSession.h"
#include "common/ui/transcript/ChatActivity.h"

namespace rk::ui {

enum class ItemKind {
    User, Step, Thinking, Text, Tool, Task, Permission, Approval, Question, Dialog, Decision, Phase, Finding, Note,
    Notice, Error, CommandOutput, Btw
};
enum class ToolKind { Host, Workspace, Web, Agent };
enum class ToolStatus { Pending, Running, Done, Error, Denied, Cancelled };

struct Chip {
    int from = 0, to = 0;  // [from, to) in the item's text
    QString kind;          // addr | sym
    QString value;         // what navigation gets ("0x140012a40" or the symbol)
    bool operator==(const Chip&) const = default;
};

struct TranscriptItem {
    quint64 id = 0;        // stable for the item's life
    quint32 revision = 0;  // bumped on every change: view caches key on (id, revision, width)
    ItemKind kind = ItemKind::Notice;
    int depth = 0;
    QString key;           // toolCallId / permission key / request id / btw record id
    qint64 timeMs = 0;
    QString text;          // markdown for Text/Thinking/User/Btw/Note; plain for the others
    bool streaming = false;
    QList<Chip> chips;     // in `text` (Text items) or in `label` (Tool items)
    QString title;         // Step: "rikugan"; Phase: the phase; Finding: category; Note: title; Btw: question
    // Tool
    ToolKind toolKind = ToolKind::Host;
    QString toolName, label, summary;
    ToolStatus status = ToolStatus::Pending;
    QJsonObject args;
    QString result, diff, code;
    QJsonArray webSources;  // [{title, url, snippet}]
    qint64 startMs = 0, endMs = 0;
    // a thought is open while it streams and folds when it ends, unless the user opened or closed it meanwhile
    bool expanded = false;
    bool userToggled = false;
    // consecutive calls of one tool: the first carries the count and whether the group is open, the others its id
    int groupCount = 0;
    quint64 groupHead = 0;
    bool groupOpen = false;
    // a database change of this tool call
    QString mutation;
    bool mutationReversible = false, undone = false;
    int mutationIndex = -1;  // position in the chat's MutationLog when recorded; -1 when not undoable from here
    // Task: the subagents spawned by one assistant message
    QList<AgentRow> agents;
    // cards: Permission {key, tool, kind, details, subagentId}; Approval {kind, toolName, arguments, reason};
    // Question {question, options}; Dialog {id, method, title, options, placeholder, message}; Decision {kind,
    // payload, steps status}; Finding {address, relevance}; Note {genre, path}
    QJsonObject payload;
    bool resolved = false;
    QString resolution;    // what the user chose, or "cancelled"
    int requestId = 0;
};

class TranscriptModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit TranscriptModel(QObject* parent = nullptr);
    ~TranscriptModel() override;

    // Follows `chat` (frames, transcript replay, cards, busy state, queue). Rebuilds when the transcript loads.
    void bind(ChatSession* chat);
    ChatSession* chat() const { return chat_; }

    // The reducer's inputs, public for tests (bind connects them).
    void applyFrame(const QJsonObject& frame);
    void applyTranscript(const QJsonArray& messages);
    void applyState(const QJsonObject& getState);
    void setBusy(bool busy);
    void addCard(ItemKind kind, int requestId, const QJsonObject& payload);
    void cancelCard(int requestId);
    void promptFinished(const QString& status, const QString& error);
    // A command the user typed that omc does not echo back as a user message.
    void addLocalUser(const QString& text);
    // Host tool names (Host kind); the rest are classified by name.
    void setHostTools(const QSet<QString>& names);
    void clear();

    const TranscriptItem& at(int row) const { return items_.at(row); }
    int rowOfId(quint64 id) const;
    int reversibleChanges() const;  // records of the chat's MutationLog that /undo can revert
    int rowOfKey(ItemKind kind, const QString& key) const;
    void toggleExpanded(int row);
    void toggleGroup(int row);
    QList<int> groupRows(int headRow) const;  // the head and its members, in order; empty when not a group
    // A card answered from the view: shows the answer, no more buttons.
    void resolveCard(int row, const QString& resolution);
    const ChatActivity& activity() const { return activity_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;  // DisplayRole: the item's plain text

Q_SIGNALS:
    void activityChanged();
    void streamingTail();  // the newest item grew (the view keeps it pinned)
    // /agents, /login, /new ... and /model without an argument: for the panel
    void uiAction(const QString& action, const QString& arg);
    void awaitingChanged();  // a card opened or closed

public:
    bool awaitingUser() const;  // any unresolved card
    bool awaitingButtons() const;  // a card only buttons can answer

private:
    int append(TranscriptItem item);
    void touch(int row, bool streaming = false);
    void flushPending();
    void stream(quint64 id, const QString& delta);  // queue a delta for `id` (revealed by revealTick)
    void revealTick();
    void revealAll(quint64 id = 0);                  // the rest of `id` now (0: of every row)
    void closeStaleCards();
    TranscriptItem* itemById(quint64 id);
    void activity(const std::function<void(ChatActivity&)>& fn);

    void onMessageStart(const QJsonObject& m);
    void onMessageUpdate(const QJsonObject& f);
    void onMessageEnd(const QJsonObject& m);
    void onToolStart(const QString& id, const QString& name, const QJsonObject& args, qint64 now);
    void onToolEnd(const QString& id, const QJsonObject& result, bool isError, qint64 now);
    void onSubagentEvent(const QJsonObject& f);
    void refreshAgents();
    void finishStreaming();
    // a thought that stopped streaming: its end time, and folded unless the user toggled it
    void foldThought(TranscriptItem& t);
    void ensureStep(qint64 now);
    quint64 textItem(ItemKind kind, qint64 now);
    void addNotice(const QString& text, bool error = false);
    void fillToolResult(TranscriptItem& t, const QJsonObject& result, bool isError);
    void restoreAssistant(const QJsonObject& m);
    void restoreToolResult(const QJsonObject& m);
    void computeChips(TranscriptItem& t);
    void learnSymbols(const QJsonObject& args);  // {address, new_name|name|function_name}
    ToolKind kindOf(const QString& tool) const;

    QPointer<ChatSession> chat_;
    QList<TranscriptItem> items_;
    quint64 nextId_ = 1;
    QHash<quint64, int> rowCache_;  // id -> row (rows only grow or reset)

    // the current turn
    quint64 step_ = 0, text_ = 0, thinking_ = 0, task_ = 0;
    QHash<QString, quint64> tools_;      // toolCallId -> item (cleared at turn_start: providers reuse ids)
    QHash<int, QString> blockTool_;      // contentIndex of the streaming message -> toolCallId
    QHash<QString, quint64> agentTask_;  // spawn_subagent toolCallId -> Task item
    QSet<QString> hostTools_;
    QHash<QString, QString> knownSymbols_;  // name -> "0x..." (renames and lookups of this chat: chips)
    QRegularExpression symbolsRe_;          // every known name; empty pattern: rebuild
    QString lastError_;

    ChatActivity activity_;
    QSet<int> pending_;
    QTimer flush_;
    struct Backlog {
        QString rest;            // streamed text not shown yet
        qsizetype perFrame = 0;  // characters shown per frame
    };
    QHash<quint64, Backlog> backlog_;
    QTimer reveal_;
    int registryObserver_ = 0;
    bool agentsDirty_ = false;
};

}  // namespace rk::ui
