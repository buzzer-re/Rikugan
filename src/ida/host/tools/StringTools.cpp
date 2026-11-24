// IDA string tools.
#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

struct StringItem {
    ea_t ea;
    int length;
    int type;
};

// idautils.Strings(): rebuilds the string list with the current options
std::vector<StringItem> stringList() {
    get_strlist_options();
    build_strlist();
    const size_t qty = get_strlist_qty();
    std::vector<StringItem> out;
    out.reserve(qty);
    string_info_t si;
    for (size_t i = 0; i < qty; ++i)
        if (get_strlist_item(&si, i)) out.push_back({si.ea, si.length, si.type});
    return out;
}

// str(StringItem): get_strlit_contents(ea, length, strtype).decode("UTF-8", "replace")
QString itemText(const StringItem& s) {
    qstring utf8;
    if (get_strlit_contents(&utf8, s.ea, size_t(s.length), s.type) < 0)
        throw ToolError(QStringLiteral("'NoneType' object has no attribute 'decode'"));
    return qs(utf8);
}

QString itemRow(const StringItem& s, const QString& text) {
    return QStringLiteral("  ") + hx(s.ea) + QStringLiteral("  [%1] ").arg(s.length) + text;
}

}  // namespace

void registerStringTools(ToolRegistry& reg, IdaHost&) {
    addTool(reg, "list_strings", "strings", "List defined strings in the binary with pagination.",
            {intParam("offset", "Start index", 0), intParam("limit", "Max results", 50)}, Cacheable, [](ToolCall& call) {
                const qint64 offset = call.integer("offset");
                const qint64 limit = call.integer("limit");
                const std::vector<StringItem> strings = stringList();
                const auto [b, e] = pySlice(qsizetype(strings.size()), offset, offset + limit);
                QStringList lines{QStringLiteral("Strings %1\u2013%2 of %3:").arg(offset).arg(offset + (e - b)).arg(strings.size())};
                for (qsizetype i = b; i < e; ++i) lines << itemRow(strings[size_t(i)], itemText(strings[size_t(i)]));
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "search_strings", "strings", "Search for strings containing the given substring.",
            {strParam("query", "Search substring (case-insensitive)"), intParam("limit", "Max results", 20)}, 0,
            [](ToolCall& call) {
                const QString query = call.str("query");
                const QString q = query.toLower();
                const qint64 limit = call.integer("limit");
                QStringList results;
                const std::vector<StringItem> strings = stringList();
                for (size_t i = 0; i < strings.size(); ++i) {
                    const QString text = itemText(strings[i]);
                    if (text.toLower().contains(q)) {
                        results << itemRow(strings[i], text);
                        if (results.size() >= limit) break;
                    }
                    if ((i & 0xfff) == 0) call.throwIfCancelled();
                }
                if (results.isEmpty()) return ToolResult::ok(QStringLiteral("No strings matching '%1'").arg(query));
                return ToolResult::ok(QStringLiteral("Found %1 string(s):\n").arg(results.size()) + results.join(QLatin1Char('\n')));
            });

    addTool(reg, "get_string_at", "strings", "Read the string at a specific address.",
            {strParam("address", "Address (hex string)")}, 0, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const size_t len = get_max_strlit_length(ea, STRTYPE_C, ALOPT_IGNHEADS);
                qstring utf8;
                if (get_strlit_contents(&utf8, ea, len, STRTYPE_C) < 0) return ToolResult::ok(QStringLiteral("No string at ") + hx(ea));
                return ToolResult::ok(qs(utf8));
            });
}

}  // namespace rk::ida
