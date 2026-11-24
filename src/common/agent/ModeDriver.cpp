#include "common/agent/ModeDriver.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatSession.h"
#include "common/agent/ChatTools.h"
#include "common/agent/DbContext.h"
#include "common/agent/ModeState.h"
#include "common/agent/Skills.h"
#include "common/agent/Subagents.h"
#include "common/agent/SystemPrompt.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/OmcSession.h"
#include "common/security/Sanitize.h"
#include "common/util/Embedded.h"
#include "common/util/Files.h"

namespace rk {

namespace {

constexpr int kPlanStepTurns = 10;     // turns per /plan step
constexpr int kExecuteStepTurns = 10;  // turns per /modify change
constexpr int kMaxChanges = 20;        // changes per /modify plan at most
constexpr int kDocumentTurns = 15;     // turns of the /research note-writing phase
constexpr int kMinResearchExploreTurns = 2;
constexpr int kSkillMaxChars = 50000;  // skill body cap, in code points
constexpr int kContextMessages = 6;    // last messages quoted as /modify's prior context
constexpr int kContextChars = 600;     // chars kept per quoted message

// The chat view hides user messages that start with this marker (mode prompts are not the user's words).
const QString kHidden = QStringLiteral("[SYSTEM] ");

QString modeName(Mode m) {
    switch (m) {
    case Mode::Plan: return QStringLiteral("plan");
    case Mode::Explore: return QStringLiteral("explore");
    case Mode::Modify: return QStringLiteral("modify");
    case Mode::Research: return QStringLiteral("research");
    }
    return {};
}

// prompts/modes/<rel> without its final newline (MANIFEST conventions).
QString promptFile(const char* rel) {
    QString t = embeddedText(std::string("prompts/modes/") + rel);
    if (t.endsWith(QLatin1Char('\n'))) t.chop(1);
    return t;
}

// One pass over {{name}} placeholders, so substituted text is never scanned again.
QString fill(const QString& tpl, const QHash<QString, QString>& vals) {
    static const QRegularExpression re(QStringLiteral("\\{\\{(\\w+)\\}\\}"));
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(tpl); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += tpl.mid(last, m.capturedStart() - last);
        const auto v = vals.constFind(m.captured(1));
        out += v != vals.constEnd() ? *v : m.captured(0);
        last = m.capturedEnd();
    }
    out += tpl.mid(last);
    return out;
}

QString withoutMarker(const QString& t) { return t.startsWith(kHidden) ? t.mid(kHidden.size()) : t; }

// The numbered steps of a plan answer
QStringList parsePlan(const QString& text) {
    static const QRegularExpression step(QStringLiteral("^\\d+[.)]\\s*(.+)$"));
    QStringList steps;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        const QRegularExpressionMatch m = step.match(line);
        if (m.hasMatch()) steps << m.captured(1).trimmed();
        if (line.toUpper() == QLatin1String("END_PLAN")) break;
    }
    return steps;
}

// A skill body with injection markers stripped, capped and wrapped in <skill name="...">
QString sanitizeSkillBody(const QString& body, const QString& name) {
    if (body.isEmpty()) return body;
    QString text = sanitize::stripInjectionMarkers(body);
    qsizetype points = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (points == kSkillMaxChars) {
            text = text.left(i) + QStringLiteral("\n... [truncated]");
            break;
        }
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) ++i;
        ++points;
    }
    static const QRegularExpression closing(QStringLiteral("</\\s*skill\\s*>"), QRegularExpression::CaseInsensitiveOption);
    text.replace(closing, QStringLiteral("[/skill]"));
    return QStringLiteral("<skill name=\"%1\">\n%2\n</skill>").arg(sanitize::escapeAttr(name), text);
}

// str.title()
QString pyTitle(const QString& s) {
    QString out;
    out.reserve(s.size());
    bool prevCased = false;
    for (QChar c : s) {
        if (c.isLetter()) {
            out += prevCased ? c.toLower() : c.toUpper();
            prevCased = true;
        } else {
            out += c;
            prevCased = false;
        }
    }
    return out;
}

QString messageText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    QString t;
    for (const QJsonValue& part : content.toArray()) {
        const QJsonObject p = part.toObject();
        if (p.value(QStringLiteral("type")).toString() == QLatin1String("text")) t += p.value(QStringLiteral("text")).toString();
    }
    return t;
}

// KnowledgeBase.has_minimum_for_planning / planning_gap_description
bool hasHighHypothesis(const ModeState& kb) {
    const QList<Finding> fs = kb.findings();
    return std::any_of(fs.begin(), fs.end(), [](const Finding& f) {
        return f.category == QLatin1String("hypothesis") && f.relevance == QLatin1String("high");
    });
}

QString planningGap(const ModeState& kb) {
    QStringList gaps;
    if (kb.functions().isEmpty()) gaps << QStringLiteral("0 relevant functions (need ≥1)");
    if (kb.hypotheses().isEmpty()) gaps << QStringLiteral("0 hypotheses (need ≥1)");
    else if (!hasHighHypothesis(kb)) gaps << QStringLiteral("0 high-relevance hypotheses (need ≥1 with relevance='high')");
    return gaps.join(QStringLiteral("; "));
}

// What a mode needs from the UI thread, read once in start().
struct Setup {
    Mode mode = Mode::Plan;
    QString goal;       // the text after the command
    QString skillSlug;  // a mode skill (/plan or /modify) or empty
    QString skillBody;
    int turnLimit = 100;
    QString binaryName;
    QString notesDir;
};

// Counts the chat session's turn_start frames for the running prompt and aborts it past the cap.
struct TurnWatch {
    std::atomic<int> turns{0};
    std::atomic<int> cap{0};
    std::atomic<bool> overLimit{false};
    std::weak_ptr<OmcSession> session;
};

class ModeRun {
public:
    ModeRun(OmcSession& s, std::shared_ptr<ChatContext> ctx, std::shared_ptr<DbContext> db, Setup setup,
            std::shared_ptr<CancelToken> token)
        : s_(s), ctx_(std::move(ctx)), db_(std::move(db)), in_(std::move(setup)), mode_(modeName(in_.mode)),
          token_(std::move(token)) {}

    void run() {
        watch_->session = s_.weak_from_this();
        std::shared_ptr<TurnWatch> w = watch_;
        ctx_->setFrameHook([w](std::string_view type) {
            if (type != "turn_start") return;
            const int n = ++w->turns;
            const int cap = w->cap.load();
            if (cap > 0 && n > cap && !w->overLimit.exchange(true))
                if (auto s = w->session.lock()) s->abort();
        });
        switch (in_.mode) {
        case Mode::Plan: runPlan(); break;
        case Mode::Explore: runExplore(); break;
        case Mode::Modify: runModify(); break;
        case Mode::Research: runResearch(); break;
        }
        // the mode ends here whatever happened: no read-only phase, no mode-only tools, no mode state
        ctx_->setFrameHook({});
        ctx_->setPhase({}, false);
        setTools({}, {QStringLiteral("exploration_report"), QStringLiteral("research_note")});
        ctx_->mode.reset();
        phaseEvent(QStringLiteral("done"), endText_);
    }

private:
    enum class End { Done, Limit, Cancelled, Error };
    struct Reply {
        End end = End::Error;
        QString text;
        QString error;
        int turns = 0;
    };

    // ---- plumbing ----

    void post(const QJsonObject& e) {
        if (ctx_->ui) ctx_->ui->postUiEvent(e);
    }
    void say(const QString& text, bool error) {
        post(QJsonObject{{"type", "rikugan_command_output"}, {"text", text}, {"isError", error}});
    }
    void phaseEvent(const QString& phase, const QString& text) {
        post(QJsonObject{{"type", "rikugan_mode_phase"}, {"mode", mode_}, {"phase", phase}, {"text", text}});
    }
    void enterPhase(const QString& phase, bool readOnly, const QString& text) {
        ctx_->setPhase(phase, readOnly);
        phaseEvent(phase, text);
    }
    void stepEvent(int index, const QString& status, const QString& text) {
        post(QJsonObject{{"type", "rikugan_plan_step"}, {"index", index}, {"status", status}, {"text", text}});
    }
    void fail(const QString& text) {
        say(text, true);
        endText_ = text;
    }

    // Mode-only chat tools change only between prompts, while the session is idle.
    void setTools(const QStringList& show, const QStringList& hide) {
        bool changed = false;
        for (const QString& t : show) {
            changed |= !ctx_->toolShown(t);
            ctx_->setToolShown(t, true);
        }
        for (const QString& t : hide) {
            changed |= ctx_->toolShown(t);
            ctx_->setToolShown(t, false);
        }
        if (changed && s_.isOpen())
            s_.request(QJsonObject{{"type", "set_host_tools"}, {"tools", chatHostTools(*ctx_)}});
    }

    // One prompt on the chat's session, run to completion or aborted after `cap` turns.
    Reply send(const QString& text, int cap) {
        // stop() between prompts cancels the token but finds no run to abort
        if (token_->cancelled()) return Reply{End::Cancelled, {}, {}, 0};
        watch_->turns = 0;
        watch_->overLimit = false;
        watch_->cap = cap;
        QString body = withoutMarker(text);
        if (first_) {
            // the context block a typed prompt gets, once, behind the hidden-message marker
            body = contextPrefix(*db_, *ctx_) + body;
            first_ = false;
        }
        const OmcSession::PromptOutcome out = s_.prompt(kHidden + body);
        watch_->cap = 0;
        Reply r;
        r.text = out.lastText;
        r.error = out.error;
        r.turns = watch_->turns.load();
        if (out.status == QLatin1String("completed")) r.end = End::Done;
        else if (out.status == QLatin1String("aborted")) r.end = watch_->overLimit ? End::Limit : End::Cancelled;
        else r.end = End::Error;
        return r;
    }

    // A prompt that did not finish: the mode ends.
    bool stopped(const Reply& r) {
        if (r.end == End::Cancelled) {
            endText_ = QStringLiteral("Cancelled.");
            return true;
        }
        if (r.end == End::Error) {
            fail(r.error.isEmpty() ? QStringLiteral("The model request failed.") : r.error);
            return true;
        }
        return false;
    }

    UserAnswer decide(const QString& kind, const QJsonObject& payload) {
        if (!ctx_->ui) return UserAnswer{true, {}};
        return ctx_->ui->askDecision(kind, payload, token_);
    }

    // The approval loop of /plan and /modify: shows each parsed plan, regenerates on reject. Returns the approved
    // steps, or nothing when the mode ends.
    std::optional<QStringList> approve(Reply r, const std::function<QString(const QString& feedback)>& regenerate,
                                       const QString& invalidFirst, const QString& invalidAgain, const QString& cancelled) {
        QString invalid = invalidFirst;
        for (;;) {
            if (stopped(r)) return std::nullopt;
            const QStringList steps = parsePlan(r.text);
            if (steps.isEmpty()) {
                fail(invalid);
                return std::nullopt;
            }
            for (int i = 0; i < steps.size(); ++i) stepEvent(i, QStringLiteral("pending"), steps[i]);
            const UserAnswer a = decide(QStringLiteral("plan"),
                                        QJsonObject{{"text", r.text}, {"steps", QJsonArray::fromStringList(steps)}});
            if (token_->cancelled()) {
                endText_ = QStringLiteral("Cancelled.");
                return std::nullopt;
            }
            if (a.cancelled || a.text == QLatin1String("cancel")) {
                fail(cancelled);
                return std::nullopt;
            }
            if (a.text == QLatin1String("approve")) return steps;
            // the user dropped, reworded or added steps on the card: that list runs
            if (a.text.startsWith(QLatin1String("approve:"))) {
                QStringList edited;
                for (const QJsonValue& v : QJsonDocument::fromJson(a.text.mid(8).toUtf8()).array())
                    if (!v.toString().trimmed().isEmpty()) edited << v.toString().trimmed();
                if (!edited.isEmpty()) {
                    post(QJsonObject{{"type", "rikugan_plan_revised"}, {"steps", QJsonArray::fromStringList(edited)}});
                    return edited;
                }
            }
            // "regenerate" or "reject:<feedback>"
            const QString feedback = a.text.startsWith(QLatin1String("reject:")) ? a.text.mid(7).trimmed() : QString();
            r = send(regenerate(feedback.isEmpty() ? QString() : QStringLiteral(" Their feedback: ") + feedback), in_.turnLimit);
            invalid = invalidAgain;
        }
    }

    // ---- /plan ----

    void runPlan() {
        QString prompt;
        if (!in_.skillSlug.isEmpty())
            prompt = fill(promptFile("plan/skill_generation.md"), {{"skill_name", in_.skillSlug},
                                                                   {"skill_body", sanitizeSkillBody(in_.skillBody, in_.skillSlug)},
                                                                   {"user_message", in_.goal}});
        else
            prompt = fill(promptFile("plan/generation.md"), {{"user_message", in_.goal}});
        enterPhase(QStringLiteral("plan"), true, QStringLiteral("Generating a plan: %1").arg(in_.goal.left(60)));
        const auto steps = approve(
            send(prompt, in_.turnLimit),
            [](const QString& fb) { return fill(promptFile("plan/regenerate.md"), {{"feedback", fb}}); },
            QStringLiteral("Failed to generate a valid plan."), QStringLiteral("Failed to generate a valid plan."),
            QStringLiteral("Plan cancelled by user."));
        if (!steps) return;

        enterPhase(QStringLiteral("execute"), false, QStringLiteral("Plan approved. Executing steps."));
        for (int i = 0; i < steps->size(); ++i) {
            const QString& step = (*steps)[i];
            stepEvent(i, QStringLiteral("running"), step);
            const Reply r = send(fill(promptFile("plan/step.md"), {{"index", QString::number(i + 1)}, {"description", step}}),
                                 kPlanStepTurns);
            if (r.end == End::Cancelled || r.end == End::Error) {
                stepEvent(i, QStringLiteral("failed"), step);
                for (int j = i + 1; j < steps->size(); ++j) stepEvent(j, QStringLiteral("skipped"), (*steps)[j]);
                stopped(r);
                return;
            }
            stepEvent(i, QStringLiteral("done"), step);
        }
    }

    // ---- /explore (explore only) ----

    void runExplore() {
        ctx_->mode = std::make_shared<ModeState>();
        ctx_->mode->userGoal = in_.goal;
        setTools({QStringLiteral("exploration_report")}, {});
        enterPhase(QStringLiteral("explore"), false, QStringLiteral("Starting exploration: %1").arg(in_.goal.left(60)));
        const Reply r = send(promptFile("exploration/explore.md") + QStringLiteral("\n\nUser request: ") + in_.goal, in_.turnLimit);
        if (stopped(r)) return;
        if (r.end == End::Limit) {
            fail(QStringLiteral("Exploration turn limit reached (%1) without sufficient findings for planning. "
                                "Try a more specific request.")
                     .arg(in_.turnLimit));
            return;
        }
        say(withoutMarker(fill(promptFile("exploration/explore_only_summary.md"), {{"knowledge_summary", ctx_->mode->summary()}})),
            false);
    }

    // ---- /modify ----

    QString recentContext() {
        const QJsonObject r = s_.request(QJsonObject{{"type", "get_messages"}});
        const QJsonArray msgs = r.value(QStringLiteral("data")).toObject().value(QStringLiteral("messages")).toArray();
        QStringList said;
        for (qsizetype i = std::max<qsizetype>(0, msgs.size() - kContextMessages); i < msgs.size(); ++i) {
            const QJsonObject m = msgs[i].toObject();
            const QString role = m.value(QStringLiteral("role")).toString();
            if (role != QLatin1String("user") && role != QLatin1String("assistant")) continue;
            QString text = messageText(m.value(QStringLiteral("content")));
            if (role == QLatin1String("user")) text = stripContextPrefix(text);
            text = text.trimmed();
            if (text.isEmpty()) continue;
            said << QStringLiteral("%1: %2").arg(role == QLatin1String("user") ? QStringLiteral("User") : QStringLiteral("Rikugan"),
                                                 text.left(kContextChars));
        }
        if (said.isEmpty()) return {};
        return fill(promptFile("exploration/prior_context.md"), {{"conversation_digest", said.join(QStringLiteral("\n\n"))}});
    }

    void persistPlan(const QString& goal, const QStringList& steps) {
        if (db_->host->databasePath().isEmpty()) return;
        const QString path = db_->memoryPath();
        QString text = QStringLiteral("\n## Plan (%1)\nGoal: %2\n")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")), goal.left(200));
        for (int i = 0; i < steps.size(); ++i) text += QStringLiteral("%1. %2\n").arg(i + 1).arg(steps[i]);
        text += QLatin1Char('\n');
        QFile f(path);
        const bool fresh = !QFileInfo::exists(path);
        if (!f.open(QIODevice::Append)) return;
        if (fresh) f.write(memoryFileHeader());
        f.write(text.toUtf8());
    }

    void runModify() {
        // a mode skill's text leads the goal
        const QString goal = in_.skillSlug.isEmpty()
                                 ? in_.goal
                                 : QStringLiteral("[Skill: %1]\n%2\n\nUser request: %3")
                                       .arg(in_.skillSlug, sanitizeSkillBody(in_.skillBody, in_.skillSlug), in_.goal);
        auto kb = std::make_shared<ModeState>();
        kb->userGoal = goal;
        ctx_->mode = kb;

        // EXPLORE: an isolated subagent shares the knowledge base
        enterPhase(QStringLiteral("explore"), false, QStringLiteral("Starting exploration: %1").arg(in_.goal.left(60)));
        SubagentSpec spec;
        spec.name = QStringLiteral("Exploration");
        const QString prior = recentContext();
        spec.task = prior.isEmpty() ? goal : prior + QStringLiteral("\n\n") + goal;
        spec.maxTurns = in_.turnLimit;
        spec.systemAddendum = promptFile("exploration/explore.md");
        spec.extraTools = QStringList{QStringLiteral("exploration_report")};
        spec.silent = false;
        const SubagentOutcome sub = runSubagent(*ctx_, spec, token_);
        if (sub.status == QLatin1String("aborted") || token_->cancelled()) {
            endText_ = QStringLiteral("Cancelled.");
            return;
        }
        if (sub.status == QLatin1String("error")) {
            fail(QStringLiteral("Exploration subagent failed: %1").arg(sub.error));
            return;
        }
        if (kb->functions().isEmpty() || kb->hypotheses().isEmpty() || !hasHighHypothesis(*kb)) {
            fail(QStringLiteral("Subagent exploration finished without sufficient findings. Gap: %1. Try a more specific request.")
                     .arg(planningGap(*kb)));
            return;
        }

        // PLAN
        setTools({QStringLiteral("exploration_report")}, {});
        enterPhase(QStringLiteral("plan"), true, QStringLiteral("Subagent exploration complete. Moving to planning."));
        const QString summary = kb->summary();
        const QString synthesis = fill(promptFile("exploration/plan_synthesis.md"), {{"knowledge_summary", summary}});
        // the explore section leads the first message instead of being a system-prompt addendum for every phase
        const QString first = promptFile("exploration/explore.md") + QStringLiteral("\n\n") +
                              withoutMarker(fill(promptFile("exploration/subagent_summary.md"), {{"knowledge_summary", summary}})) +
                              QStringLiteral("\n\n") + synthesis;
        const auto steps = approve(
            send(first, in_.turnLimit),
            [&synthesis](const QString& fb) {
                return fill(promptFile("exploration/plan_regenerate.md"), {{"feedback", fb}, {"plan_synthesis", synthesis}});
            },
            QStringLiteral("Failed to generate a valid modification plan from exploration findings."),
            QStringLiteral("Failed to generate a valid modification plan."), QStringLiteral("Modification plan cancelled by user."));
        if (!steps) return;
        enterPhase(QStringLiteral("execute"), false, QStringLiteral("Plan approved. Executing patches."));
        persistPlan(goal, *steps);

        // EXECUTE
        for (int i = 0; i < steps->size(); ++i) {
            const QString& step = (*steps)[i];
            if (i >= kMaxChanges) {
                for (int j = i; j < steps->size(); ++j) stepEvent(j, QStringLiteral("skipped"), (*steps)[j]);
                fail(QStringLiteral("Execute turn limit reached (%1). Some patches may not have been applied.").arg(kMaxChanges));
                return;
            }
            stepEvent(i, QStringLiteral("running"), step);
            const Reply r = send(fill(promptFile("exploration/execute.md"), {{"index", QString::number(i + 1)},
                                                                             {"total", QString::number(steps->size())},
                                                                             {"description", step}}),
                                 kExecuteStepTurns);
            if (r.end == End::Cancelled || r.end == End::Error) {
                stepEvent(i, QStringLiteral("failed"), step);
                for (int j = i + 1; j < steps->size(); ++j) stepEvent(j, QStringLiteral("skipped"), (*steps)[j]);
                stopped(r);
                return;
            }
            stepEvent(i, QStringLiteral("done"), step);
        }

        // SAVE
        enterPhase(QStringLiteral("save"), false, QStringLiteral("All patches applied. Awaiting save decision."));
        const QList<PatchRecord> patches = kb->patches();
        QJsonArray detail;
        for (const PatchRecord& p : patches)
            detail.append(QJsonObject{{"address", hex(p.address)},
                                      {"original_hex", QString::fromLatin1(p.originalBytes.toHex())},
                                      {"new_hex", QString::fromLatin1(p.newBytes.toHex())},
                                      {"description", p.description}});
        const UserAnswer a = decide(QStringLiteral("save"), QJsonObject{{"patches", detail}});
        if (a.cancelled || token_->cancelled()) {
            endText_ = QStringLiteral("Cancelled.");
            return;
        }
        if (a.text == QLatin1String("save")) {
            endText_ = QStringLiteral("%1 patch(es) saved.").arg(patches.size());
            stopped(send(promptFile("exploration/save.md"), kExecuteStepTurns));
            return;
        }
        // discard: the original bytes go back, newest patch first
        bool rolledBack = false;
        bool anyOriginal = false;
        for (auto it = patches.crbegin(); it != patches.crend(); ++it) {
            if (it->originalBytes.isEmpty()) continue;
            if (!anyOriginal) rolledBack = true;
            anyOriginal = true;
            try {
                if (!ctx_->host->writeBytes(it->address, it->originalBytes)) rolledBack = false;
            } catch (const ToolError&) {
                rolledBack = false;
            }
        }
        endText_ = rolledBack ? QStringLiteral("%1 patch(es) discarded; original bytes restored.").arg(patches.size())
                              : QStringLiteral("%1 patch(es) discarded; the changes stay until the database is reloaded "
                                               "without saving.")
                                    .arg(patches.size());
        stopped(send(promptFile(rolledBack ? "exploration/discard_restored.md" : "exploration/discard_kept.md"), kExecuteStepTurns));
    }

    // ---- /research (no reviewer chain) ----

    void runResearch() {
        ensureDir(in_.notesDir);
        auto kb = std::make_shared<ModeState>();
        kb->userGoal = in_.goal;
        kb->notesDir = in_.notesDir;
        ctx_->mode = kb;

        // EXPLORE: research_note withheld
        setTools({QStringLiteral("exploration_report")}, {QStringLiteral("research_note")});
        enterPhase(QStringLiteral("explore"), false, QStringLiteral("Starting research: %1").arg(in_.goal.left(60)));
        const Reply explore =
            send(promptFile("research/explore.md") + QStringLiteral("\n\nUser request: ") + in_.goal, in_.turnLimit);
        if (stopped(explore)) return;
        if (explore.end == End::Limit)
            say(QStringLiteral("Exploration turn limit reached (%1) without sufficient findings for planning. "
                               "Try a more specific request.")
                    .arg(in_.turnLimit),
                true);
        if (explore.turns < kMinResearchExploreTurns) {
            endText_ = QStringLiteral("Exploration ended too early; no notes were written.");
            return;
        }

        // DOCUMENT
        setTools({QStringLiteral("research_note")}, {});
        enterPhase(QStringLiteral("document"), false, QStringLiteral("Exploration complete. Writing research notes..."));
        const Reply doc = send(fill(promptFile("research/document.md"), {{"knowledge_summary", kb->summary()}}), kDocumentTurns);
        if (doc.end == End::Cancelled) {
            endText_ = QStringLiteral("Cancelled.");
            return;
        }

        // INDEX
        enterPhase(QStringLiteral("index"), false, QStringLiteral("Writing the research index."));
        const QList<ResearchNote> notes = kb->notes();
        if (!notes.isEmpty()) {
            QMap<QString, QList<ResearchNote>> byGenre;
            for (const ResearchNote& n : notes) byGenre[n.genre] << n;
            QStringList sections;
            for (auto it = byGenre.cbegin(); it != byGenre.cend(); ++it) {
                QStringList lines;
                for (const ResearchNote& n : it.value()) lines << QStringLiteral("- [[%1]] — %2").arg(n.slug, n.title);
                QString genre = it.key();
                sections << fill(promptFile("research/index_genre.md"),
                                 {{"genre_title", pyTitle(genre.replace(QLatin1Char('-'), QLatin1Char(' ')))},
                                  {"note_lines", lines.join(QLatin1Char('\n'))}});
            }
            const QString index = fill(promptFile("research/index.md"), {{"binary_name", in_.binaryName},
                                                                         {"goal", in_.goal},
                                                                         {"genre_sections", sections.join(QStringLiteral("\n\n"))}});
            const QString path = in_.notesDir + QStringLiteral("/index.md");
            QString error;
            if (writeFileAtomic(path, (index + QLatin1Char('\n')).toUtf8(), 0, &error))
                post(QJsonObject{{"type", "rikugan_research_note"}, {"genre", "index"}, {"title", "Research Index"}, {"path", path}});
            else
                say(QStringLiteral("Cannot write %1: %2").arg(path, error), true);
        }
        QString complete;
        if (notes.isEmpty()) {
            complete = fill(promptFile("research/complete_empty.md"), {{"note_count", QStringLiteral("0")}, {"notes_dir", in_.notesDir}});
        } else {
            QStringList lines;
            for (const ResearchNote& n : notes) lines << QStringLiteral("- **%1** (%2)").arg(n.title, n.genre);
            complete = fill(promptFile("research/complete.md"), {{"note_count", QString::number(notes.size())},
                                                                 {"notes_dir", in_.notesDir},
                                                                 {"note_lines", lines.join(QLatin1Char('\n'))}});
        }
        say(complete, false);
        endText_ = QStringLiteral("%1 note(s) written.").arg(notes.size());
    }

    OmcSession& s_;
    std::shared_ptr<ChatContext> ctx_;
    std::shared_ptr<DbContext> db_;
    Setup in_;
    QString mode_;
    std::shared_ptr<TurnWatch> watch_ = std::make_shared<TurnWatch>();
    std::shared_ptr<CancelToken> token_;
    bool first_ = true;
    QString endText_;
};

}  // namespace

void ModeDriver::start(ChatSession& chat, Mode mode, const QString& goal, const QString& skillSlug) {
    if (goal.trimmed().isEmpty() && skillSlug.isEmpty()) {
        chat.postLocal(QJsonObject{{"type", "rikugan_command_output"},
                                   {"text", QStringLiteral("Usage: /%1 <request>").arg(modeName(mode))},
                                   {"isError", true}});
        return;
    }
    const RikuganConfig& cfg = RikuganConfig::instance();
    const std::shared_ptr<ChatContext> ctx = chat.context();
    const std::shared_ptr<DbContext> db = chat.db();
    Setup setup;
    setup.mode = mode;
    setup.goal = goal.trimmed();
    setup.skillSlug = skillSlug;
    if (!skillSlug.isEmpty()) setup.skillBody = skillBody(skillSlug);
    setup.turnLimit = cfg.explorationTurnLimit;
    const QString dbPath = db->host->databasePath();
    setup.binaryName = dbPath.isEmpty() ? QStringLiteral("unknown") : QFileInfo(dbPath).fileName();
    setup.notesDir = db->notesDir();
    chat.runJob([ctx, db, setup](OmcSession& s, const std::shared_ptr<CancelToken>& token) {
        ModeRun(s, ctx, db, setup, token).run();
    });
}

}  // namespace rk
