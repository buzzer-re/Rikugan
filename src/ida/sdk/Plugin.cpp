// IDA entry point: the Rikugan dock widget, database lifetime, cursor tracking, context-menu actions.
#include <memory>
#include <vector>

#include <QPointer>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <omc.h>

#include "common/agent/Actions.h"
#include "common/agent/DbContext.h"
#include "common/agent/Subagents.h"
#include "common/host/Host.h"
#include "common/omc/AgentHome.h"
#include "common/omc/OmcLibrary.h"
#include "common/omc/ServiceSession.h"
#include "common/tools/ToolDispatcher.h"
#include "common/ui/ChatHub.h"
#include "common/ui/PanelCore.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"
#include "ida/host/IdaHost.h"
#include "ida/host/IdaSdk.h"

namespace {

constexpr char kTitle[] = "Rikugan";

struct RikuganPlugmod;
RikuganPlugmod* g_mod = nullptr;

QString funcNameAt(ea_t ea, ea_t* start) {
    func_t* f = get_func(ea);
    if (!f) return {};
    *start = f->start_ea;
    qstring name;
    get_func_name(&name, f->start_ea);
    return QString::fromUtf8(name.c_str());
}

rk::ActionContext actionContext(TWidget* widget) {
    rk::ActionContext ctx;
    const ea_t ea = get_screen_ea();
    ctx.ea = ea;
    ea_t start = BADADDR;
    ctx.funcName = funcNameAt(ea, &start);
    if (start != BADADDR) ctx.funcEa = start;
    ea_t s = BADADDR, e = BADADDR;
    if (widget && read_range_selection(widget, &s, &e) && s != BADADDR && e != BADADDR && e > s)
        ctx.selectedText = QStringLiteral("0x%1-0x%2").arg(quint64(s), 0, 16).arg(quint64(e), 0, 16);
    return ctx;
}

struct RikuganPlugmod : public plugmod_t, public event_listener_t {
    TWidget* widget = nullptr;
    QPointer<rk::PanelCore> panel;
    std::shared_ptr<rk::DbContext> db;
    std::vector<std::unique_ptr<action_handler_t>> handlers;
    std::vector<std::string> actionNames;

    RikuganPlugmod() {
        rk::setLogSink([](const QString& m) { msg("%s\n", m.toUtf8().constData()); });
        rk::AgentHome::prepare();
        registerActions();
        ::hook_event_listener(HT_UI, this, this);
        ::hook_event_listener(HT_IDB, this, this);
        const char* idb = get_path(PATH_TYPE_IDB);
        if (idb && *idb) openDatabase();
        msg("Rikugan loaded (libomc %s)\n", omc_version());
    }

    // IDA dlcloses rikugan.so right after this while its event loop keeps running: everything that could still run
    // plugin code goes first (see ModuleLifetime.h).
    ~RikuganPlugmod() override {
        ::unhook_event_listener(HT_UI, this);
        ::unhook_event_listener(HT_IDB, this);
        rk::Host::closing = true;
        if (panel) {
            panel->setDatabase(nullptr);
            delete panel.data();  // now, with its pending events; close_widget below may defer deleting the dock
        }
        rk::ui::ChatHub::instance().closeAll();  // the chats outlive the dock, not the plugin
        db.reset();
        // subagents run their own sessions: they stop before the module goes
        rk::SubagentRegistry::instance().cancelChat(QString());
        for (const std::string& n : actionNames) unregister_action(n.c_str());
        if (widget) close_widget(widget, 0);
        rk::ServiceSession::shutdown(5000);
        rk::ToolDispatcher::instance().shutdown();
        rk::shutdownModule(5000);
        g_mod = nullptr;
    }

    bool idaapi run(size_t) override {
        togglePanel();
        return true;
    }

    rk::PanelCore* showPanel() {
        if (!widget) {
            widget = create_empty_widget(kTitle);
            auto* w = reinterpret_cast<QWidget*>(widget);
            auto* layout = new QVBoxLayout(w);
            layout->setContentsMargins(0, 0, 0, 0);
            panel = new rk::PanelCore(w);
            layout->addWidget(panel);
            panel->setDatabase(db);
            display_widget(widget, WOPN_DP_TAB | WOPN_RESTORE | WOPN_PERSIST);
        } else {
            activate_widget(widget, true);
        }
        return panel.data();
    }

    void togglePanel() {
        if (widget && panel && panel->isVisible()) {
            close_widget(widget, WCLS_SAVE);
            return;
        }
        showPanel();
    }

    void openDatabase() {
        rk::Host::closing = false;
        if (!db) db = rk::DbContext::create(std::make_unique<rk::ida::IdaHost>());
        if (panel) panel->setDatabase(db);
    }

    void closeDatabase() {
        // queued main-thread tool work must not run against a closing database
        rk::Host::closing = true;
        if (panel) panel->setDatabase(nullptr);
        if (db) rk::ui::ChatHub::instance().closeDatabase(db->dbId);  // its chats end with it (the dock's do not)
        db.reset();
    }

    ssize_t idaapi on_event(ssize_t code, va_list va) override {
        switch (code) {
        case ui_database_inited: openDatabase(); break;
        case idb_event::closebase: closeDatabase(); break;
        case ui_screen_ea_changed: {
            const ea_t ea = va_arg(va, ea_t);
            if (db) {
                static_cast<rk::ida::IdaHost*>(db->host.get())->setCurrentAddress(ea);
                if (panel) panel->setLocation(ea, db->host->currentFunctionLabel());
            }
            break;
        }
        case ui_widget_invisible: {
            TWidget* w = va_arg(va, TWidget*);
            if (w == widget) {
                widget = nullptr;  // IDA destroys the QWidget tree, PanelCore included
                panel = nullptr;
            }
            break;
        }
        case ui_finish_populating_widget_popup: {
            TWidget* w = va_arg(va, TWidget*);
            TPopupMenu* popup = va_arg(va, TPopupMenu*);
            const int type = get_widget_type(w);
            const bool disasm = type == BWN_DISASM, pseudo = type == BWN_PSEUDOCODE;
            if (!disasm && !pseudo) break;
            for (const rk::ActionDef& def : rk::idaActionDefs())
                if ((disasm && def.disasm) || (pseudo && def.pseudo))
                    attach_action_to_popup(w, popup, ("rikugan:" + def.id.toStdString()).c_str(), "Rikugan/");
            attach_action_to_popup(w, popup, "rikugan:send_to_bulk_rename", "Rikugan/");
            attach_action_to_popup(w, popup, "rikugan:open_tools", "Rikugan/");
            break;
        }
        default: break;
        }
        return 0;
    }

    struct PromptAction : public action_handler_t {
        rk::ActionDef def;
        explicit PromptAction(rk::ActionDef d) : def(std::move(d)) {}
        int idaapi activate(action_activation_ctx_t* ctx) override {
            if (!g_mod) return 0;
            const QString prompt = rk::actionPrompt(QStringLiteral("ida"), def.id, actionContext(ctx ? ctx->widget : nullptr));
            rk::PanelCore* p = g_mod->showPanel();
            if (!p) return 0;
            if (def.autoSubmit) p->submitToCurrentChat(prompt);
            else p->prefillComposer(prompt);
            return 1;
        }
        action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
    };

    struct OpenToolsAction : public action_handler_t {
        int idaapi activate(action_activation_ctx_t*) override {
            if (g_mod)
                if (rk::PanelCore* p = g_mod->showPanel()) p->showTools();
            return 1;
        }
        action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
    };

    struct BulkRenameAction : public action_handler_t {
        int idaapi activate(action_activation_ctx_t*) override {
            if (!g_mod) return 0;
            func_t* f = get_func(get_screen_ea());
            if (!f) return 0;
            if (rk::PanelCore* p = g_mod->showPanel()) p->addToBulkRenamer({quint64(f->start_ea)});
            return 1;
        }
        action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
    };

    void addAction(const std::string& name, const QString& label, action_handler_t* h, const QString& shortcut,
                   const QString& tooltip) {
        handlers.emplace_back(h);
        actionNames.push_back(name);
        // action_desc_t keeps pointers: the strings live in actionNames / static storage below
        static std::vector<std::unique_ptr<std::string>> strings;
        strings.push_back(std::make_unique<std::string>(label.toStdString()));
        const char* lbl = strings.back()->c_str();
        strings.push_back(std::make_unique<std::string>(shortcut.toStdString()));
        const char* sc = strings.back()->empty() ? nullptr : strings.back()->c_str();
        strings.push_back(std::make_unique<std::string>(tooltip.toStdString()));
        const char* tt = strings.back()->c_str();
        const action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(actionNames.back().c_str(), lbl, h, this, sc, tt, -1);
        register_action(desc);
    }

    void registerActions() {
        for (const rk::ActionDef& def : rk::idaActionDefs())
            addAction("rikugan:" + def.id.toStdString(), def.label, new PromptAction(def), def.shortcut, def.tooltip);
        addAction("rikugan:open_tools", QStringLiteral("Bulk Renamer"), new OpenToolsAction, {}, QStringLiteral("Open Rikugan's bulk renamer"));
        attach_action_to_menu("Edit/Plugins/Rikugan/", "rikugan:open_tools", SETMENU_APP);
        addAction("rikugan:send_to_bulk_rename", QStringLiteral("Send to Bulk Rename"), new BulkRenameAction, {},
                  QStringLiteral("Send function to Rikugan Bulk Renamer"));
    }
};

plugmod_t* idaapi rk_init() {
    if (!is_idaq()) return nullptr;  // the panel needs IDA's Qt UI
    QString error;
    if (!rk::loadOmcLibrary(&error)) {
        msg("Rikugan: %s\n", error.toUtf8().constData());
        return nullptr;
    }
    g_mod = new RikuganPlugmod;
    return g_mod;
}

}  // namespace

plugin_t PLUGIN = {IDP_INTERFACE_VERSION, PLUGIN_MULTI | PLUGIN_FIX, rk_init, nullptr, nullptr,
                   "Rikugan", "", "Rikugan", "Ctrl+Shift+I"};
