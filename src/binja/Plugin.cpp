// Binary Ninja entry points: the Rikugan sidebar, view/address tracking, context-menu commands.
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <binaryninjaapi.h>
#include <filecontext.h>
#include <pane.h>
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
#include "common/config/Paths.h"
#include "common/omc/AgentHome.h"
#include "common/omc/OmcLibrary.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/ChatHub.h"
#include "common/ui/PanelCore.h"
#include "common/ui/Theme.h"
#include "common/update/Updater.h"
#include "common/util/ModuleLifetime.h"
#include "common/util/CrashGuard.h"
#include "common/util/Log.h"

using namespace BinaryNinja;

namespace {

constexpr char kSidebarName[] = "Rikugan";

// A crash inside Rikugan: the crash log, and every open file saved as <file>.rikugan-recovered.bndb beside it.
void installCrashGuard() {
    rk::crash::Hooks h;
    h.crashDir = rk::paths::logsDir().toStdString();
    h.version = std::string("Rikugan ") + RIKUGAN_VERSION + ", libomc " + omc_version() + ", Binary Ninja " + GetVersionString();
    h.saveOnMain = []() -> std::string {
        std::string out;
        for (FileContext* fc : FileContext::getOpenFileContexts()) {
            BinaryViewRef bv;
            if (ViewFrame* vf = fc->getCurrentViewFrame()) bv = vf->getCurrentBinaryView();
            if (!bv)
                for (const BinaryViewRef& v : fc->getAllDataViews())
                    if (v && v->GetTypeName() != "Raw") {
                        bv = v;
                        break;
                    }
            if (!bv) continue;
            const std::string path = rk::crash::recoveryPath(fc->getMetadata()->GetFilename(), "bndb");
            out += (bv->CreateDatabase(path) ? path : "could not save " + path) + "\n";
        }
        return out;
    };
    h.postToMain = [](std::function<void()> job) { ExecuteOnMainThread(std::move(job)); };
    h.isMainThread = [] { return IsMainThread(); };
    h.reportPrevious = [](const std::string& log) {
        LogWarn("Rikugan crashed in an earlier session. Crash log: %s", log.c_str());
    };
    rk::crash::install(std::move(h));
}

// One DbContext per open file, created when the panel first shows it (UI thread only).
std::map<BNFileMetadata*, std::shared_ptr<rk::DbContext>> g_dbs;
QPointer<rk::PanelCore> g_panel;  // the sidebar's panel: context-menu actions go there
// every panel: the sidebar's, and the ones opened as a full-window tab or as a pane
std::vector<QPointer<rk::PanelCore>> g_panels;

void forEachPanel(const std::function<void(rk::PanelCore&)>& fn) {
    std::erase_if(g_panels, [](const QPointer<rk::PanelCore>& p) { return p.isNull(); });
    for (const QPointer<rk::PanelCore>& p : g_panels) fn(*p);
}

std::shared_ptr<rk::DbContext> dbFor(const BinaryViewRef& bv) {
    if (!bv) return nullptr;
    BNFileMetadata* key = bv->GetFile()->GetObject();
    auto it = g_dbs.find(key);
    if (it != g_dbs.end()) return it->second;
    auto db = rk::DbContext::create(std::make_unique<rk::binja::BnHost>(bv));
    g_dbs[key] = db;
    rk::ui::ChatHub::instance().notifyDatabasesChanged();
    return db;
}

constexpr const char* kFullWindowTab = "rikugan_full_window";

QWidget* fullWindowTab(UIContext* ui) {
    for (QWidget* tab : ui->getTabs())
        if (tab->objectName() == QLatin1String(kFullWindowTab)) return tab;
    return nullptr;
}

// A Binary Ninja window for UI work started outside a click: the active one, which is null while another program has
// the focus, else the one showing Rikugan, else any.
UIContext* someWindow() {
    if (UIContext* ui = UIContext::activeContext()) return ui;
    for (const QPointer<rk::PanelCore>& p : g_panels)
        if (p)
            if (UIContext* ui = UIContext::contextForWidget(p)) return ui;
    const std::set<UIContext*> all = UIContext::allContexts();
    return all.empty() ? nullptr : *all.begin();
}

// open_database: Binary Ninja opens a file's tab asynchronously (a large binary loads after
// openFilename returns), so the tool waits for OnAfterOpenFile to hand over the new tab's database, by file path.
struct PendingTabs {
    std::mutex mu;
    std::condition_variable cv;
    std::map<QString, std::shared_ptr<rk::DbContext>> opened;  // path -> its database (null: waiting)
};
PendingTabs g_pendingTabs;

QString samePath(const QString& path) {
    const QFileInfo fi(path);
    const QString c = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
#ifdef _WIN32
    return c.toLower();
#else
    return c;
#endif
}

// The full-window chat manager lists every view: each open tab's database exists once it is open.
void registerTabs(UIContext* ui) {
    for (QWidget* tab : ui->getTabs())
        if (ViewFrame* frame = ui->getViewFrameForTab(tab)) dbFor(frame->getCurrentBinaryView());
}

rk::binja::BnHost* bnHost(const std::shared_ptr<rk::DbContext>& db) {
    return db ? static_cast<rk::binja::BnHost*>(db->host.get()) : nullptr;
}

void showLocation(const std::shared_ptr<rk::DbContext>& db, uint64_t offset) {
    if (!db) return;
    bnHost(db)->setCurrentLocation(offset);
    forEachPanel([&](rk::PanelCore& p) {
        if (p.database() == db) p.setLocation(offset, db->host->currentFunctionLabel());
    });
}

// The panel's "Open a binary…" and recent chats: a tab already showing that database comes to the front.
bool openInBinja(QWidget* anchor, const QString& file, const QString& dbId) {
    UIContext* ui = UIContext::contextForWidget(anchor);
    if (!ui) ui = someWindow();
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

void openFullWindow(QWidget* from, const std::shared_ptr<rk::DbContext>& db);

// A panel for `bv`'s database inside `parent` (the sidebar, a pane; the full-window tab sets its own toggle). Its
// header button opens the full-window tab.
rk::PanelCore* newPanel(QWidget* parent, const BinaryViewRef& bv) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new rk::PanelCore(parent);
    layout->addWidget(panel);
    panel->setFileOpener(QStringLiteral("binja"),
                         [parent](const QString& file, const QString& dbId) { return openInBinja(parent, file, dbId); });
    if (auto db = dbFor(bv)) panel->setDatabase(db);
    QPointer<rk::PanelCore> keep = panel;
    panel->setWindowToggle(false, [keep] {
        if (keep) openFullWindow(keep, keep->database());
    });
    g_panels.emplace_back(panel);
    return panel;
}

class RikuganSidebarWidget : public SidebarWidget {
public:
    RikuganSidebarWidget(ViewFrame* frame) : SidebarWidget(kSidebarName) {
        panel_ = newPanel(this, frame ? frame->getCurrentBinaryView() : nullptr);
        g_panel = panel_;
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
    // "Open in pane": the panel beside the disassembly, as large as the user makes it
    bool canUseAsPane(SplitPaneWidget*, BinaryViewRef) const override { return true; }
    Pane* createPane(SplitPaneWidget*, BinaryViewRef data) override {
        auto* host = new QWidget;
        newPanel(host, data);
        return new WidgetPane(host, QString::fromLatin1(kSidebarName));
    }
    SidebarWidgetLocation defaultLocation() const override { return LeftContent; }
    SidebarContextSensitivity contextSensitivity() const override { return GlobalSidebarContext; }
};

class Notifications : public UIContextNotification {
public:
    bool OnBeforeCloseFile(UIContext*, FileContext* file, ViewFrame*) override {
        BNFileMetadata* key = file->getMetadata()->GetObject();
        auto it = g_dbs.find(key);
        if (it != g_dbs.end()) {
            forEachPanel([&](rk::PanelCore& p) {
                if (p.database() == it->second) p.setDatabase(nullptr);
            });
            rk::ui::ChatHub::instance().closeDatabase(it->second->dbId);  // its chats end with the file
            g_dbs.erase(it);
            rk::ui::ChatHub::instance().notifyDatabasesChanged();
        }
        return true;
    }
    void OnAfterOpenFile(UIContext* context, FileContext* file, ViewFrame* frame) override {
        if (!frame) return;
        {
            std::lock_guard<std::mutex> g(g_pendingTabs.mu);
            auto it = g_pendingTabs.opened.find(samePath(file->getFilename()));
            if (it != g_pendingTabs.opened.end()) {
                it->second = dbFor(frame->getCurrentBinaryView());
                g_pendingTabs.cv.notify_all();
                // the agent opened it from the full-window chat: the chats stay in front
                if (QWidget* full = fullWindowTab(context)) context->activateTab(full);
                return;
            }
        }
        // the full-window chat manager lists it at once
        if (fullWindowTab(context)) dbFor(frame->getCurrentBinaryView());
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

// Back from the full-window tab `host`: the tab showing its database comes to the front with the Rikugan sidebar
// open, and the full-window tab closes.
void closeFullWindow(QWidget* host) {
    UIContext* ui = UIContext::contextForWidget(host);
    if (!ui) return;
    auto* panel = host->findChild<rk::PanelCore*>();
    const std::shared_ptr<rk::DbContext> db = panel ? panel->database() : nullptr;
    if (db)
        for (QWidget* tab : ui->getTabs()) {
            ViewFrame* frame = ui->getViewFrameForTab(tab);
            const BinaryViewRef bv = frame ? frame->getCurrentBinaryView() : nullptr;
            if (bv && dbFor(bv) == db) {
                ui->activateTab(tab);
                break;
            }
        }
    if (Sidebar* sb = ui->sidebar()) sb->activate(QString::fromLatin1(kSidebarName));
    ui->closeTab(host);
}

// The panel in a tab of its own, as large as Binary Ninja's window, for `db` (the one `from`'s window shows), and
// the Rikugan sidebar closes: one panel shows the chat. A tab already showing that database comes to the front.
void openFullWindow(QWidget* from, const std::shared_ptr<rk::DbContext>& db) {
    UIContext* ui = from ? UIContext::contextForWidget(from) : nullptr;
    if (!ui) ui = someWindow();
    if (!ui) return;
    registerTabs(ui);
    // one full-window tab per window: its manager reaches every database's chats
    if (QWidget* existing = fullWindowTab(ui)) {
        if (auto* panel = existing->findChild<rk::PanelCore*>(); panel && db) panel->setDatabase(db);
        ui->activateTab(existing);
    } else {
        auto* host = new QWidget;
        host->setObjectName(QLatin1String(kFullWindowTab));
        rk::PanelCore* panel = newPanel(host, nullptr);
        QPointer<QWidget> keep = host;
        // a binary opened from the manager gets its tab; the chats stay in front
        panel->setFileOpener(QStringLiteral("binja"), [keep](const QString& file, const QString& dbId) {
            if (!keep) return false;
            const bool opened = openInBinja(keep, file, dbId);
            if (UIContext* u = UIContext::contextForWidget(keep)) {
                registerTabs(u);
                u->activateTab(keep);
            }
            return opened;
        });
        panel->setManagerShown(true);
        if (db) panel->setDatabase(db);
        panel->setWindowToggle(true, [keep] {
            // the click comes from inside the tab: close it once the click has returned
            QTimer::singleShot(0, rk::uiAnchor(), [keep] {
                if (keep) closeFullWindow(keep);
            });
        });
        ui->createTabForWidget(QString::fromLatin1(kSidebarName), host);
    }
    if (Sidebar* sb = ui->sidebar()) sb->deactivate(QString::fromLatin1(kSidebarName));
}

void registerCommands() {
    PluginCommand::Register("Rikugan\\Open Panel", "Open the Rikugan panel",
                            [](BinaryView*) { ExecuteOnMainThread([] { openPanel(); }); });
    PluginCommand::Register("Rikugan\\Open in Full Window", "Open Rikugan in a tab of its own, as large as the window",
                            [](BinaryView* bv) {
                                BinaryViewRef keep = bv;
                                ExecuteOnMainThread([keep] { openFullWindow(nullptr, dbFor(keep)); });
                            });
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
    installCrashGuard();
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
    // open_database: a new tab of the user's, opened on the main thread; the full-window chats stay in front
    rk::databases::setTabOpener([](const QString& path, const std::shared_ptr<rk::CancelToken>& token,
                                   QString* error) -> std::shared_ptr<rk::DbContext> {
        const QString key = samePath(path);
        {
            std::lock_guard<std::mutex> g(g_pendingTabs.mu);
            g_pendingTabs.opened[key] = nullptr;
        }
        std::shared_ptr<rk::DbContext> db;
        bool window = false, started = false;
        ExecuteOnMainThreadAndWait([&] {
            UIContext* ui = someWindow();
            if (!ui) return;
            window = true;
            // a tab already showing it
            for (QWidget* tab : ui->getTabs()) {
                ViewFrame* frame = ui->getViewFrameForTab(tab);
                FileContext* fc = frame ? frame->getFileContext() : nullptr;
                if (fc && samePath(fc->getFilename()) == key) {
                    db = dbFor(frame->getCurrentBinaryView());
                    return;
                }
            }
            started = ui->openFilename(path);
        });
        if (!db && started) {
            // loading a large binary takes a while; the user's Stop ends the wait (the tab still opens)
            std::unique_lock<std::mutex> lk(g_pendingTabs.mu);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
            while (!g_pendingTabs.opened[key] && !(token && token->cancelled()) && std::chrono::steady_clock::now() < deadline)
                g_pendingTabs.cv.wait_for(lk, std::chrono::milliseconds(250));
            db = g_pendingTabs.opened[key];
        }
        {
            std::lock_guard<std::mutex> g(g_pendingTabs.mu);
            g_pendingTabs.opened.erase(key);
        }
        if (!db && error)
            *error = !window                       ? QStringLiteral("no Binary Ninja window to open it in")
                     : !started                    ? QStringLiteral("Binary Ninja refused to open it (see its log)")
                     : token && token->cancelled() ? QStringLiteral("cancelled while Binary Ninja was loading it")
                                                   : QStringLiteral("Binary Ninja is still loading it after 10 minutes");
        return db;
    });
    // Binary Ninja's own MCP server: its tools can stand in for Rikugan's once the user agrees (HostMcpBar)
    rk::binja::installHostMcp();
    Sidebar::addSidebarWidgetType(new RikuganSidebarType);
    UIContext::registerNotification(new Notifications);
    registerCommands();
    // a newer release, once a day (Settings > Behavior > Check for updates); release packages only
    rk::update::Updater::instance().start();
    LogInfo("Rikugan loaded (libomc %s)", omc_version());
    return true;
}
}
