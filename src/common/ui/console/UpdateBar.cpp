#include "common/ui/console/UpdateBar.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/ui/Theme.h"
#include "common/update/Updater.h"

namespace rk::ui {

using update::Updater;

UpdateBar::UpdateBar(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("rk_updatebar"));
    setAttribute(Qt::WA_StyledBackground);
    // the text over the buttons: Binary Ninja's sidebar is too narrow for both on one line
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(10, 6, 8, 6);
    col->setSpacing(4);
    text_ = new QLabel(this);
    text_->setWordWrap(true);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    col->addWidget(text_);
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    row->addStretch(1);
    primary_ = new QPushButton(tr("Update"), this);
    primary_->setProperty("rkPrimary", true);
    notes_ = new QPushButton(tr("Release notes"), this);
    secondary_ = new QPushButton(this);
    for (QPushButton* b : {primary_, notes_, secondary_}) {
        b->setCursor(Qt::PointingHandCursor);
        row->addWidget(b);
    }
    col->addLayout(row);
    Updater& u = Updater::instance();
    connect(primary_, &QPushButton::clicked, &u, &Updater::installAvailable);
    connect(notes_, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(Updater::instance().release().page); });
    connect(secondary_, &QPushButton::clicked, this, [] {
        Updater& u = Updater::instance();
        if (u.state() == Updater::State::Available) u.later();
        else u.acknowledge();
    });
    connect(&u, &Updater::changed, this, &UpdateBar::sync);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &UpdateBar::restyle);
    restyle();
    sync();
}

void UpdateBar::restyle() {
    const Theme& t = theme();
    text_->setFont(t.uiSmall);
    setStyleSheet(QStringLiteral("#rk_updatebar { background: %1; border-bottom: 1px solid %2; } #rk_updatebar QLabel { color: %3; }")
                      .arg(t.accentSoft.name(), t.line.name(), t.fg.name()));
}

void UpdateBar::sync() {
    const Updater& u = Updater::instance();
    const QString version = u.release().version;
    const QString host = update::hostName(u.package());
    QString text;
    bool update = false, notes = false;
    QString second;
    switch (u.state()) {
    case Updater::State::Idle:
        break;
    case Updater::State::Checking:
        text = tr("Checking for a newer Rikugan\u2026");
        break;
    case Updater::State::UpToDate:
        text = tr("Rikugan %1 is the latest release.").arg(update::currentVersion());
        second = tr("Dismiss");
        break;
    case Updater::State::Available:
        if (u.dismissed()) break;
        text = tr("Rikugan %1 is available (you have %2).").arg(version, update::currentVersion());
        primary_->setToolTip(tr("Downloads Rikugan %1, checks it against the release's SHA-256 and puts it in place of "
                                "this copy. %2 uses it after a restart.").arg(version, host));
        update = notes = true;
        second = tr("Later");
        break;
    case Updater::State::Installing:
        text = u.message();
        break;
    case Updater::State::Installed:
        if (u.dismissed()) break;
        text = tr("Rikugan %1 is installed. Restart %2 to use it.").arg(version, host);
        notes = true;
        second = tr("Dismiss");
        break;
    case Updater::State::Failed:
        text = tr("Rikugan was not updated: %1").arg(u.message());
        second = tr("Dismiss");
        break;
    }
    text_->setText(text);
    primary_->setVisible(update);
    notes_->setVisible(notes);
    secondary_->setText(second);
    secondary_->setVisible(!second.isEmpty());
    setVisible(!text.isEmpty());
}

}  // namespace rk::ui
