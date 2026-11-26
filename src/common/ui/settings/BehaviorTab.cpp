#include "common/ui/settings/BehaviorTab.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "common/config/Paths.h"
#include "common/update/Updater.h"

namespace rk {

BehaviorTab::BehaviorTab(const RikuganConfig& cfg, QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* group = new QGroupBox(tr("Behavior"));
    auto* form = new QFormLayout(group);

    autoContext_ = new QCheckBox(tr("Auto-inject binary context into system prompt"));
    autoContext_->setChecked(cfg.autoContext);
    form->addRow(autoContext_);

    restoreSessions_ = new QCheckBox(tr("Restore previous chats on startup"));
    restoreSessions_->setChecked(cfg.restoreSessionsOnStart);
    restoreSessions_->setToolTip(tr("Disable this when large analysis sessions make the panel slow to open."));
    form->addRow(restoreSessions_);

    dontAutoLoad_ = new QCheckBox(tr("Don't auto-load chats (show a placeholder, pick from the sidebar)"));
    dontAutoLoad_->setChecked(cfg.dontAutoLoadChats);
    dontAutoLoad_->setToolTip(tr("List saved chats in the sidebar but don't open any on startup.\n"
                                 "Rikugan opens to a placeholder; selecting a chat loads it on demand."));
    form->addRow(dontAutoLoad_);

    auto* storageRow = new QHBoxLayout;
    sessionStorage_ = new QLineEdit(cfg.sessionStorageDir);
    sessionStorage_->setPlaceholderText(tr("Default: %1").arg(paths::home() + QStringLiteral("/sessions")));
    storageRow->addWidget(sessionStorage_, 1);
    auto* browse = new QPushButton(tr("Browse"));
    browse->setFixedWidth(70);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString cur = sessionStorage_->text().trimmed();
        const QString path = QFileDialog::getExistingDirectory(this, tr("Choose Session Storage Directory"),
                                                               cur.isEmpty() ? paths::sessionsRoot() : cur);
        if (!path.isEmpty()) sessionStorage_->setText(path);
    });
    storageRow->addWidget(browse);
    form->addRow(tr("Session storage:"), storageRow);

    exploreTurns_ = new QSpinBox;
    exploreTurns_->setRange(5, 200);
    exploreTurns_->setValue(cfg.explorationTurnLimit);
    exploreTurns_->setToolTip(tr("Maximum turns the agent spends in the exploration phase before forcing a transition "
                                 "(or reporting an error if findings are insufficient)."));
    form->addRow(tr("Exploration turn limit:"), exploreTurns_);

    silentRetry_ = new QCheckBox(tr("Show loading indicator instead of error messages during retries"));
    silentRetry_->setChecked(cfg.silentRetryMode);
    silentRetry_->setToolTip(
        tr("When enabled, rate-limit retries show a subtle text indicator instead of red error messages."));
    form->addRow(silentRetry_);

    filesystem_ = new QCheckBox(tr("Allow file access without asking (read, write, edit)"));
    filesystem_->setChecked(cfg.autoAllowFiles);
    filesystem_->setToolTip(
        tr("Off: the agent asks the first time it reads, writes or edits files in a chat, and \"Allow for this chat\" "
           "keeps that answer for the chat. On: every chat reads, writes and edits files without asking. The working "
           "directory is a scratch folder kept for this database; other paths, the binary's folder included, are "
           "reached by absolute path and nothing confines them. File changes cannot be undone with /undo."));
    form->addRow(filesystem_);

    shell_ = new QCheckBox(tr("Allow shell commands without asking"));
    shell_->setChecked(cfg.autoAllowCommands);
    shell_->setToolTip(tr("Off: the agent asks the first time it runs a shell command in a chat. On: every chat runs "
                          "shell commands without asking. What a command does cannot be undone with /undo."));
    form->addRow(shell_);

    approveMutations_ = new QCheckBox(tr("Require approval for mutating tools (rename, retype, etc.)"));
    approveMutations_->setChecked(cfg.approveMutations);
    approveMutations_->setToolTip(tr("Ask before each tool call that changes the database."));
    form->addRow(approveMutations_);

    renamerBatch_ = new QSpinBox;
    renamerBatch_->setRange(1, 100);
    renamerBatch_->setValue(cfg.bulkRenamerBatchSize);
    renamerBatch_->setToolTip(tr("Functions sent to the model in one bulk renamer request."));
    form->addRow(tr("Bulk renamer batch size:"), renamerBatch_);

    renamerConcurrent_ = new QSpinBox;
    renamerConcurrent_->setRange(1, 16);
    renamerConcurrent_->setValue(cfg.bulkRenamerMaxConcurrent);
    renamerConcurrent_->setToolTip(tr("Bulk renamer requests running at the same time."));
    form->addRow(tr("Bulk renamer max concurrent:"), renamerConcurrent_);

    layout->addWidget(group);

    // updates: once a day, asking before anything is installed (update/Updater.h)
    auto* updates = new QGroupBox(tr("Updates"));
    auto* uform = new QFormLayout(updates);
    using update::Updater;
    Updater& updater = Updater::instance();
    auto* urow = new QHBoxLayout;
    checkForUpdates_ = new QCheckBox(tr("Check for updates once a day"));
    checkForUpdates_->setChecked(cfg.checkForUpdates);
    checkForUpdates_->setToolTip(tr("Rikugan looks for a newer release on GitHub once a day. It shows it in the panel and "
                                    "installs it only when you say so. The host uses it after a restart."));
    urow->addWidget(checkForUpdates_, 1);
    auto* checkNow = new QPushButton(tr("Check now"));
    urow->addWidget(checkNow);
    uform->addRow(urow);
    updateStatus_ = new QLabel;
    updateStatus_->setWordWrap(true);
    updateStatus_->setProperty("rkMuted", true);
    uform->addRow(updateStatus_);
    auto status = [this] {
        const Updater& u = Updater::instance();
        QString s = tr("Rikugan %1").arg(update::currentVersion());
        switch (u.state()) {
        case Updater::State::Checking: s += tr(" \u00B7 checking\u2026"); break;
        case Updater::State::UpToDate: s += tr(" \u00B7 the latest release"); break;
        case Updater::State::Available: s += tr(" \u00B7 %1 is available (the panel offers it)").arg(u.release().version); break;
        case Updater::State::Installing: s += QStringLiteral(" \u00B7 ") + u.message(); break;
        case Updater::State::Installed: s += tr(" \u00B7 %1 is installed: restart to use it").arg(u.release().version); break;
        case Updater::State::Failed: s += QStringLiteral(" \u00B7 ") + u.message(); break;
        case Updater::State::Idle: break;
        }
        updateStatus_->setText(s);
    };
    connect(&updater, &Updater::changed, this, status);
    connect(checkNow, &QPushButton::clicked, &updater, [] { Updater::instance().check(true); });
    if (!updater.supported()) {
        // a developer's build: updating it is rebuilding it
        checkForUpdates_->setEnabled(false);
        checkNow->setEnabled(false);
        updateStatus_->setText(tr("Rikugan %1 \u00B7 this build was not installed from a release package, so it does not "
                                  "update itself").arg(update::currentVersion()));
    } else {
        status();
    }
    layout->addWidget(updates);
    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
}

void BehaviorTab::apply(RikuganConfig& c) const {
    c.autoContext = autoContext_->isChecked();
    c.restoreSessionsOnStart = restoreSessions_->isChecked();
    c.dontAutoLoadChats = dontAutoLoad_->isChecked();
    c.sessionStorageDir = sessionStorage_->text().trimmed();
    c.explorationTurnLimit = exploreTurns_->value();
    c.silentRetryMode = silentRetry_->isChecked();
    c.autoAllowFiles = filesystem_->isChecked();
    c.autoAllowCommands = shell_->isChecked();
    c.approveMutations = approveMutations_->isChecked();
    c.bulkRenamerBatchSize = renamerBatch_->value();
    c.bulkRenamerMaxConcurrent = renamerConcurrent_->value();
    c.checkForUpdates = checkForUpdates_->isChecked();
}

}  // namespace rk
