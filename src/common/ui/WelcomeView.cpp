#include "common/ui/WelcomeView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSet>
#include <QVBoxLayout>

#include "common/ui/Theme.h"

namespace rk::ui {

QList<Suggestion> buildSuggestions(const QStringList& skillSlugs, std::optional<quint64> cursorAddress) {
    const QSet<QString> slugs(skillSlugs.begin(), skillSlugs.end());
    QList<Suggestion> s{
        {QString(), QStringLiteral("Summarize what this binary does"), QStringLiteral("Summarize what this binary does"), {}},
        {QStringLiteral("/explore"), QStringLiteral("How does this sample persist?"), QStringLiteral("/explore How does this sample persist?"), {}},
    };
    if (slugs.contains(QStringLiteral("deobfuscation")))
        s.append({QStringLiteral("/deobfuscation"), QStringLiteral("Hunt for crypto and obfuscation"), QStringLiteral("/deobfuscation "), {}});
    const QString at = cursorAddress ? QStringLiteral(" at 0x%1").arg(*cursorAddress, 0, 16) : QString();
    s.append({QStringLiteral("/modify"), QStringLiteral("Patch the check") + at, QStringLiteral("/modify Patch the check") + at, {}});
    s.append({QStringLiteral("/plan"), QStringLiteral("Rename the networking helpers"), QStringLiteral("/plan Rename the networking helpers"), {}});
    return s;
}

SuggestionRow::SuggestionRow(const Suggestion& suggestion, QWidget* parent) : QFrame(parent), prompt_(suggestion.prompt) {
    setObjectName(QStringLiteral("rk_suggestion"));
    setAttribute(Qt::WA_StyledBackground);
    setAttribute(Qt::WA_Hover);
    // as tall as its wrapped text at the row's width (a narrow sidebar wraps the detail)
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setCursor(Qt::PointingHandCursor);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(12, 7, 12, 7);
    row->setSpacing(8);
    if (!suggestion.command.isEmpty()) {
        auto* cmd = new QLabel(suggestion.command, this);
        cmd->setObjectName(QStringLiteral("rk_suggestion_cmd"));
        cmd->setFont(theme().mono);
        row->addWidget(cmd);
    }
    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    auto* title = new QLabel(suggestion.title, this);
    title->setWordWrap(true);
    text->addWidget(title);
    if (!suggestion.detail.isEmpty()) {
        auto* detail = new QLabel(suggestion.detail, this);
        detail->setObjectName(QStringLiteral("rk_suggestion_detail"));
        detail->setWordWrap(true);  // a narrow sidebar wraps it rather than widening the pane
        text->addWidget(detail);
    }
    row->addLayout(text, 1);
}

void SuggestionRow::mouseReleaseEvent(QMouseEvent* event) {
    QFrame::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) Q_EMIT activated(prompt_);
}

WelcomeView::WelcomeView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_welcome"));
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(6);
    layout->addStretch(2);
    auto* logo = new QLabel(QStringLiteral("\u25C6"), this);
    logo->setObjectName(QStringLiteral("rk_welcome_logo"));
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);
    title_ = new QLabel(tr("New chat"), this);
    title_->setObjectName(QStringLiteral("rk_welcome_title"));
    title_->setAlignment(Qt::AlignCenter);
    title_->setWordWrap(true);
    layout->addWidget(title_);
    facts_ = new QLabel(tr("Ask anything about the loaded binary, or pick a starting point."), this);
    facts_->setObjectName(QStringLiteral("rk_welcome_facts"));
    facts_->setAlignment(Qt::AlignCenter);
    facts_->setWordWrap(true);
    layout->addWidget(facts_);
    layout->addSpacing(10);
    rows_ = new QVBoxLayout;
    rows_->setSpacing(6);
    layout->addLayout(rows_);
    layout->addSpacing(8);
    hint_ = new QLabel(tr("/ for commands \u00b7 Ctrl+K for everything \u00b7 Ctrl+M for the model"), this);
    hint_->setObjectName(QStringLiteral("rk_welcome_facts"));
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);
    layout->addStretch(3);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        restyle();
        setSuggestions(suggestions_);
    });
    restyle();
    setSuggestions(buildSuggestions({}, std::nullopt));
}

void WelcomeView::setSuggestions(const QList<Suggestion>& suggestions) {
    suggestions_ = suggestions;
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    for (const Suggestion& s : suggestions) {
        auto* row = new SuggestionRow(s, this);
        connect(row, &SuggestionRow::activated, this, [this](const QString& key) {
            if (noDatabase_) Q_EMIT openRequested(key);
            else Q_EMIT activated(key);
        });
        rows_->addWidget(row);
    }
}

void WelcomeView::setBinarySummary(const BinarySummary& summary) {
    noDatabase_ = false;
    hint_->show();
    title_->setText(summary.name.isEmpty() ? tr("New chat") : tr("New chat \u00b7 %1").arg(summary.shortName()));
    title_->setToolTip(summary.name);
    facts_->setText(summary.chips.isEmpty() ? tr("Ask anything about the loaded binary, or pick a starting point.")
                                            : summary.chips.join(QStringLiteral(" \u00b7 ")));
}

void WelcomeView::setNoDatabase(const QString& facts, const QList<Suggestion>& rows) {
    noDatabase_ = true;
    hint_->hide();
    title_->setText(tr("No binary open"));
    title_->setToolTip({});
    facts_->setText(facts);
    setSuggestions(rows);
}

void WelcomeView::restyle() {
    const Theme& t = theme();
    QFont big = t.ui;
    if (big.pointSizeF() > 0) big.setPointSizeF(big.pointSizeF() + 3);
    big.setWeight(QFont::Bold);
    title_->setFont(big);
    setStyleSheet(QStringLiteral("#rk_welcome { background: %1; }"
                                 "#rk_welcome QLabel { color: %2; background: transparent; }"
                                 "#rk_welcome #rk_welcome_logo { color: %3; font-size: 22pt; }"
                                 "#rk_welcome #rk_welcome_facts, #rk_welcome #rk_suggestion_detail { color: %4; }"
                                 "#rk_suggestion { background: %1; border: 1px solid %5; border-radius: 6px; }"
                                 "#rk_suggestion:hover { background: %6; border-color: %7; }"
                                 "#rk_welcome #rk_suggestion_cmd { color: %3; }")
                      .arg(t.bg.name(), t.fg.name(), t.accent.name(), t.fg3.name(), t.line2.name(), t.bg2.name(), t.userLine.name()));
}

}  // namespace rk::ui
