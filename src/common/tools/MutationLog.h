#pragma once
// Mutation tracking and /undo. One log per chat, in memory only.
#include <functional>
#include <mutex>

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace rk {

struct MutationRecord {
    QString toolName;
    QJsonObject arguments;
    QString reverseTool;
    QJsonObject reverseArguments;
    qint64 timestampMs = 0;
    QString description;
    bool reversible = true;
    QString irreversibleReason;

    QJsonObject toJson() const;
};

class MutationLog {
public:
    // Runs a tool directly (no approvals, no logging) and returns its text; throws ToolError on failure.
    using Executor = std::function<QString(const QString& tool, const QJsonObject& args)>;

    static const QSet<QString>& reverseBuilders();
    static const QSet<QString>& knownIrreversible();
    // The success prefixes ("Renamed", "Named", "Set ") of reversible tools; other tools are taken at their word.
    static bool mutationSucceeded(const QString& tool, const QString& result);
    // State a reverse needs, read before the mutation; "_capture_failed" holds the reason when it can't be read.
    static QJsonObject capturePreState(const QString& tool, const QJsonObject& args, const Executor& run);
    static MutationRecord buildReverseRecord(const QString& tool, const QJsonObject& args, const QJsonObject& pre);

    void push(MutationRecord r);
    QList<MutationRecord> records() const;
    int size() const;

    struct UndoOutcome {
        QString text;                 // the chat message ("Undid 2 mutation(s)." ...)
        QStringList undone;           // descriptions popped (MUTATION_UNDONE)
    };
    // Undoes the last `count` mutations, newest first: irreversible records are
    // popped with "Cannot undo", UndoNoLongerPossible pops, any other failure stops and keeps the record.
    UndoOutcome undo(int count, const Executor& run);

    // Called (any thread) after every change, for the mutation log panel.
    void setOnChanged(std::function<void()> cb);

private:
    void changed();
    mutable std::mutex mu_;
    QList<MutationRecord> log_;
    std::function<void()> onChanged_;
};

}  // namespace rk
