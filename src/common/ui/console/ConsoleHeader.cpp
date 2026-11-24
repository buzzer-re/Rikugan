#include "common/ui/console/ConsoleHeader.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMenu>
#include <QResizeEvent>
#include <QToolButton>

#include "common/ui/Theme.h"

namespace rk::ui {

ConsoleHeader::ConsoleHeader(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_header"));
    setAttribute(Qt::WA_StyledBackground);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 3, 6, 3);
    row->setSpacing(2);

    title_ = new QToolButton(this);
    title_->setObjectName(QStringLiteral("rk_title"));
    title_->setCursor(Qt::PointingHandCursor);
    title_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    title_->setIconSize(QSize(14, 14));
    title_->setToolTip(tr("Switch chat"));
    title_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    row->addWidget(title_, 0);
    row->addStretch(1);

    mcp_ = new QToolButton(this);
    mcp_->setObjectName(QStringLiteral("rk_mcp"));
    mcp_->setCursor(Qt::PointingHandCursor);
    mcp_->hide();
    row->addWidget(mcp_);
    full_ = new QToolButton(this);
    full_->setIconSize(QSize(12, 12));
    full_->setToolTip(tr("Open in full window"));
    full_->setCursor(Qt::PointingHandCursor);
    full_->hide();
    row->addWidget(full_);
    palette_ = new QToolButton(this);
    palette_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    palette_->setIconSize(QSize(12, 12));
    palette_->setToolTip(tr("Commands (Ctrl+K)"));
    palette_->setCursor(Qt::PointingHandCursor);
    row->addWidget(palette_);
    add_ = new QToolButton(this);
    add_->setText(QStringLiteral("+"));
    add_->setToolTip(tr("New chat"));
    add_->setCursor(Qt::PointingHandCursor);
    row->addWidget(add_);
    more_ = new QToolButton(this);
    more_->setText(QStringLiteral("\u22EF"));
    more_->setToolTip(tr("More"));
    more_->setCursor(Qt::PointingHandCursor);
    more_->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(more_);

    menu_ = new QMenu(this);
    chatActions_ << menu_->addAction(tr("Fork chat"), this, &ConsoleHeader::forkRequested);
    chatActions_ << menu_->addAction(tr("Rename chat\u2026"), this, &ConsoleHeader::renameRequested);
    chatActions_ << menu_->addAction(tr("Export as Markdown\u2026"), this, &ConsoleHeader::exportRequested);
    chatActions_ << menu_->addAction(tr("Export as HTML"), this, &ConsoleHeader::exportHtmlRequested);
    chatActions_ << menu_->addAction(tr("Delete chat\u2026"), this, &ConsoleHeader::deleteRequested);
    menu_->addSeparator();
    undo_ = menu_->addAction(tr("Undo last database change"), this, &ConsoleHeader::undoRequested);
    menu_->addAction(tr("Agents\tCtrl+J"), this, &ConsoleHeader::agentsRequested);
    menu_->addAction(tr("Bulk renamer"), this, &ConsoleHeader::renamerRequested);
    menu_->addSeparator();
    menu_->addAction(tr("Settings\u2026"), this, &ConsoleHeader::settingsRequested);
    more_->setMenu(menu_);

    connect(title_, &QToolButton::clicked, this, &ConsoleHeader::switcherRequested);
    connect(palette_, &QToolButton::clicked, this, &ConsoleHeader::paletteRequested);
    connect(add_, &QToolButton::clicked, this, &ConsoleHeader::newChatRequested);
    connect(mcp_, &QToolButton::clicked, this, &ConsoleHeader::hostMcpClicked);
    connect(full_, &QToolButton::clicked, this, &ConsoleHeader::fullWindowRequested);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &ConsoleHeader::restyle);
    restyle();
    setTitle(tr("New chat"));
}

void ConsoleHeader::setTitle(const QString& title, const QString& icon) {
    titleIcon_ = icon;
    title_->setIcon(kindIcon(icon, theme().fg));
    title_->setText(QStringLiteral(" ") + title + QStringLiteral("  \u25BE"));
    fit();
}

void ConsoleHeader::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fit();
}

void ConsoleHeader::fit() {
    const QString dot = mcpBusy_ ? QStringLiteral("\u25CC") : mcpOn_ ? QStringLiteral("\u25CF") : QStringLiteral("\u25CB");
    // the platform's own spelling of the shortcut: Ctrl+K, or ⌘K on macOS
    palette_->setText(QKeySequence(QStringLiteral("Ctrl+K")).toString(QKeySequence::NativeText));
    mcp_->setText(dot + QStringLiteral(" MCP"));
    auto needed = [this] {
        const QMargins m = layout()->contentsMargins();
        int w = m.left() + m.right() + title_->sizeHint().width();
        for (QToolButton* b : {mcp_, full_, palette_, add_, more_})
            if (!b->isHidden()) w += b->sizeHint().width() + layout()->spacing();
        return w;
    };
    if (needed() > width()) palette_->setText(QString());
    if (needed() > width()) mcp_->setText(dot);
}

void ConsoleHeader::setChatActionsEnabled(bool enabled) {
    for (QAction* a : std::as_const(chatActions_)) a->setEnabled(enabled);
}

void ConsoleHeader::setUndoAvailable(bool available) { undo_->setEnabled(available); }

void ConsoleHeader::setHostMcp(bool shown, bool on, bool busy, const QString& tooltip) {
    mcp_->setVisible(shown);
    mcp_->setEnabled(!busy);
    mcp_->setToolTip(tooltip);
    mcpBusy_ = busy;
    if (on != mcpOn_) {
        mcpOn_ = on;
        restyle();
    }
    fit();
}

void ConsoleHeader::setFullWindowShown(bool shown) {
    full_->setVisible(shown);
    fit();
}

QWidget* ConsoleHeader::titleButton() const { return title_; }

void ConsoleHeader::restyle() {
    const Theme& t = theme();
    QFont bold = t.ui;
    bold.setWeight(QFont::DemiBold);
    title_->setFont(bold);
    title_->setIcon(kindIcon(titleIcon_, t.fg));
    palette_->setFont(t.uiSmall);
    palette_->setIcon(kindIcon(QStringLiteral("search"), t.fg2));
    full_->setIcon(kindIcon(QStringLiteral("expand"), t.fg2));
    mcp_->setFont(t.uiSmall);
    setStyleSheet(QStringLiteral("#rk_header { background: %1; border-bottom: 1px solid %2; }"
                                 "#rk_header QToolButton { color: %3; border: none; border-radius: 5px; padding: 3px 7px; }"
                                 "#rk_header QToolButton:hover { background: %4; }"
                                 "#rk_header QToolButton::menu-indicator { image: none; width: 0; }"
                                 "#rk_title { color: %5; }"
                                 "#rk_header #rk_mcp { color: %6; }")
                      .arg(t.bg.name(), t.line.name(), t.fg2.name(), t.bg3.name(), t.fg.name(),
                           (mcpOn_ ? t.accent : t.fg2).name()));
    fit();
}

}  // namespace rk::ui
