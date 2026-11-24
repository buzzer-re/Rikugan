#include "binja/host/BnHost.h"

#include <QUuid>

#include <uicontext.h>
#include <viewframe.h>

#include "binja/host/BnPython.h"
#include "binja/tools/BnTools.h"
#include "common/agent/RecentSessions.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

constexpr const char* kInstanceIdKey = "rikugan_db_id";

}  // namespace

BnHost::BnHost(Ref<BinaryView> view, bool ownsFile) : view_(std::move(view)), ownsFile_(ownsFile) {}

BnHost::~BnHost() {
    python_.reset();  // its interpreter holds the view
    if (ownsFile_ && view_) {
        Ref<FileMetadata> file = view_->GetFile();
        view_ = nullptr;
        if (file) file->Close();  // the views and their analysis go with it
    }
}

void BnHost::setCurrentLocation(uint64_t addr) {
    location_.store(addr);
    hasLocation_.store(true);
}

QString BnHost::hostName() const { return QStringLiteral("binja"); }

QString BnHost::databasePath() const {
    Ref<FileMetadata> file = view_->GetFile();
    return file ? QString::fromStdString(file->GetFilename()) : QString();
}

QString BnHost::inputPath() const {
    Ref<FileMetadata> file = view_->GetFile();
    return file ? QString::fromStdString(file->GetOriginalFilename()) : QString();
}

QString BnHost::storedInstanceId(BinaryView& view) {
    Ref<Metadata> md = view.QueryMetadata(kInstanceIdKey);
    return md && md->IsString() ? QString::fromStdString(md->GetString()) : QString();
}

QString BnHost::databaseInstanceId() {
    std::lock_guard<std::mutex> g(idMu_);
    if (!instanceId_.isEmpty()) return instanceId_;
    instanceId_ = storedInstanceId(*view_);
    if (!instanceId_.isEmpty()) return instanceId_;
    // a binary reopened from the panel's recent chats continues them; anything else starts fresh
    instanceId_ = takeExpectedSession(databasePath());
    if (instanceId_.isEmpty()) instanceId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    view_->StoreMetadata(kInstanceIdKey, new Metadata(instanceId_.toStdString()));
    return instanceId_;
}

std::optional<uint64_t> BnHost::uiAddress() const {
    // the active view frame's offset when no location was reported
    UIContext* ctx = UIContext::activeContext();
    if (!ctx) return std::nullopt;
    ViewFrame* frame = ctx->getCurrentViewFrame();
    if (!frame) return std::nullopt;
    BinaryViewRef shown = frame->getCurrentBinaryView();
    if (!shown || shown->GetFile() != view_->GetFile()) return std::nullopt;
    return frame->getCurrentOffset();
}

std::optional<uint64_t> BnHost::currentAddress() const {
    if (hasLocation_.load()) return location_.load();
    auto* self = const_cast<BnHost*>(this);
    try {
        return onMain(*self, MainAccess::Read, [this] { return uiAddress(); });
    } catch (const ToolError&) {
        return std::nullopt;
    }
}

QString BnHost::currentFunctionLabel() const {
    const std::optional<uint64_t> ea = currentAddress();
    if (!ea) return {};
    Ref<Function> func = functionAt(view_, *ea);
    return func ? functionName(func) : QString();
}

bool BnHost::navigateTo(uint64_t ea) {
    // BinaryView::Navigate for the known view types, then the active view frame
    const bool ok = onMain(*this, MainAccess::Read, [&]() -> bool {
        for (const char* viewType : {"Graph:IL", "Graph:Disassembly", "Linear:Disassembly", "Linear"})
            if (view_->Navigate(viewType, ea)) return true;
        UIContext* ctx = UIContext::activeContext();
        ViewFrame* frame = ctx ? ctx->getCurrentViewFrame() : nullptr;
        return frame && frame->navigate(view_, ea);
    });
    if (ok) setCurrentLocation(ea);
    return ok;
}

void BnHost::navigate(uint64_t ea) { navigateTo(ea); }

bool BnHost::runOnMainThread(const std::function<void()>& fn, MainAccess) {
    if (BinaryNinja::IsMainThread()) {
        fn();
        return true;
    }
    if (Host::closing.load()) return false;
    auto call = std::make_shared<MainThreadCall>(fn);
    BinaryNinja::ExecuteOnMainThread([call] { call->runOnMain(); });
    return call->wait();
}

bool BnHost::isMainThread() const { return BinaryNinja::IsMainThread(); }

bool BnHost::hasDecompiler() const { return true; }

QString BnHost::binaryInfo() { return binaryInfoText(view_); }

void BnHost::registerTools(ToolRegistry& registry) { registerAllTools(registry); }

BnPython& BnHost::python() {
    std::lock_guard<std::mutex> g(pythonMu_);
    if (!python_) python_ = std::make_unique<BnPython>(view_);
    return *python_;
}

PythonResult BnHost::runPython(const QString& code, const std::shared_ptr<CancelToken>& token) {
    return runGuardedPython(code, [token] { return token && token->cancelled(); });
}

PythonResult BnHost::runGuardedPython(const QString& code, const std::function<bool()>& cancelled) {
    return python().runGuarded(code, currentAddress().value_or(0), cancelled);
}

PythonResult BnHost::runTrustedPython(const QString& source, const std::function<bool()>& cancelled) {
    return python().runTrusted(source, cancelled);
}

bool BnHost::writeBytes(uint64_t ea, const QByteArray& bytes) {
    return view_->Write(ea, bytes.constData(), size_t(bytes.size())) == size_t(bytes.size());
}

QByteArray BnHost::readBytes(uint64_t ea, size_t n) {
    QByteArray out(qsizetype(n), '\0');
    const size_t got = view_->Read(out.data(), ea, n);
    out.truncate(qsizetype(got));
    return out;
}

}  // namespace rk::binja
