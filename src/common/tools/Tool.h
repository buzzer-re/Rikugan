#pragma once
// Host tools: what the model can call that Rikugan implements (analysis tools, ask_user, save_memory ...).
//
// A tool is a ToolSpec: its name, description and parameters are the model-facing contract, plus a handler.
// Handlers run on a ToolDispatcher worker; a tool flagged MainThread runs on the host's main thread instead
// (ToolDispatcher marshals it). Handlers report problems by throwing ToolError (the model sees
// "Error: <message>") or by returning text.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace rk {

class Host;
struct ChatContext;

enum ToolFlag : uint32_t {
    Mutating = 1u << 0,            // changes the analysis database: undo record, cache flush, approve_mutations
    MainThread = 1u << 1,          // runs on the host main thread (IDA: every tool; BN: UI and mutating tools)
    RequiresDecompiler = 1u << 2,  // hidden when the host has no decompiler
    Cacheable = 1u << 3,           // result kept in the per-database ResultCache until a mutation
    Script = 1u << 4,              // runs model-written code: always asks the user first
    Internal = 1u << 5,            // never shown to the model (undo pre-state readers)
    Exclusive = 1u << 6,           // omc runs it alone and in message order (Mutating and Script imply it)
    Blocking = 1u << 7,            // waits on the user or another agent: runs on its own thread, not the pool
    External = 1u << 8,            // served by another program (the host's MCP server): a Mutating one has no undo
};

class ToolError : public std::runtime_error {
public:
    explicit ToolError(const QString& message) : std::runtime_error(message.toStdString()) {}
    QString message() const { return QString::fromStdString(what()); }
};

// A reverse operation can never succeed; undo drops the record instead of stopping.
class UndoNoLongerPossible : public ToolError {
public:
    using ToolError::ToolError;
};

struct ToolParam {
    enum Type { String, Integer, Number, Boolean, Array, Object };
    QString name;
    Type type = String;
    QString description;
    QJsonValue def = QJsonValue(QJsonValue::Undefined);  // Undefined: required
    QStringList enumValues;
    QJsonObject items;  // array item schema, e.g. {"type":"string"}

    bool required() const { return def.isUndefined(); }
};

struct ToolResult {
    QString text;
    bool isError = false;
    QJsonObject details;  // Rikugan-side data for the UI (never sent to the model)

    static ToolResult ok(QString t) { return ToolResult{std::move(t), false, {}}; }
    static ToolResult error(QString t) { return ToolResult{std::move(t), true, {}}; }
};

class CancelToken {
public:
    bool cancelled() const { return flag_.load(std::memory_order_relaxed); }
    void cancel() { flag_.store(true, std::memory_order_relaxed); }

private:
    std::atomic<bool> flag_{false};
};

struct ToolSpec;

class ToolCall {
public:
    ToolCall(const ToolSpec& spec, QJsonObject args, QString toolCallId, Host& host, ChatContext* chat,
             std::shared_ptr<CancelToken> token, std::function<void(const QString&)> update = {});

    const ToolSpec& spec() const { return spec_; }
    const QJsonObject& args() const { return args_; }
    const QString& toolCallId() const { return toolCallId_; }
    Host& host() const { return host_; }
    // The chat the call belongs to; null for internal calls made outside a chat (bulk renamer, undo).
    ChatContext* chat() const { return chat_; }

    // Arguments converted with Python's str/int/float/bool rules, and the parameter's default when absent.
    // A missing required argument throws ToolError("Missing required argument: <name>").
    bool has(const char* name) const;
    QString str(const char* name) const;
    qint64 integer(const char* name) const;
    double number(const char* name) const;
    bool boolean(const char* name) const;
    QJsonArray array(const char* name) const;
    QJsonValue value(const char* name) const;
    // An integer, or a string parsed like Python int(s, 0) ("0x401000", "4198400", "0o17", "0b1").
    uint64_t address(const char* name) const;

    bool cancelled() const { return token_ && token_->cancelled(); }
    const std::shared_ptr<CancelToken>& token() const { return token_; }
    void throwIfCancelled() const;
    // Partial result shown while the tool runs (tool_execution_update).
    void update(const QString& partial) const;

private:
    const ToolParam* param(const char* name) const;

    const ToolSpec& spec_;
    QJsonObject args_;
    QString toolCallId_;
    Host& host_;
    ChatContext* chat_;
    std::shared_ptr<CancelToken> token_;
    std::function<void(const QString&)> update_;
};

using ToolFn = std::function<ToolResult(ToolCall&)>;

struct ToolSpec {
    QString name;
    QString category;
    QString description;
    std::vector<ToolParam> params;
    uint32_t flags = 0;
    int resultLimit = 20000;
    ToolFn fn;
    QJsonObject inputSchema = {};  // an External tool's own JSON schema, sent as is instead of `params`

    bool has(ToolFlag f) const { return (flags & f) != 0; }
    bool exclusive() const { return (flags & (Mutating | Script | Exclusive)) != 0; }
    // {"type":"object","properties":{...},"required":[...]}.
    QJsonObject schema() const;
};

// address() for any JSON value; throws ToolError on garbage.
uint64_t parseAddress(const QJsonValue& v);
// Python int(s, 0) for strings; nullopt-like: returns false on garbage.
bool parsePythonInt(const QString& s, qint64* out);

// Hex address as the tools print it: f"0x{ea:x}".
inline QString hex(uint64_t ea) { return QStringLiteral("0x") + QString::number(ea, 16); }

}  // namespace rk
