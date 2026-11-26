#pragma once
// The live state of one chat that the panel shows outside the transcript: the activity strip (running state, current
// tool, agents, todo, goal, mode) and the status line (model, usage). Built by TranscriptModel.
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace rk::ui {

struct AgentRow {
    QString id;                 // subagent id (empty until the subagent registered)
    QString name, task;
    QString status = QStringLiteral("running");  // running | done | failed | cancelled
    QString currentTool, currentArgs;
    int tools = 0;
    qint64 tokens = 0, startedMs = 0, endedMs = 0;
    QString output;             // newest reply line / final summary
    bool operator==(const AgentRow&) const = default;
};

struct ChatActivity {
    bool running = false;
    qint64 turnStartMs = 0;     // when the chat went busy
    QString currentTool;        // "decompile_function sub_401000" while a tool runs
    QList<AgentRow> agents;     // this chat's subagents, newest last
    QJsonArray todoPhases;      // [{name, tasks:[{content, status}]}]
    QJsonObject goal;           // goal_updated.goal; empty: no goal
    QString mode;               // the running mode (/plan, /modify ...), empty when none
    QString phase;              // its phase
    int undoable = 0;           // database changes /undo can revert
    qint64 tokensIn = 0, tokensOut = 0;
    double cost = 0;
    int contextPercent = -1;    // -1: unknown
    qint64 contextTokens = 0, contextWindow = 0;
    QString model, thinking;
    QString lastOutcome;        // idle | error | cancelled (how the last prompt ended)

    int runningAgents() const {
        int n = 0;
        for (const AgentRow& a : agents) n += a.status == QLatin1String("running") ? 1 : 0;
        return n;
    }
    // (done, total) over every todo task
    std::pair<int, int> todoProgress() const {
        int done = 0, total = 0;
        for (const auto& p : todoPhases)
            for (const auto& t : p.toObject().value(QStringLiteral("tasks")).toArray()) {
                ++total;
                if (t.toObject().value(QStringLiteral("status")).toString() == QLatin1String("completed")) ++done;
            }
        return {done, total};
    }
};

}  // namespace rk::ui
