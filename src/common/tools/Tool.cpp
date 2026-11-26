#include "common/tools/Tool.h"

#include <cmath>

#include <QJsonDocument>

namespace rk {

namespace {

QString typeName(ToolParam::Type t) {
    switch (t) {
    case ToolParam::String: return QStringLiteral("string");
    case ToolParam::Integer: return QStringLiteral("integer");
    case ToolParam::Number: return QStringLiteral("number");
    case ToolParam::Boolean: return QStringLiteral("boolean");
    case ToolParam::Array: return QStringLiteral("array");
    case ToolParam::Object: return QStringLiteral("object");
    }
    return QStringLiteral("string");
}

}  // namespace

QJsonObject ToolSpec::schema() const {
    if (!inputSchema.isEmpty()) return inputSchema;
    QJsonObject props;
    QJsonArray required;
    for (const ToolParam& p : params) {
        QJsonObject prop{{"type", typeName(p.type)}};
        if (!p.description.isEmpty()) prop.insert(QStringLiteral("description"), p.description);
        if (!p.enumValues.isEmpty()) prop.insert(QStringLiteral("enum"), QJsonArray::fromStringList(p.enumValues));
        if (!p.items.isEmpty()) prop.insert(QStringLiteral("items"), p.items);
        props.insert(p.name, prop);
        if (p.required()) required.append(p.name);
    }
    QJsonObject s{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty()) s.insert(QStringLiteral("required"), required);
    return s;
}

bool parsePythonInt(const QString& raw, qint64* out) {
    QString s = raw.trimmed();
    s.remove(QLatin1Char('_'));
    bool neg = false;
    if (s.startsWith(QLatin1Char('-')) || s.startsWith(QLatin1Char('+'))) {
        neg = s.startsWith(QLatin1Char('-'));
        s = s.mid(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == QLatin1Char('0')) {
        const QChar p = s[1].toLower();
        if (p == QLatin1Char('x')) base = 16;
        else if (p == QLatin1Char('o')) base = 8;
        else if (p == QLatin1Char('b')) base = 2;
        if (base != 10) s = s.mid(2);
    }
    // int("0400", 0) is a SyntaxError in Python: no implicit octal
    if (base == 10 && s.size() > 1 && s[0] == QLatin1Char('0') && s.count(QLatin1Char('0')) != s.size()) return false;
    if (s.isEmpty()) return false;
    bool ok = false;
    const quint64 v = s.toULongLong(&ok, base);
    if (!ok) return false;
    *out = neg ? -qint64(v) : qint64(v);
    return true;
}

uint64_t parseAddress(const QJsonValue& v) {
    if (v.isDouble()) return uint64_t(qint64(v.toDouble()));
    if (v.isString()) {
        qint64 n = 0;
        if (parsePythonInt(v.toString(), &n)) return uint64_t(n);
        throw ToolError(QStringLiteral("invalid literal for int() with base 0: '%1'").arg(v.toString()));
    }
    throw ToolError(QStringLiteral("invalid address: %1").arg(v.toVariant().toString()));
}

ToolCall::ToolCall(const ToolSpec& spec, QJsonObject args, QString toolCallId, Host& host, ChatContext* chat,
                   std::shared_ptr<CancelToken> token, std::function<void(const QString&)> update)
    : spec_(spec),
      args_(std::move(args)),
      toolCallId_(std::move(toolCallId)),
      host_(host),
      chat_(chat),
      token_(std::move(token)),
      update_(std::move(update)) {}

const ToolParam* ToolCall::param(const char* name) const {
    for (const ToolParam& p : spec_.params)
        if (p.name == QLatin1String(name)) return &p;
    return nullptr;
}

bool ToolCall::has(const char* name) const {
    const QJsonValue v = args_.value(QLatin1String(name));
    return !v.isUndefined() && !v.isNull();
}

QJsonValue ToolCall::value(const char* name) const {
    const QJsonValue v = args_.value(QLatin1String(name));
    if (!v.isUndefined() && !v.isNull()) return v;
    const ToolParam* p = param(name);
    if (p && !p->required()) return p->def;
    if (p) throw ToolError(QStringLiteral("Missing required argument: %1").arg(QLatin1String(name)));
    return QJsonValue();
}

QString ToolCall::str(const char* name) const {
    const QJsonValue v = value(name);
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == std::floor(d) && std::abs(d) < 9e15 ? QString::number(qint64(d)) : QString::number(d);
    }
    if (v.isNull() || v.isUndefined()) return {};
    return QString::fromUtf8(QJsonDocument(v.isArray() ? QJsonDocument(v.toArray()) : QJsonDocument(v.toObject()))
                                 .toJson(QJsonDocument::Compact));
}

qint64 ToolCall::integer(const char* name) const {
    const QJsonValue v = value(name);
    if (v.isBool()) return v.toBool() ? 1 : 0;
    if (v.isDouble()) return qint64(v.toDouble());
    if (v.isString()) {
        // int(float(value)) first; also accept int(value, 0) forms like "0x1e"
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (ok) return qint64(d);
        qint64 n = 0;
        if (parsePythonInt(v.toString(), &n)) return n;
    }
    throw ToolError(QStringLiteral("Invalid arguments for %1: %2 must be an integer").arg(spec_.name, QLatin1String(name)));
}

double ToolCall::number(const char* name) const {
    const QJsonValue v = value(name);
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (ok) return d;
    }
    throw ToolError(QStringLiteral("Invalid arguments for %1: %2 must be a number").arg(spec_.name, QLatin1String(name)));
}

bool ToolCall::boolean(const char* name) const {
    const QJsonValue v = value(name);
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    const QString s = v.toString().trimmed().toLower();
    return s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes");
}

QJsonArray ToolCall::array(const char* name) const {
    const QJsonValue v = value(name);
    if (v.isArray()) return v.toArray();
    if (v.isString()) {
        const QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
        if (d.isArray()) return d.array();
    }
    if (v.isNull() || v.isUndefined()) return {};
    throw ToolError(QStringLiteral("Invalid arguments for %1: %2 must be an array").arg(spec_.name, QLatin1String(name)));
}

uint64_t ToolCall::address(const char* name) const { return parseAddress(value(name)); }

void ToolCall::throwIfCancelled() const {
    if (cancelled()) throw ToolError(QStringLiteral("Tool call cancelled"));
}

void ToolCall::update(const QString& partial) const {
    if (update_) update_(partial);
}

}  // namespace rk
