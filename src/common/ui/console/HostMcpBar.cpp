#include "common/ui/console/HostMcpBar.h"

#include <algorithm>
#include <iterator>

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include "common/agent/ChatSession.h"
#include "common/agent/DbContext.h"
#include "common/agent/RecentSessions.h"
#include "common/ui/ChatHub.h"
#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

// between silent looks for a server that did not answer: soon after the database shows (the server may be starting),
// then rarely
constexpr int kRetryDelaysMs[] = {2000, 5000, 10000, 30000, 60000};

}  // namespace

HostMcpBar::HostMcpBar(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("rk_mcpbar"));
    setAttribute(Qt::WA_StyledBackground);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 5, 8, 5);
    row->setSpacing(6);
    text_ = new QLabel(this);
    text_->setWordWrap(true);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(text_, 1);
    yes_ = new QPushButton(this);
    yes_->setProperty("rkPrimary", true);
    yes_->setCursor(Qt::PointingHandCursor);
    row->addWidget(yes_, 0, Qt::AlignTop);
    no_ = new QPushButton(this);
    no_->setCursor(Qt::PointingHandCursor);
    row->addWidget(no_, 0, Qt::AlignTop);
    connect(yes_, &QPushButton::clicked, this, [this] {
        hide();
        if (auto f = std::move(onYes_)) f();
    });
    connect(no_, &QPushButton::clicked, this, [this] {
        hide();
        if (auto f = std::move(onNo_)) f();
    });
    retry_ = new QTimer(this);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, &HostMcpBar::recheck);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &HostMcpBar::restyle);
    restyle();
    hide();
}

void HostMcpBar::restyle() {
    const Theme& t = theme();
    text_->setFont(t.uiSmall);
    setStyleSheet(QStringLiteral("#rk_mcpbar { background: %1; border-bottom: 1px solid %2; } #rk_mcpbar QLabel { color: %3; }")
                      .arg(t.accentSoft.name(), t.line.name(), t.fg.name()));
}

bool HostMcpBar::available() const { return HostMcp::instance().supported() && !db_.expired(); }

bool HostMcpBar::on() const {
    const auto db = db_.lock();
    return db && db->hostMcp.load();
}

QString HostMcpBar::tooltip() const {
    const QString server = HostMcp::instance().serverName();
    if (probing_) return tr("Looking for %1's MCP server\u2026").arg(server);
    if (on())
        return tr("Using %1's own MCP tools (%2) instead of Rikugan's analysis tools. Their changes are not in /undo.\n"
                  "Click to go back to Rikugan's tools.")
            .arg(server)
            .arg(HostMcp::instance().toolCount());
    return tr("Use %1's own MCP server tools in this database's chats.").arg(server);
}

void HostMcpBar::setDatabase(const std::shared_ptr<DbContext>& db) {
    if (db_.lock() == db) return;
    db_ = db;
    retry_->stop();
    retries_ = 0;
    hide();
    Q_EMIT stateChanged();
    recheck();
}

void HostMcpBar::recheck() {
    const auto db = db_.lock();
    if (!db || probing_ || db->hostMcp.load() || !HostMcp::instance().supported() || found_.contains(db->dbId)) return;
    // an earlier no: the button is there when the user changes their mind
    if (hostMcpConsent(db->sessionsDir()) == false) return;
    probe(false);
}

void HostMcpBar::toggle() {
    const auto db = db_.lock();
    if (!db || probing_) return;
    if (db->hostMcp.load()) {
        hide();
        apply(*db, false, true);
        Q_EMIT stateChanged();
        return;
    }
    probe(true);
}

void HostMcpBar::probe(bool asked) {
    if (probing_) return;
    probing_ = true;
    Q_EMIT stateChanged();
    std::weak_ptr<DbContext> target = db_;
    QPointer<HostMcpBar> self(this);
    HostMcp::instance().probe([self, target, asked](const HostMcp::Probe& p) {
        if (self) self->probed(target, asked, p);
    });
}

void HostMcpBar::probed(const std::weak_ptr<DbContext>& target, bool asked, const HostMcp::Probe& p) {
    probing_ = false;
    const auto db = target.lock();
    const bool current = db && db == db_.lock();
    const QString server = HostMcp::instance().serverName();
    if (db && p.ok && p.tools > 0) {
        found_.insert(db->dbId);
        const std::optional<bool> consent = hostMcpConsent(db->sessionsDir());
        if (asked || consent == true) {
            // clicking the button is the consent
            apply(*db, true, asked);
        } else if (current && !consent) {
            std::weak_ptr<DbContext> w = db;
            notify(tr("%1's MCP server is running with %2 tools. Use them in this database's chats instead of "
                    "Rikugan's analysis tools? Changes made with them are not in /undo.")
                     .arg(server)
                     .arg(p.tools),
                 tr("Use them"),
                 [this, w] {
                     if (auto d = w.lock()) apply(*d, true, true);
                     Q_EMIT stateChanged();
                 },
                 tr("No"),
                 [w] {
                     if (auto d = w.lock()) setHostMcpConsent(d->sessionsDir(), false);
                 });
        }
    } else if (asked && current) {
        if (p.ok) {
            notify(tr("%1's MCP server at %2 offers no tools.").arg(server, p.url), QString(), {}, tr("Dismiss"));
        } else {
            // a server that does not answer: whatever the setting says (it may have been started from the menu, or
            // the setting turned on without a restart), starting it is the next step
            const QString why = p.enabled ? tr("%1's MCP server does not answer at %2.").arg(server, p.url)
                                          : tr("%1's MCP server is not running at %2 (\u201cui.mcp.enabled\u201d is off).")
                                                .arg(server, p.url);
            const QString where = tr("Its address can be changed in Rikugan's Settings \u2192 MCP.");
            notify(why + QLatin1Char(' ') + where + QLatin1Char(' ') + tr("Start it now?"), tr("Start it"),
                 [this] {
                     if (!HostMcp::instance().startServer()) {
                         notify(tr("%1 could not start its MCP server. Start it from Plugins \u2192 MCP \u2192 Start Server, "
                                  "or turn on \u201cui.mcp.enabled\u201d in its settings, then click MCP again.")
                                  .arg(HostMcp::instance().serverName()),
                              QString(), {}, tr("Dismiss"));
                         return;
                     }
                     // the server listens a moment after the command; this look is the user's, not a silent one
                     retry_->stop();
                     QTimer::singleShot(600, this, [this] { probe(true); });
                 },
                 tr("Dismiss"));
            text_->setToolTip(p.error);
        }
    }
    // the database changed while this probe ran: look for the one showing now
    if (!current) recheck();
    else scheduleRetry();
    Q_EMIT stateChanged();
}

void HostMcpBar::scheduleRetry() {
    const auto db = db_.lock();
    if (!db || db->hostMcp.load() || found_.contains(db->dbId) || !HostMcp::instance().supported() ||
        hostMcpConsent(db->sessionsDir()) == false) {
        retry_->stop();
        return;
    }
    retry_->start(kRetryDelaysMs[retries_]);
    retries_ = std::min(retries_ + 1, int(std::size(kRetryDelaysMs)) - 1);
}

void HostMcpBar::notify(const QString& text, const QString& yes, std::function<void()> onYes, const QString& no,
                      std::function<void()> onNo) {
    text_->setText(text);
    text_->setToolTip(QString());
    yes_->setText(yes);
    yes_->setVisible(!yes.isEmpty());
    no_->setText(no);
    no_->setVisible(!no.isEmpty());
    onYes_ = std::move(onYes);
    onNo_ = std::move(onNo);
    QFrame::show();
}

void HostMcpBar::apply(DbContext& db, bool on, bool remember) {
    if (on) HostMcp::instance().install(db.registry);
    db.hostMcp = on;
    if (remember) setHostMcpConsent(db.sessionsDir(), on);
    for (const LiveChat& chat : std::as_const(ChatHub::instance().chats(db.dbId)))
        if (chat.session) chat.session->hostToolsChanged();
}

}  // namespace rk::ui
