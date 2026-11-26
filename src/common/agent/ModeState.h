#pragma once
// What /explore, /modify and /research collect while they run. Shared by the chat context and the mode driver;
// filled by exploration_report and research_note from tool workers.
#include <cstdint>
#include <mutex>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace rk {

struct Finding {
    QString category;
    std::optional<uint64_t> address;
    QString summary;
    QString evidence;
    QString relevance = QStringLiteral("medium");
};

struct FunctionInfo {
    uint64_t address = 0;
    QString name;
    QString summary;
    QString relevance;
};

struct PatchRecord {
    uint64_t address = 0;
    QByteArray originalBytes;
    QByteArray newBytes;
    QString description;
    bool verified = false;
    QString verificationResult;
};

struct ResearchNote {
    QString genre, title, slug, path;
    QStringList relatedNotes;
};

class ModeState {
public:
    void addFinding(Finding f);
    void addFunction(FunctionInfo f);
    void addPatch(PatchRecord p);
    void addNote(ResearchNote n);
    QList<Finding> findings() const;
    QList<FunctionInfo> functions() const;
    QList<PatchRecord> patches() const;
    QList<ResearchNote> notes() const;
    // The knowledge-base summary handed to the plan synthesis (KnowledgeBase.to_summary).
    QString summary() const;

    QStringList hypotheses() const;

    QString notesDir;  // research: <binary dir>/notes
    QString userGoal;  // set before the mode starts
    // A cancelled mode: its tools stay callable until the next prompt ends and answer this instead (set before the
    // state is shared).
    QString cancelledNotice;

private:
    mutable std::mutex mu_;
    QList<Finding> findings_;
    QList<FunctionInfo> functions_;
    QList<PatchRecord> patches_;
    QList<ResearchNote> notes_;
};

}  // namespace rk
