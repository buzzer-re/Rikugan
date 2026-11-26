// Binary Ninja string tools.
#include <QStringDecoder>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

struct StringRow {
    uint64_t ea;
    uint64_t length;
    QString text;
};

// StringReference.value: bv.read(start, length).decode(<type codec>, errors='replace')
QString decodeString(BinaryView* bv, const BNStringReference& ref) {
    const QByteArray raw = readRaw(bv, ref.start, size_t(ref.length));
    switch (ref.type) {
    case AsciiString: {
        QString out;
        out.reserve(raw.size());
        for (char c : raw) out.append(uint8_t(c) < 0x80 ? QChar(uint8_t(c)) : QChar(0xFFFD));
        return out;
    }
    case Utf16String: {
        QStringDecoder dec(QStringDecoder::Utf16);
        return dec.decode(raw);
    }
    case Utf32String: {
        QStringDecoder dec(QStringDecoder::Utf32);
        return dec.decode(raw);
    }
    case Utf8String:
    default: return QString::fromUtf8(raw);
    }
}

std::vector<StringRow> allStrings(BinaryView* bv) {
    std::vector<StringRow> rows;
    for (const BNStringReference& ref : bv->GetStrings()) rows.push_back({ref.start, ref.length, decodeString(bv, ref)});
    return rows;
}

QString stringLine(const StringRow& s) { return QStringLiteral("  %1  [%2] %3").arg(hex(s.ea)).arg(s.length).arg(s.text); }

}  // namespace

void registerStringTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "list_strings", "strings", "List defined strings in the binary with pagination.",
        {param("offset", ToolParam::Integer, "Start index", 0), param("limit", ToolParam::Integer, "Max results", 50)},
        Cacheable, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const std::vector<StringRow> strings = allStrings(bv);
            const qint64 total = qint64(strings.size());
            // Python slice semantics for strings[offset:offset + limit]
            auto clamp = [total](qint64 i) { return i < 0 ? std::max<qint64>(0, total + i) : std::min(i, total); };
            const qint64 offset = call.integer("offset");
            const qint64 limit = call.integer("limit");
            const qint64 from = clamp(offset);
            const qint64 to = std::max(from, clamp(offset + limit));
            QStringList lines{QStringLiteral("Strings %1\u2013%2 of %3:").arg(offset).arg(offset + (to - from)).arg(total)};
            for (qint64 i = from; i < to; ++i) lines.append(stringLine(strings[size_t(i)]));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "search_strings", "strings", "Search for strings containing the given substring.",
        {param("query", ToolParam::String, "Search substring (case-insensitive)"),
         param("limit", ToolParam::Integer, "Max results", 20)},
        0, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString query = call.str("query");
            const QString q = query.toLower();
            const qint64 limit = call.integer("limit");
            QStringList results;
            for (const BNStringReference& ref : bv->GetStrings()) {
                call.throwIfCancelled();
                const StringRow row{ref.start, ref.length, decodeString(bv, ref)};
                if (row.text.toLower().contains(q)) {
                    results.append(stringLine(row));
                    if (results.size() >= limit) break;
                }
            }
            if (results.isEmpty()) return ToolResult::ok(QStringLiteral("No strings matching '%1'").arg(query));
            return ToolResult::ok(QStringLiteral("Found %1 string(s):\n").arg(results.size()) +
                                  results.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool("get_string_at", "strings", "Read the string at a specific address.",
                          {param("address", ToolParam::String, "Address (hex string)")}, 0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const uint64_t ea = call.address("address");
                              BNStringReference ref{};
                              if (bv->GetStringAtAddress(ea, ref)) {
                                  const QString value = decodeString(bv, ref);
                                  if (!value.isEmpty()) return ToolResult::ok(value);
                              }
                              for (const BNStringReference& s : bv->GetStrings())
                                  if (s.start == ea) return ToolResult::ok(decodeString(bv, s));
                              return ToolResult::ok(QStringLiteral("No string at %1").arg(hex(ea)));
                          }));
}

}  // namespace rk::binja
