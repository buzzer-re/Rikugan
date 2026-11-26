#include "common/agent/ModeState.h"

#include <QSet>
#include <QStringList>

#include "common/tools/Tool.h"

namespace rk {

void ModeState::addFinding(Finding f) {
    std::lock_guard<std::mutex> g(mu_);
    // same address and category: the newer finding replaces the older (KnowledgeBase.add_finding)
    if (f.address) {
        for (Finding& e : findings_)
            if (e.address == f.address && e.category == f.category) {
                e = f;
                return;
            }
    }
    findings_ << f;
}

void ModeState::addFunction(FunctionInfo f) {
    std::lock_guard<std::mutex> g(mu_);
    for (FunctionInfo& e : functions_)
        if (e.address == f.address) {
            e = f;
            return;
        }
    functions_ << f;
}

void ModeState::addPatch(PatchRecord p) {
    std::lock_guard<std::mutex> g(mu_);
    patches_ << p;
}

void ModeState::addNote(ResearchNote n) {
    std::lock_guard<std::mutex> g(mu_);
    notes_ << n;
}

QList<Finding> ModeState::findings() const {
    std::lock_guard<std::mutex> g(mu_);
    return findings_;
}

QList<FunctionInfo> ModeState::functions() const {
    std::lock_guard<std::mutex> g(mu_);
    return functions_;
}

QList<PatchRecord> ModeState::patches() const {
    std::lock_guard<std::mutex> g(mu_);
    return patches_;
}

QList<ResearchNote> ModeState::notes() const {
    std::lock_guard<std::mutex> g(mu_);
    return notes_;
}

QStringList ModeState::hypotheses() const {
    std::lock_guard<std::mutex> g(mu_);
    QStringList out;
    for (const Finding& f : findings_)
        if (f.category == QLatin1String("hypothesis") && !out.contains(f.summary)) out << f.summary;
    return out;
}

QString ModeState::summary() const {
    const QList<FunctionInfo> fns = functions();
    const QList<Finding> fs = findings();
    const QStringList hyps = hypotheses();
    QStringList parts{QStringLiteral("## Exploration Summary\n\nGoal: %1\n").arg(userGoal)};
    if (!fns.isEmpty()) {
        parts << QStringLiteral("### Relevant Functions");
        for (const FunctionInfo& f : fns) {
            const QString rel = f.relevance != QLatin1String("medium") ? QStringLiteral(" [%1]").arg(f.relevance) : QString();
            parts << QStringLiteral("- `%1` %2: %3%4").arg(hex(f.address), f.name, f.summary, rel);
        }
    }
    if (!hyps.isEmpty()) {
        parts << QStringLiteral("\n### Hypotheses");
        for (int i = 0; i < hyps.size(); ++i) parts << QStringLiteral("%1. %2").arg(i + 1).arg(hyps[i]);
    }
    if (!fs.isEmpty()) {
        parts << QStringLiteral("\n### All Findings (%1 total)").arg(fs.size());
        for (const Finding& f : fs)
            parts << QStringLiteral("- [%1] %2: %3").arg(f.category, f.address ? hex(*f.address) : QStringLiteral("N/A"), f.summary);
    }
    return parts.join(QLatin1Char('\n'));
}

}  // namespace rk
