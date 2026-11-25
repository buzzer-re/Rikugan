#include "ida/host/IdaHost.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "common/util/Embedded.h"
#include "ida/host/IdaTools.h"
#include "ida/sdk/MainThreadRequest.h"
#include "ida/sdk/Microcode.h"

namespace rk::ida {

namespace {

constexpr uint64_t kNoAddress = ~uint64_t(0);

// The persistent execute_python namespace, preloaded with the IDA Python modules that are available.
const char kNamespaceSetup[] = R"PY(
import importlib as __rk_importlib
__rk_ns = {}
for __rk_m in ("idaapi", "idautils", "idc", "ida_funcs", "ida_name", "ida_bytes", "ida_segment", "ida_struct",
               "ida_enum", "ida_typeinf", "ida_nalt", "ida_xref", "ida_kernwin"):
    try:
        __rk_ns[__rk_m] = __rk_importlib.import_module(__rk_m)
    except ImportError:
        pass
del __rk_m
)PY";

extlang_object_t python() {
    extlang_object_t py = find_extlang_by_name("Python");
    if (!py) throw ToolError(QStringLiteral("IDAPython is not available"));
    return py;
}

void runSnippet(const extlang_object_t& py, const QByteArray& code, const char* what) {
    qstring err;
    if (!py->eval_snippet(code.constData(), &err))
        throw ToolError(QStringLiteral("%1 failed: %2").arg(QLatin1String(what), qs(err)));
}

QString readStringVar(const extlang_object_t& py, const char* name) {
    idc_value_t rv;
    qstring err;
    if (!py->eval_expr(&rv, BADADDR, name, &err))
        throw ToolError(QStringLiteral("IDAPython returned no result: %1").arg(qs(err)));
    if (rv.vtype != VT_STR) throw ToolError(QStringLiteral("IDAPython returned no result"));
    return qs(rv.qstr());
}

// onMain for the Host accessors, which never throw: `fallback` once Rikugan is shutting down.
template <class R, class F>
R mainOr(IdaHost& host, MainAccess access, R fallback, F&& f) {
    try {
        return onMain(host, access, f);
    } catch (const ToolError&) {
        return fallback;
    }
}

}  // namespace

IdaHost::IdaHost() : currentEa_(kNoAddress), microcode_(std::make_unique<MicrocodeOptimizers>(*this)) {}

IdaHost::~IdaHost() = default;

QString IdaHost::databasePath() const { return qs(get_path(PATH_TYPE_IDB)); }

QString IdaHost::inputPath() const {
    char buf[QMAXPATH];
    if (get_input_file_path(buf, sizeof(buf)) <= 0) return {};
    return QString::fromUtf8(buf);
}

QString IdaHost::databaseInstanceId() {
    return mainOr(*this, MainAccess::Write, QString(), [] {
        netnode node("$ rikugan", 0, true);
        qstring value;
        if (node.supstr(&value, 0) > 0 && !value.empty()) return qs(value);
        const QByteArray id = QUuid::createUuid().toByteArray(QUuid::Id128);
        node.supset(0, id.constData());
        return QString::fromLatin1(id);
    });
}

std::optional<uint64_t> IdaHost::currentAddress() const {
    const uint64_t ea = currentEa_.load(std::memory_order_relaxed);
    if (ea != kNoAddress) return ea;
    auto* self = const_cast<IdaHost*>(this);
    const ea_t screen = mainOr(*self, MainAccess::Read, BADADDR, [] { return get_screen_ea(); });
    if (screen == BADADDR) return std::nullopt;
    return uint64_t(screen);
}

QString IdaHost::currentFunctionLabel() const {
    auto* self = const_cast<IdaHost*>(this);
    return mainOr(*self, MainAccess::Read, QString(), [this] {
        const std::optional<uint64_t> ea = currentAddress();
        if (!ea) return QString();
        func_t* pfn = get_func(ea_t(*ea));
        return pfn ? nameAt(pfn->start_ea) : QString();
    });
}

void IdaHost::navigate(uint64_t ea) {
    mainOr(*this, MainAccess::Read, false, [ea] { return jumpto(ea_t(ea)); });
}

void IdaHost::databaseChanged() {
    // Hex-Rays redraws a pseudocode view for an edit made in it, not for a name or type a tool changed: every open
    // one decompiles again, and the disassembly views repaint
    const bool decompiler = hasDecompiler();
    mainOr(*this, MainAccess::Write, false, [decompiler] {
        if (decompiler) {
            for (char c = 'A'; c <= 'Z'; ++c) {
                const char title[] = {'P', 's', 'e', 'u', 'd', 'o', 'c', 'o', 'd', 'e', '-', c, '\0'};
                if (TWidget* w = find_widget(title))
                    if (vdui_t* vu = get_widget_vdui(w)) vu->refresh_view(true);
            }
        }
        refresh_idaview_anyway();
        return true;
    });
}

bool IdaHost::runOnMainThread(const std::function<void()>& fn, MainAccess access) {
    if (is_main_thread()) {
        fn();
        return true;
    }
    if (Host::closing.load()) return false;
    auto call = std::make_shared<MainThreadCall>(fn);
    const int requestId = postMainThreadCall(call, access == MainAccess::Write);
    if (!call->wait()) {
        cancelMainThreadCall(requestId);
        return false;
    }
    return true;
}

bool IdaHost::isMainThread() const { return is_main_thread(); }

bool IdaHost::hasDecompiler() const {
    const int known = decompiler_.load();
    if (known >= 0) return known == 1;
    auto* self = const_cast<IdaHost*>(this);
    try {
        const bool has = onMain(*self, MainAccess::Read, [] { return init_hexrays_plugin(); });
        decompiler_.store(has ? 1 : 0);
        return has;
    } catch (const ToolError&) {
        return false;  // shutting down; not cached
    }
}

QString IdaHost::binaryInfo() {
    return mainOr(*this, MainAccess::Read, QString(), [] { return binaryInfoText(); });
}

void IdaHost::registerTools(ToolRegistry& registry) {
    registerNavigationTools(registry, *this);
    registerFunctionTools(registry, *this);
    registerStringTools(registry, *this);
    registerDatabaseTools(registry, *this);
    registerDisassemblyTools(registry, *this);
    registerDecompilerTools(registry, *this);
    registerXrefTools(registry, *this);
    registerAnnotationTools(registry, *this);
    registerTypeTools(registry, *this);
    registerScriptingTools(registry, *this);
    registerMicrocodeTools(registry, *this);
}

QString IdaHost::pyLiteral(const QString& s) {
    return QStringLiteral("bytes.fromhex(\"%1\").decode(\"utf-8\")").arg(QString::fromLatin1(s.toUtf8().toHex()));
}

void IdaHost::ensurePython() {
    const extlang_object_t py = python();
    if (pythonReady_) return;
    const QString prelude = embeddedText("prompts/python/guard_prelude.py");
    if (prelude.isEmpty()) throw ToolError(QStringLiteral("The Python guard prelude is missing from this build"));
    runSnippet(py, prelude.toUtf8(), "IDAPython guard prelude");
    runSnippet(py, QByteArray(kNamespaceSetup), "IDAPython namespace setup");
    pythonReady_ = true;
}

QString IdaHost::evalPythonHelper(const QString& statements, const QString& resultExpr) {
    ensurePython();
    const extlang_object_t py = python();
    runSnippet(py, (statements + QStringLiteral("\n__rk_result = str(") + resultExpr + QStringLiteral(")\n")).toUtf8(),
               "IDAPython");
    return readStringVar(py, "__rk_result");
}

PythonResult IdaHost::runPython(const QString& code, const std::shared_ptr<CancelToken>& token) {
    PythonResult result;
    try {
        const QString json = onMain(*this, MainAccess::Write, [&] {
            ensurePython();
            const extlang_object_t py = python();
            runSnippet(py, (QStringLiteral("__rk_result = __rk_exec(") + pyLiteral(code) + QStringLiteral(", __rk_ns)")).toUtf8(),
                       "IDAPython evaluation");
            return readStringVar(py, "__rk_result");
        });
        const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
        result.output = obj.value(QStringLiteral("output")).toString();
        const QJsonValue error = obj.value(QStringLiteral("error"));
        result.ok = error.isNull() || error.isUndefined();
        if (!result.ok) result.error = error.toString();
    } catch (const ToolError& e) {
        result.ok = false;
        result.error = e.message();
    }
    return result;
}

bool IdaHost::writeBytes(uint64_t ea, const QByteArray& bytes) {
    return mainOr(*this, MainAccess::Write, false, [&] {
        patch_bytes(ea_t(ea), bytes.constData(), size_t(bytes.size()));
        return true;
    });
}

QByteArray IdaHost::readBytes(uint64_t ea, size_t n) {
    return mainOr(*this, MainAccess::Read, QByteArray(), [&] {
        QByteArray out(qsizetype(n), '\0');
        const ssize_t got = get_bytes(out.data(), ssize_t(n), ea_t(ea), GMB_READALL);
        out.resize(got > 0 ? qsizetype(got) : 0);
        return out;
    });
}

}  // namespace rk::ida
