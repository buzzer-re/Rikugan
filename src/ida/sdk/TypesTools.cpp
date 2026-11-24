// IDA type tools, IDA 9 paths (ida_struct and ida_enum are gone; offsets of the UDT API are in
// bits). Subclasses ctree_visitor_t and user_lvar_modifier_t: built without RTTI.
#include <map>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"

namespace rk::ida {

namespace {

using u128 = unsigned __int128;

QString u128Str(u128 v) {
    if (v == 0) return QStringLiteral("0");
    QString s;
    while (v) {
        s.prepend(QChar(char16_t(u'0' + int(v % 10))));
        v /= 10;
    }
    return s;
}

QString u128Hex(u128 v) {
    if (v == 0) return QStringLiteral("0");
    QString s;
    while (v) {
        s.prepend(QLatin1Char("0123456789abcdef"[int(v & 15)]));
        v >>= 4;
    }
    return s;
}

QByteArray u8(const QString& s) { return s.toUtf8(); }

// json.loads of a JSON array
QJsonArray parseJsonArray(const QString& text) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        throw ToolError(err.errorString() + QStringLiteral(" (char %1)").arg(err.offset));
    if (!doc.isArray()) throw ToolError(QStringLiteral("expected a JSON array"));
    return doc.array();
}

// fld["key"] on a dict: KeyError('key') otherwise
QJsonValue item(const QJsonValue& v, const char* key) {
    if (!v.isObject()) throw ToolError(QStringLiteral("string indices must be integers, not 'str'"));
    const QJsonObject o = v.toObject();
    if (!o.contains(QLatin1String(key))) throw ToolError(QStringLiteral("'%1'").arg(QLatin1String(key)));
    return o.value(QLatin1String(key));
}

QString jsonText(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
    if (v.isBool()) return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    return {};
}

// tinfo_t.add_udm raises ValueError("Invalid input data: ...") on any error code
tinfo_code_t addUdmOrThrow(tinfo_t& tif, const udm_t& udm) {
    const tinfo_code_t err = tif.add_udm(udm);
    if (err != TERR_OK) throw ToolError(QStringLiteral("Invalid input data: ") + qs(tinfo_errstr(err)));
    return err;
}

bool structTinfo(const QString& name, tinfo_t* out) {
    tinfo_t tif;
    if (tif.get_named_type(nullptr, u8(name).constData()) && tif.is_udt()) {
        *out = tif;
        return true;
    }
    return false;
}

int findUdm(const tinfo_t& tif, const QString& name) { return tif.find_udm(u8(name).constData(), STRMEM_NAME); }

QString createStruct(const QString& name, const QJsonArray& fieldList) {
    tinfo_t existing;
    if (existing.get_named_type(nullptr, u8(name).constData()) && existing.is_udt())
        return QStringLiteral("Struct '%1' already exists").arg(name);
    tinfo_t tif;
    if (!tif.create_udt(false)) return QStringLiteral("Failed to create empty UDT for struct '%1'").arg(name);

    for (const QJsonValue& fld : fieldList) {
        const QByteArray fname = u8(jsonText(item(fld, "name")));
        const QJsonObject o = fld.toObject();
        const QString ftype = o.contains(QStringLiteral("type")) ? jsonText(o.value(QStringLiteral("type"))) : QStringLiteral("int");
        const QJsonValue off = o.value(QStringLiteral("offset"));
        const uint64 offsetBits = off.isUndefined() || off.isNull() ? 0 : uint64(qint64(off.toDouble()) * 8);

        tinfo_code_t err = tif.add_udm(fname.constData(), u8(ftype).constData(), offsetBits);
        if (err != TERR_OK) {
            tinfo_t ftif;
            if (!parseTypeString(&ftif, ftype)) parse_decl(&ftif, nullptr, nullptr, "int;", PT_SIL);
            err = addUdmOrThrow(tif, udm_t(fname.constData(), ftif, offsetBits));
        }
        const QString comment = jsonText(o.value(QStringLiteral("comment")));
        if (!comment.isEmpty() && err == TERR_OK) {
            const int idx = tif.find_udm(fname.constData(), STRMEM_NAME);
            if (idx >= 0) tif.set_udm_cmt(size_t(idx), u8(comment).constData());
        }
    }

    const tinfo_code_t err = tif.set_named_type(nullptr, u8(name).constData());
    if (err != TERR_OK) return QStringLiteral("Failed to register struct '%1' (err=%2)").arg(name, QString::number(int(err)));
    return QStringLiteral("Created struct '%1' with %2 fields, size=%3")
        .arg(name, QString::number(fieldList.size()), QString::number(uint64(tif.get_size())));
}

QString modifyStruct(const QString& name, const QString& action, const QString& fieldName, const QString& newName,
                     const QString& fieldType, qint64 offset, const QString& comment, qint64 newSize) {
    tinfo_t tif;
    if (!structTinfo(name, &tif)) return QStringLiteral("Struct '%1' not found").arg(name);

    if (action == QLatin1String("add_field")) {
        const uint64 offsetBits = offset >= 0 ? uint64(offset) * 8 : 0;
        tinfo_code_t err = tif.add_udm(u8(fieldName).constData(), u8(fieldType).constData(), offsetBits);
        if (err != TERR_OK) {
            tinfo_t ftif;
            if (!parseTypeString(&ftif, fieldType)) return QStringLiteral("Failed to parse type '%1'").arg(fieldType);
            err = addUdmOrThrow(tif, udm_t(u8(fieldName).constData(), ftif, offsetBits));
        }
        return QStringLiteral("Added field '%1' (%2) to '%3'").arg(fieldName, fieldType, name);
    }
    if (action == QLatin1String("remove_field")) {
        const int idx = findUdm(tif, fieldName);
        if (idx == -1) return QStringLiteral("Field '%1' not found in '%2'").arg(fieldName, name);
        const tinfo_code_t err = tif.del_udm(size_t(idx));
        if (err == TERR_OK) return QStringLiteral("Removed field '%1' from '%2'").arg(fieldName, name);
        return QStringLiteral("Failed to remove field (error code %1)").arg(int(err));
    }
    if (action == QLatin1String("rename_field")) {
        const int idx = findUdm(tif, fieldName);
        if (idx == -1) return QStringLiteral("Field '%1' not found").arg(fieldName);
        const tinfo_code_t err = tif.rename_udm(size_t(idx), u8(newName).constData());
        if (err == TERR_OK) return QStringLiteral("Renamed '%1' \u2192 '%2'").arg(fieldName, newName);
        return QStringLiteral("Rename failed (error code %1)").arg(int(err));
    }
    if (action == QLatin1String("retype_field")) {
        const int idx = findUdm(tif, fieldName);
        if (idx == -1) return QStringLiteral("Field '%1' not found").arg(fieldName);
        tinfo_t ftif;
        if (!parseTypeString(&ftif, fieldType)) return QStringLiteral("Failed to parse type '%1'").arg(fieldType);
        const tinfo_code_t err = tif.set_udm_type(size_t(idx), ftif);
        if (err == TERR_OK) return QStringLiteral("Retyped '%1' to '%2'").arg(fieldName, fieldType);
        return QStringLiteral("Retype failed (error code %1)").arg(int(err));
    }
    if (action == QLatin1String("set_field_comment")) {
        const int idx = findUdm(tif, fieldName);
        if (idx == -1) return QStringLiteral("Field '%1' not found").arg(fieldName);
        tif.set_udm_cmt(size_t(idx), u8(comment).constData());
        return QStringLiteral("Set comment on '%1'").arg(fieldName);
    }
    if (action == QLatin1String("resize")) {
        if (newSize <= 0) return QStringLiteral("New size must be positive");
        const uint64 current = tif.get_size();
        if (uint64(newSize) <= current)
            return QStringLiteral("Cannot shrink struct (current=%1, requested=%2)").arg(current).arg(newSize);
        const uint64 pad = uint64(newSize) - current;
        tinfo_t padTif;
        const QByteArray decl = pad == 1 ? QByteArray("char;") : QByteArray("char[") + QByteArray::number(pad) + "];";
        parse_decl(&padTif, nullptr, nullptr, decl.constData(), PT_SIL);
        udm_t udm;
        udm.name = u8(QStringLiteral("_padding_") + QString::number(current, 16)).constData();
        udm.type = padTif;
        addUdmOrThrow(tif, udm);
        return QStringLiteral("Resized '%1' from %2 to %3").arg(name, QString::number(current), QString::number(newSize));
    }
    return QStringLiteral("Unknown action: %1").arg(action);
}

QString memberRow(const udm_t& udm) {
    const QString cmt = qs(udm.cmt);
    return QStringLiteral("  +") + hexPad(udm.offset / 8, 4) + QStringLiteral("  ") + ljust(typeString(udm.type), 24) +
           QLatin1Char(' ') + ljust(qs(udm.name), 24) + QStringLiteral(" (%1 bytes)").arg(udm.size / 8) +
           (cmt.isEmpty() ? QString() : QStringLiteral("  ; ") + cmt);
}

QString structInfo(const QString& name) {
    tinfo_t tif;
    if (!structTinfo(name, &tif)) return QStringLiteral("Struct '%1' not found").arg(name);
    const uint64 size = tif.get_size();
    QStringList lines{QStringLiteral("Struct: ") + name,
                      QStringLiteral("Size: %1 (0x%2)").arg(size).arg(QString::number(size, 16)),
                      QStringLiteral("Members: %1").arg(tif.get_udt_nmembers()), QString()};
    udt_type_data_t udt;
    if (!tif.get_udt_details(&udt)) return QStringLiteral("Struct '%1' found but could not read UDT details").arg(name);
    for (const udm_t& udm : udt) {
        if (udm.is_gap()) continue;
        lines << memberRow(udm);
    }
    return lines.join(QLatin1Char('\n'));
}

QString listNamedTypes(const QString& filter, bool enums) {
    QStringList lines{enums ? QStringLiteral("Enums:") : QStringLiteral("Structs:")};
    int count = 0;
    const uint32 limit = get_ordinal_limit();
    const QString needle = filter.toLower();
    for (uint32 ordinal = 1; ordinal < limit; ++ordinal) {
        tinfo_t tif;
        if (!tif.get_numbered_type(nullptr, ordinal)) continue;
        if (enums ? !tif.is_enum() : !tif.is_udt()) continue;
        qstring tname;
        tif.get_type_name(&tname);
        const QString sname = qs(tname);
        if (sname.isEmpty()) continue;
        if (!filter.isEmpty() && !sname.toLower().contains(needle)) continue;
        if (enums)
            lines << QStringLiteral("  ") + ljust(sname, 40);
        else
            lines << QStringLiteral("  ") + ljust(sname, 40) + QStringLiteral(" size=%1  (%2)").arg(uint64(tif.get_size())).arg(tif.is_union() ? QStringLiteral("union") : QStringLiteral("struct"));
        if (++count >= 200) {
            lines << QStringLiteral("  ... (truncated)");
            break;
        }
    }
    if (count == 0) lines << QStringLiteral("  (none)");
    return lines.join(QLatin1Char('\n'));
}

// idc.get_enum
tid_t enumId(const QString& name) {
    tinfo_t tif;
    if (tif.get_named_type(nullptr, u8(name).constData()) && tif.is_enum()) return tif.get_tid();
    return BADADDR;
}

// idc.add_enum(BADADDR, name, 0)
tid_t addEnum(const QString& name) {
    enum_type_data_t ei;
    const int radix = is_char0(0) ? 1 : get_radix(0, 0);
    ei.set_enum_radix(radix, false);
    tinfo_t tif;
    if (tif.create_enum(ei) && tif.set_named_type(nullptr, u8(name).constData()) == TERR_OK) return tif.get_tid();
    return BADADDR;
}

// idc.add_enum_member: 0 or an error code; tinfo_t.add_edm raises on failure
int addEnumMember(tid_t eid, const QString& name, qint64 value, qint64 bmask) {
    tinfo_t tif;
    if (!tif.get_type_by_tid(eid)) return TERR_BAD_TYPE;
    edm_t edm;
    edm.name = u8(name).constData();
    edm.value = uint64(value);
    const tinfo_code_t err = tif.add_edm(edm, bmask64_t(bmask));
    if (err != TERR_OK) throw ToolError(QStringLiteral("Invalid input data: ") + qs(tinfo_errstr(err)));
    return 0;
}

struct OffsetCollector : public ctree_visitor_t {
    OffsetCollector() : ctree_visitor_t(CV_FAST) {}
    int idaapi visit_expr(cexpr_t* expr) override {
        if (expr->op == cot_memref || expr->op == cot_memptr) {
            const uint32 off = expr->m;
            uint64 size = uint64(int64(expr->ptrsize));
            if (expr->ptrsize == 0) size = uint64(expr->type.get_size());
            auto it = accesses.find(off);
            if (it != accesses.end()) it->second = {std::max(it->second.first, size), it->second.second + 1};
            else accesses[off] = {size, 1};
        }
        return 0;
    }
    std::map<uint32, std::pair<uint64, qint64>> accesses;  // offset -> (size, count)
};

struct TypeSetter : public user_lvar_modifier_t {
    TypeSetter(const lvar_t& v, const tinfo_t& t) : lv(v), tif(t) {}
    bool idaapi modify_lvars(lvar_uservec_t* lvinf) override {
        lvar_saved_info_t* info = lvinf->find_info(lv);
        if (info == nullptr) {
            lvinf->keep_info(lv);
            info = lvinf->find_info(lv);
        }
        if (info == nullptr) return false;
        info->type = tif;
        info->size = ssize_t(tif.get_size());
        return true;
    }
    const lvar_t& lv;
    const tinfo_t& tif;
};

QString suggestStruct(cfunc_t* cfunc, ea_t ea) {
    OffsetCollector collector;
    collector.apply_to(&cfunc->body, nullptr);
    if (collector.accesses.empty()) return QStringLiteral("No pointer field accesses detected in this function");

    static const std::map<uint64, QString> sizeType{{1, "uint8_t"}, {2, "uint16_t"}, {4, "uint32_t"}, {8, "uint64_t"}};
    QStringList lines{QStringLiteral("Suggested struct from access analysis at ") + hx(ea) + QLatin1Char(':'), QString()};
    u128 total = 0;
    for (const auto& [off, sc] : collector.accesses) {
        const auto [size, count] = sc;
        const auto it = sizeType.find(size);
        const QString tname = it != sizeType.end() ? it->second : QStringLiteral("char[%1]").arg(size);
        lines << QStringLiteral("  +") + hexPad(off, 4) + QStringLiteral("  ") + ljust(tname, 16) +
                     QStringLiteral(" field_%1;    // accessed %2x").arg(QString::number(off, 16)).arg(count);
        total = std::max(total, u128(off) + size);
    }
    lines << QStringLiteral("\nEstimated struct size: ") + u128Str(total) + QStringLiteral(" (0x") + u128Hex(total) +
                 QStringLiteral(") bytes");
    lines << QStringLiteral("\nSuggested C declaration:");
    lines << QStringLiteral("struct auto_struct_%1 {").arg(QString::number(ea, 16));
    u128 prevEnd = 0;
    for (const auto& [off, sc] : collector.accesses) {
        const uint64 size = sc.first;
        if (u128(off) > prevEnd)
            lines << QStringLiteral("    char _pad_") + u128Hex(prevEnd) + QLatin1Char('[') + u128Str(u128(off) - prevEnd) +
                         QStringLiteral("];");
        const auto it = sizeType.find(size);
        if (size > 8 && it == sizeType.end())
            lines << QStringLiteral("    char field_%1[%2];").arg(QString::number(off, 16)).arg(size);
        else
            lines << QStringLiteral("    ") + (it != sizeType.end() ? it->second : QStringLiteral("uint32_t")) +
                         QStringLiteral(" field_%1;").arg(QString::number(off, 16));
        prevEnd = u128(off) + size;
    }
    lines << QStringLiteral("};");
    return lines.join(QLatin1Char('\n'));
}

QString applyTypeToVariable(cfunc_t* cfunc, const QString& varName, const QString& typeStr, const tinfo_t& tif, ea_t ea) {
    const QByteArray wanted = u8(varName);
    for (lvar_t& lv : *cfunc->get_lvars()) {
        if (lv.name != wanted.constData()) continue;
        const QString done = QStringLiteral("Set type of '%1' to '%2'").arg(varName, typeStr);
        lvar_saved_info_t lsi;
        lsi.ll = lv;
        lsi.type = tif;
        lsi.size = ssize_t(tif.get_size());
        lsi.name = lv.name;
        if (modify_user_lvar_info(cfunc->entry_ea, MLI_TYPE, lsi)) return done;
        TypeSetter setter(lv, tif);
        if (modify_user_lvars(cfunc->entry_ea, setter)) return done;
        if (lv.set_lvar_type(tif)) {
            lv.set_user_type();
            return done + QStringLiteral(" (in-memory, may not persist)");
        }
        return QStringLiteral("Failed to retype '%1' \u2014 try right-click \u2192 Set lvar type in the decompiler").arg(varName);
    }
    return QStringLiteral("Variable '%1' not found in ").arg(varName) + hx(ea);
}

}  // namespace

void registerTypeTools(ToolRegistry& reg, IdaHost& host) {
    addTool(reg, "create_struct", "types", "Create a new struct with typed fields.",
            {strParam("name", "Struct name"), strParam("fields", "JSON array of fields: [{name, type, offset?, comment?}, ...]")},
            Mutating, [](ToolCall& call) {
                const QJsonArray fields = parseJsonArray(call.str("fields"));
                return ToolResult::ok(createStruct(call.str("name"), fields));
            });

    addTool(reg, "modify_struct", "types", "Modify an existing struct: add/remove/rename/retype fields.",
            {strParam("name", "Struct name"),
             strParam("action", "Action: add_field, remove_field, rename_field, retype_field, set_field_comment, resize"),
             strParam("field_name", "Field name to modify", ""), strParam("new_name", "New name (for rename_field)", ""),
             strParam("field_type", "Type string (for add_field/retype_field)", "int"),
             intParam("offset", "Offset (for add_field)", -1), strParam("comment", "Comment text (for set_field_comment)", ""),
             intParam("new_size", "New struct size (for resize)", 0)},
            Mutating, [](ToolCall& call) {
                return ToolResult::ok(modifyStruct(call.str("name"), call.str("action"), call.str("field_name"), call.str("new_name"),
                                                   call.str("field_type"), call.integer("offset"), call.str("comment"),
                                                   call.integer("new_size")));
            });

    addTool(reg, "get_struct_info", "types", "Get full struct layout: fields, types, offsets, sizes.",
            {strParam("name", "Struct name")}, 0, [](ToolCall& call) { return ToolResult::ok(structInfo(call.str("name"))); });

    addTool(reg, "list_structs", "types", "List all structs in the IDB.", {strParam("filter", "Name filter (substring match)", "")},
            0, [](ToolCall& call) { return ToolResult::ok(listNamedTypes(call.str("filter"), false)); });

    addTool(reg, "create_enum", "types", "Create a new enum with name/value pairs.",
            {strParam("name", "Enum name"), strParam("members", "JSON array of members: [{name, value}, ...]"),
             boolParam("bitfield", "Create as bitfield enum", false)},
            Mutating, [](ToolCall& call) {
                const QString name = call.str("name");
                const QJsonArray members = parseJsonArray(call.str("members"));
                const bool bitfield = call.boolean("bitfield");
                if (enumId(name) != BADADDR) return ToolResult::ok(QStringLiteral("Enum '%1' already exists").arg(name));
                const tid_t eid = addEnum(name);
                if (eid == BADADDR) return ToolResult::ok(QStringLiteral("Failed to create enum '%1'").arg(name));
                for (const QJsonValue& m : members) {
                    const QString mname = jsonText(item(m, "name"));
                    const qint64 mval = qint64(item(m, "value").toDouble());
                    addEnumMember(eid, mname, mval, bitfield ? mval : -1);
                }
                return ToolResult::ok(QStringLiteral("Created enum '%1' with %2 members").arg(name, QString::number(members.size())));
            });

    addTool(reg, "modify_enum", "types", "Modify an existing enum.",
            {strParam("name", "Enum name"), strParam("action", "Action: add_member, remove_member, rename_member"),
             strParam("member_name", "Member name", ""), strParam("new_name", "New name (for rename)", ""),
             intParam("value", "Value (for add_member)", 0)},
            Mutating, [](ToolCall& call) {
                const QString name = call.str("name"), action = call.str("action");
                const tid_t eid = enumId(name);
                if (eid == BADADDR) return ToolResult::ok(QStringLiteral("Enum '%1' not found").arg(name));
                if (action == QLatin1String("add_member")) {
                    const QString member = call.str("member_name");
                    const qint64 value = call.integer("value");
                    const int err = addEnumMember(eid, member, value, -1);
                    return ToolResult::ok(err == 0 ? QStringLiteral("Added '%1' = %2").arg(member, QString::number(value))
                                                   : QStringLiteral("Failed (error %1)").arg(err));
                }
                if (action == QLatin1String("remove_member"))
                    return ToolResult::ok(QStringLiteral("remove_member requires ida_enum (not available in IDA 9.x via idc)"));
                if (action == QLatin1String("rename_member"))
                    return ToolResult::ok(QStringLiteral("rename_member via idc not fully supported \u2014 use execute_python"));
                return ToolResult::ok(QStringLiteral("Unknown action: %1").arg(action));
            });

    addTool(reg, "get_enum_info", "types", "Get all enum members with values.", {strParam("name", "Enum name")}, 0,
            [](ToolCall& call) {
                const QString name = call.str("name");
                tinfo_t tif;
                if (!tif.get_named_type(nullptr, u8(name).constData()) || !tif.is_enum())
                    return ToolResult::ok(QStringLiteral("Enum '%1' not found").arg(name));
                QStringList lines{QStringLiteral("Enum: ") + name, QString()};
                enum_type_data_t edt;
                if (!tif.get_enum_details(&edt)) {
                    lines << QStringLiteral("  (iteration not supported: Type is not a structure)");
                } else {
                    for (const edm_t& edm : edt)
                        lines << QStringLiteral("  ") + ljust(qs(edm.name), 40) +
                                     QStringLiteral(" = 0x%1 (%2)").arg(QString::number(edm.value, 16)).arg(edm.value);
                }
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "list_enums", "types", "List all enums in the IDB.", {strParam("filter", "Name filter (substring match)", "")}, 0,
            [](ToolCall& call) { return ToolResult::ok(listNamedTypes(call.str("filter"), true)); });

    addTool(reg, "create_typedef", "types", "Create a type alias (typedef).",
            {strParam("name", "New type alias name"), strParam("base_type", "Base type (e.g. 'unsigned int', 'DWORD')")}, Mutating,
            [](ToolCall& call) {
                const QString decl = QStringLiteral("typedef ") + call.str("base_type") + QLatin1Char(' ') + call.str("name") +
                                     QLatin1Char(';');
                const int result = parseDecls(decl);
                if (result >= 0) return ToolResult::ok(QStringLiteral("Created typedef: ") + decl);
                return ToolResult::ok(QStringLiteral("Failed to create typedef: ") + decl);
            });

    addTool(reg, "apply_struct_to_address", "types", "Apply a struct type at a data address.",
            {strParam("struct_name", "Struct name to apply"), strParam("address", "Data address (hex string)")}, Mutating,
            [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString structName = call.str("struct_name");
                tinfo_t tif;
                if (!structTinfo(structName, &tif)) return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(structName));
                const uint64 size = tif.get_size();
                if (apply_tinfo(ea, tif, TINFO_DEFINITE))
                    return ToolResult::ok(QStringLiteral("Applied struct '%1' at ").arg(structName) + hx(ea) +
                                          QStringLiteral(" (%1 bytes)").arg(size));
                return ToolResult::ok(QStringLiteral("Failed to apply struct at ") + hx(ea));
            });

    addTool(reg, "apply_type_to_variable", "types", "Retype a local variable in a decompiled function.",
            {strParam("func_address", "Function address (hex string)"), strParam("var_name", "Variable name in the decompiler"),
             strParam("type_str", "C type string to apply")},
            Mutating, [&host](ToolCall& call) {
                if (!host.hasDecompiler()) return ToolResult::ok(QStringLiteral("Hex-Rays not available"));
                const ea_t ea = call.address("func_address");
                cfuncptr_t cfunc = decompileAt(ea);
                const QString typeStr = call.str("type_str");
                tinfo_t tif;
                if (!parseTypeString(&tif, typeStr)) return ToolResult::ok(QStringLiteral("Failed to parse type: ") + typeStr);
                if (cfunc == nullptr) throw ToolError(QStringLiteral("'NoneType' object has no attribute 'get_lvars'"));
                return ToolResult::ok(applyTypeToVariable(cfunc, call.str("var_name"), typeStr, tif, ea));
            });

    addTool(reg, "set_function_prototype", "types", "Set a function's full calling convention and prototype.",
            {strParam("address", "Function address (hex string)"),
             strParam("prototype", "Full C prototype (e.g. 'int __fastcall foo(void* ctx, int len)')")},
            Mutating, [](ToolCall& call) {
                const ea_t ea = call.address("address");
                const QString prototype = call.str("prototype");
                if (setTypeAt(ea, prototype)) return ToolResult::ok(QStringLiteral("Set prototype at ") + hx(ea) + QStringLiteral(": ") + prototype);
                return ToolResult::ok(QStringLiteral("Failed to set prototype. Check syntax: ") + prototype);
            });

    addTool(reg, "import_c_header", "types", "Parse C header code and define all structs/enums/typedefs found.",
            {strParam("c_code", "C header code containing struct/enum/typedef definitions")}, Mutating, [](ToolCall& call) {
                const int result = parseDecls(call.str("c_code"));
                if (result >= 0)
                    return ToolResult::ok(QStringLiteral("Successfully parsed C declarations (%1 type(s) defined)").arg(result));
                return ToolResult::ok(QStringLiteral("Failed to parse C declarations. Check syntax."));
            });

    addTool(reg, "suggest_struct_from_accesses", "types", "Analyze field access patterns and suggest a struct layout.",
            {strParam("address", "Function or pointer address to analyze (hex string)")}, RequiresDecompiler,
            [&host](ToolCall& call) {
                if (!host.hasDecompiler()) return ToolResult::ok(QStringLiteral("Hex-Rays not available"));
                const ea_t ea = call.address("address");
                cfuncptr_t cfunc = decompileAt(ea);
                if (cfunc == nullptr) throw ToolError(QStringLiteral("'NoneType' object has no attribute 'body'"));
                return ToolResult::ok(suggestStruct(cfunc, ea));
            });

    addTool(reg, "propagate_type", "types", "Re-run type propagation after struct changes to update xrefs and decompilation.",
            {strParam("struct_name", "Struct name to propagate"),
             intParam("field_index", "Specific field index to propagate (-1 for all)", -1)},
            Mutating, [](ToolCall& call) {
                const QString structName = call.str("struct_name");
                tinfo_t tif;
                if (!structTinfo(structName, &tif)) return ToolResult::ok(QStringLiteral("Struct '%1' not found").arg(structName));
                tif.set_named_type(nullptr, u8(structName).constData(), NTF_REPLACE);
                auto_wait();
                return ToolResult::ok(QStringLiteral("Triggered type propagation for '%1'").arg(structName));
            });

    addTool(reg, "get_type_libraries", "types", "List loaded type libraries (TILs).", {}, 0, [](ToolCall&) {
        const til_t* ti = get_idati();
        if (ti == nullptr) return ToolResult::ok(QStringLiteral("Type libraries: (unavailable)"));
        QStringList lines{QStringLiteral("Type libraries:")};
        for (int i = 0; i < ti->nbases; ++i) {
            const til_t* til = ti->base[i];
            if (til == nullptr) continue;
            const QString name = til->name && *til->name ? qs(til->name) : QStringLiteral("(unnamed)");
            const QString desc = qs(til->desc);
            lines << QStringLiteral("  ") + name + (desc.isEmpty() ? QString() : QStringLiteral(" - ") + desc);
        }
        return ToolResult::ok(lines.join(QLatin1Char('\n')));
    });

    addTool(reg, "import_type_from_library", "types", "Import a specific type from a loaded TIL.",
            {strParam("til_name", "Type library name"), strParam("type_name", "Type name to import")}, Mutating,
            [](ToolCall& call) {
                const QString tilName = call.str("til_name"), typeName = call.str("type_name");
                tinfo_t tif;
                if (tif.get_named_type(nullptr, u8(typeName).constData()))
                    return ToolResult::ok(QStringLiteral("Type '%1' already exists locally").arg(typeName));
                qstring err;
                til_t* til = load_til(u8(tilName).constData(), &err);
                if (til == nullptr) return ToolResult::ok(QStringLiteral("Type library '%1' not found").arg(tilName));
                QString result = QStringLiteral("Type '%1' not found in '%2'").arg(typeName, tilName);
                if (tif.get_named_type(til, u8(typeName).constData())) {
                    const QString decl = typeString(tif);
                    if (parseDecls(QStringLiteral("typedef ") + decl + QLatin1Char(' ') + typeName + QLatin1Char(';')) >= 0)
                        result = QStringLiteral("Imported '%1' from '%2'").arg(typeName, tilName);
                }
                free_til(til);
                return ToolResult::ok(result);
            });
}

}  // namespace rk::ida
