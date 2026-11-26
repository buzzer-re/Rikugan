#include "common/agent/ChatExport.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include "common/agent/SystemPrompt.h"

namespace rk {

namespace {

QString contentText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue& v : content.toArray()) {
        const QJsonObject b = v.toObject();
        if (b.value(QLatin1String("type")).toString() == QLatin1String("text")) parts << b.value(QLatin1String("text")).toString();
    }
    return parts.join(QLatin1Char('\n'));
}

// Wrapping the sanitizer adds around tool results and memory, dropped from exports.
QString stripSanitizerTags(const QString& text) {
    static const QRegularExpression tag(
        QStringLiteral("^\\[The following is (?:a tool execution result|output from an EXTERNAL MCP server)[^\\]]*\\]\\n?"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression wrap(QStringLiteral(
        "<(?:tool_result|mcp_result|binary_data|persistent_memory|skill)\\b[^>]*>\\n?|</(?:tool_result|mcp_result|binary_data|"
        "persistent_memory|skill)>\\n?"));
    QString t = text;
    t.remove(tag);
    t.remove(wrap);
    return t.trimmed();
}

// Fence language for exported text.
QString exportLang(const QString& text, const QString& toolName = {}, const QString& argKey = {}) {
    if (argKey == QLatin1String("code") || argKey == QLatin1String("script") || toolName == QLatin1String("execute_python"))
        return QStringLiteral("python");
    if (argKey == QLatin1String("command") || toolName == QLatin1String("bash")) return QStringLiteral("bash");
    const QString t = text.trimmed();
    if ((t.startsWith(QLatin1Char('{')) && t.endsWith(QLatin1Char('}'))) || (t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'))))
        return QStringLiteral("json");
    static const QRegularExpression c(QStringLiteral("\\b(?:void|int|char|unsigned|struct|return)\\b.*[;{]"));
    if (c.match(t).hasMatch()) return QStringLiteral("c");
    return {};
}

QString fence(const QString& text, const QString& lang) {
    QString f = QStringLiteral("```");
    while (text.contains(f)) f += QLatin1Char('`');
    return f + lang + QLatin1Char('\n') + text + QLatin1Char('\n') + f;
}

QString exportToolArgs(const QString& toolName, const QJsonObject& args) {
    for (const char* key : {"code", "script", "command"}) {
        const QString v = args.value(QLatin1String(key)).toString();
        if (!v.isEmpty()) return fence(v, exportLang(v, toolName, QLatin1String(key)));
    }
    if (args.isEmpty()) return {};
    return fence(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented)).trimmed(), QStringLiteral("json"));
}

QString exportToolResult(const QString& text) {
    QString t = stripSanitizerTags(text);
    constexpr int kTruncate = 2000;
    if (t.size() > kTruncate) t = t.left(kTruncate) + QStringLiteral("\n... (%1 chars truncated)").arg(t.size() - kTruncate);
    return fence(t, exportLang(t));
}

}  // namespace

std::optional<QString> typedUserText(const QJsonObject& m) {
    const auto str = [&m](const char* k) { return m.value(QLatin1String(k)).toString(); };
    if (str("role") != QLatin1String("user") || m.value(QLatin1String("synthetic")).toBool() ||
        str("attribution") == QLatin1String("agent"))
        return std::nullopt;
    const QString customType = str("customType");
    QString text;
    if (customType == QLatin1String("skill-prompt")) {
        // the content is the expanded skill; details.prompt is the invocation, "/skill:<slug> ..." as CommandRouter
        // sent it for the "/<slug> ..." the user typed
        text = m.value(QLatin1String("details")).toObject().value(QLatin1String("prompt")).toString().trimmed();
        if (text.startsWith(QLatin1String("/skill:"))) text.remove(1, 6);
    } else {
        // every other custom message is omc's (replayed ones carry sourceRole "custom"), as are shell/hook output
        // and file mentions
        if (!customType.isEmpty() || !str("sourceRole").isEmpty()) return std::nullopt;
        text = stripContextPrefix(contentText(m.value(QLatin1String("content"))));
        if (text.trimmed().startsWith(QLatin1String("[SYSTEM]"))) return std::nullopt;
    }
    int images = 0;
    for (const QJsonValue& v : m.value(QLatin1String("content")).toArray())
        if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("image")) ++images;
    if (images > 0) {
        const QString note = images == 1 ? QStringLiteral("[1 image]") : QStringLiteral("[%1 images]").arg(images);
        text = text.trimmed().isEmpty() ? note : text + QLatin1Char('\n') + note;
    }
    if (text.trimmed().isEmpty()) return std::nullopt;
    return text;
}

bool writeMarkdownExport(const QJsonArray& messages, const QString& path, const QString& model, const QString& file,
                         QString* error) {
    QStringList lines{QStringLiteral("# Rikugan Chat Export\n")};
    lines << QStringLiteral("- **Model**: %1").arg(model.isEmpty() ? QStringLiteral("unknown") : model);
    lines << QStringLiteral("- **Exported**: %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (!file.isEmpty()) lines << QStringLiteral("- **File**: `%1`").arg(file);
    lines << QString() << QStringLiteral("---\n");
    for (const QJsonValue& v : messages) {
        const QJsonObject m = v.toObject();
        const QString role = m.value(QLatin1String("role")).toString();
        if (role == QLatin1String("user")) {
            if (const std::optional<QString> text = typedUserText(m)) lines << QStringLiteral("## You\n\n%1\n").arg(*text);
        } else if (role == QLatin1String("assistant")) {
            QStringList text;
            QList<QJsonObject> calls;
            for (const QJsonValue& bv : m.value(QLatin1String("content")).toArray()) {
                const QJsonObject b = bv.toObject();
                const QString type = b.value(QLatin1String("type")).toString();
                if (type == QLatin1String("text")) text << b.value(QLatin1String("text")).toString();
                else if (type == QLatin1String("toolCall")) calls << b;
            }
            const QString body = text.join(QString()).trimmed();
            if (!body.isEmpty()) lines << QStringLiteral("## Rikugan\n\n%1\n").arg(body);
            for (const QJsonObject& tc : calls) {
                const QString name = tc.value(QLatin1String("name")).toString();
                lines << QStringLiteral("**Tool call**: `%1`\n").arg(name);
                lines << exportToolArgs(name, tc.value(QLatin1String("arguments")).toObject());
                lines << QString();
            }
        } else if (role == QLatin1String("toolResult")) {
            const bool isError = m.value(QLatin1String("isError")).toBool();
            lines << QStringLiteral("**%1** (`%2`):\n")
                         .arg(isError ? QStringLiteral("Error") : QStringLiteral("Result"), m.value(QLatin1String("toolName")).toString());
            lines << exportToolResult(contentText(m.value(QLatin1String("content"))));
            lines << QString();
        }
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = f.errorString();
        return false;
    }
    f.write(lines.join(QLatin1Char('\n')).toUtf8());
    return true;
}

}  // namespace rk
