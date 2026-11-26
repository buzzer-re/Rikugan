#include "common/ui/transcript/TranscriptModel.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>

#include "common/agent/ChatExport.h"
#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/agent/Subagents.h"
#include "common/omc/UserErrors.h"
#include "common/tools/MutationLog.h"
#include "common/tools/ToolRegistry.h"
#include "common/util/ModuleLifetime.h"

namespace rk::ui {

namespace {

constexpr int kFlushMs = 16;
constexpr int kRevealMs = 16;     // one display frame
constexpr int kRevealFrames = 6;  // a burst is all on screen this many frames after it came

QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// the goal the activity strip shows: none once it is complete or dropped
QJsonObject liveGoal(const QJsonObject& g) {
    const QString status = str(g, "status");
    return status == QLatin1String("complete") || status == QLatin1String("dropped") ? QJsonObject() : g;
}

// tools whose payload the transcript shows elsewhere (the question card, the todo list)
bool hiddenTool(const QString& name) { return name == QLatin1String("ask_user") || name == QLatin1String("todo"); }

QString contentText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue& v : content.toArray()) {
        const QJsonObject b = v.toObject();
        if (str(b, "type") == QLatin1String("text")) parts << str(b, "text");
    }
    return parts.join(QLatin1Char('\n'));
}

// The argument a tool row shows next to its name.
QString mainArg(const QJsonObject& a) {
    for (const char* k : {"path", "command", "pattern", "query", "address", "function_name", "name", "type_name", "url",
                          "task", "start_address", "ea"}) {
        const QJsonValue v = a.value(QLatin1String(k));
        if (v.isString() && !v.toString().isEmpty()) return v.toString().section(QLatin1Char('\n'), 0, 0).left(120);
        if (v.isDouble()) return QStringLiteral("0x%1").arg(qulonglong(v.toDouble()), 0, 16);
    }
    return {};
}

QString toolLabel(const QString& name, const QJsonObject& a) {
    QString label = mainArg(a);
    const QString renamedTo = a.value(QLatin1String("new_name")).toString();
    if (!renamedTo.isEmpty()) label += QStringLiteral(" \u2192 ") + renamedTo;
    if (name == QLatin1String("web_search") && label.size()) label = QLatin1Char('"') + label + QLatin1Char('"');
    return label;
}

// A one-glance outcome of a tool call: "renamed", "3 lines", the first line ...
QString summarize(const QString& tool, const QString& text, bool isError, const QJsonObject& details) {
    if (isError) {
        if (text.contains(QLatin1String("denied by user"))) return QStringLiteral("denied");
        return text.section(QLatin1Char('\n'), 0, 0).left(60);
    }
    if (tool == QLatin1String("web_search")) {
        const int n = int(details.value(QLatin1String("response")).toObject().value(QLatin1String("sources")).toArray().size());
        return QStringLiteral("%1 result%2").arg(n).arg(n == 1 ? "" : "s");
    }
    if (tool == QLatin1String("edit")) {
        int add = 0, del = 0;
        for (const QString& l : details.value(QLatin1String("diff")).toString().split(QLatin1Char('\n'))) {
            if (l.startsWith(QLatin1Char('+'))) ++add;
            else if (l.startsWith(QLatin1Char('-'))) ++del;
        }
        return QStringLiteral("+%1 \u2212%2").arg(add).arg(del);
    }
    if (text.startsWith(QLatin1String("Renamed"))) return QStringLiteral("renamed");
    if (text.startsWith(QLatin1String("Set ")) || text.startsWith(QLatin1String("Named"))) return QStringLiteral("set");
    const int lines = int(text.count(QLatin1Char('\n'))) + (text.isEmpty() ? 0 : 1);
    if (lines > 1) return QStringLiteral("%1 lines").arg(lines);
    return text.left(60);
}

AgentRow rowFromInfo(AgentRow r, const SubagentInfo& i) {
    r.id = i.id;
    if (!i.name.isEmpty()) r.name = i.name;
    r.status = i.status == SubagentStatus::Running || i.status == SubagentStatus::Pending ? QStringLiteral("running")
               : i.status == SubagentStatus::Failed                                         ? QStringLiteral("failed")
               : i.status == SubagentStatus::Cancelled                                      ? QStringLiteral("cancelled")
                                                                                            : QStringLiteral("done");
    r.currentTool = i.currentTool;
    r.currentArgs = i.currentArgs;
    r.tools = i.toolCount;
    r.tokens = i.tokens;
    r.startedMs = i.createdAt;
    r.endedMs = i.completedAt;
    if (!i.recentOutput.isEmpty()) r.output = i.recentOutput.last().section(QLatin1Char('\n'), 0, 0);
    if (r.status != QLatin1String("running") && !i.summary.isEmpty()) r.output = i.summary.section(QLatin1Char('\n'), 0, 0).left(200);
    return r;
}

QJsonObject approvalPayload(const ApprovalRequest& r) {
    return QJsonObject{{"kind", r.kind == ApprovalRequest::Mutation ? "mutation" : "script"},
                       {"toolName", r.toolName},
                       {"arguments", r.arguments},
                       {"reason", r.reason}};
}

}  // namespace

TranscriptModel::TranscriptModel(QObject* parent) : QAbstractListModel(parent) {
    flush_.setSingleShot(true);
    flush_.setInterval(kFlushMs);
    connect(&flush_, &QTimer::timeout, this, &TranscriptModel::flushPending);
    reveal_.setInterval(kRevealMs);
    reveal_.setTimerType(Qt::PreciseTimer);
    connect(&reveal_, &QTimer::timeout, this, &TranscriptModel::revealTick);
    QPointer<TranscriptModel> self(this);
    // registry changes come from any thread, many per second: one queued refresh at a time
    registryObserver_ = SubagentRegistry::instance().addObserver([self] {
        QMetaObject::invokeMethod(
            uiAnchor(), [self] {
                if (!self || self->agentsDirty_) return;
                self->agentsDirty_ = true;
                QTimer::singleShot(kFlushMs * 4, self.data(), [self] {
                    if (!self) return;
                    self->agentsDirty_ = false;
                    self->refreshAgents();
                });
            },
            Qt::QueuedConnection);
    });
}

TranscriptModel::~TranscriptModel() { SubagentRegistry::instance().removeObserver(registryObserver_); }

void TranscriptModel::bind(ChatSession* chat) {
    if (chat_) disconnect(chat_, nullptr, this, nullptr);
    chat_ = chat;
    clear();
    if (!chat) return;
    if (std::shared_ptr<DbContext> db = chat->db()) {
        QSet<QString> names;
        for (const ToolSpec* s : db->registry.visible(chat->context()->profile, db->host->hasDecompiler())) names.insert(s->name);
        setHostTools(names);
    }
    connect(chat, &ChatSession::frame, this, &TranscriptModel::applyFrame);
    connect(chat, &ChatSession::transcriptLoaded, this, &TranscriptModel::applyTranscript);
    connect(chat, &ChatSession::stateChanged, this, &TranscriptModel::applyState);
    connect(chat, &ChatSession::busyChanged, this, &TranscriptModel::setBusy);
    connect(chat, &ChatSession::promptFinished, this,
            [this](const QString& status, const QString& error, const QString&) { promptFinished(status, error); });
    connect(chat, &ChatSession::approvalRequested, this,
            [this](int id, const ApprovalRequest& r) { addCard(ItemKind::Approval, id, approvalPayload(r)); });
    connect(chat, &ChatSession::questionAsked, this, [this](int id, const QString& q, const QStringList& options) {
        addCard(ItemKind::Question, id, QJsonObject{{"question", q}, {"options", QJsonArray::fromStringList(options)}});
    });
    connect(chat, &ChatSession::decisionRequested, this, [this](int id, const QString& kind, const QJsonObject& payload) {
        addCard(ItemKind::Decision, id, QJsonObject{{"kind", kind}, {"payload", payload}});
    });
    connect(chat, &ChatSession::cardCancelled, this, &TranscriptModel::cancelCard);
    setBusy(chat->busy());
}

void TranscriptModel::setHostTools(const QSet<QString>& names) { hostTools_ = names; }

void TranscriptModel::clear() {
    flush_.stop();
    reveal_.stop();
    backlog_.clear();
    pending_.clear();
    beginResetModel();
    items_.clear();
    rowCache_.clear();
    endResetModel();
    step_ = text_ = thinking_ = task_ = 0;
    textLead_.clear();
    tools_.clear();
    blockTool_.clear();
    agentTask_.clear();
    knownSymbols_.clear();
    symbolsRe_.setPattern({});
    lastError_.clear();
    activity_ = ChatActivity{};
    Q_EMIT activityChanged();
    Q_EMIT awaitingChanged();
}

// ---- rows ------------------------------------------------------------------------------------------------

int TranscriptModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(items_.size()); }

QVariant TranscriptModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= items_.size() || role != Qt::DisplayRole) return {};
    const TranscriptItem& t = items_.at(index.row());
    switch (t.kind) {
    case ItemKind::Tool:
        return t.toolName + QLatin1Char(' ') + t.label + (t.summary.isEmpty() ? QString() : QStringLiteral(" \u00b7 ") + t.summary);
    case ItemKind::Step: return t.title;
    case ItemKind::Task: {
        QStringList names;
        for (const AgentRow& a : t.agents) names << a.name;
        return QStringLiteral("agents: ") + names.join(QStringLiteral(", "));
    }
    case ItemKind::Permission:
        return QStringLiteral("permission %1 %2").arg(str(t.payload, "tool"), str(t.payload, "details"));
    case ItemKind::Question: return str(t.payload, "question");
    case ItemKind::Dialog: return str(t.payload, "title");
    case ItemKind::Btw: return t.title + QLatin1Char('\n') + t.text;
    default: return t.text;
    }
}

int TranscriptModel::reversibleChanges() const {
    if (!chat_) return 0;
    int n = 0;
    for (const MutationRecord& r : chat_->context()->mutations->records()) n += r.reversible ? 1 : 0;
    return n;
}

int TranscriptModel::rowOfId(quint64 id) const {
    auto it = rowCache_.constFind(id);
    return it == rowCache_.constEnd() ? -1 : it.value();
}

int TranscriptModel::rowOfKey(ItemKind kind, const QString& key) const {
    for (int i = int(items_.size()) - 1; i >= 0; --i)
        if (items_[i].kind == kind && items_[i].key == key) return i;
    return -1;
}

TranscriptItem* TranscriptModel::itemById(quint64 id) {
    const int r = rowOfId(id);
    return r < 0 ? nullptr : &items_[r];
}

int TranscriptModel::append(TranscriptItem item) {
    item.id = nextId_++;
    if (!item.timeMs) item.timeMs = nowMs();
    const int row = int(items_.size());
    // a call of the same tool right after another joins its group (one row "name ×N" until opened)
    int head = -1;
    if (item.kind == ItemKind::Tool && row > 0) {
        const TranscriptItem& prev = items_.at(row - 1);
        if (prev.kind == ItemKind::Tool && prev.depth == item.depth && prev.toolName == item.toolName) {
            head = prev.groupHead ? rowOfId(prev.groupHead) : row - 1;
            if (head >= 0) {
                item.groupHead = items_.at(head).id;
                items_[head].groupCount = std::max(2, items_.at(head).groupCount + 1);
            }
        }
    }
    beginInsertRows({}, row, row);
    rowCache_.insert(item.id, row);
    items_.append(std::move(item));
    endInsertRows();
    if (head >= 0) touch(head);
    return row;
}

void TranscriptModel::touch(int row, bool streaming) {
    if (row < 0 || row >= items_.size()) return;
    ++items_[row].revision;
    // a group's row sums up its calls: it changes with them
    if (const quint64 head = items_[row].groupHead) touch(rowOfId(head));
    if (streaming) {
        // coalesced: one repaint of the tail per frame
        pending_.insert(row);
        if (!flush_.isActive()) flush_.start();
        Q_EMIT streamingTail();
        return;
    }
    pending_.remove(row);
    const QModelIndex i = index(row);
    Q_EMIT dataChanged(i, i);
}

void TranscriptModel::flushPending() {
    const QSet<int> rows = std::exchange(pending_, {});
    for (int r : rows) {
        const QModelIndex i = index(r);
        Q_EMIT dataChanged(i, i);
    }
}

void TranscriptModel::toggleExpanded(int row) {
    if (row < 0 || row >= items_.size()) return;
    items_[row].expanded = !items_[row].expanded;
    items_[row].userToggled = true;
    touch(row);
}

void TranscriptModel::toggleGroup(int row) {
    if (row < 0 || row >= items_.size() || items_[row].groupCount < 2) return;
    items_[row].groupOpen = !items_[row].groupOpen;
    touch(row);
}

QList<int> TranscriptModel::groupRows(int headRow) const {
    QList<int> out;
    if (headRow < 0 || headRow >= items_.size() || items_[headRow].groupCount < 2) return out;
    const quint64 id = items_[headRow].id;
    out << headRow;
    for (int r = headRow + 1; r < items_.size() && items_[r].groupHead == id; ++r) out << r;
    return out;
}

void TranscriptModel::activity(const std::function<void(ChatActivity&)>& fn) {
    fn(activity_);
    Q_EMIT activityChanged();
}

// ---- turn structure ----------------------------------------------------------------------------------------

void TranscriptModel::ensureStep(qint64 now) {
    if (step_) return;
    TranscriptItem s;
    s.kind = ItemKind::Step;
    s.title = QStringLiteral("rikugan");
    s.timeMs = now;
    s.payload = QJsonObject{{"offsetMs", activity_.turnStartMs ? double(now - activity_.turnStartMs) : 0.0}};
    const int row = append(std::move(s));
    step_ = items_[row].id;
}

quint64 TranscriptModel::textItem(ItemKind kind, qint64 now) {
    quint64& cur = kind == ItemKind::Thinking ? thinking_ : text_;
    if (cur) return cur;
    ensureStep(now);
    TranscriptItem t;
    t.kind = kind;
    t.depth = 1;
    t.streaming = true;
    t.expanded = kind == ItemKind::Thinking;  // open while it streams
    t.timeMs = now;
    const int row = append(std::move(t));
    cur = items_[row].id;
    return cur;
}

void TranscriptModel::stream(quint64 id, const QString& delta) {
    if (delta.isEmpty()) return;
    Backlog& b = backlog_[id];
    b.rest += delta;
    // the waiting text goes out evenly over the next frames; a bigger burst speeds it up
    b.perFrame = std::max<qsizetype>(b.perFrame, (b.rest.size() + kRevealFrames - 1) / kRevealFrames);
    if (!reveal_.isActive()) reveal_.start();
}

void TranscriptModel::revealTick() {
    for (auto it = backlog_.begin(); it != backlog_.end();) {
        Backlog& b = it.value();
        qsizetype n = std::min(b.perFrame, b.rest.size());
        if (n < b.rest.size() && b.rest.at(n - 1).isHighSurrogate()) ++n;  // never half a character
        if (TranscriptItem* item = itemById(it.key())) {
            item->text += QStringView(b.rest).left(n);
            touch(rowOfId(it.key()));
        }
        b.rest.remove(0, n);
        it = b.rest.isEmpty() ? backlog_.erase(it) : std::next(it);
    }
    if (backlog_.isEmpty()) reveal_.stop();
    Q_EMIT streamingTail();  // the view keeps the growing text in sight
}

void TranscriptModel::revealAll(quint64 id) {
    for (auto it = backlog_.begin(); it != backlog_.end();) {
        if (id && it.key() != id) {
            ++it;
            continue;
        }
        if (TranscriptItem* item = itemById(it.key())) item->text += it.value().rest;
        it = backlog_.erase(it);
    }
    if (backlog_.isEmpty()) reveal_.stop();
}

void TranscriptModel::foldThought(TranscriptItem& t) {
    if (!t.endMs) t.endMs = nowMs();
    if (!t.userToggled) t.expanded = false;
}

void TranscriptModel::finishStreaming() {
    revealAll();
    for (quint64* cur : {&text_, &thinking_}) {
        if (TranscriptItem* t = itemById(*cur)) {
            t->streaming = false;
            if (t->kind == ItemKind::Thinking) foldThought(*t);
            computeChips(*t);
            touch(rowOfId(*cur));
        }
        *cur = 0;
    }
    textLead_.clear();
}

void TranscriptModel::addNotice(const QString& text, bool error) {
    if (text.trimmed().isEmpty()) return;
    TranscriptItem n;
    n.kind = error ? ItemKind::Error : ItemKind::Notice;
    n.text = text;
    append(std::move(n));
}

ToolKind TranscriptModel::kindOf(const QString& tool) const {
    if (tool == QLatin1String("spawn_subagent")) return ToolKind::Agent;
    if (tool == QLatin1String("web_search")) return ToolKind::Web;
    static const QSet<QString> workspace{"read", "write", "edit", "ast_edit", "grep", "glob", "bash", "find"};
    if (workspace.contains(tool)) return ToolKind::Workspace;
    if (hostTools_.contains(tool)) return ToolKind::Host;
    return tool.startsWith(QLatin1String("mcp__")) ? ToolKind::Workspace : ToolKind::Host;
}

// Addresses and symbols in a text or tool label become chips that navigate. A symbol is an auto name
// (sub_401000) or a name this chat gave or met with its address. In Markdown, chips inside code blocks are left
// alone and an inline code span that is exactly one chip becomes the chip (its range then covers the backticks).
void TranscriptModel::computeChips(TranscriptItem& t) {
    const bool markdown = t.kind != ItemKind::Tool;
    const QString& s = markdown ? t.text : t.label;
    t.chips.clear();
    if (s.isEmpty()) return;
    static const QRegularExpression addr(QStringLiteral("\\b0x[0-9a-fA-F]{4,16}\\b"));
    static const QRegularExpression autoName(
        QStringLiteral("\\b(?:sub|loc|unk|off|byte|word|dword|qword|j)_([0-9A-Fa-f]{4,16})\\b"));
    QList<Chip> found;
    auto add = [&](qsizetype from, qsizetype to, const char* kind, const QString& value) {
        for (const Chip& c : std::as_const(found))
            if (from < c.to && c.from < to) return;
        found << Chip{int(from), int(to), QString::fromLatin1(kind), value};
    };
    for (auto it = autoName.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd(), "sym", QStringLiteral("0x") + m.captured(1).toLower());
    }
    if (!knownSymbols_.isEmpty()) {
        if (symbolsRe_.pattern().isEmpty()) {
            QStringList names;
            for (auto it = knownSymbols_.cbegin(); it != knownSymbols_.cend(); ++it)
                if (it.key().size() >= 4) names << QRegularExpression::escape(it.key());
            // longest first: a name that prefixes another must not win
            std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
            symbolsRe_.setPattern(names.isEmpty() ? QStringLiteral("(?!)") : QStringLiteral("\\b(?:%1)\\b").arg(names.join(QLatin1Char('|'))));
        }
        for (auto it = symbolsRe_.globalMatch(s); it.hasNext();) {
            const auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), "sym", knownSymbols_.value(m.captured()));
        }
    }
    for (auto it = addr.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        add(m.capturedStart(), m.capturedEnd(), "addr", m.captured().toLower());
    }
    if (markdown) {
        static const QRegularExpression fence(QStringLiteral("```[\\s\\S]*?(?:```|$)"));
        static const QRegularExpression inlineCode(QStringLiteral("`([^`\\n]+)`"));
        QList<std::pair<qsizetype, qsizetype>> fences;
        for (auto it = fence.globalMatch(s); it.hasNext();) {
            const auto m = it.next();
            fences.append({m.capturedStart(), m.capturedEnd()});
        }
        QList<Chip> kept;
        for (Chip c : std::as_const(found)) {
            if (std::any_of(fences.cbegin(), fences.cend(), [&](const auto& f) { return c.from >= f.first && c.to <= f.second; }))
                continue;
            bool drop = false;
            for (auto it = inlineCode.globalMatch(s); it.hasNext();) {
                const auto m = it.next();
                if (c.from < m.capturedStart() || c.to > m.capturedEnd()) continue;
                if (m.capturedStart(1) == c.from && m.capturedEnd(1) == c.to) {
                    c.from -= 1;
                    c.to += 1;
                } else {
                    drop = true;
                }
                break;
            }
            if (!drop) kept << c;
        }
        found = kept;
    }
    std::sort(found.begin(), found.end(), [](const Chip& a, const Chip& b) { return a.from < b.from; });
    t.chips = found;
}

void TranscriptModel::learnSymbols(const QJsonObject& args) {
    const QJsonValue a = args.value(QLatin1String("address")).isUndefined() ? args.value(QLatin1String("ea"))
                                                                             : args.value(QLatin1String("address"));
    QString address;
    if (a.isString()) address = a.toString().trimmed().toLower();
    else if (a.isDouble()) address = QStringLiteral("0x%1").arg(qulonglong(a.toDouble()), 0, 16);
    if (!address.startsWith(QLatin1String("0x"))) return;
    for (const char* k : {"new_name", "name", "function_name"}) {
        const QString name = str(args, k).trimmed();
        if (name.size() < 4 || name.contains(QLatin1Char(' ')) || knownSymbols_.value(name) == address) continue;
        knownSymbols_.insert(name, address);
        symbolsRe_.setPattern({});
    }
}

// ---- frames ---------------------------------------------------------------------------------------------

void TranscriptModel::applyFrame(const QJsonObject& f) {
    const QString type = str(f, "type");
    const qint64 now = nowMs();
    if (type == QLatin1String("message_update")) {
        onMessageUpdate(f);
    } else if (type == QLatin1String("message_start")) {
        onMessageStart(f.value(QLatin1String("message")).toObject());
    } else if (type == QLatin1String("message_end")) {
        onMessageEnd(f.value(QLatin1String("message")).toObject());
    } else if (type == QLatin1String("tool_execution_start")) {
        onToolStart(str(f, "toolCallId"), str(f, "toolName"), f.value(QLatin1String("args")).toObject(), now);
    } else if (type == QLatin1String("tool_execution_update")) {
        if (TranscriptItem* t = itemById(tools_.value(str(f, "toolCallId")))) {
            t->result = contentText(f.value(QLatin1String("partialResult")).toObject().value(QLatin1String("content")));
            touch(rowOfId(t->id), true);
        }
    } else if (type == QLatin1String("tool_execution_end")) {
        onToolEnd(str(f, "toolCallId"), f.value(QLatin1String("result")).toObject(), f.value(QLatin1String("isError")).toBool(), now);
    } else if (type == QLatin1String("turn_start")) {
        finishStreaming();
        // providers reuse tool call ids across turns (call_1 ...): a new turn's call gets a new row
        tools_.clear();
        blockTool_.clear();
    } else if (type == QLatin1String("turn_end")) {
        finishStreaming();
    } else if (type == QLatin1String("auto_compaction_start") || type == QLatin1String("manual_compaction_start")) {
        addNotice(QStringLiteral("Compacting context\u2026"));
    } else if (type == QLatin1String("auto_compaction_end") || type == QLatin1String("manual_compaction_end")) {
        const QString err = str(f, "errorMessage").isEmpty() ? str(f, "error") : str(f, "errorMessage");
        if (f.value(QLatin1String("aborted")).toBool()) addNotice(QStringLiteral("Compaction cancelled."));
        else if (f.value(QLatin1String("skipped")).toBool()) addNotice(QStringLiteral("Nothing to compact."));
        else if (!err.isEmpty()) addNotice(QStringLiteral("Compaction failed: %1").arg(err), true);
        else addNotice(QStringLiteral("Context compacted."));
    } else if (type == QLatin1String("auto_retry_start")) {
        finishStreaming();
        addNotice(QStringLiteral("Retrying (%1/%2): %3")
                      .arg(f.value(QLatin1String("attempt")).toInt())
                      .arg(f.value(QLatin1String("maxAttempts")).toInt())
                      .arg(str(f, "errorMessage")));
    } else if (type == QLatin1String("auto_retry_end")) {
        if (f.contains(QLatin1String("success")) && !f.value(QLatin1String("success")).toBool()) {
            lastError_ = str(f, "finalError");
            addNotice(QStringLiteral("Retry failed: %1").arg(lastError_), true);
        }
    } else if (type == QLatin1String("notice")) {
        addNotice(str(f, "message"), str(f, "level") == QLatin1String("error"));
    } else if (type == QLatin1String("command_output")) {
        TranscriptItem c;
        c.kind = ItemKind::CommandOutput;
        c.text = str(f, "text");
        append(std::move(c));
    } else if (type == QLatin1String("rikugan_command_output")) {
        TranscriptItem c;
        c.kind = f.value(QLatin1String("isError")).toBool() ? ItemKind::Error : ItemKind::CommandOutput;
        c.text = str(f, "text");
        c.markdown = f.value(QLatin1String("markdown")).toBool();
        append(std::move(c));
    } else if (type == QLatin1String("rikugan_ui_action")) {
        Q_EMIT uiAction(str(f, "action"), str(f, "arg"));
    } else if (type == QLatin1String("extension_ui_request")) {
        const QString method = str(f, "method");
        if (method == QLatin1String("select") || method == QLatin1String("input") || method == QLatin1String("editor") ||
            method == QLatin1String("confirm")) {
            finishStreaming();
            TranscriptItem d;
            d.kind = ItemKind::Dialog;
            d.key = str(f, "id");
            d.payload = f;
            append(std::move(d));
            Q_EMIT awaitingChanged();
        } else if (method == QLatin1String("cancel")) {
            const int row = rowOfKey(ItemKind::Dialog, str(f, "targetId"));
            if (row >= 0 && !items_[row].resolved) resolveCard(row, QStringLiteral("cancelled"));
        } else if (method == QLatin1String("notify")) {
            addNotice(str(f, "message"), str(f, "notifyType") == QLatin1String("error"));
        } else if (method == QLatin1String("open_url")) {
            QString text = str(f, "url");
            if (!str(f, "instructions").isEmpty()) text = str(f, "instructions") + QLatin1Char('\n') + text;
            addNotice(text);
        }
    } else if (type == QLatin1String("rikugan_permission")) {
        finishStreaming();
        TranscriptItem p;
        p.kind = ItemKind::Permission;
        p.key = str(f, "key");
        p.payload = f;
        append(std::move(p));
        Q_EMIT awaitingChanged();
    } else if (type == QLatin1String("rikugan_permission_resolved")) {
        const int row = rowOfKey(ItemKind::Permission, str(f, "key"));
        if (row >= 0) {
            const QString outcome = str(f, "outcome");
            TranscriptItem& p = items_[row];
            p.resolved = true;
            p.resolution = outcome;
            touch(row);
            // the tool row this permission gated (created later by tool_execution_start) shows a denial itself
            Q_EMIT awaitingChanged();
        }
    } else if (type == QLatin1String("rikugan_mutations_changed") || type == QLatin1String("rikugan_mutation_undone")) {
        if (type == QLatin1String("rikugan_mutation_undone")) {
            // undo pops newest first
            for (int i = int(items_.size()) - 1; i >= 0; --i) {
                TranscriptItem& t = items_[i];
                if (t.kind == ItemKind::Tool && !t.mutation.isEmpty() && t.mutationIndex >= 0 && !t.undone) {
                    t.undone = true;
                    touch(i);
                    break;
                }
            }
        }
        if (chat_) activity([&](ChatActivity& a) { a.undoable = reversibleChanges(); });
    } else if (type == QLatin1String("rikugan_subagent_event")) {
        onSubagentEvent(f);
    } else if (type == QLatin1String("rikugan_finding") || type == QLatin1String("rikugan_patch_applied")) {
        TranscriptItem n;
        n.kind = ItemKind::Finding;
        if (type == QLatin1String("rikugan_finding")) {
            n.title = str(f, "category").isEmpty() ? QStringLiteral("general") : str(f, "category");
            n.text = str(f, "summary");
        } else {
            n.title = QStringLiteral("patch_result");
            n.text = str(f, "description");
            if (!str(f, "original_hex").isEmpty() || !str(f, "new_hex").isEmpty())
                n.text += QStringLiteral(" (%1 \u2192 %2)").arg(str(f, "original_hex"), str(f, "new_hex"));
        }
        n.payload = QJsonObject{{"address", str(f, "address")},
                                {"relevance", str(f, "relevance").isEmpty() ? QStringLiteral("medium") : str(f, "relevance")}};
        computeChips(n);
        append(std::move(n));
    } else if (type == QLatin1String("rikugan_research_note")) {
        TranscriptItem n;
        n.kind = ItemKind::Note;
        n.title = str(f, "title");
        n.payload = QJsonObject{{"genre", str(f, "genre").isEmpty() ? QStringLiteral("general") : str(f, "genre")}, {"path", str(f, "path")}};
        append(std::move(n));
    } else if (type == QLatin1String("rikugan_open_failed")) {
        lastError_ = str(f, "error");  // raw: promptFinished reports the same error again
        addNotice(QStringLiteral("Could not start the chat: %1").arg(userFacingError(lastError_)), true);
        activity([](ChatActivity& a) { a.lastOutcome = QStringLiteral("error"); });
        // nothing to talk to yet: the panel walks the user through connecting a model
        if (needsModelSetup(lastError_)) Q_EMIT uiAction(QStringLiteral("setup"), QString());
    } else if (type == QLatin1String("rikugan_mode_phase")) {
        finishStreaming();
        step_ = 0;
        const QString phase = str(f, "phase");
        TranscriptItem p;
        p.kind = ItemKind::Phase;
        p.title = phase;
        p.text = str(f, "text");
        append(std::move(p));
        activity([&](ChatActivity& a) {
            a.phase = phase == QLatin1String("done") ? QString() : phase;
            if (phase == QLatin1String("done")) a.mode.clear();
            else if (a.mode.isEmpty()) a.mode = str(f, "mode").isEmpty() ? QStringLiteral("mode") : str(f, "mode");
        });
    } else if (type == QLatin1String("rikugan_plan_step")) {
        for (int i = int(items_.size()) - 1; i >= 0; --i) {
            TranscriptItem& d = items_[i];
            if (d.kind != ItemKind::Decision || str(d.payload, "kind") != QLatin1String("plan")) continue;
            QJsonObject st = d.payload.value(QLatin1String("stepStatus")).toObject();
            st.insert(QString::number(f.value(QLatin1String("index")).toInt()), str(f, "status"));
            d.payload.insert(QStringLiteral("stepStatus"), st);
            touch(i);
            break;
        }
    } else if (type == QLatin1String("rikugan_plan_revised")) {
        // the plan the user edited on its card replaces the one the model wrote
        for (int i = int(items_.size()) - 1; i >= 0; --i) {
            TranscriptItem& d = items_[i];
            if (d.kind != ItemKind::Decision || str(d.payload, "kind") != QLatin1String("plan")) continue;
            QJsonObject p = d.payload.value(QLatin1String("payload")).toObject();
            p.insert(QStringLiteral("steps"), f.value(QLatin1String("steps")).toArray());
            d.payload.insert(QStringLiteral("payload"), p);
            d.payload.remove(QStringLiteral("stepStatus"));
            touch(i);
            break;
        }
    } else if (type == QLatin1String("goal_updated")) {
        const QJsonObject g = f.value(QLatin1String("goal")).toObject();
        activity([&](ChatActivity& a) { a.goal = liveGoal(g); });
        if (str(g, "status") == QLatin1String("complete")) addNotice(QStringLiteral("Goal complete."));
    } else if (type == QLatin1String("btw_record") || type == QLatin1String("btw_delta")) {
        const QJsonObject rec = f.value(QLatin1String("record")).toObject();
        const QString id = type == QLatin1String("btw_delta") ? str(f, "recordId") : str(rec, "id");
        int row = rowOfKey(ItemKind::Btw, id);
        if (row < 0) {
            TranscriptItem b;
            b.kind = ItemKind::Btw;
            b.key = id;
            b.streaming = true;
            row = append(std::move(b));
        }
        TranscriptItem& b = items_[row];
        if (type == QLatin1String("btw_delta")) {
            b.text += str(f, "delta");
            touch(row, true);
        } else {
            // the newest turn: the record itself or its last follow-up
            const QJsonArray follow = rec.value(QLatin1String("followUps")).toArray();
            const QJsonObject turn = follow.isEmpty() ? rec : follow.last().toObject();
            b.title = str(turn, "question");
            if (!str(turn, "answer").isEmpty()) b.text = str(turn, "answer");
            b.streaming = str(turn, "status") == QLatin1String("running");
            if (!str(turn, "error").isEmpty()) b.text += QStringLiteral("\n\n") + str(turn, "error");
            touch(row);
        }
    } else if (type == QLatin1String("todo_reminder")) {
        // the model stopped with open todos: omc nudges it; nothing to show beyond the todo list
    }
}

void TranscriptModel::onMessageStart(const QJsonObject& m) {
    const QString role = str(m, "role");
    if (role == QLatin1String("user")) {
        const std::optional<QString> text = typedUserText(m);
        if (!text) return;
        finishStreaming();
        step_ = 0;
        // a command typed here is on screen as typed (addLocalUser): omc's message for it takes that line, which
        // then reads as it will on replay (a /skill's prompt, a /goal's objective)
        const quint64 echo = std::exchange(localEcho_, 0);
        if (echo && !items_.isEmpty() && items_.last().id == echo &&
            (str(m, "customType") == QLatin1String("skill-prompt") ||
             items_.last().text.simplified() == (QStringLiteral("/goal ") + *text).simplified())) {
            items_.last().text = *text;
            touch(int(items_.size()) - 1);
            return;
        }
        TranscriptItem u;
        u.kind = ItemKind::User;
        u.text = *text;
        append(std::move(u));
    } else if (role == QLatin1String("assistant")) {
        finishStreaming();
        // each assistant message is a step on the timeline
        step_ = 0;
        task_ = 0;
        blockTool_.clear();
    }
}

void TranscriptModel::addLocalUser(const QString& text) {
    finishStreaming();
    step_ = 0;
    TranscriptItem u;
    u.kind = ItemKind::User;
    u.text = text;
    localEcho_ = items_.at(append(std::move(u))).id;
}

void TranscriptModel::onMessageUpdate(const QJsonObject& f) {
    const QJsonObject ev = f.value(QLatin1String("assistantMessageEvent")).toObject();
    const QString t = str(ev, "type");
    const int index = ev.value(QLatin1String("contentIndex")).toInt(-1);
    const qint64 now = nowMs();
    if (t == QLatin1String("text_delta")) {
        // no row (nor step) until there is something to show: a whitespace-only answer leaves nothing behind
        const QString d = str(ev, "delta");
        if (!text_ && d.trimmed().isEmpty()) textLead_ += d;
        else stream(textItem(ItemKind::Text, now), std::exchange(textLead_, {}) + d);
    } else if (t == QLatin1String("text_end")) {
        textLead_.clear();
        // the full text: it also stands for a block whose deltas never came
        const QString content = str(ev, "content");
        const bool full = !content.trimmed().isEmpty();
        if (full && !text_) textItem(ItemKind::Text, now);
        revealAll(text_);
        if (TranscriptItem* it = itemById(text_)) {
            if (full) it->text = content;
            it->streaming = false;
            computeChips(*it);
            touch(rowOfId(text_));
        }
        text_ = 0;
    } else if (t == QLatin1String("thinking_delta")) {
        stream(textItem(ItemKind::Thinking, now), str(ev, "delta"));
    } else if (t == QLatin1String("thinking_end")) {
        revealAll(thinking_);
        if (TranscriptItem* it = itemById(thinking_)) {
            it->streaming = false;
            foldThought(*it);
            touch(rowOfId(thinking_));
        }
        thinking_ = 0;
    } else if (t == QLatin1String("toolcall_start")) {
        const QJsonObject block = f.value(QLatin1String("block")).toObject().isEmpty()
                                      ? ev.value(QLatin1String("block")).toObject()
                                      : f.value(QLatin1String("block")).toObject();
        const QString id = str(block, "id");
        if (id.isEmpty()) return;
        blockTool_.insert(index, id);
        // the text before the call is complete: later text opens a new row below it
        revealAll(text_);
        if (TranscriptItem* it = itemById(text_)) {
            it->streaming = false;
            computeChips(*it);
            touch(rowOfId(text_));
        }
        text_ = 0;
        textLead_.clear();
        onToolStart(id, str(block, "name"), {}, now);
    } else if (t == QLatin1String("toolcall_end")) {
        const QJsonObject call = ev.value(QLatin1String("toolCall")).toObject();
        const QString id = str(call, "id").isEmpty() ? blockTool_.value(index) : str(call, "id");
        if (TranscriptItem* it = itemById(tools_.value(id))) {
            it->args = call.value(QLatin1String("arguments")).toObject();
            it->label = toolLabel(it->toolName, it->args);
            computeChips(*it);
            touch(rowOfId(it->id));
        }
    }
}

void TranscriptModel::onMessageEnd(const QJsonObject& m) {
    if (str(m, "role") != QLatin1String("assistant")) return;
    finishStreaming();
    const QJsonObject u = m.value(QLatin1String("usage")).toObject();
    if (!u.isEmpty())
        activity([&](ChatActivity& a) {
            a.tokensIn += qint64(u.value(QLatin1String("input")).toDouble() + u.value(QLatin1String("cacheRead")).toDouble() +
                                 u.value(QLatin1String("cacheWrite")).toDouble());
            a.tokensOut += qint64(u.value(QLatin1String("output")).toDouble());
            a.cost += u.value(QLatin1String("cost")).toObject().value(QLatin1String("total")).toDouble();
        });
    if (str(m, "stopReason") == QLatin1String("error")) {
        lastError_ = str(m, "errorMessage");
        addNotice(lastError_.isEmpty() ? QStringLiteral("Unknown error") : lastError_, true);
        activity([](ChatActivity& a) { a.lastOutcome = QStringLiteral("error"); });
    }
}

void TranscriptModel::onToolStart(const QString& id, const QString& name, const QJsonObject& args, qint64 now) {
    if (hiddenTool(name)) return;
    TranscriptItem* existing = itemById(tools_.value(id));
    if (!existing) {
        ensureStep(now);
        if (name == QLatin1String("spawn_subagent")) {
            // the subagents of one assistant message share one task card
            if (!itemById(task_)) {
                TranscriptItem task;
                task.kind = ItemKind::Task;
                task.depth = 1;
                task.timeMs = now;
                task_ = items_[append(std::move(task))].id;
            }
            agentTask_.insert(id, task_);
            tools_.insert(id, task_);
            TranscriptItem* task = itemById(task_);
            AgentRow r;
            r.task = str(args, "task");
            r.name = r.task.left(40);
            r.startedMs = now;
            r.status = QStringLiteral("running");
            task->agents << r;
            task->payload.insert(QStringLiteral("calls"), task->payload.value(QLatin1String("calls")).toArray() + QJsonValue(id));
            touch(rowOfId(task_));
            activity([&](ChatActivity& a) { a.agents << r; });
            return;
        }
        TranscriptItem t;
        t.kind = ItemKind::Tool;
        t.depth = 1;
        t.key = id;
        t.toolName = name;
        t.toolKind = kindOf(name);
        t.status = ToolStatus::Running;
        t.startMs = now;
        t.timeMs = now;
        tools_.insert(id, items_[append(std::move(t))].id);
        existing = itemById(tools_.value(id));
    }
    TranscriptItem& t = *existing;
    if (t.kind == ItemKind::Task) {
        // args arrived with tool_execution_start: fill the agent row created from the streamed call
        if (!args.isEmpty()) {
            const int i = int(t.payload.value(QLatin1String("calls")).toArray().toVariantList().indexOf(id));
            if (i >= 0 && i < t.agents.size() && t.agents[i].task.isEmpty()) {
                t.agents[i].task = str(args, "task");
                t.agents[i].name = t.agents[i].task.left(40);
            }
            touch(rowOfId(t.id));
        }
        return;
    }
    if (!args.isEmpty()) {
        t.args = args;
        t.label = toolLabel(name, args);
        if (name == QLatin1String("write")) t.code = str(args, "content");
        learnSymbols(args);
        computeChips(t);
    }
    t.status = ToolStatus::Running;
    if (!t.startMs) t.startMs = now;
    touch(rowOfId(t.id));
    const QString current = t.toolName + (t.label.isEmpty() ? QString() : QLatin1Char(' ') + t.label);
    activity([&](ChatActivity& a) { a.currentTool = current; });
}

void TranscriptModel::fillToolResult(TranscriptItem& t, const QJsonObject& result, bool isError) {
    const QJsonObject details = result.value(QLatin1String("details")).toObject();
    t.result = contentText(result.value(QLatin1String("content")));
    t.status = isError ? (t.result.contains(QLatin1String("denied by user")) ? ToolStatus::Denied : ToolStatus::Error)
                       : ToolStatus::Done;
    t.summary = summarize(t.toolName, t.result, isError, details);
    if (t.toolName == QLatin1String("edit")) t.diff = str(details, "diff");
    if (t.toolName == QLatin1String("web_search"))
        t.webSources = details.value(QLatin1String("response")).toObject().value(QLatin1String("sources")).toArray();
    const QJsonObject mutation = details.value(QLatin1String("rikugan")).toObject().value(QLatin1String("mutation")).toObject();
    if (!mutation.isEmpty()) {
        t.mutation = str(mutation, "description");
        t.mutationReversible = mutation.value(QLatin1String("reversible")).toBool();
    }
}

void TranscriptModel::onToolEnd(const QString& id, const QJsonObject& result, bool isError, qint64 now) {
    const QJsonObject details = result.value(QLatin1String("details")).toObject();
    if (agentTask_.contains(id)) {
        // a subagent finished: its row takes the outcome
        TranscriptItem* task = itemById(agentTask_.value(id));
        if (!task) return;
        const int i = int(task->payload.value(QLatin1String("calls")).toArray().toVariantList().indexOf(id));
        if (i < 0 || i >= task->agents.size()) return;
        AgentRow& r = task->agents[i];
        const QString sid = str(details, "subagentId");
        if (!sid.isEmpty()) r.id = sid;
        if (const auto info = SubagentRegistry::instance().get(r.id)) r = rowFromInfo(r, *info);
        if (r.status == QLatin1String("running")) r.status = isError ? QStringLiteral("failed") : QStringLiteral("done");
        if (!r.endedMs) r.endedMs = now;
        touch(rowOfId(task->id));
        const AgentRow done = r;
        activity([&](ChatActivity& a) {
            for (AgentRow& x : a.agents)
                if ((!done.id.isEmpty() && x.id == done.id) || (x.id.isEmpty() && x.task == done.task)) x = done;
        });
        return;
    }
    if (id.isEmpty()) return;
    TranscriptItem* t = itemById(tools_.value(id));
    if (!t) {
        // todo carries the list for the activity strip
        const QJsonArray phases = details.value(QLatin1String("phases")).toArray();
        if (!phases.isEmpty() || details.contains(QLatin1String("phases")))
            activity([&](ChatActivity& a) { a.todoPhases = phases; });
        return;
    }
    fillToolResult(*t, result, isError);
    t->endMs = now;
    if (!t->mutation.isEmpty() && chat_) t->mutationIndex = int(chat_->context()->mutations->size()) - 1;
    touch(rowOfId(t->id));
    activity([&](ChatActivity& a) {
        a.currentTool.clear();
        if (chat_) a.undoable = reversibleChanges();
    });
}

void TranscriptModel::onSubagentEvent(const QJsonObject& f) {
    const QString sid = str(f, "subagentId");
    if (sid.isEmpty()) return;
    const std::optional<SubagentInfo> info = SubagentRegistry::instance().get(sid);
    if (!info) return;
    // attach a new subagent to the first running row with its task and no id yet
    bool known = false;
    for (const TranscriptItem& t : std::as_const(items_))
        if (t.kind == ItemKind::Task)
            for (const AgentRow& r : t.agents) known |= r.id == sid;
    if (!known) {
        for (int i = int(items_.size()) - 1; i >= 0 && !known; --i) {
            TranscriptItem& t = items_[i];
            if (t.kind != ItemKind::Task) continue;
            for (AgentRow& r : t.agents) {
                if (r.id.isEmpty() && (r.task == info->task || info->task.endsWith(r.task))) {
                    r.id = sid;
                    known = true;
                    break;
                }
            }
        }
        if (!known) {
            // a subagent started outside a spawn_subagent call (/modify explore, Agents panel)
            activity([&](ChatActivity& a) {
                if (std::none_of(a.agents.cbegin(), a.agents.cend(), [&](const AgentRow& r) { return r.id == sid; }))
                    a.agents << rowFromInfo(AgentRow{}, *info);
            });
        }
    }
    refreshAgents();
}

void TranscriptModel::refreshAgents() {
    for (int i = 0; i < items_.size(); ++i) {
        TranscriptItem& t = items_[i];
        if (t.kind != ItemKind::Task) continue;
        bool changed = false;
        for (AgentRow& r : t.agents) {
            if (r.id.isEmpty()) continue;
            if (const auto info = SubagentRegistry::instance().get(r.id)) {
                const AgentRow next = rowFromInfo(r, *info);
                if (!(next == r)) {
                    r = next;
                    changed = true;
                }
            }
        }
        if (changed) touch(i);
    }
    bool changed = false;
    ChatActivity next = activity_;
    for (AgentRow& r : next.agents) {
        if (r.id.isEmpty()) {
            // rows of this chat's task cards carry the id once known
            for (const TranscriptItem& t : std::as_const(items_))
                if (t.kind == ItemKind::Task)
                    for (const AgentRow& x : t.agents)
                        if (!x.id.isEmpty() && x.task == r.task) r.id = x.id;
        }
        if (r.id.isEmpty()) continue;
        if (const auto info = SubagentRegistry::instance().get(r.id)) {
            const AgentRow n = rowFromInfo(r, *info);
            if (!(n == r)) {
                r = n;
                changed = true;
            }
        }
    }
    if (changed) {
        activity_ = next;
        Q_EMIT activityChanged();
    }
}

// ---- cards and session state -----------------------------------------------------------------------

void TranscriptModel::addCard(ItemKind kind, int requestId, const QJsonObject& payload) {
    finishStreaming();
    TranscriptItem c;
    c.kind = kind;
    c.requestId = requestId;
    c.key = QString::number(requestId);
    c.payload = payload;
    append(std::move(c));
    Q_EMIT awaitingChanged();
}

void TranscriptModel::cancelCard(int requestId) {
    for (int i = int(items_.size()) - 1; i >= 0; --i) {
        TranscriptItem& c = items_[i];
        if (c.requestId == requestId && !c.resolved &&
            (c.kind == ItemKind::Approval || c.kind == ItemKind::Question || c.kind == ItemKind::Decision)) {
            resolveCard(i, QStringLiteral("cancelled"));
            return;
        }
    }
}

void TranscriptModel::resolveCard(int row, const QString& resolution) {
    if (row < 0 || row >= items_.size()) return;
    items_[row].resolved = true;
    items_[row].resolution = resolution;
    touch(row);
    Q_EMIT awaitingChanged();
}

bool TranscriptModel::awaitingUser() const {
    for (const TranscriptItem& t : items_)
        if (!t.resolved && (t.kind == ItemKind::Permission || t.kind == ItemKind::Approval || t.kind == ItemKind::Question ||
                            t.kind == ItemKind::Dialog || t.kind == ItemKind::Decision))
            return true;
    return false;
}

bool TranscriptModel::awaitingButtons() const {
    for (const TranscriptItem& t : items_) {
        if (t.resolved) continue;
        if (t.kind == ItemKind::Permission || t.kind == ItemKind::Approval || t.kind == ItemKind::Dialog ||
            t.kind == ItemKind::Decision)
            return true;
        if (t.kind == ItemKind::Question && !t.payload.value(QLatin1String("options")).toArray().isEmpty()) return true;
    }
    return false;
}

void TranscriptModel::applyState(const QJsonObject& st) {
    activity([&](ChatActivity& a) {
        const QJsonObject m = st.value(QLatin1String("model")).toObject();
        if (!m.isEmpty()) a.model = str(m, "provider") + QLatin1Char('/') + str(m, "id");
        if (st.contains(QLatin1String("thinkingLevel"))) a.thinking = str(st, "thinkingLevel");
        const QJsonObject cu = st.value(QLatin1String("contextUsage")).toObject();
        a.contextTokens = qint64(cu.value(QLatin1String("tokens")).toDouble());
        a.contextWindow = qint64(cu.value(QLatin1String("contextWindow")).toDouble());
        a.contextPercent = cu.value(QLatin1String("percent")).isNull() || !cu.contains(QLatin1String("percent"))
                               ? -1
                               : int(cu.value(QLatin1String("percent")).toDouble());
        // omc keeps a finished goal in its state: like goal_updated, only a live one is shown
        if (st.contains(QLatin1String("goal")))
            a.goal = liveGoal(st.value(QLatin1String("goal")).toObject().value(QLatin1String("goal")).toObject());
    });
}

void TranscriptModel::setBusy(bool busy) {
    if (busy == activity_.running) return;
    if (!busy) {
        finishStreaming();
        closeStaleCards();
    }
    activity([&](ChatActivity& a) {
        a.running = busy;
        if (busy) {
            a.turnStartMs = nowMs();
            a.lastOutcome.clear();
        } else {
            a.currentTool.clear();
        }
    });
}

// The run ended (finished, stopped, failed): whatever a card still asks has nobody waiting for it, so the composer
// must not stay locked behind it. Each one is answered "cancelled" (a worker still waiting gets that) and closed.
void TranscriptModel::closeStaleCards() {
    for (int i = 0; i < items_.size(); ++i) {
        const TranscriptItem& c = items_.at(i);
        if (c.resolved) continue;
        switch (c.kind) {
        case ItemKind::Permission:
            if (chat_) chat_->answerPermission(c.key, PermissionChoice::Deny);
            break;
        case ItemKind::Approval:
            if (chat_) chat_->answerApproval(c.requestId, ApprovalChoice::Deny);
            break;
        case ItemKind::Question:
        case ItemKind::Decision:
            if (chat_) chat_->answerQuestion(c.requestId, UserAnswer{true, {}});
            break;
        case ItemKind::Dialog:
            if (chat_) chat_->answerUiRequest(c.key, QJsonObject{{"cancelled", true}});
            break;
        default: continue;
        }
        resolveCard(i, QStringLiteral("cancelled"));
    }
}

void TranscriptModel::promptFinished(const QString& status, const QString& error) {
    finishStreaming();
    step_ = 0;
    // a run stopped while tools were in flight: their rows stop spinning
    for (int i = 0; i < items_.size(); ++i) {
        TranscriptItem& t = items_[i];
        if (t.kind == ItemKind::Tool && (t.status == ToolStatus::Running || t.status == ToolStatus::Pending)) {
            t.status = status == QLatin1String("aborted") ? ToolStatus::Cancelled : ToolStatus::Done;
            touch(i);
        }
    }
    if (status == QLatin1String("aborted")) {
        addNotice(QStringLiteral("Cancelled by user"), true);
        activity([](ChatActivity& a) { a.lastOutcome = QStringLiteral("cancelled"); });
    } else if (status == QLatin1String("error")) {
        if (!error.isEmpty() && error != lastError_) {
            addNotice(userFacingError(error), true);
            if (needsModelSetup(error)) Q_EMIT uiAction(QStringLiteral("setup"), QString());
        }
        activity([](ChatActivity& a) { a.lastOutcome = QStringLiteral("error"); });
    } else if (activity_.lastOutcome != QLatin1String("error")) {
        activity([](ChatActivity& a) { a.lastOutcome = QStringLiteral("idle"); });
    }
    lastError_.clear();
}

// ---- transcript replay ---------------------------------------------------------------------------------

void TranscriptModel::applyTranscript(const QJsonArray& messages) {
    flush_.stop();
    reveal_.stop();
    backlog_.clear();
    pending_.clear();
    beginResetModel();
    items_.clear();
    rowCache_.clear();
    step_ = text_ = thinking_ = task_ = 0;
    textLead_.clear();
    tools_.clear();
    blockTool_.clear();
    agentTask_.clear();
    // items are appended without per-row signals: one reset for the whole replay
    const bool wasSignalling = !signalsBlocked();
    blockSignals(true);
    for (const QJsonValue& v : messages) {
        const QJsonObject m = v.toObject();
        const QString role = str(m, "role");
        if (role == QLatin1String("user")) {
            const std::optional<QString> text = typedUserText(m);
            if (!text) continue;
            step_ = 0;
            TranscriptItem u;
            u.kind = ItemKind::User;
            u.text = *text;
            u.timeMs = qint64(m.value(QLatin1String("timestamp")).toDouble());
            append(std::move(u));
        } else if (role == QLatin1String("assistant")) {
            restoreAssistant(m);
        } else if (role == QLatin1String("toolResult")) {
            restoreToolResult(m);
        }
        // custom roles, compaction summaries ...: not chat lines
    }
    blockSignals(!wasSignalling);
    step_ = text_ = thinking_ = task_ = 0;
    tools_.clear();
    endResetModel();
    Q_EMIT activityChanged();
    Q_EMIT awaitingChanged();
}

void TranscriptModel::restoreAssistant(const QJsonObject& m) {
    step_ = 0;
    task_ = 0;
    const qint64 ts = qint64(m.value(QLatin1String("timestamp")).toDouble());
    for (const QJsonValue& bv : m.value(QLatin1String("content")).toArray()) {
        const QJsonObject b = bv.toObject();
        const QString type = str(b, "type");
        if (type == QLatin1String("text") || type == QLatin1String("thinking")) {
            const QString text = type == QLatin1String("text") ? str(b, "text") : str(b, "thinking");
            if (text.trimmed().isEmpty()) continue;
            ensureStep(ts);
            TranscriptItem t;
            t.kind = type == QLatin1String("text") ? ItemKind::Text : ItemKind::Thinking;
            t.depth = 1;
            t.text = text;
            t.timeMs = ts;
            computeChips(t);
            append(std::move(t));
        } else if (type == QLatin1String("toolCall")) {
            const QString name = str(b, "name");
            const QJsonObject args = b.value(QLatin1String("arguments")).toObject();
            if (hiddenTool(name)) continue;
            onToolStart(str(b, "id"), name, args, ts);
            if (TranscriptItem* t = itemById(tools_.value(str(b, "id")))) {
                if (t->kind == ItemKind::Tool) t->status = ToolStatus::Done;
            }
        }
    }
    const QString stop = str(m, "stopReason");
    if (stop == QLatin1String("aborted")) addNotice(QStringLiteral("Cancelled by user"), true);
    else if (stop == QLatin1String("error")) addNotice(str(m, "errorMessage").isEmpty() ? QStringLiteral("Unknown error") : str(m, "errorMessage"), true);
    activity_.currentTool.clear();
}

void TranscriptModel::restoreToolResult(const QJsonObject& m) {
    const QString id = str(m, "toolCallId");
    const bool isError = m.value(QLatin1String("isError")).toBool();
    const QJsonObject result{{"content", m.value(QLatin1String("content"))}, {"details", m.value(QLatin1String("details"))}};
    if (str(m, "toolName") == QLatin1String("todo")) {
        const QJsonArray phases = m.value(QLatin1String("details")).toObject().value(QLatin1String("phases")).toArray();
        if (!isError) activity_.todoPhases = phases;
        return;
    }
    if (agentTask_.contains(id)) {
        onToolEnd(id, result, isError, qint64(m.value(QLatin1String("timestamp")).toDouble()));
        return;
    }
    TranscriptItem* t = itemById(tools_.value(id));
    if (!t || t->kind != ItemKind::Tool) return;
    fillToolResult(*t, result, isError);
    // the mutation log is not persisted: a replayed change is shown but cannot be undone from here
    t->mutationIndex = -1;
}

}  // namespace rk::ui
