// Helpers shared by the Binary Ninja tools (functions, symbols, disassembly, comments, types, pagination).
#include <algorithm>

#include "binja/host/BnHost.h"
#include "binja/tools/BnTools.h"

using namespace BinaryNinja;

namespace rk::binja {

void registerAllTools(ToolRegistry& registry) {
    // registry._TOOL_MODULES order (filesystem tools are Rikugan's own, not the host's)
    registerNavigationTools(registry);
    registerFunctionTools(registry);
    registerStringTools(registry);
    registerDatabaseTools(registry);
    registerDisassemblyTools(registry);
    registerDecompilerTools(registry);
    registerXrefTools(registry);
    registerAnnotationTools(registry);
    registerTypeTools(registry);
    registerScriptingTools(registry);
    registerIlTools(registry);
    registerIlAnalysisTools(registry);
    registerIlTransformTools(registry);
}

ToolParam param(const char* name, ToolParam::Type type, const char* description, QJsonValue def) {
    ToolParam p;
    p.name = QString::fromUtf8(name);
    p.type = type;
    p.description = QString::fromUtf8(description);
    p.def = std::move(def);
    return p;
}

ToolSpec makeTool(const char* name, const char* category, const char* description, std::vector<ToolParam> params,
                  uint32_t flags, ToolFn fn) {
    ToolSpec spec;
    spec.name = QString::fromUtf8(name);
    spec.category = QString::fromUtf8(category);
    spec.description = QString::fromUtf8(description);
    spec.params = std::move(params);
    spec.flags = flags;
    spec.fn = std::move(fn);
    return spec;
}

BnHost& bnHost(const ToolCall& call) { return static_cast<BnHost&>(call.host()); }

Ref<BinaryView> requireView(const ToolCall& call) {
    Ref<BinaryView> bv = bnHost(call).view();
    if (!bv)
        throw ToolError(QStringLiteral(
            "No active BinaryView. Open a binary in Binary Ninja and invoke Rikugan from that view."));
    return bv;
}

// ---- formatting ----

QString ljust(const QString& s, int width) { return s.leftJustified(width, QLatin1Char(' ')); }

QString rjust(qint64 n, int width) { return QString::number(n).rightJustified(width, QLatin1Char(' ')); }

QString hexPad(uint64_t v, int width) {
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(width, QLatin1Char('0'));
}

QString signedHex(int64_t v) {
    if (v < 0) return QStringLiteral("-0x") + QString::number(uint64_t(0) - uint64_t(v), 16);
    return hex(uint64_t(v));
}

QString pyLeft(const QString& s, qsizetype n) {
    // Python slices code points; keep a surrogate pair whole
    if (s.size() <= n) return s;
    qsizetype units = 0;
    for (qsizetype points = 0; points < n && units < s.size(); ++points)
        units += (s.at(units).isHighSurrogate() && units + 1 < s.size()) ? 2 : 1;
    return s.left(units);
}

QString hexBytes(const QByteArray& data) {
    QStringList parts;
    parts.reserve(data.size());
    for (char c : data) parts.append(QString::number(uint8_t(c), 16).rightJustified(2, QLatin1Char('0')));
    return parts.join(QLatin1Char(' '));
}

// ---- pagination ----

std::pair<qint64, qint64> normalizePage(qint64 offset, qint64 limit) {
    return {std::max<qint64>(0, offset), std::max<qint64>(1, std::min<qint64>(limit, 200))};
}

QString formatPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title, const QString& empty,
                   const QString& nextHint) {
    const auto [off, lim] = normalizePage(offset, limit);
    const qint64 total = rows.size();
    const QStringList page = off < total ? rows.mid(qsizetype(off), qsizetype(lim)) : QStringList();
    QStringList lines{QStringLiteral("%1 %2-%3 of %4:").arg(title).arg(off).arg(off + page.size()).arg(total)};
    if (page.isEmpty()) lines.append(empty);
    else lines.append(page);
    const qint64 next = off + page.size();
    if (next < total) {
        const QString hint = nextHint.isEmpty()
                                 ? QStringLiteral("Use offset=%1 limit=%2 for the next page.").arg(next).arg(lim)
                                 : nextHint;
        lines.append(QStringLiteral("  ... %1 more. %2").arg(total - next).arg(hint));
    }
    return lines.join(QLatin1Char('\n'));
}

QString formatToolPage(const QStringList& rows, qint64 offset, qint64 limit, const QString& title, const char* tool,
                       const QString& empty) {
    const auto [off, lim] = normalizePage(offset, limit);
    const qint64 next = std::min<qint64>(rows.size(), off + lim);
    return formatPage(rows, offset, limit, title, empty,
                      QStringLiteral("Call %1 with offset=%2 limit=%3.").arg(QLatin1String(tool)).arg(next).arg(lim));
}

// ---- functions ----

Ref<Function> functionAt(BinaryView* bv, uint64_t ea) {
    // bv.get_function_at (default platform first), then get_functions_containing
    const std::vector<Ref<Function>> at = bv->GetAnalysisFunctionsForAddress(ea);
    if (!at.empty()) {
        Ref<Platform> platform = bv->GetDefaultPlatform();
        for (const Ref<Function>& f : at) {
            Ref<Platform> fp = f->GetPlatform();
            if ((fp ? fp->GetObject() : nullptr) == (platform ? platform->GetObject() : nullptr)) return f;
        }
        return at.front();
    }
    const std::vector<Ref<Function>> containing = bv->GetAnalysisFunctionsContainingAddress(ea);
    if (!containing.empty()) return containing.front();
    return nullptr;
}

QString functionName(Function* func) {
    if (Ref<Symbol> sym = func->GetSymbol()) {
        if (std::string raw = sym->GetRawName(); !raw.empty()) return qs(raw);
        if (std::string full = sym->GetFullName(); !full.empty()) return qs(full);
    }
    return QStringLiteral("sub_") + QString::number(func->GetStart(), 16);
}

uint64_t functionEnd(Function* func) {
    const uint64_t start = func->GetStart();
    const uint64_t highest = func->GetHighestAddress();
    if (highest >= start) return highest;
    Ref<BinaryView> bv = func->GetView();
    const std::vector<uint64_t> addrs = functionInstructionAddresses(bv, func);
    if (!addrs.empty()) return *std::max_element(addrs.begin(), addrs.end()) + 1;
    return start;
}

std::vector<Ref<Function>> sortedFunctions(BinaryView* bv) {
    std::vector<Ref<Function>> funcs = bv->GetAnalysisFunctionList();
    std::stable_sort(funcs.begin(), funcs.end(),
                     [](const Ref<Function>& a, const Ref<Function>& b) { return a->GetStart() < b->GetStart(); });
    return funcs;
}

std::vector<uint64_t> functionInstructionAddresses(BinaryView* bv, Function* func, size_t maxInstructions) {
    std::vector<uint64_t> out;
    for (const Ref<BasicBlock>& bb : func->GetBasicBlocks()) {
        uint64_t ea = bb->GetStart();
        const uint64_t end = bb->GetEnd();
        while (ea < end && out.size() < maxInstructions) {
            out.push_back(ea);
            const size_t len = instructionLength(bv, ea);
            ea += std::max<size_t>(1, len);
        }
    }
    return out;
}

// ---- symbols ----

Ref<Symbol> symbolAt(BinaryView* bv, uint64_t ea) { return bv->GetSymbolByAddress(ea); }

QString symbolName(Symbol* sym) {
    if (!sym) return {};
    if (std::string full = sym->GetFullName(); !full.empty()) return qs(full);
    if (std::string shortName = sym->GetShortName(); !shortName.empty()) return qs(shortName);
    return qs(sym->GetRawName());
}

QString resolveNameAt(BinaryView* bv, uint64_t ea) {
    if (Ref<Symbol> sym = symbolAt(bv, ea)) {
        const QString name = symbolName(sym);
        if (!name.isEmpty()) return name;
    }
    if (Ref<Function> func = functionAt(bv, ea)) return functionName(func);
    return {};
}

std::vector<Ref<Symbol>> symbolsByName(BinaryView* bv, const QString& name) {
    return bv->GetSymbolsByName(ss(name));
}

bool renameSymbolAt(BinaryView* bv, uint64_t ea, const QString& newName) {
    Ref<Symbol> existing = symbolAt(bv, ea);
    const BNSymbolType type = existing ? existing->GetType() : FunctionSymbol;
    bv->DefineUserSymbol(new Symbol(type, ss(newName), ea));
    return true;
}

// ---- disassembly / bytes / comments ----

QString disassemblyLine(BinaryView* bv, uint64_t ea) {
    // bv.get_disassembly: the architecture's text for the bytes at ea, stripped
    Ref<Architecture> arch = bv->GetDefaultArchitecture();
    if (!arch) return {};
    const size_t maxLen = arch->GetMaxInstructionLength();
    std::vector<uint8_t> buf(std::max<size_t>(1, maxLen));
    size_t len = bv->Read(buf.data(), ea, maxLen);
    if (len == 0) return {};
    std::vector<InstructionTextToken> tokens;
    if (!arch->GetInstructionText(buf.data(), ea, len, tokens)) return {};
    std::string text;
    for (const InstructionTextToken& t : tokens) text += t.text;
    return qs(text).trimmed();
}

size_t instructionLength(BinaryView* bv, uint64_t ea) {
    if (Ref<Architecture> arch = bv->GetDefaultArchitecture()) {
        const size_t len = bv->GetInstructionLength(arch, ea);
        if (len > 0) return len;
    }
    return disassemblyLine(bv, ea).isEmpty() ? 0 : 1;
}

QByteArray readRaw(BinaryView* bv, uint64_t ea, size_t size) {
    QByteArray data(qsizetype(size), '\0');
    const size_t got = size ? bv->Read(data.data(), ea, size) : 0;
    data.truncate(qsizetype(got));
    return data;
}

QByteArray readSafe(BinaryView* bv, uint64_t ea, size_t size) {
    QByteArray data = readRaw(bv, ea, size);
    if (size_t(data.size()) < size) data.append(QByteArray(qsizetype(size) - data.size(), '\0'));
    return data;
}

QString commentAt(BinaryView* bv, uint64_t ea) { return qs(bv->GetCommentForAddress(ea)); }

void updateAnalysisAndWait(BinaryView* bv, Function* func) {
    if (func) func->Reanalyze();
    bv->UpdateAnalysisAndWait();
}

// ---- types ----

Ref<Type> parseTypeString(BinaryView* bv, const QString& source) {
    QualifiedNameAndType result;
    std::string errors;
    if (bv->ParseTypeString(ss(source), result, errors) && result.type) return result.type;
    throw ToolError(QStringLiteral("Failed to parse type string: %1").arg(source));
}

// ---- IL ----

namespace {

Ref<DisassemblySettings> hlilTextSettings() {
    // HighLevelILInstruction.__str__: default settings with DisableLineFormatting
    Ref<DisassemblySettings> settings = DisassemblySettings::GetDefaultSettings()->Duplicate();
    settings->SetOption(DisableLineFormatting);
    return settings;
}

QString joinTokens(const std::vector<InstructionTextToken>& tokens) {
    std::string text;
    for (const InstructionTextToken& t : tokens) text += t.text;
    return qs(text);
}

}  // namespace

size_t IlFunction::instructionCount() const {
    if (llil) return llil->GetInstructionCount();
    if (mlil) return mlil->GetInstructionCount();
    if (hlil) return hlil->GetInstructionCount();
    return 0;
}

std::vector<Ref<BasicBlock>> IlFunction::basicBlocks() const {
    if (llil) return llil->GetBasicBlocks();
    if (mlil) return mlil->GetBasicBlocks();
    if (hlil) return hlil->GetBasicBlocks();
    return {};
}

std::vector<size_t> IlFunction::instructionOrder() const {
    std::vector<size_t> order;
    for (const Ref<BasicBlock>& block : basicBlocks())
        for (uint64_t i = block->GetStart(); i < block->GetEnd(); ++i) order.push_back(size_t(i));
    return order;
}

size_t IlFunction::exprIndexForInstruction(size_t index) const {
    if (llil) return llil->GetIndexForInstruction(index);
    if (mlil) return mlil->GetIndexForInstruction(index);
    if (hlil) return hlil->GetIndexForInstruction(index);
    return 0;
}

QString IlFunction::exprText(size_t exprIndex, bool hlilAst) const {
    if (llil) {
        std::vector<InstructionTextToken> tokens;
        if (!llil->GetExprText(llil->GetArchitecture(), exprIndex, tokens)) return QStringLiteral("invalid");
        return joinTokens(tokens);
    }
    if (mlil) {
        std::vector<InstructionTextToken> tokens;
        if (!mlil->GetExprText(mlil->GetArchitecture(), exprIndex, tokens)) return QStringLiteral("invalid");
        return joinTokens(tokens);
    }
    if (hlil) {
        Ref<DisassemblySettings> settings = hlilTextSettings();
        QStringList lines;
        for (const DisassemblyTextLine& line : hlil->GetExprText(exprIndex, hlilAst, settings))
            lines.append(joinTokens(line.tokens));
        return lines.join(QLatin1Char('\n'));
    }
    return {};
}

QString IlFunction::instructionText(size_t index) const { return exprText(exprIndexForInstruction(index)); }

uint64_t IlFunction::instructionAddress(size_t index) const {
    if (llil) return llil->GetInstruction(index).address;
    if (mlil) return mlil->GetInstruction(index).address;
    if (hlil) return hlil->GetInstruction(index).address;
    return 0;
}

uint64_t IlFunction::exprAddress(size_t exprIndex) const {
    if (llil) return llil->GetExpr(exprIndex).address;
    if (mlil) return mlil->GetExpr(exprIndex).address;
    if (hlil) return hlil->GetExpr(exprIndex).address;
    return 0;
}

IlFunction IlFunction::ssaForm() const {
    IlFunction out;
    out.level = level;
    if (llil) out.llil = llil->GetSSAForm();
    if (mlil) out.mlil = mlil->GetSSAForm();
    if (hlil) out.hlil = hlil->GetSSAForm();
    return out;
}

IlFunction ilOf(Function* func, IlLevel level) {
    IlFunction il;
    il.level = level;
    switch (level) {
    case IlLevel::Llil: il.llil = func->GetLowLevelIL(); break;
    case IlLevel::Mlil: il.mlil = func->GetMediumLevelIL(); break;
    case IlLevel::Hlil: il.hlil = func->GetHighLevelIL(); break;
    }
    return il;
}

IlFunction ilWithFallback(Function* func, const QString& level) {
    // getattr(func, level), then mlil, hlil, llil
    std::vector<IlLevel> order;
    if (level == QLatin1String("llil") || level == QLatin1String("low_level_il")) order.push_back(IlLevel::Llil);
    else if (level == QLatin1String("hlil") || level == QLatin1String("high_level_il")) order.push_back(IlLevel::Hlil);
    for (IlLevel l : {IlLevel::Mlil, IlLevel::Hlil, IlLevel::Llil}) order.push_back(l);
    for (IlLevel l : order)
        if (IlFunction il = ilOf(func, l)) return il;
    return {};
}

}  // namespace rk::binja
