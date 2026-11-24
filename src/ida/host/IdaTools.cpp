#include "ida/host/IdaTools.h"

namespace rk::ida {

QString nameAt(ea_t ea) { return qs(get_name(ea)); }

namespace {
ToolParam param(const char* name, ToolParam::Type type, const char* description, QJsonValue def) {
    ToolParam p;
    p.name = QString::fromUtf8(name);
    p.type = type;
    p.description = QString::fromUtf8(description);
    p.def = std::move(def);
    return p;
}
}  // namespace

ToolParam strParam(const char* name, const char* description, QJsonValue def) {
    return param(name, ToolParam::String, description, std::move(def));
}
ToolParam intParam(const char* name, const char* description, QJsonValue def) {
    return param(name, ToolParam::Integer, description, std::move(def));
}
ToolParam boolParam(const char* name, const char* description, QJsonValue def) {
    return param(name, ToolParam::Boolean, description, std::move(def));
}

void addTool(ToolRegistry& reg, const char* name, const char* category, const char* description,
             std::vector<ToolParam> params, uint32_t flags, ToolFn fn) {
    ToolSpec spec;
    spec.name = QString::fromUtf8(name);
    spec.category = QString::fromUtf8(category);
    spec.description = QString::fromUtf8(description);
    spec.params = std::move(params);
    spec.flags = flags | MainThread;
    spec.fn = std::move(fn);
    reg.add(std::move(spec));
}

QString commentAt(ea_t ea, bool repeatable) {
    qstring buf;
    if (get_cmt(&buf, ea, repeatable) < 0) return {};
    return qs(buf);
}

QString mnemonicAt(ea_t ea) {
    qstring buf;
    if (!print_insn_mnem(&buf, ea)) return {};
    return qs(buf);
}

QString operandAt(ea_t ea, int n) {
    qstring buf;
    if (!print_operand(&buf, ea, n) || buf.empty()) return {};
    qstring clean;
    tag_remove(&clean, buf);
    return qs(clean);
}

QString typeAt(ea_t ea) {
    tinfo_t tif;
    if (!get_tinfo(&tif, ea)) return {};
    qstring out;
    if (!tif.print(&out, nullptr, PRTYPE_1LINE)) return {};
    return qs(out);
}

bool setTypeAt(ea_t ea, const QString& decl) {
    if (decl.isEmpty()) {
        del_tinfo(ea);
        return true;
    }
    QByteArray text = decl.toUtf8();
    if (!text.endsWith(';')) text.append(';');
    tinfo_t tif;
    qstring name;
    if (!parse_decl(&tif, &name, nullptr, text.constData(), PT_SIL)) return false;
    return apply_tinfo(ea, tif, TINFO_DEFINITE);
}

int parseDecls(const QString& text) { return parse_decls(nullptr, text.toUtf8().constData(), msg, 0); }

bool parseTypeString(tinfo_t* out, const QString& typeStr) {
    const QByteArray text = typeStr.toUtf8();
    tinfo_t tif;
    if (tif.parse(text.constData())) {
        *out = tif;
        return true;
    }
    QByteArray decl = text;
    if (!typeStr.trimmed().endsWith(QLatin1Char(';'))) decl.append(';');
    if (parse_decl(&tif, nullptr, nullptr, decl.constData(), PT_SIL)) {
        *out = tif;
        return true;
    }
    if (tif.get_named_type(nullptr, text.constData())) {
        *out = tif;
        return true;
    }
    if (parse_decl(&tif, nullptr, nullptr, text.constData(), PT_SIL)) {
        *out = tif;
        return true;
    }
    return false;
}

QString typeString(const tinfo_t& tif) {
    qstring out;
    tif.print(&out, nullptr, PRTYPE_1LINE);
    return qs(out);
}

cfuncptr_t decompileAt(ea_t ea) {
    func_t* pfn = get_func(ea);
    if (pfn == nullptr) return cfuncptr_t(nullptr);
    hexrays_failure_t hf;
    return decompile_func(pfn, &hf, 0);
}

}  // namespace rk::ida
