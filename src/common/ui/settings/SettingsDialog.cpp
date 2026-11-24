#include "common/ui/settings/SettingsDialog.h"

#include <algorithm>

#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QMessageBox>
#include <QScreen>
#include <QTabWidget>
#include <QVBoxLayout>

#include "common/agent/HostMcp.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AgentHome.h"
#include "common/omc/AuthSync.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/settings/BehaviorTab.h"
#include "common/ui/settings/McpTab.h"
#include "common/ui/settings/ProviderTab.h"
#include "common/ui/settings/ProfilesTab.h"
#include "common/ui/settings/SkillsTab.h"

namespace rk {

struct SettingsDialog::Impl {
    SettingsDialog* q;
    ProviderTab* models = nullptr;
    BehaviorTab* behavior = nullptr;
    SkillsTab* skills = nullptr;
    McpTab* mcp = nullptr;
    ProfilesTab* profiles = nullptr;

    void accept();
};

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent), d(std::make_unique<Impl>()) {
    d->q = this;
    setWindowTitle(tr("Rikugan Settings"));
    setWindowModality(Qt::ApplicationModal);
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        resize(std::min(int(avail.width() * 0.45), 900), std::min(int(avail.height() * 0.7), 800));
    } else {
        resize(700, 600);
    }
    setMinimumWidth(400);

    const RikuganConfig& cfg = RikuganConfig::instance();
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget;
    d->models = new ProviderTab(cfg);
    tabs->addTab(d->models, tr("Provider"));
    d->behavior = new BehaviorTab(cfg);
    tabs->addTab(d->behavior, tr("Behavior"));
    d->skills = new SkillsTab(cfg);
    tabs->addTab(d->skills, tr("Skills"));
    d->mcp = new McpTab(cfg);
    tabs->addTab(d->mcp, tr("MCP"));
    d->profiles = new ProfilesTab(cfg);
    tabs->addTab(d->profiles, tr("Profiles"));
    layout->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { d->accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::Impl::accept() {
    for (const QString& e : {models->validate(), mcp->validate()}) {
        if (e.isEmpty()) continue;
        QMessageBox::warning(q, tr("Rikugan Settings"), e);
        return;
    }
    RikuganConfig c = RikuganConfig::instance();
    const bool hadOmcLogins = c.useOmcLogins;
    models->apply(c);
    behavior->apply(c);
    skills->apply(c);
    profiles->apply(c);
    mcp->apply(c);
    c.clamp();

    if (const QString e = mcp->save(); !e.isEmpty()) {
        QMessageBox::warning(q, tr("Rikugan Settings"), e);
        return;
    }
    if (!c.save()) {
        QMessageBox::warning(q, tr("Rikugan Settings"), tr("Could not save the configuration."));
        return;
    }
    RikuganConfig::instance() = c;
    HostMcp::instance().setUrlOverride(c.hostMcpUrl);
    AgentHome::writeModelsYaml();
    AgentHome::syncSkills();
    if (c.useOmcLogins && !hadOmcLogins)
        ServiceSession::get()->post([](OmcSession&) { AuthSync::run(); });  // file work, off the UI thread
    Q_EMIT q->settingsChanged();
    q->QDialog::accept();
}

}  // namespace rk
