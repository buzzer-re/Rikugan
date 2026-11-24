#include "binja/host/BnPython.h"

#include <chrono>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>

#include "common/util/Embedded.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

constexpr const char* kPreludePath = "prompts/python/guard_prelude.py";
constexpr const char* kBootMarker = "\x02RKBOOT\x02";
constexpr const char* kBegin = "\x02RKB\x02";
constexpr const char* kEnd = "\x02RKE\x02";

// Installed once into the private globals dict `__rk_g` of our interpreter instance, next to the guard prelude's
// __rk_exec. __rk_ns is the persistent namespace of execute_python.
constexpr const char* kBootstrap = R"PY(
import base64 as _rk_b64
import binaryninja as _rk_bn
__rk_ns = {'binaryninja': _rk_bn}
try:
    import binaryninjaui as _rk_bnui
    __rk_ns['binaryninjaui'] = _rk_bnui
except ImportError:
    pass
__rk_state = {}


def __rk_call(src_b64, bv, current_address):
    __rk_ns['bv'] = bv
    __rk_ns['current_address'] = current_address
    return __rk_exec(_rk_b64.b64decode(src_b64).decode('utf-8'), __rk_ns)


def __rk_trusted(src_b64, bv):
    ns = {'__name__': '__rk_trusted__', 'bv': bv, '__rk_state': __rk_state}
    exec(_rk_b64.b64decode(src_b64).decode('utf-8'), ns)
    return str(ns.get('__rk_result', ''))
)PY";

std::string b64(const QByteArray& data) { return data.toBase64().toStdString(); }

std::string execLine(const QByteArray& source) {
    return "exec(__import__('base64').b64decode('" + b64(source) + "').decode('utf-8'), __rk_g)";
}

QString trimmedError(const std::string& err) { return QString::fromUtf8(err).trimmed(); }

}  // namespace

BnPython::BnPython(Ref<BinaryView> view) : view_(std::move(view)) {}

BnPython::~BnPython() {
    std::lock_guard<std::mutex> run(runMu_);
    if (!instance_) return;
    instance_->UnregisterOutputListener(this);
    instance_->SetCurrentBinaryView(nullptr);
    instance_->Stop();
}

void BnPython::NotifyOutput(const std::string& text) {
    std::lock_guard<std::mutex> g(mu_);
    out_ += text;
}

void BnPython::NotifyWarning(const std::string& text) {
    std::lock_guard<std::mutex> g(mu_);
    err_ += text;
}

void BnPython::NotifyError(const std::string& text) {
    std::lock_guard<std::mutex> g(mu_);
    err_ += text;
}

void BnPython::NotifyInputReadyStateChanged(BNScriptingProviderInputReadyState state) {
    {
        std::lock_guard<std::mutex> g(mu_);
        state_ = state;
        if (state == ReadyForScriptExecution) ++readyCount_;
    }
    cv_.notify_all();
}

BnPython::LineResult BnPython::runLine(const std::string& line, const CancelFn& cancelled) {
    LineResult r;
    uint64_t start = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        out_.clear();
        err_.clear();
        start = readyCount_;
    }
    instance_->SetCurrentBinaryView(view_);
    switch (instance_->ExecuteScriptInput(line)) {
    case SuccessfulScriptExecution: break;
    case IncompleteScriptInput:
        r.error = QStringLiteral("Binary Ninja's Python interpreter rejected the command as incomplete");
        return r;
    case ScriptExecutionCancelled: r.cancelled = true; return r;
    case InvalidScriptInput:
    default:
        r.error = QStringLiteral("Binary Ninja's Python interpreter is busy with a previous script");
        return r;
    }

    using Clock = std::chrono::steady_clock;
    bool cancelSent = false;
    Clock::time_point giveUp;
    const bool onMain = BinaryNinja::IsMainThread();
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (readyCount_ > start) {
            r.finished = true;
            break;
        }
        if (state_ == ReadyForScriptProgramInput) {
            // the script reads stdin: answer with an empty line, as an exhausted console would
            lk.unlock();
            instance_->ExecuteScriptInput("\n");
            lk.lock();
            continue;
        }
        const bool stop = (cancelled && cancelled()) || Host::closing.load();
        if (stop && !cancelSent) {
            lk.unlock();
            instance_->CancelScriptInput();
            lk.lock();
            cancelSent = true;
            giveUp = Clock::now() + std::chrono::seconds(5);
            continue;
        }
        if (cancelSent && Clock::now() >= giveUp) break;
        if (onMain) {
            // The script may wait on the main thread (execute_on_main_thread_and_wait, UI calls): keep serving
            // posted main-thread work, but not user input, while it runs.
            lk.unlock();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
            lk.lock();
            cv_.wait_for(lk, std::chrono::milliseconds(10));
        } else {
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
    }
    r.cancelled = cancelSent;
    r.out = out_;
    r.err = err_;
    return r;
}

QString BnPython::ensureReady(const CancelFn& cancelled) {
    if (bootstrapped_) return {};
    if (!instance_) {
        Ref<ScriptingProvider> provider = ScriptingProvider::GetByName("Python");
        if (!provider) return QStringLiteral("Binary Ninja's Python scripting provider is not available");
        instance_ = provider->CreateNewInstance();
        if (!instance_) return QStringLiteral("Could not create a Binary Ninja Python interpreter");
        instance_->RegisterOutputListener(this);
        {
            std::lock_guard<std::mutex> g(mu_);
            state_ = instance_->GetInputReadyState();
        }
    }
    const QString prelude = rk::embeddedText(kPreludePath);
    hasPrelude_ = !prelude.isEmpty();
    std::string line = "__rk_g = {'__name__': '__rk__'}; " + execLine(QByteArray(kBootstrap));
    if (hasPrelude_) line += "; " + execLine(prelude.toUtf8());
    line += "; print('\\x02RKBOOT\\x02')";
    const LineResult r = runLine(line, cancelled);
    if (!r.error.isEmpty()) return r.error;
    if (r.cancelled) return QStringLiteral("Tool call cancelled");
    if (r.out.find(kBootMarker) == std::string::npos) {
        const QString err = trimmedError(r.err);
        return err.isEmpty() ? QStringLiteral("Rikugan's Python helpers failed to load") : err;
    }
    bootstrapped_ = true;
    return {};
}

PythonResult BnPython::runHelper(const std::string& call, const CancelFn& cancelled) {
    PythonResult res;
    const std::string line = "print('\\x02RKB\\x02' + " + call + " + '\\x02RKE\\x02')";
    const LineResult r = runLine(line, cancelled);
    if (!r.error.isEmpty()) {
        res.error = r.error;
        return res;
    }
    if (r.cancelled) {
        res.error = QStringLiteral("Tool call cancelled");
        return res;
    }
    const size_t end = r.out.rfind(kEnd);
    const size_t begin = end == std::string::npos ? std::string::npos : r.out.rfind(kBegin, end);
    if (begin == std::string::npos) {
        const QString err = trimmedError(r.err);
        res.error = err.isEmpty() ? QStringLiteral("Python produced no result") : err;
        return res;
    }
    const size_t from = begin + std::char_traits<char>::length(kBegin);
    res.ok = true;
    res.output = QString::fromUtf8(r.out.data() + from, qsizetype(end - from));
    return res;
}

PythonResult BnPython::runGuarded(const QString& code, uint64_t currentAddress,
                                  const CancelFn& cancelled) {
    std::lock_guard<std::mutex> run(runMu_);
    PythonResult res;
    if (const QString err = ensureReady(cancelled); !err.isEmpty()) {
        res.error = err;
        return res;
    }
    if (!hasPrelude_) {
        res.error = QStringLiteral("The Python guard prelude is not embedded in this build");
        return res;
    }
    instance_->SetCurrentAddress(currentAddress);
    const PythonResult raw = runHelper(
        "__rk_g['__rk_call']('" + b64(code.toUtf8()) + "', bv, " + std::to_string(currentAddress) + ")", cancelled);
    if (!raw.ok) return raw;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.output.toUtf8());
    if (!doc.isObject()) {
        res.error = QStringLiteral("Malformed result from the Python guard: %1").arg(raw.output.left(200));
        return res;
    }
    const QJsonObject o = doc.object();
    res.output = o.value(QStringLiteral("output")).toString();
    const QJsonValue error = o.value(QStringLiteral("error"));
    res.ok = error.isNull() || error.isUndefined();
    if (!res.ok) res.error = error.toString();
    return res;
}

PythonResult BnPython::runTrusted(const QString& source, const CancelFn& cancelled) {
    std::lock_guard<std::mutex> run(runMu_);
    PythonResult res;
    if (const QString err = ensureReady(cancelled); !err.isEmpty()) {
        res.error = err;
        return res;
    }
    return runHelper("__rk_g['__rk_trusted']('" + b64(source.toUtf8()) + "', bv)", cancelled);
}

}  // namespace rk::binja
