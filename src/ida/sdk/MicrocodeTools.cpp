// IDA microcode tools: listing, formatting and optimizers. nop_microcode's optimizer is a native
// optinsn_t; install_microcode_optimizer's run in IDAPython. Subclasses SDK types: built without RTTI.
#include <algorithm>
#include <set>

#include "ida/host/IdaHost.h"
#include "ida/host/IdaTools.h"
#include "ida/sdk/Microcode.h"

namespace rk::ida {

namespace {

// ---------------------------------------------------------------------------------------------------- formatting

// Maturity names by index (0 = MMAT_GENERATED); the index goes to gen_microcode as is
const QStringList& maturityNames() {
    static const QStringList names{"MMAT_GENERATED", "MMAT_PREOPTIMIZED", "MMAT_LOCOPT",  "MMAT_CALLS",
                                   "MMAT_GLBOPT1",   "MMAT_GLBOPT2",      "MMAT_GLBOPT3", "MMAT_LVARS"};
    return names;
}

void requireHexrays(IdaHost& host) {
    if (!host.hasDecompiler()) throw ToolError(QStringLiteral("Hex-Rays decompiler is not available"));
}

int parseMaturity(const QString& raw) {
    const QString name = raw.trimmed().toUpper();
    const int idx = int(maturityNames().indexOf(name));
    if (idx >= 0) return idx;
    qint64 val = 0;
    bool ok = false;
    const QString digits = name.trimmed();
    val = digits.toLongLong(&ok);
    if (ok && val >= 0 && val <= 7) return int(val);
    throw ToolError(QStringLiteral("Unknown maturity level: ") + pyRepr(name) + QStringLiteral(". Valid levels: ") +
                    maturityNames().join(QStringLiteral(", ")));
}

QString maturityLabel(int level) {
    if (level >= 0 && level < maturityNames().size()) return maturityNames()[level];
    return QStringLiteral("MMAT_%1").arg(level);
}

QString insnText(const minsn_t* ins) { return qs(ins->dstr()); }

QString insnEa(const minsn_t* ins) {
    return ins->ea != BADADDR ? altHex010(ins->ea) : QStringLiteral("  --------");
}

std::vector<qint64> preds(const mblock_t* blk) {
    std::vector<qint64> out;
    for (int j = 0; j < blk->npred(); ++j) out.push_back(blk->pred(j));
    return out;
}

std::vector<qint64> succs(const mblock_t* blk) {
    std::vector<qint64> out;
    for (int j = 0; j < blk->nsucc(); ++j) out.push_back(blk->succ(j));
    return out;
}

QString formatBlock(const mblock_t* blk) {
    QStringList lines{QStringLiteral("--- Block %1 [").arg(blk->serial) + hx(blk->start) + QStringLiteral("..") + hx(blk->end) +
                      QStringLiteral("] preds=") + pyIntList(preds(blk)) + QStringLiteral(" succs=") + pyIntList(succs(blk)) +
                      QStringLiteral(" type=%1 ---").arg(int(blk->type))};
    for (const minsn_t* ins = blk->head; ins != nullptr; ins = ins->next) {
        lines << QStringLiteral("  ") + insnEa(ins) + QStringLiteral("  ") + insnText(ins);
        if (ins == blk->tail) break;
    }
    return lines.join(QLatin1Char('\n'));
}

QString formatMba(mba_t* mba) {
    qint64 total = 0;
    for (int i = 0; i < mba->qty; ++i) {
        const mblock_t* blk = mba->get_mblock(i);
        for (const minsn_t* ins = blk->head; ins != nullptr; ins = ins->next) {
            ++total;
            if (ins == blk->tail) break;
        }
    }
    QStringList parts{QStringLiteral("Maturity: ") + maturityLabel(mba->maturity) + QStringLiteral(" (%1)").arg(int(mba->maturity)),
                      QStringLiteral("Blocks: %1, Instructions: %2").arg(mba->qty).arg(total), QString()};
    for (int i = 0; i < mba->qty; ++i) {
        parts << formatBlock(mba->get_mblock(i));
        parts << QString();
    }
    return parts.join(QLatin1Char('\n'));
}

QString pseudocodeText(cfunc_t* cfunc) {
    QStringList lines;
    const strvec_t& sv = cfunc->get_pseudocode();
    for (size_t i = 0; i < sv.size(); ++i) {
        qstring text;
        tag_remove(&text, sv[i].line);
        lines << rjust(qint64(i) + 1, 4) + QStringLiteral("  ") + qs(text);
    }
    return lines.join(QLatin1Char('\n'));
}

QString funcName(const func_t* pfn) {
    const QString name = nameAt(pfn->start_ea);
    if (!name.isEmpty()) return name + QStringLiteral(" (") + hx(pfn->start_ea) + QLatin1Char(')');
    return QStringLiteral("sub_") + QString::number(pfn->start_ea, 16);
}

QString mopTypeName(mopt_t t) {
    switch (t) {
    case mop_z: return QStringLiteral("mop_z");
    case mop_r: return QStringLiteral("mop_r");
    case mop_n: return QStringLiteral("mop_n");
    case mop_d: return QStringLiteral("mop_d");
    case mop_S: return QStringLiteral("mop_S");
    case mop_v: return QStringLiteral("mop_v");
    case mop_b: return QStringLiteral("mop_b");
    case mop_f: return QStringLiteral("mop_f");
    case mop_l: return QStringLiteral("mop_l");
    case mop_a: return QStringLiteral("mop_a");
    case mop_h: return QStringLiteral("mop_h");
    case mop_str: return QStringLiteral("mop_str");
    case mop_c: return QStringLiteral("mop_c");
    case mop_fn: return QStringLiteral("mop_fn");
    case mop_p: return QStringLiteral("mop_p");
    case mop_sc: return QStringLiteral("mop_sc");
    default: return QStringLiteral("mop_%1").arg(int(t));
    }
}

// operand_detail
QString operandDetail(const mop_t& op) {
    const QString size = QStringLiteral("size=%1").arg(op.size);
    QString detail;
    switch (op.t) {
    case mop_z: detail = QStringLiteral("(empty)"); break;
    case mop_r: detail = QStringLiteral("reg r%1 ").arg(op.r) + size; break;
    case mop_n: {
        const uint64 val = op.nnn->value;
        detail = QStringLiteral("imm 0x%1 (%2) ").arg(QString::number(val, 16)).arg(val) + size;
        break;
    }
    case mop_d:
        detail = QStringLiteral("nested_insn ") + size + QStringLiteral("   => ") + insnText(op.d);
        break;
    case mop_S: detail = QStringLiteral("stkvar off=") + altHex(op.s->off) + QLatin1Char(' ') + size; break;
    case mop_v: detail = QStringLiteral("global addr=") + hx(op.g) + QLatin1Char(' ') + size; break;
    case mop_b: detail = QStringLiteral("block @%1").arg(op.b); break;
    case mop_f: detail = QStringLiteral("callinfo ") + size; break;
    case mop_l: detail = QStringLiteral("local idx=%1 off=").arg(op.l->idx) + altHex(op.l->off) + QLatin1Char(' ') + size; break;
    case mop_a: detail = QStringLiteral("addr_of ") + size; break;
    case mop_h: detail = QStringLiteral("helper '") + qs(op.helper) + QStringLiteral("' ") + size; break;
    case mop_str: detail = QStringLiteral("string '") + qs(op.cstr) + QStringLiteral("' ") + size; break;
    case mop_c: detail = QStringLiteral("cases ") + size; break;
    case mop_fn: detail = QStringLiteral("funcnum ") + size; break;
    case mop_p: detail = QStringLiteral("pair ") + size; break;
    case mop_sc: detail = QStringLiteral("scattered ") + size; break;
    default: detail = QStringLiteral("unknown_type(%1) ").arg(int(op.t)) + size; break;
    }
    return QLatin1Char('[') + mopTypeName(op.t) + QStringLiteral("] ") + detail;
}

// ---------------------------------------------------------------------------------------------------- microcode

func_t* funcOrRaise(ea_t ea) {
    func_t* pfn = get_func(ea);
    if (pfn == nullptr) throw ToolError(QStringLiteral("No function at ") + hx(ea));
    return pfn;
}

// Generated microcode at a maturity: an mba (owned, or the cfunc's) or text captured by the maturity hook.
struct Microcode {
    cfuncptr_t cfunc{nullptr};
    mba_t* mba = nullptr;
    bool owned = false;
    QString captured;
    bool hasText = false;

    Microcode() = default;
    Microcode(const Microcode&) = delete;
    Microcode& operator=(const Microcode&) = delete;
    ~Microcode() {
        if (owned) delete mba;
    }
};

struct MaturityCapture {
    int target;
    QString captured;
    bool done = false;
};

ssize_t idaapi captureCallback(void* ud, hexrays_event_t event, va_list va) {
    if (event != hxe_maturity) return 0;
    auto* cap = static_cast<MaturityCapture*>(ud);
    try {
        cfunc_t* cfunc = va_arg(va, cfunc_t*);
        if (!cap->done && cfunc != nullptr && cfunc->mba != nullptr && int(cfunc->mba->maturity) == cap->target) {
            cap->captured = formatMba(cfunc->mba);
            cap->done = true;
        }
    } catch (...) {
    }
    return 0;
}

void genMicrocodeAt(ea_t ea, int maturity, Microcode* out) {
    func_t* pfn = funcOrRaise(ea);
    if (maturity >= 7) {
        out->cfunc = decompileAt(pfn->start_ea);
        if (out->cfunc == nullptr) throw ToolError(QStringLiteral("Decompilation failed for ") + hx(pfn->start_ea));
        out->mba = out->cfunc->mba;
        return;
    }

    mba_ranges_t mbr;
    mbr.ranges.push_back(range_t(pfn->start_ea, pfn->end_ea));
    hexrays_failure_t hf;
    mlist_t retlist;
    if (mba_t* mba = gen_microcode(mbr, &hf, &retlist, DECOMP_NO_WAIT, mba_maturity_t(maturity))) {
        out->mba = mba;
        out->owned = true;
        return;
    }

    MaturityCapture cap{maturity, {}, false};
    install_hexrays_callback(captureCallback, &cap);
    decompileAt(pfn->start_ea);
    remove_hexrays_callback(captureCallback, &cap);
    if (cap.done) {
        out->captured = cap.captured;
        out->hasText = true;
        return;
    }
    throw ToolError(QStringLiteral("Could not generate microcode at ") + maturityLabel(maturity) + QStringLiteral(" for ") +
                    hx(pfn->start_ea));
}

// ---------------------------------------------------------------------------------------------------- optimizers

struct NopOptimizer : public optinsn_t {
    explicit NopOptimizer(std::set<ea_t> eas) : targets(std::move(eas)) {}
    int idaapi func(mblock_t* blk, minsn_t* ins, int) override {
        if (ins->opcode == m_nop || targets.count(ins->ea) == 0) return 0;
        if (blk != nullptr) blk->make_nop(ins);
        else ins->_make_nop();
        ++applied;
        return 1;
    }
    std::set<ea_t> targets;
    qint64 applied = 0;
};

const char kOptimizerHelpers[] = R"PY(
import textwrap as __rk_textwrap
import ida_hexrays as __rk_hx

__rk_mc_opts = {}


class __rk_DynInsn(__rk_hx.optinsn_t):
    def __init__(self, name, description, optimize_fn):
        super().__init__()
        self.name = name
        self.description = description
        self._optimize = optimize_fn

    def func(self, blk, ins, optflags):
        try:
            result = self._optimize(blk, ins)
            return int(result) if result else 0
        except Exception:
            return 0


class __rk_DynBlock(__rk_hx.optblock_t):
    def __init__(self, name, description, optimize_fn):
        super().__init__()
        self.name = name
        self.description = description
        self._optimize = optimize_fn

    def func(self, blk):
        try:
            result = self._optimize(blk)
            return int(result) if result else 0
        except Exception:
            return 0


def __rk_mc_install(name, description, kind, python_code):
    namespace = {"__builtins__": __rk_safe_builtins(), "ida_hexrays": __rk_hx}
    for attr in dir(__rk_hx):
        if attr.startswith("m_") or attr.startswith("mop_") or attr.startswith("MMAT_") or attr.startswith("BLT_"):
            namespace[attr] = getattr(__rk_hx, attr)
    try:
        exec(compile(__rk_textwrap.dedent(python_code), f"<optimizer:{name}>", "exec"), namespace)
        optimize_fn = namespace.get("optimize")
        if optimize_fn is None or not callable(optimize_fn):
            raise ValueError("Code must define a callable 'optimize' function.")
    except ValueError as e:
        return str(e)
    except Exception as e:
        return f"Failed to compile optimizer code: {e}"
    opt = (__rk_DynInsn if kind == "instruction" else __rk_DynBlock)(name, description, optimize_fn)
    opt.install()
    __rk_mc_opts[name] = opt
    return ""


def __rk_mc_remove(name):
    opt = __rk_mc_opts.pop(name, None)
    if opt is not None:
        try:
            opt.remove()
        except (AttributeError, RuntimeError):
            pass
    return ""
)PY";

}  // namespace

struct MicrocodeOptimizers::Entry {
    enum Kind { Nop, Instruction, Block };
    QString name;
    Kind kind = Nop;
    QString description;
    std::unique_ptr<NopOptimizer> nop;
};

MicrocodeOptimizers::MicrocodeOptimizers(IdaHost& h) : host(h) {}

MicrocodeOptimizers::~MicrocodeOptimizers() {
    for (const auto& e : entries) {
        if (e->nop) {
            remove_optinsn_handler(e->nop.get());
        } else {
            try {
                host.evalPythonHelper({}, QStringLiteral("__rk_mc_remove(") + IdaHost::pyLiteral(e->name) + QLatin1Char(')'));
            } catch (...) {
            }
        }
    }
}

namespace {

MicrocodeOptimizers::Entry* findOptimizer(MicrocodeOptimizers& opts, const QString& name) {
    for (const auto& e : opts.entries)
        if (e->name == name) return e.get();
    return nullptr;
}

// remove_optimizer
void removeOptimizer(MicrocodeOptimizers& opts, const QString& name) {
    auto it = std::find_if(opts.entries.begin(), opts.entries.end(), [&](const auto& e) { return e->name == name; });
    if (it == opts.entries.end()) return;
    std::unique_ptr<MicrocodeOptimizers::Entry> entry = std::move(*it);
    opts.entries.erase(it);
    if (entry->nop) remove_optinsn_handler(entry->nop.get());
    else opts.host.evalPythonHelper({}, QStringLiteral("__rk_mc_remove(") + IdaHost::pyLiteral(name) + QLatin1Char(')'));
}

QStringList activeNames(const MicrocodeOptimizers& opts) {
    QStringList names;
    for (const auto& e : opts.entries) names << e->name;
    return names;
}

}  // namespace

void registerMicrocodeTools(ToolRegistry& reg, IdaHost& host) {
    addTool(reg, "get_microcode", "microcode", "Get the microcode for a function at a specific maturity level.",
            {strParam("address", "Function address (hex string, e.g. '0x401000')"),
             strParam("maturity", "Maturity level name or number (0-7)", "MMAT_LVARS")},
            RequiresDecompiler, [&host](ToolCall& call) {
                requireHexrays(host);
                const ea_t ea = call.address("address");
                const int mat = parseMaturity(call.str("maturity"));
                func_t* pfn = funcOrRaise(ea);
                Microcode mc;
                genMicrocodeAt(ea, mat, &mc);
                const QString header = QStringLiteral("=== Microcode for ") + funcName(pfn) + QStringLiteral(" at ") +
                                       maturityLabel(mat) + QStringLiteral(" ===\n");
                if (mc.hasText) return ToolResult::ok(header + mc.captured);
                return ToolResult::ok(header + QStringLiteral("Function: ") + hx(pfn->start_ea) + QStringLiteral(" - ") +
                                      hx(pfn->end_ea) + QLatin1Char('\n') + formatMba(mc.mba));
            });

    addTool(reg, "get_microcode_block", "microcode", "Get detailed microcode for a single basic block.",
            {strParam("address", "Function address (hex string)"), intParam("block_index", "Block index (0-based)"),
             strParam("maturity", "Maturity level", "MMAT_LVARS")},
            RequiresDecompiler, [&host](ToolCall& call) {
                requireHexrays(host);
                const ea_t ea = call.address("address");
                const qint64 blockIndex = call.integer("block_index");
                const int mat = parseMaturity(call.str("maturity"));
                func_t* pfn = funcOrRaise(ea);
                Microcode mc;
                genMicrocodeAt(ea, mat, &mc);
                if (mc.hasText)
                    return ToolResult::ok(QStringLiteral("Block detail not available at this maturity (captured via hook).\n") + mc.captured);
                mba_t* mba = mc.mba;
                if (blockIndex < 0 || blockIndex >= mba->qty)
                    return ToolResult::ok(QStringLiteral("Block index %1 out of range [0, %2]").arg(blockIndex).arg(mba->qty - 1));
                const mblock_t* blk = mba->get_mblock(int(blockIndex));
                QStringList lines{QStringLiteral("=== Block %1 of ").arg(blk->serial) + funcName(pfn) + QStringLiteral(" at ") +
                                      maturityLabel(mat) + QStringLiteral(" ==="),
                                  QString()};
                lines << QStringLiteral("Range: ") + hx(blk->start) + QStringLiteral(" .. ") + hx(blk->end);
                lines << QStringLiteral("Predecessors: ") + pyIntList(preds(blk));
                lines << QStringLiteral("Successors:   ") + pyIntList(succs(blk));
                lines << QStringLiteral("Block type:   %1").arg(int(blk->type));
                lines << QString();
                lines << QStringLiteral("Instructions:");
                int idx = 0;
                for (const minsn_t* ins = blk->head; ins != nullptr; ins = ins->next) {
                    lines << QStringLiteral("  [") + rjust(idx, 3) + QStringLiteral("] ") + insnEa(ins) + QStringLiteral("  ") +
                                 insnText(ins);
                    const std::pair<const char*, const mop_t*> ops[] = {{"L", &ins->l}, {"R", &ins->r}, {"D", &ins->d}};
                    for (const auto& [label, op] : ops)
                        if (op->t != mop_z)
                            lines << QStringLiteral("         ") + QLatin1String(label) + QStringLiteral(": ") + operandDetail(*op);
                    ++idx;
                    if (ins == blk->tail) break;
                }
                lines << QStringLiteral("\nTotal: %1 instructions").arg(idx);
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "nop_microcode", "microcode", "NOP out specific microcode instructions and redecompile.",
            {strParam("func_address", "Address of the function to patch"),
             strParam("instruction_addresses",
                      "Comma-separated hex addresses of instructions to NOP (e.g. '0x401004,0x401008,0x40100c')"),
             strParam("optimizer_name", "Name for this NOP rule (for later removal)", "")},
            RequiresDecompiler | Mutating, [&host](ToolCall& call) {
                requireHexrays(host);
                MicrocodeOptimizers& opts = host.microcode();
                func_t* pfn = funcOrRaise(call.address("func_address"));
                const ea_t start = pfn->start_ea;
                QStringList raw;
                for (const QString& s : call.str("instruction_addresses").split(QLatin1Char(',')))
                    if (!s.trimmed().isEmpty()) raw << s.trimmed();
                if (raw.isEmpty()) return ToolResult::ok(QStringLiteral("No instruction addresses provided."));
                std::set<ea_t> targets;
                for (const QString& a : raw) targets.insert(ea_t(parseAddress(a)));

                QString name = call.str("optimizer_name");
                if (name.isEmpty()) name = QStringLiteral("nop_") + hx(start) + QStringLiteral("_%1").arg(opts.entries.size());
                if (findOptimizer(opts, name)) removeOptimizer(opts, name);

                const qsizetype count = qsizetype(targets.size());
                auto entry = std::make_unique<MicrocodeOptimizers::Entry>();
                entry->name = name;
                entry->kind = MicrocodeOptimizers::Entry::Nop;
                entry->nop = std::make_unique<NopOptimizer>(std::move(targets));
                NopOptimizer* opt = entry->nop.get();
                install_optinsn_handler(opt);
                opts.entries.push_back(std::move(entry));

                cfuncptr_t cfunc = decompileAt(start);
                if (cfunc == nullptr)
                    return ToolResult::ok(QStringLiteral("Optimizer '%1' installed (NOPs %2 EAs) but redecompilation failed.")
                                              .arg(name, QString::number(count)));
                return ToolResult::ok(QStringLiteral("Optimizer '") + name + QStringLiteral("' installed \u2014 NOPing %1 instructions.\n").arg(count) +
                                      QStringLiteral("Applied %1 times during redecompilation.\n").arg(opt->applied) +
                                      QStringLiteral("\n--- Cleaned pseudocode ---\n") + pseudocodeText(cfunc));
            });

    addTool(reg, "install_microcode_optimizer", "microcode", "Install a custom microcode optimizer written in Python.",
            {strParam("name", "Unique name for this optimizer (used to remove it later)"),
             strParam("description", "What this optimizer does (for list_microcode_optimizers)"),
             strParam("optimizer_type", "Type: 'instruction' (called per-instruction) or 'block' (called per-block)"),
             strParam("python_code",
                      "Python code defining an 'optimize' function. For instruction type: def optimize(blk, ins) -> int. "
                      "For block type: def optimize(blk) -> int. Return the number of changes made (0 = no change). "
                      "All ida_hexrays constants (m_nop, m_mov, mop_r, etc.) are available.")},
            RequiresDecompiler | Mutating | Script, [&host](ToolCall& call) {
                requireHexrays(host);
                MicrocodeOptimizers& opts = host.microcode();
                const QString name = call.str("name"), description = call.str("description");
                if (findOptimizer(opts, name))
                    return ToolResult::ok(QStringLiteral("Optimizer '%1' already exists. Remove it first with remove_microcode_optimizer.").arg(name));
                const QString optType = call.str("optimizer_type").trimmed().toLower();
                if (optType != QLatin1String("instruction") && optType != QLatin1String("block"))
                    return ToolResult::ok(QStringLiteral("optimizer_type must be 'instruction' or 'block'."));
                const QString pythonCode = call.str("python_code");
                if (!opts.pythonHelpersReady) {
                    host.evalPythonHelper(QString::fromUtf8(kOptimizerHelpers), QStringLiteral("''"));
                    opts.pythonHelpersReady = true;
                }
                const QString failure = host.evalPythonHelper(
                    {}, QStringLiteral("__rk_mc_install(") + IdaHost::pyLiteral(name) + QStringLiteral(", ") +
                            IdaHost::pyLiteral(description) + QStringLiteral(", ") + IdaHost::pyLiteral(optType) +
                            QStringLiteral(", ") + IdaHost::pyLiteral(pythonCode) + QLatin1Char(')'));
                if (!failure.isEmpty()) return ToolResult::ok(failure);
                auto entry = std::make_unique<MicrocodeOptimizers::Entry>();
                entry->name = name;
                entry->kind = optType == QLatin1String("instruction") ? MicrocodeOptimizers::Entry::Instruction
                                                                      : MicrocodeOptimizers::Entry::Block;
                entry->description = description;
                opts.entries.push_back(std::move(entry));
                return ToolResult::ok(QStringLiteral("Optimizer '") + name + QStringLiteral("' (") + optType +
                                      QStringLiteral(") installed successfully.\nDescription: ") + description +
                                      QStringLiteral("\nUse redecompile_function() to test it on a function."));
            });

    addTool(reg, "remove_microcode_optimizer", "microcode", "Remove a previously installed microcode optimizer.",
            {strParam("name", "Name of the optimizer to remove")}, RequiresDecompiler | Mutating, [&host](ToolCall& call) {
                requireHexrays(host);
                MicrocodeOptimizers& opts = host.microcode();
                const QString name = call.str("name");
                if (!findOptimizer(opts, name)) {
                    QStringList available = activeNames(opts);
                    if (available.isEmpty()) available << QStringLiteral("(none)");
                    return ToolResult::ok(QStringLiteral("No optimizer named '") + name + QStringLiteral("'. Active optimizers: ") +
                                          available.join(QStringLiteral(", ")));
                }
                removeOptimizer(opts, name);
                return ToolResult::ok(QStringLiteral("Optimizer '") + name + QStringLiteral("' removed."));
            });

    addTool(reg, "list_microcode_optimizers", "microcode", "List all currently installed microcode optimizers.", {},
            RequiresDecompiler, [&host](ToolCall&) {
                requireHexrays(host);
                const MicrocodeOptimizers& opts = host.microcode();
                if (opts.entries.empty()) return ToolResult::ok(QStringLiteral("No microcode optimizers installed."));
                QStringList lines{QStringLiteral("Installed optimizers (%1):").arg(opts.entries.size())};
                for (const auto& e : opts.entries) {
                    const bool insn = e->kind != MicrocodeOptimizers::Entry::Block;
                    const QString desc = e->nop ? QStringLiteral("NOP %1 addresses (applied %2x)").arg(e->nop->targets.size()).arg(e->nop->applied)
                                                : e->description;
                    lines << QStringLiteral("  ") + e->name + QStringLiteral(" [") +
                                 (insn ? QStringLiteral("instruction") : QStringLiteral("block")) + QStringLiteral("] \u2014 ") + desc;
                }
                return ToolResult::ok(lines.join(QLatin1Char('\n')));
            });

    addTool(reg, "redecompile_function", "microcode", "Force redecompilation of a function and return cleaned pseudocode.",
            {strParam("address", "Function address to redecompile (hex string)")}, RequiresDecompiler, [&host](ToolCall& call) {
                requireHexrays(host);
                func_t* pfn = funcOrRaise(call.address("address"));
                mark_cfunc_dirty(pfn->start_ea);
                cfuncptr_t cfunc = decompileAt(pfn->start_ea);
                if (cfunc == nullptr) return ToolResult::ok(QStringLiteral("Decompilation returned None for ") + hx(pfn->start_ea));
                const QStringList active = activeNames(host.microcode());
                const QString status = active.isEmpty() ? QStringLiteral("No optimizers active")
                                                        : QStringLiteral("Active optimizers: ") + active.join(QStringLiteral(", "));
                return ToolResult::ok(QStringLiteral("=== Redecompiled ") + funcName(pfn) + QStringLiteral(" ===\n") + status +
                                      QStringLiteral("\n\n") + pseudocodeText(cfunc));
            });
}

}  // namespace rk::ida
