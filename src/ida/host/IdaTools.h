#pragma once
// Shared pieces of the IDA analysis tools. Every tool runs on the main thread.
#include <memory>
#include <optional>

#include <QByteArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "common/tools/Tool.h"
#include "common/tools/ToolRegistry.h"
#include "ida/host/IdaFormat.h"
#include "ida/host/IdaSdk.h"

namespace rk::ida {

class IdaHost;

inline QString qs(const qstring& s) { return QString::fromUtf8(s.c_str(), qsizetype(s.length())); }
inline QString qs(const char* s) { return s ? QString::fromUtf8(s) : QString(); }

// ida_name.get_name(ea)
QString nameAt(ea_t ea);
// "0x..." (f"0x{ea:x}")
inline QString hx(uint64_t ea) { return rk::hex(ea); }

// ToolParam builders; `def` undefined = required.
ToolParam strParam(const char* name, const char* description, QJsonValue def = QJsonValue(QJsonValue::Undefined));
ToolParam intParam(const char* name, const char* description, QJsonValue def = QJsonValue(QJsonValue::Undefined));
ToolParam boolParam(const char* name, const char* description, QJsonValue def = QJsonValue(QJsonValue::Undefined));

// Adds an IDA tool: MainThread is always set.
void addTool(ToolRegistry& reg, const char* name, const char* category, const char* description,
             std::vector<ToolParam> params, uint32_t flags, ToolFn fn);

// idc.get_cmt(ea, rptble) or ""
QString commentAt(ea_t ea, bool repeatable);
// idc.print_insn_mnem / idc.print_operand (tags removed)
QString mnemonicAt(ea_t ea);
QString operandAt(ea_t ea, int n);
// idc.get_type(ea) or "" when it has none
QString typeAt(ea_t ea);
// idc.SetType(ea, decl)
bool setTypeAt(ea_t ea, const QString& decl);
// idc.parse_decls(text, 0): the number of errors
int parseDecls(const QString& text);
// An empty tinfo when nothing parses
bool parseTypeString(tinfo_t* out, const QString& typeStr);
// str(tinfo_t)
QString typeString(const tinfo_t& tif);
// get_binary_info's text
QString binaryInfoText();

// ida_hexrays.decompile(ea): null when there is no function or decompilation failed
cfuncptr_t decompileAt(ea_t ea);

// One register function per tool group.
void registerNavigationTools(ToolRegistry& reg, IdaHost& host);
void registerFunctionTools(ToolRegistry& reg, IdaHost& host);
void registerStringTools(ToolRegistry& reg, IdaHost& host);
void registerDatabaseTools(ToolRegistry& reg, IdaHost& host);
void registerDisassemblyTools(ToolRegistry& reg, IdaHost& host);
void registerDecompilerTools(ToolRegistry& reg, IdaHost& host);
void registerXrefTools(ToolRegistry& reg, IdaHost& host);
void registerAnnotationTools(ToolRegistry& reg, IdaHost& host);
void registerTypeTools(ToolRegistry& reg, IdaHost& host);
void registerScriptingTools(ToolRegistry& reg, IdaHost& host);
void registerMicrocodeTools(ToolRegistry& reg, IdaHost& host);

}  // namespace rk::ida
