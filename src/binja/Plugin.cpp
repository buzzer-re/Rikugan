// Binary Ninja entry points: the Rikugan sidebar, view/address tracking, context-menu commands.
#include <map>
#include <memory>

#include <QImage>
#include <QPointer>
#include <QVBoxLayout>

#include <binaryninjaapi.h>
#include <sidebar.h>
#include <sidebarwidget.h>
#include <theme.h>
#include <uicontext.h>
#include <uitypes.h>
#include <viewframe.h>

#include <omc.h>

#include "binja/host/BnHost.h"
#include "binja/host/BnHostMcp.h"
#include "common/agent/Actions.h"
#include "common/agent/Databases.h"
#include "common/agent/DbContext.h"
#include "common/omc/AgentHome.h"
#include "common/omc/OmcLibrary.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/ChatHub.h"
#include "common/ui/PanelCore.h"
#include "common/ui/Theme.h"
#include "common/util/Log.h"

using namespace BinaryNinja;

namespace {

constexpr char kSidebarName[] = "Rikugan";

// One DbContext per open file, created when the panel first shows it (UI thread only).
std::map<BNFileMetadata*, std::shared_ptr<rk::DbContext>> g_dbs;
QPointer<rk::PanelCore> g_panel;

std::shared_ptr<rk::DbContext> dbFor(const BinaryViewRef& bv) {
    if (!bv) return nullptr;
    BNFileMetadata* key = bv->GetFile()->GetObject();
    auto it = g_dbs.find(key);
    if (it != g_dbs.end()) return it->second;
    auto db = rk::DbContext::create(std::make_unique<rk::binja::BnHost>(bv));
    g_dbs[key] = db;
    return db;
}

rk::binja::BnHost* bnHost(const std::shared_ptr<rk::DbContext>& db) {
    return db ? static_cast<rk::binja::BnHost*>(db->host.get()) : nullptr;
}

void showLocation(const std::shared_ptr<rk::DbContext>& db, uint64_t offset) {
    if (!db) return;
    bnHost(db)->setCurrentLocation(offset);
    if (g_panel && g_panel->database() == db) g_panel->setLocation(offset, db->host->currentFunctionLabel());
}

// The panel's "Open a binary…" and recent chats: a tab already showing that database comes to the front.
bool openInBinja(QWidget* anchor, const QString& file, const QString& dbId) {
    UIContext* ui = UIContext::contextForWidget(anchor);
    if (!ui) ui = UIContext::activeContext();
    if (!ui) return false;
    if (!dbId.isEmpty())
        for (QWidget* tab : ui->getTabs()) {
            ViewFrame* frame = ui->getViewFrameForTab(tab);
            const BinaryViewRef bv = frame ? frame->getCurrentBinaryView() : nullptr;
            if (bv && rk::binja::BnHost::storedInstanceId(*bv) == dbId) {
                ui->activateTab(tab);
                return true;
            }
        }
    return ui->openFilename(file);
}

class RikuganSidebarWidget : public SidebarWidget {
public:
    RikuganSidebarWidget(ViewFrame* frame) : SidebarWidget(kSidebarName) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        panel_ = new rk::PanelCore(this);
        layout->addWidget(panel_);
        g_panel = panel_;
        panel_->setFileOpener(QStringLiteral("binja"),
                              [this](const QString& file, const QString& dbId) { return openInBinja(this, file, dbId); });
        notifyViewChanged(frame);
    }

    void notifyViewChanged(ViewFrame* frame) override {
        const BinaryViewRef bv = frame ? frame->getCurrentBinaryView() : nullptr;
        auto db = dbFor(bv);
        if (panel_->database() != db) panel_->setDatabase(db);
    }

    void notifyViewLocationChanged(View*, const ViewLocation& location) override {
        showLocation(panel_->database(), location.getOffset());
    }

private:
    rk::PanelCore* panel_;
};

class RikuganSidebarType : public SidebarWidgetType {
public:
    RikuganSidebarType()
        : SidebarWidgetType(QImage(QStringLiteral(":/rikugan/rikugan_icon_light.png")), QString::fromLatin1(kSidebarName)) {}
    SidebarWidget* createWidget(ViewFrame* frame, BinaryViewRef) override { return new RikuganSidebarWidget(frame); }
    SidebarWidgetLocation defaultLocation() const override { return LeftContent; }
    SidebarContextSensitivity contextSensitivity() const override { return GlobalSidebarContext; }
};

class Notifications : public UIContextNotification {
public:
    bool OnBeforeCloseFile(UIContext*, FileContext* file, ViewFrame*) override {
        BNFileMetadata* key = file->getMetadata()->GetObject();
        auto it = g_dbs.find(key);
        if (it != g_dbs.end()) {
            if (g_panel && g_panel->database() == it->second) g_panel->setDatabase(nullptr);
            rk::ui::ChatHub::instance().closeDatabase(it->second->dbId);  // its chats end with the file
            g_dbs.erase(it);
        }
        return true;
    }
    void OnAfterSaveFile(UIContext*, FileContext* file, ViewFrame*) override {
        // "Save as" moves the database: its chats are reopened from the new file
        auto it = g_dbs.find(file->getMetadata()->GetObject());
        if (it != g_dbs.end()) it->second->recordLocation();
    }
    void OnAddressChange(UIContext*, ViewFrame* frame, View*, const ViewLocation& location) override {
        if (!frame) return;
        auto it = g_dbs.find(frame->getCurrentBinaryView() ? frame->getCurrentBinaryView()->GetFile()->GetObject() : nullptr);
        if (it != g_dbs.end()) showLocation(it->second, location.getOffset());
    }
};

// The panel, shown (the sidebar widget is created on first activation).
rk::PanelCore* openPanel() {
    if (Sidebar* sb = Sidebar::current()) sb->activate(QString::fromLatin1(kSidebarName));
    return g_panel.data();
}

rk::ActionContext actionContext(BinaryView* bv, uint64_t addr) {
    rk::ActionContext ctx;
    ctx.ea = addr;
    const auto funcs = bv->GetAnalysisFunctionsContainingAddress(addr);
    if (!funcs.empty()) {
        ctx.funcEa = funcs.front()->GetStart();
        if (Ref<Symbol> sym = funcs.front()->GetSymbol()) ctx.funcName = QString::fromStdString(sym->GetFullName());
    }
    return ctx;
}

void runAction(const rk::ActionDef& def, BinaryView* bv, uint64_t addr) {
    const rk::ActionContext ctx = actionContext(bv, addr);
    const QString prompt = rk::actionPrompt(QStringLiteral("binja"), def.id, ctx);
    Ref<BinaryView> keep = bv;
    ExecuteOnMainThread([def, prompt, keep] {
        rk::PanelCore* panel = openPanel();
        if (!panel) return;
        if (def.autoSubmit) panel->submitToCurrentChat(prompt);
        else panel->prefillComposer(prompt);
    });
}

void registerCommands() {
    PluginCommand::Register("Rikugan\\Open Panel", "Open the Rikugan panel",
                            [](BinaryView*) { ExecuteOnMainThread([] { openPanel(); }); });
    PluginCommand::Register("Rikugan\\Bulk Renamer", "Open Rikugan's bulk renamer", [](BinaryView*) {
        ExecuteOnMainThread([] {
            if (rk::PanelCore* p = openPanel()) p->showTools();
        });
    });
    for (const rk::ActionDef& def : rk::binjaActionDefs()) {
        const std::string name = "Rikugan\\" + def.label.toStdString();
        PluginCommand::RegisterForAddress(name, def.tooltip.toStdString(),
                                          [def](BinaryView* bv, uint64_t addr) { runAction(def, bv, addr); });
    }
    PluginCommand::RegisterForFunction("Rikugan\\Send to Bulk Rename", "Send function to Rikugan Bulk Renamer",
                                       [](BinaryView*, Function* func) {
                                           const quint64 start = func->GetStart();
                                           ExecuteOnMainThread([start] {
                                               if (rk::PanelCore* p = openPanel()) p->addToBulkRenamer({start});
                                           });
                                       });
}

}  // namespace

extern "C" {
BN_DECLARE_CORE_ABI_VERSION
BN_DECLARE_UI_ABI_VERSION

BINARYNINJAPLUGIN bool CorePluginInit() { return true; }

BINARYNINJAPLUGIN bool UIPluginInit() {
    rk::setLogSink([](const QString& m) { LogInfo("%s", m.toUtf8().constData()); });
    if (QString error; !rk::loadOmcLibrary(&error)) {
        LogError("Rikugan: %s", error.toUtf8().constData());
        return false;
    }
    rk::AgentHome::prepare();
    // the panel speaks Binary Ninja's colours: its addresses, symbols, highlight hues and code tokens
    rk::ui::setHostColors([] {
        return QHash<QString, QColor>{
            {"host", getThemeColor(BlueStandardHighlightColor)},     {"workspace", getThemeColor(OrangeStandardHighlightColor)},
            {"web", getThemeColor(CyanStandardHighlightColor)},      {"agent", getThemeColor(MagentaStandardHighlightColor)},
            {"ok", getThemeColor(GreenStandardHighlightColor)},      {"warn", getThemeColor(YellowStandardHighlightColor)},
            {"err", getThemeColor(RedStandardHighlightColor)},       {"addr", getThemeColor(AddressColor)},
            {"sym", getThemeColor(CodeSymbolColor)},                 {"synComment", getThemeColor(CommentColor)},
            {"synString", getThemeColor(StringColor)},               {"synNumber", getThemeColor(NumberColor)},
            {"synKeyword", getThemeColor(KeywordColor)},             {"synBuiltin", getThemeColor(TypeNameColor)},
            {"synMeta", getThemeColor(DataSymbolColor)},
        };
    });
    // open_database: the file is analysed in the background, with no tab, so the user's view and the panel's chats
    // stay where they are
    rk::databases::setLoader([](const QString& path, const std::shared_ptr<rk::CancelToken>& token,
                                QString* error) -> std::shared_ptr<rk::DbContext> {
        Ref<BinaryView> bv = Load(path.toStdString(), true, "{}", [token](size_t, size_t) { return !(token && token->cancelled()); });
        if (!bv) {
            if (error) *error = token && token->cancelled() ? QStringLiteral("cancelled") : QStringLiteral("Binary Ninja could not load it");
            return nullptr;
        }
        std::shared_ptr<rk::DbContext> db;
        ExecuteOnMainThreadAndWait([&] { db = rk::DbContext::create(std::make_unique<rk::binja::BnHost>(bv, true)); });
        return db;
    });
    // Binary Ninja's own MCP server: its tools can stand in for Rikugan's once the user agrees (HostMcpBar)
    rk::binja::installHostMcp();
    Sidebar::addSidebarWidgetType(new RikuganSidebarType);
    UIContext::registerNotification(new Notifications);
    registerCommands();
    LogInfo("Rikugan loaded (libomc %s)", omc_version());
    return true;
}
}
