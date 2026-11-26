#pragma once
// Binary Ninja analysis tools and the helpers they share (functions, symbols, disassembly, comments, types,
// pagination).
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <binaryninjaapi.h>
#include <highlevelilinstruction.h>
#include <lowlevelilinstruction.h>
#include <mediumlevelilinstruction.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "common/tools/Tool.h"
#include "common/tools/ToolRegistry.h"

namespace rk::binja {

class BnHost;

using BinaryNinja::Ref;

// ---- registration (one per tool group) ----
void registerAllTools(ToolRegistry& registry);
void registerNavigationTools(ToolRegistry& registry);
void registerFunctionTools(ToolRegistry& registry);
void registerStringTools(ToolRegistry& registry);
void registerDatabaseTools(ToolRegistry& registry);
void registerDisassemblyTools(ToolRegistry& registry);
void registerDecompilerTools(ToolRegistry& registry);
void registerXrefTools(ToolRegistry& registry);
void registerAnnotationTools(ToolRegistry& registry);
void registerTypeTools(ToolRegistry& registry);
void registerScriptingTools(ToolRegistry& registry);
void registerIlTools(ToolRegistry& registry);
void registerIlAnalysisTools(ToolRegistry& registry);
void registerIlTransformTools(ToolRegistry& registry);

// get_binary_info's text (also Host::binaryInfo).
QString binaryInfoText(BinaryNinja::BinaryView* bv);

// ---- tool specs ----
ToolParam param(const char* name, ToolParam::Type type, const char* description,
                 QJsonValue def = QJsonValue(QJsonValue::Undefined));
ToolSpec makeTool(const char* name, const char* category, const char* description, std::vector<ToolParam> params,
                  uint32_t flags, ToolFn fn);

BnHost& bnHost(const ToolCall& call);
// compat.require_bv
Ref<BinaryNinja::BinaryView> requireView(const ToolCall& call);

// Mutating tools: the change as one Binary Ninja undo entry.
template <class F>
auto withUndo(BinaryNinja::BinaryView* bv, F&& f) -> decltype(f()) {
    const std::string id = bv->BeginUndoActions();
    try {
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            bv->CommitUndoActions(id);
        } else {
            auto result = f();
            bv->CommitUndoActions(id);
            return result;
        }
    } catch (...) {
        bv->CommitUndoActions(id);
        throw;
    }
}

// ---- strings and Python formatting ----
inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline std::string ss(const QString& s) { return s.toStdString(); }
// f"{s:Ns}"
QString ljust(const QString& s, int width);
// f"{n:Nd}"
QString rjust(qint64 n, int width);
// f"0x{v:0Nx}"
QString hexPad(uint64_t v, int width);
// f"0x{v:x}" for a signed Python int ("-0x5")
QString signedHex(int64_t v);
// Python str[:n]
QString pyLeft(const QString& s, qsizetype n);

// ---- pagination ----
std::pair<qint64, qint64> normalizePage(qint64 offset, qint64 limit);
QString formatPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title,
                   const QString& empty = QStringLiteral("  (none)"), const QString& nextHint = {});
// format_page for a tool whose hint is "Call <tool> with offset=<next> limit=<limit>."
QString formatToolPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title, const char* tool,
                       const QString& empty = QStringLiteral("  (none)"));

// ---- function helpers ----
Ref<BinaryNinja::Function> functionAt(BinaryNinja::BinaryView* bv, uint64_t ea);
QString functionName(BinaryNinja::Function* func);
uint64_t functionEnd(BinaryNinja::Function* func);
std::vector<Ref<BinaryNinja::Function>> sortedFunctions(BinaryNinja::BinaryView* bv);
std::vector<uint64_t> functionInstructionAddresses(BinaryNinja::BinaryView* bv, BinaryNinja::Function* func,
                                                   size_t maxInstructions = 100000);

// Pseudocode of the function at `ea`; line splitting with Python str.splitlines() semantics
QString pseudocode(BinaryNinja::BinaryView* bv, uint64_t ea, bool withLineNumbers);
QStringList pySplitLines(const QString& text);
// func.callers / func.callees names
std::set<std::string> callerNames(BinaryNinja::BinaryView* bv, BinaryNinja::Function* func);
std::set<std::string> calleeNames(BinaryNinja::BinaryView* bv, BinaryNinja::Function* func);

// ---- symbol helpers ----
Ref<BinaryNinja::Symbol> symbolAt(BinaryNinja::BinaryView* bv, uint64_t ea);
QString symbolName(BinaryNinja::Symbol* sym);
QString resolveNameAt(BinaryNinja::BinaryView* bv, uint64_t ea);
std::vector<Ref<BinaryNinja::Symbol>> symbolsByName(BinaryNinja::BinaryView* bv, const QString& name);
bool renameSymbolAt(BinaryNinja::BinaryView* bv, uint64_t ea, const QString& newName);

// ---- disassembly, comments, reads ----
QString disassemblyLine(BinaryNinja::BinaryView* bv, uint64_t ea);
size_t instructionLength(BinaryNinja::BinaryView* bv, uint64_t ea);
QByteArray readSafe(BinaryNinja::BinaryView* bv, uint64_t ea, size_t size);
QByteArray readRaw(BinaryNinja::BinaryView* bv, uint64_t ea, size_t size);
QString commentAt(BinaryNinja::BinaryView* bv, uint64_t ea);
void updateAnalysisAndWait(BinaryNinja::BinaryView* bv, BinaryNinja::Function* func = nullptr);
QString hexBytes(const QByteArray& data);

// ---- type helpers ----
// Throws ToolError("Failed to parse type string: <src>").
Ref<BinaryNinja::Type> parseTypeString(BinaryNinja::BinaryView* bv, const QString& source);

// ---- IL ----
enum class IlLevel { Llil, Mlil, Hlil };

// One IL function of any level, iterated like Binary Ninja's Python IL objects.
struct IlFunction {
    IlLevel level = IlLevel::Mlil;
    Ref<BinaryNinja::LowLevelILFunction> llil;
    Ref<BinaryNinja::MediumLevelILFunction> mlil;
    Ref<BinaryNinja::HighLevelILFunction> hlil;

    explicit operator bool() const { return llil || mlil || hlil; }
    size_t instructionCount() const;
    std::vector<Ref<BinaryNinja::BasicBlock>> basicBlocks() const;
    // Instruction indices in Python `il.instructions` order (block by block).
    std::vector<size_t> instructionOrder() const;
    // str(il[i]) and il[i].address
    QString instructionText(size_t index) const;
    uint64_t instructionAddress(size_t index) const;
    size_t exprIndexForInstruction(size_t index) const;
    // str() of an expression; HLIL expressions render as statements unless `hlilAst`
    QString exprText(size_t exprIndex, bool hlilAst = false) const;
    uint64_t exprAddress(size_t exprIndex) const;
    IlFunction ssaForm() const;
};

// func.llil / func.mlil / func.hlil (null members when unavailable)
IlFunction ilOf(BinaryNinja::Function* func, IlLevel level);
// The requested level, then mlil, hlil, llil
IlFunction ilWithFallback(BinaryNinja::Function* func, const QString& level);

}  // namespace rk::binja
