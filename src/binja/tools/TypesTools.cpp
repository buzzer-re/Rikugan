// Binary Ninja type tools.
#include <algorithm>
#include <map>
#include <optional>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

// worker threads: the core API is thread-safe, and the UI must not wait on analysis
constexpr uint32_t kMutating = Mutating;

struct Member {
    QString name;
    QString type;
    qint64 offset = 0;
    qint64 size = 4;
    QString comment;
};

// ---- Python value coercions for JSON input ----

QString pyTypeName(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Bool: return QStringLiteral("bool");
    case QJsonValue::Double: return v.toDouble() == double(qint64(v.toDouble())) ? QStringLiteral("int") : QStringLiteral("float");
    case QJsonValue::String: return QStringLiteral("str");
    case QJsonValue::Array: return QStringLiteral("list");
    case QJsonValue::Object: return QStringLiteral("dict");
    default: return QStringLiteral("NoneType");
    }
}

// str(value)
QString pyStr(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == double(qint64(d))) return QString::number(qint64(d));
        return QString::number(d, 'g', 17);
    }
    case QJsonValue::String: return v.toString();
    case QJsonValue::Array: return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    default: return QStringLiteral("None");
    }
}

// isinstance(value, int) (bool included)
std::optional<qint64> pyIntInstance(const QJsonValue& v) {
    if (v.isBool()) return v.toBool() ? 1 : 0;
    if (v.isDouble() && v.toDouble() == double(qint64(v.toDouble()))) return qint64(v.toDouble());
    return std::nullopt;
}

// int(value)
qint64 pyInt(const QJsonValue& v) {
    if (const auto i = pyIntInstance(v)) return *i;
    if (v.isDouble()) return qint64(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().trimmed().remove(QLatin1Char('_')).toLongLong(&ok, 10);
        if (ok) return n;
        throw ToolError(QStringLiteral("invalid literal for int() with base 10: '%1'").arg(v.toString()));
    }
    throw ToolError(QStringLiteral("int() argument must be a string, a bytes-like object or a real number, not '%1'")
                        .arg(pyTypeName(v)));
}

std::optional<QJsonArray> parseJsonList(const QString& text, const char* what, QString* error) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        *error = QStringLiteral("Invalid %1 JSON: %2 (char %3)").arg(QLatin1String(what), err.errorString()).arg(err.offset);
        return std::nullopt;
    }
    if (!doc.isArray()) {
        *error = QStringLiteral("%1 must be a JSON array").arg(QLatin1String(what));
        return std::nullopt;
    }
    return doc.array();
}

// ---- type helpers ----

std::map<std::string, std::pair<QualifiedName, Ref<Type>>> namedTypes(BinaryView* bv) {
    std::map<std::string, std::pair<QualifiedName, Ref<Type>>> out;
    for (const auto& [name, type] : bv->GetTypes()) out[name.GetString()] = {name, type};
    return out;
}

Ref<Type> typeByName(BinaryView* bv, const QString& name) {
    if (Ref<Type> t = bv->GetTypeByName(QualifiedName(ss(name)))) return t;
    const auto all = namedTypes(bv);
    const auto it = all.find(ss(name));
    return it == all.end() ? nullptr : it->second.second;
}

bool isStruct(Type* t) { return t && t->GetClass() == StructureTypeClass && t->GetStructure(); }
bool isEnum(Type* t) { return t && t->GetClass() == EnumerationTypeClass && t->GetEnumeration(); }

QString typeToStr(Type* t) {
    if (!t) return QStringLiteral("void");
    const QString before = qs(t->GetStringBeforeName());
    return before.isEmpty() ? qs(t->GetString()) : before;
}

Ref<Type> parseFieldType(BinaryView* bv, const QString& typeStr) {
    for (const QString& src : {typeStr + QStringLiteral(" __rikugan_tmp"), typeStr}) {
        try {
            return parseTypeString(bv, src);
        } catch (const ToolError&) {
        }
    }
    return nullptr;
}

qint64 sizeofType(BinaryView* bv, const QString& typeStr, qint64 def = 4) {
    Ref<Type> t = parseFieldType(bv, typeStr);
    if (!t) return def;
    const uint64_t width = t->GetWidth();
    return width > 0 ? qint64(width) : def;
}

std::vector<Member> structMembers(Type* t) {
    std::vector<Member> rows;
    if (!isStruct(t)) return rows;
    for (const StructureMember& m : t->GetStructure()->GetMembers()) {
        Type* mt = m.type.GetValue();
        const qint64 width = mt ? qint64(mt->GetWidth()) : 0;
        rows.push_back({qs(m.name), typeToStr(mt), qint64(m.offset), width > 0 ? width : 4, QString()});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Member& a, const Member& b) { return a.offset < b.offset; });
    return rows;
}

qint64 structSize(const std::vector<Member>& members) {
    qint64 size = 0;
    for (const Member& m : members) size = std::max(size, m.offset + m.size);
    return size;
}

std::vector<std::pair<QString, qint64>> enumMembers(Type* t) {
    std::vector<std::pair<QString, qint64>> out;
    if (!isEnum(t)) return out;
    const uint64_t width = t->GetWidth();
    const bool isSigned = t->IsSigned().GetValue();
    for (const EnumerationMember& m : t->GetEnumeration()->GetMembers()) {
        // types.convert_integer(value, signed, width)
        int64_t value = int64_t(m.value);
        if (isSigned && width > 0 && width < 8) {
            const uint64_t bits = width * 8;
            const uint64_t mask = (uint64_t(1) << bits) - 1;
            const uint64_t v = m.value & mask;
            value = (v & (uint64_t(1) << (bits - 1))) ? int64_t(v) - int64_t(uint64_t(1) << bits) : int64_t(v);
        } else if (!isSigned && width > 0 && width < 8) {
            value = int64_t(m.value & ((uint64_t(1) << (width * 8)) - 1));
        }
        out.emplace_back(qs(m.name), value);
    }
    return out;
}

// {name: type} in parser order
std::vector<std::pair<QualifiedName, Ref<Type>>> parseTypesFromSource(BinaryView* bv, const QString& source) {
    TypeParserResult result;
    std::string errors;
    if (!bv->ParseTypesFromSource(ss(source), {}, {}, result, errors))
        throw ToolError(QStringLiteral("Binary Ninja failed to parse C declarations"));
    std::vector<std::pair<QualifiedName, Ref<Type>>> out;
    for (const ParsedType& p : result.types) {
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == p.name; });
        if (it != out.end()) it->second = p.type;
        else out.emplace_back(p.name, p.type);
    }
    return out;
}

// The names defined
QStringList defineTypesFromSource(BinaryView* bv, const QString& source) {
    QStringList defined;
    for (const auto& [name, type] : parseTypesFromSource(bv, source)) {
        bv->DefineUserType(name, type);
        defined.append(qs(name.GetString()));
    }
    if (!defined.isEmpty()) updateAnalysisAndWait(bv);
    return defined;
}

QString buildStructDecl(const QString& name, const std::vector<Member>& fields) {
    std::vector<Member> ordered = fields;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Member& a, const Member& b) { return a.offset < b.offset; });
    QStringList lines{QStringLiteral("struct %1 {").arg(name)};
    qint64 cur = 0;
    for (const Member& f : ordered) {
        if (f.offset > cur) {
            lines.append(QStringLiteral("    char _pad_%1[%2];").arg(QString::number(cur, 16)).arg(f.offset - cur));
            cur = f.offset;
        }
        lines.append(QStringLiteral("    %1 %2;").arg(f.type, f.name));
        cur = std::max(cur, f.offset + std::max<qint64>(1, f.size ? f.size : 4));
    }
    lines.append(QStringLiteral("};"));
    return lines.join(QLatin1Char('\n'));
}

bool redefineStructWithBuilder(BinaryView* bv, const QString& name, const std::vector<Member>& fields) {
    StructureBuilder sb;
    int added = 0;
    for (const Member& f : fields) {
        Ref<Type> ftype = parseFieldType(bv, f.type);
        if (!ftype) continue;
        if (f.offset >= 0) sb.AddMemberAtOffset(Confidence<Ref<Type>>(ftype), ss(f.name), uint64_t(f.offset));
        else sb.AddMember(Confidence<Ref<Type>>(ftype), ss(f.name));
        ++added;
    }
    if (added == 0) return false;
    bv->DefineUserType(QualifiedName(ss(name)), Type::StructureType(sb.Finalize()));
    updateAnalysisAndWait(bv);
    return true;
}

bool redefineStruct(BinaryView* bv, const QString& name, const std::vector<Member>& fields) {
    if (redefineStructWithBuilder(bv, name, fields)) {
        Ref<Type> t = typeByName(bv, name);
        if (t && !structMembers(t).empty()) return true;
    }
    const QStringList parsed = defineTypesFromSource(bv, buildStructDecl(name, fields));
    return parsed.contains(name) || parsed.contains(QStringLiteral("struct ") + name);
}

QString enumDecl(const QString& name, const std::vector<std::pair<QString, qint64>>& members) {
    QStringList lines{QStringLiteral("enum %1 {").arg(name)};
    for (size_t i = 0; i < members.size(); ++i)
        lines.append(QStringLiteral("    %1 = %2%3")
                         .arg(members[i].first)
                         .arg(members[i].second)
                         .arg(i + 1 < members.size() ? QStringLiteral(",") : QString()));
    lines.append(QStringLiteral("};"));
    return lines.join(QLatin1Char('\n'));
}

std::optional<Variable> findVariable(Function* func, const QString& name) {
    std::optional<Variable> found;
    size_t count = 0;
    BNVariableNameAndType* vars = BNGetFunctionVariables(func->GetObject(), &count);
    for (size_t i = 0; i < count && !found; ++i) {
        const Variable var(vars[i].var);
        if (qs(func->GetVariableNameOrDefault(var)) == name) found = var;
    }
    BNFreeVariableNameAndTypeList(vars, count);
    return found;
}

std::vector<Ref<TypeLibrary>> typeLibraries(BinaryView* bv) {
    Ref<Platform> platform = bv->GetDefaultPlatform();
    return platform ? platform->GetTypeLibraries() : std::vector<Ref<TypeLibrary>>{};
}

}  // namespace

void registerTypeTools(ToolRegistry& registry) {
    registry.add(makeTool(
        "create_struct", "types", "Create a new struct with typed fields.",
        {param("name", ToolParam::String, "Struct name"),
         param("fields", ToolParam::String, "JSON array of fields: [{name, type, offset?, comment?}, ...]")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString name = call.str("name");
            if (typeByName(bv, name)) return ToolResult::ok(QStringLiteral("Struct '%1' already exists").arg(name));
            QString error;
            const std::optional<QJsonArray> fieldList = parseJsonList(call.str("fields"), "fields", &error);
            if (!fieldList) return ToolResult::ok(error);
            std::vector<Member> normalized;
            qint64 cur = 0;
            for (const QJsonValue& v : *fieldList) {
                if (!v.isObject() || !v.toObject().contains(QStringLiteral("name"))) continue;
                const QJsonObject fld = v.toObject();
                const QString ftype =
                    fld.contains(QStringLiteral("type")) ? pyStr(fld.value(QStringLiteral("type"))) : QStringLiteral("int");
                const qint64 size = sizeofType(bv, ftype, 4);
                qint64 off = cur;
                if (fld.contains(QStringLiteral("offset")))
                    if (const auto i = pyIntInstance(fld.value(QStringLiteral("offset")))) off = *i;
                normalized.push_back({pyStr(fld.value(QStringLiteral("name"))), ftype, off, size,
                                      fld.contains(QStringLiteral("comment")) ? pyStr(fld.value(QStringLiteral("comment")))
                                                                              : QString()});
                cur = std::max(cur, off + size);
            }
            if (normalized.empty()) return ToolResult::ok(QStringLiteral("No valid fields provided"));
            const bool ok = withUndo(bv, [&] { return redefineStruct(bv, name, normalized); });
            if (!ok) return ToolResult::ok(QStringLiteral("Failed to create struct '%1'").arg(name));
            Ref<Type> t = typeByName(bv, name);
            const size_t count = t ? structMembers(t).size() : normalized.size();
            return ToolResult::ok(QStringLiteral("Created struct '%1' with %2 fields").arg(name).arg(count));
        }));

    registry.add(makeTool(
        "modify_struct", "types", "Modify an existing struct: add/remove/rename/retype fields.",
        {param("name", ToolParam::String, "Struct name"),
         param("action", ToolParam::String,
               "Action: add_field, remove_field, rename_field, retype_field, set_field_comment, resize"),
         param("field_name", ToolParam::String, "Field name to modify", ""),
         param("new_name", ToolParam::String, "New name (for rename_field)", ""),
         param("field_type", ToolParam::String, "Type string (for add_field/retype_field)", "int"),
         param("offset", ToolParam::Integer, "Offset (for add_field)", -1),
         param("comment", ToolParam::String, "Comment text (for set_field_comment)", ""),
         param("new_size", ToolParam::Integer, "New struct size (for resize)", 0)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString name = call.str("name");
            const QString action = call.str("action");
            const QString fieldName = call.str("field_name");
            const QString newName = call.str("new_name");
            const QString fieldType = call.str("field_type");
            const qint64 offset = call.integer("offset");
            const QString comment = call.str("comment");
            const qint64 newSize = call.integer("new_size");
            Ref<Type> t = typeByName(bv, name);
            if (!isStruct(t)) return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(name));
            std::vector<Member> members = structMembers(t);
            auto redefine = [&] { return withUndo(bv, [&] { return redefineStruct(bv, name, members); }); };

            if (action == QLatin1String("add_field")) {
                if (fieldName.isEmpty()) return ToolResult::ok(QStringLiteral("field_name is required"));
                const qint64 size = sizeofType(bv, fieldType, 4);
                qint64 off = 0;
                if (offset >= 0) {
                    off = offset;
                } else if (!members.empty()) {
                    const Member* last = &members.front();
                    for (const Member& m : members)
                        if (m.offset + m.size > last->offset + last->size) last = &m;
                    off = last->offset + last->size;
                }
                members.push_back({fieldName, fieldType, off, size, comment});
                return ToolResult::ok(redefine() ? QStringLiteral("Added field '%1' (%2) to '%3'").arg(fieldName, fieldType, name)
                                                 : QStringLiteral("Failed to add field"));
            }
            if (action == QLatin1String("remove_field")) {
                const size_t before = members.size();
                std::erase_if(members, [&](const Member& m) { return m.name == fieldName; });
                if (members.size() == before)
                    return ToolResult::ok(QStringLiteral("Field '%1' not found in '%2'").arg(fieldName, name));
                return ToolResult::ok(redefine() ? QStringLiteral("Removed field '%1' from '%2'").arg(fieldName, name)
                                                 : QStringLiteral("Remove failed"));
            }
            if (action == QLatin1String("rename_field")) {
                auto it = std::find_if(members.begin(), members.end(), [&](const Member& m) { return m.name == fieldName; });
                if (it == members.end()) return ToolResult::ok(QStringLiteral("Field '%1' not found").arg(fieldName));
                it->name = newName;
                return ToolResult::ok(redefine() ? QStringLiteral("Renamed '%1' \u2192 '%2'").arg(fieldName, newName)
                                                 : QStringLiteral("Rename failed"));
            }
            if (action == QLatin1String("retype_field")) {
                auto it = std::find_if(members.begin(), members.end(), [&](const Member& m) { return m.name == fieldName; });
                if (it == members.end()) return ToolResult::ok(QStringLiteral("Field '%1' not found").arg(fieldName));
                it->type = fieldType;
                it->size = sizeofType(bv, fieldType, 4);
                return ToolResult::ok(redefine() ? QStringLiteral("Retyped '%1' to '%2'").arg(fieldName, fieldType)
                                                 : QStringLiteral("Retype failed"));
            }
            if (action == QLatin1String("set_field_comment")) {
                // Binary Ninja type members do not expose stable per-field comments in all APIs.
                if (std::any_of(members.begin(), members.end(), [&](const Member& m) { return m.name == fieldName; }))
                    return ToolResult::ok(QStringLiteral("Field comments are not persisted in this Binary Ninja API; no-op"));
                return ToolResult::ok(QStringLiteral("Field '%1' not found").arg(fieldName));
            }
            if (action == QLatin1String("resize")) {
                if (newSize <= 0) return ToolResult::ok(QStringLiteral("New size must be positive"));
                const qint64 current = structSize(members);
                if (newSize < current)
                    return ToolResult::ok(QStringLiteral("Cannot shrink struct (current=%1, requested=%2)").arg(current).arg(newSize));
                if (newSize == current) return ToolResult::ok(QStringLiteral("Struct '%1' already size %2").arg(name).arg(current));
                members.push_back({QStringLiteral("_pad_end_%1").arg(QString::number(current, 16)),
                                   QStringLiteral("char[%1]").arg(newSize - current), current, newSize - current, QString()});
                return ToolResult::ok(redefine() ? QStringLiteral("Resized '%1' from %2 to %3").arg(name).arg(current).arg(newSize)
                                                 : QStringLiteral("Resize failed"));
            }
            return ToolResult::ok(QStringLiteral("Unknown action: %1").arg(action));
        }));

    registry.add(makeTool("get_struct_info", "types", "Get full struct layout: fields, types, offsets, sizes.",
                          {param("name", ToolParam::String, "Struct name")}, 0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const QString name = call.str("name");
                              Ref<Type> t = typeByName(bv, name);
                              if (!isStruct(t)) return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(name));
                              const std::vector<Member> members = structMembers(t);
                              const qint64 size = structSize(members);
                              QStringList lines{QStringLiteral("Struct: %1").arg(name),
                                                QStringLiteral("Size: %1 (%2)").arg(size).arg(hex(uint64_t(size))),
                                                QStringLiteral("Members: %1").arg(members.size()), QString()};
                              for (const Member& m : members)
                                  lines.append(QStringLiteral("  +%1  %2 %3 (%4 bytes)")
                                                   .arg(hexPad(uint64_t(m.offset), 4), ljust(m.type, 24), ljust(m.name, 24))
                                                   .arg(m.size));
                              return ToolResult::ok(lines.join(QLatin1Char('\n')));
                          }));

    registry.add(makeTool("list_structs", "types", "List all structs in the database.",
                          {param("filter", ToolParam::String, "Name filter (substring match)", "")}, 0,
                          [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const QString q = call.str("filter").toLower();
                              QStringList lines{QStringLiteral("Structs:")};
                              int count = 0;
                              for (const auto& [key, entry] : namedTypes(bv)) {
                                  if (!isStruct(entry.second)) continue;
                                  const QString name = qs(key);
                                  if (!q.isEmpty() && !name.toLower().contains(q)) continue;
                                  lines.append(QStringLiteral("  %1 size=%2")
                                                   .arg(ljust(name, 40))
                                                   .arg(structSize(structMembers(entry.second))));
                                  if (++count >= 200) {
                                      lines.append(QStringLiteral("  ... (truncated)"));
                                      break;
                                  }
                              }
                              if (count == 0) lines.append(QStringLiteral("  (none)"));
                              return ToolResult::ok(lines.join(QLatin1Char('\n')));
                          }));

    registry.add(makeTool(
        "create_enum", "types", "Create a new enum with name/value pairs.",
        {param("name", ToolParam::String, "Enum name"),
         param("members", ToolParam::String, "JSON array of members: [{name, value}, ...]"),
         param("bitfield", ToolParam::Boolean, "Create as bitfield enum", false)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString name = call.str("name");
            if (typeByName(bv, name)) return ToolResult::ok(QStringLiteral("Enum '%1' already exists").arg(name));
            QString error;
            const std::optional<QJsonArray> memberList = parseJsonList(call.str("members"), "members", &error);
            if (!memberList) return ToolResult::ok(error);
            std::vector<std::pair<QString, qint64>> members;
            for (qsizetype i = 0; i < memberList->size(); ++i) {
                const QJsonValue v = memberList->at(i);
                if (!v.isObject())
                    throw ToolError(QStringLiteral("'%1' object has no attribute 'get'").arg(pyTypeName(v)));
                const QJsonObject m = v.toObject();
                const QString mname = m.contains(QStringLiteral("name")) ? pyStr(m.value(QStringLiteral("name")))
                                                                         : QStringLiteral("MEMBER_%1").arg(i);
                const qint64 mval = m.contains(QStringLiteral("value")) ? pyInt(m.value(QStringLiteral("value"))) : i;
                members.emplace_back(mname, mval);
            }
            QString decl = enumDecl(name, members);
            if (call.boolean("bitfield")) decl += QStringLiteral("\ntypedef enum %1 %1;").arg(name);
            QStringList parsed;
            try {
                parsed = withUndo(bv, [&] { return defineTypesFromSource(bv, decl); });
            } catch (const ToolError& e) {
                return ToolResult::ok(QStringLiteral("Failed to create enum '%1': %2").arg(name, e.message()));
            }
            return ToolResult::ok(parsed.contains(name)
                                      ? QStringLiteral("Created enum '%1' with %2 members").arg(name).arg(memberList->size())
                                      : QStringLiteral("Failed to create enum '%1'").arg(name));
        }));

    registry.add(makeTool(
        "modify_enum", "types", "Modify an existing enum.",
        {param("name", ToolParam::String, "Enum name"),
         param("action", ToolParam::String, "Action: add_member, remove_member, rename_member"),
         param("member_name", ToolParam::String, "Member name", ""),
         param("new_name", ToolParam::String, "New name (for rename)", ""),
         param("value", ToolParam::Integer, "Value (for add_member)", 0)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString name = call.str("name");
            const QString action = call.str("action");
            const QString memberName = call.str("member_name");
            const QString newName = call.str("new_name");
            const qint64 value = call.integer("value");
            Ref<Type> t = typeByName(bv, name);
            if (!isEnum(t)) return ToolResult::ok(QStringLiteral("Enum '%1' not found").arg(name));
            std::vector<std::pair<QString, qint64>> members = enumMembers(t);
            if (action == QLatin1String("add_member")) {
                members.emplace_back(memberName, value);
            } else if (action == QLatin1String("remove_member")) {
                const size_t before = members.size();
                std::erase_if(members, [&](const auto& m) { return m.first == memberName; });
                if (members.size() == before) return ToolResult::ok(QStringLiteral("Member '%1' not found").arg(memberName));
            } else if (action == QLatin1String("rename_member")) {
                bool changed = false;
                for (auto& m : members)
                    if (m.first == memberName) {
                        m.first = newName;
                        changed = true;
                    }
                if (!changed) return ToolResult::ok(QStringLiteral("Member '%1' not found").arg(memberName));
            } else {
                return ToolResult::ok(QStringLiteral("Unknown action: %1").arg(action));
            }
            try {
                withUndo(bv, [&] { defineTypesFromSource(bv, enumDecl(name, members)); });
            } catch (const ToolError& e) {
                return ToolResult::ok(QStringLiteral("Failed to modify enum '%1': %2").arg(name, e.message()));
            }
            if (action == QLatin1String("add_member"))
                return ToolResult::ok(QStringLiteral("Added '%1' = %2").arg(memberName).arg(value));
            if (action == QLatin1String("remove_member"))
                return ToolResult::ok(QStringLiteral("Removed '%1'").arg(memberName));
            return ToolResult::ok(QStringLiteral("Renamed '%1' \u2192 '%2'").arg(memberName, newName));
        }));

    registry.add(makeTool("get_enum_info", "types", "Get all enum members with values.",
                          {param("name", ToolParam::String, "Enum name")}, 0, [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const QString name = call.str("name");
                              Ref<Type> t = typeByName(bv, name);
                              if (!isEnum(t)) return ToolResult::ok(QStringLiteral("Enum '%1' not found").arg(name));
                              const auto members = enumMembers(t);
                              QStringList lines{QStringLiteral("Enum: %1").arg(name), QString()};
                              if (members.empty()) lines.append(QStringLiteral("  (no members)"));
                              for (const auto& [mn, mv] : members)
                                  lines.append(QStringLiteral("  %1 = %2 (%3)").arg(ljust(mn, 40), signedHex(mv)).arg(mv));
                              return ToolResult::ok(lines.join(QLatin1Char('\n')));
                          }));

    registry.add(makeTool("list_enums", "types", "List all enums in the database.",
                          {param("filter", ToolParam::String, "Name filter (substring match)", "")}, 0,
                          [](ToolCall& call) {
                              Ref<BinaryView> bv = requireView(call);
                              const QString q = call.str("filter").toLower();
                              QStringList lines{QStringLiteral("Enums:")};
                              int count = 0;
                              for (const auto& [key, entry] : namedTypes(bv)) {
                                  if (!isEnum(entry.second)) continue;
                                  const QString name = qs(key);
                                  if (!q.isEmpty() && !name.toLower().contains(q)) continue;
                                  lines.append(QStringLiteral("  %1 (%2 members)")
                                                   .arg(ljust(name, 40))
                                                   .arg(enumMembers(entry.second).size()));
                                  if (++count >= 200) {
                                      lines.append(QStringLiteral("  ... (truncated)"));
                                      break;
                                  }
                              }
                              if (count == 0) lines.append(QStringLiteral("  (none)"));
                              return ToolResult::ok(lines.join(QLatin1Char('\n')));
                          }));

    registry.add(makeTool(
        "create_typedef", "types", "Create a type alias (typedef).",
        {param("name", ToolParam::String, "New type alias name"),
         param("base_type", ToolParam::String, "Base type (e.g. 'unsigned int', 'DWORD')")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString name = call.str("name");
            const QString decl = QStringLiteral("typedef %1 %2;").arg(call.str("base_type"), name);
            QStringList parsed;
            try {
                parsed = withUndo(bv, [&] { return defineTypesFromSource(bv, decl); });
            } catch (const ToolError& e) {
                return ToolResult::ok(QStringLiteral("Failed to create typedef: %1 (%2)").arg(decl, e.message()));
            }
            return ToolResult::ok(parsed.contains(name) ? QStringLiteral("Created typedef: %1").arg(decl)
                                                        : QStringLiteral("Failed to create typedef: %1").arg(decl));
        }));

    registry.add(makeTool(
        "apply_struct_to_address", "types", "Apply a struct type at a data address.",
        {param("struct_name", ToolParam::String, "Struct name to apply"),
         param("address", ToolParam::String, "Data address (hex string)")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString structName = call.str("struct_name");
            const uint64_t ea = call.address("address");
            Ref<Type> t = typeByName(bv, structName);
            if (!isStruct(t)) return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(structName));
            withUndo(bv, [&] { bv->DefineUserDataVariable(ea, Confidence<Ref<Type>>(t)); });
            return ToolResult::ok(QStringLiteral("Applied struct '%1' at %2").arg(structName, hex(ea)));
        }));

    registry.add(makeTool(
        "apply_type_to_variable", "types", "Retype a local variable in a function.",
        {param("func_address", ToolParam::String, "Function address (hex string)"),
         param("var_name", ToolParam::String, "Variable name in the decompiler"),
         param("type_str", ToolParam::String, "C type string to apply")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("func_address");
            const QString varName = call.str("var_name");
            const QString typeStr = call.str("type_str");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            Ref<Type> type;
            try {
                type = parseTypeString(bv, typeStr + QStringLiteral(" __rikugan_var"));
            } catch (const ToolError&) {
                try {
                    type = parseTypeString(bv, typeStr);
                } catch (const ToolError& e) {
                    return ToolResult::ok(QStringLiteral("Failed to parse type: %1").arg(e.message()));
                }
            }
            const std::optional<Variable> target = findVariable(func, varName);
            if (!target)
                return ToolResult::ok(QStringLiteral("Variable '%1' not found in %2").arg(varName, hex(func->GetStart())));
            withUndo(bv, [&] { func->CreateUserVariable(*target, Confidence<Ref<Type>>(type), ss(varName)); });
            return ToolResult::ok(QStringLiteral("Set type of '%1' to '%2'").arg(varName, typeStr));
        }));

    registry.add(makeTool(
        "set_function_prototype", "types", "Set a function's full calling convention and prototype.",
        {param("address", ToolParam::String, "Function address (hex string)"),
         param("prototype", ToolParam::String, "Full C prototype (e.g. 'int __fastcall foo(void* ctx, int len)')")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            const QString prototype = call.str("prototype");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            Ref<Type> type;
            try {
                type = parseTypeString(bv, prototype);
            } catch (const ToolError& e) {
                return ToolResult::ok(
                    QStringLiteral("Failed to set prototype. Check syntax: %1 (%2)").arg(prototype, e.message()));
            }
            withUndo(bv, [&] { func->SetUserType(type); });
            return ToolResult::ok(QStringLiteral("Set prototype at %1: %2").arg(hex(func->GetStart()), prototype));
        }));

    registry.add(makeTool(
        "import_c_header", "types", "Parse C header code and define all structs/enums/typedefs found.",
        {param("c_code", ToolParam::String, "C header code containing struct/enum/typedef definitions")}, kMutating,
        [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString code = call.str("c_code");
            QStringList parsed;
            try {
                parsed = withUndo(bv, [&] { return defineTypesFromSource(bv, code); });
            } catch (const ToolError& e) {
                return ToolResult::ok(QStringLiteral("Failed to parse C declarations. %1").arg(e.message()));
            }
            return ToolResult::ok(
                QStringLiteral("Successfully parsed C declarations (%1 type(s) defined)").arg(parsed.size()));
        }));

    registry.add(makeTool(
        "suggest_struct_from_accesses", "types", "Analyze access patterns and suggest a struct layout.",
        {param("address", ToolParam::String, "Function or pointer address to analyze (hex string)")},
        RequiresDecompiler, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const uint64_t ea = call.address("address");
            Ref<Function> func = functionAt(bv, ea);
            if (!func) return ToolResult::ok(QStringLiteral("No function at %1").arg(hex(ea)));
            const IlFunction hlil = ilOf(func, IlLevel::Hlil);
            if (!hlil) return ToolResult::ok(QStringLiteral("HLIL not available for this function"));
            static const QRegularExpression pattern(QStringLiteral("\\+\\s*0x([0-9a-fA-F]+)"));
            std::map<uint64_t, qint64> offsets;
            for (size_t index : hlil.instructionOrder()) {
                call.throwIfCancelled();
                auto it = pattern.globalMatch(hlil.instructionText(index));
                while (it.hasNext()) ++offsets[it.next().captured(1).toULongLong(nullptr, 16)];
            }
            if (offsets.empty()) return ToolResult::ok(QStringLiteral("No pointer field accesses detected in this function"));
            QStringList lines{QStringLiteral("Suggested struct from access analysis at %1:").arg(hex(func->GetStart())),
                              QString()};
            for (const auto& [off, count] : offsets)
                lines.append(QStringLiteral("  +%1  uint32_t         field_%2;    // accessed %3x")
                                 .arg(hexPad(off, 4), QString::number(off, 16))
                                 .arg(count));
            const uint64_t estSize = offsets.rbegin()->first + 4;
            lines.append(QStringLiteral("\nEstimated struct size: %1 (%2) bytes").arg(estSize).arg(hex(estSize)));
            lines.append(QStringLiteral("\nSuggested C declaration:"));
            lines.append(QStringLiteral("struct auto_struct_%1 {").arg(QString::number(ea, 16)));
            for (const auto& [off, count] : offsets) lines.append(QStringLiteral("    uint32_t field_%1;").arg(QString::number(off, 16)));
            lines.append(QStringLiteral("};"));
            return ToolResult::ok(lines.join(QLatin1Char('\n')));
        }));

    registry.add(makeTool(
        "propagate_type", "types", "Re-run analysis after struct changes to update references.",
        {param("struct_name", ToolParam::String, "Struct name to propagate"),
         param("field_index", ToolParam::Integer, "Specific field index to propagate (-1 for all)", -1)},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString structName = call.str("struct_name");
            if (!isStruct(typeByName(bv, structName)))
                return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(structName));
            withUndo(bv, [&] { updateAnalysisAndWait(bv); });
            return ToolResult::ok(QStringLiteral("Triggered type propagation for '%1'").arg(structName));
        }));

    registry.add(makeTool("get_type_libraries", "types", "List loaded type libraries.", {}, 0, [](ToolCall& call) {
        Ref<BinaryView> bv = requireView(call);
        QStringList lines{QStringLiteral("Type libraries:")};
        int count = 0;
        for (const Ref<TypeLibrary>& lib : typeLibraries(bv)) {
            lines.append(QStringLiteral("  %1").arg(qs(lib->GetName())));
            ++count;
        }
        if (count == 0) lines.append(QStringLiteral("  (none)"));
        return ToolResult::ok(lines.join(QLatin1Char('\n')));
    }));

    registry.add(makeTool(
        "import_type_from_library", "types", "Import a specific type from a loaded type library.",
        {param("til_name", ToolParam::String, "Type library name"),
         param("type_name", ToolParam::String, "Type name to import")},
        kMutating, [](ToolCall& call) {
            Ref<BinaryView> bv = requireView(call);
            const QString tilName = call.str("til_name");
            const QString typeName = call.str("type_name");
            if (typeByName(bv, typeName))
                return ToolResult::ok(QStringLiteral("Type '%1' already exists locally").arg(typeName));
            std::vector<Ref<TypeLibrary>> candidates;
            for (const Ref<TypeLibrary>& lib : typeLibraries(bv))
                if (qs(lib->GetName()) == tilName) candidates.push_back(lib);
            if (candidates.empty()) return ToolResult::ok(QStringLiteral("Type library '%1' not found").arg(tilName));
            const QString imported = QStringLiteral("Imported '%1' from '%2'").arg(typeName, tilName);
            for (const Ref<TypeLibrary>& lib : candidates) {
                if (Ref<Type> t = lib->GetNamedType(QualifiedName(ss(typeName)))) {
                    withUndo(bv, [&] { bv->DefineUserType(QualifiedName(ss(typeName)), t); });
                    return ToolResult::ok(imported);
                }
            }
            QStringList parsed;
            try {
                parsed = withUndo(bv, [&] {
                    return defineTypesFromSource(bv, QStringLiteral("typedef %1 %1;").arg(typeName));
                });
            } catch (const ToolError&) {
            }
            if (parsed.contains(typeName)) return ToolResult::ok(imported);
            return ToolResult::ok(QStringLiteral("Type '%1' not found in '%2'").arg(typeName, tilName));
        }));
}

}  // namespace rk::binja
