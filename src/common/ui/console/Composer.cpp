#include "common/ui/console/Composer.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/ui/Theme.h"
#include "common/ui/console/Palette.h"

namespace rk::ui {

namespace {

constexpr int kMinLines = 1;
constexpr int kMaxLines = 8;
constexpr int kHistory = 50;

QString groupTitle(const QString& g) {
    if (g == QLatin1String("rikugan")) return QStringLiteral("Reverse engineering");
    if (g == QLatin1String("agent")) return QStringLiteral("Agent");
    if (g == QLatin1String("session")) return QStringLiteral("Session");
    if (g == QLatin1String("skill")) return QStringLiteral("Skills");
    return g;
}

}  // namespace

// ---- editor ----

ComposerEdit::ComposerEdit(QWidget* parent) : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("rk_input"));
    setFrameShape(QFrame::NoFrame);
    setTabChangesFocus(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(4);
    connect(document(), &QTextDocument::contentsChanged, this, &ComposerEdit::syncHeight);
    syncHeight();
}

void ComposerEdit::syncHeight() {
    // QPlainTextDocumentLayout's height is in lines
    const int docLines = int(document()->size().height());
    const int lines = std::clamp(docLines, kMinLines, kMaxLines);
    const int h = lines * fontMetrics().lineSpacing() + int(document()->documentMargin() * 2) + 2 * frameWidth() + 6;
    setVerticalScrollBarPolicy(docLines > kMaxLines ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    if (h != height()) setFixedHeight(h);
}

void ComposerEdit::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    syncHeight();
}

void ComposerEdit::focusInEvent(QFocusEvent* e) {
    QPlainTextEdit::focusInEvent(e);
    if (parentWidget()) {
        parentWidget()->setProperty("rkFocus", true);
        parentWidget()->style()->unpolish(parentWidget());
        parentWidget()->style()->polish(parentWidget());
    }
}

void ComposerEdit::focusOutEvent(QFocusEvent* e) {
    QPlainTextEdit::focusOutEvent(e);
    if (parentWidget()) {
        parentWidget()->setProperty("rkFocus", false);
        parentWidget()->style()->unpolish(parentWidget());
        parentWidget()->style()->polish(parentWidget());
    }
}

namespace {
bool isPanelKey(const QKeyEvent* k) {
    if ((k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) != Qt::ControlModifier) return false;
    return k->key() == Qt::Key_K || k->key() == Qt::Key_M || k->key() == Qt::Key_J || k->key() == Qt::Key_N;
}
}  // namespace

bool ComposerEdit::event(QEvent* e) {
    // claim the panel's keys before the host's application shortcuts see them
    if (e->type() == QEvent::ShortcutOverride && isPanelKey(static_cast<QKeyEvent*>(e))) {
        e->accept();
        return true;
    }
    return QPlainTextEdit::event(e);
}

void ComposerEdit::keyPressEvent(QKeyEvent* e) {
    if (isPanelKey(e)) {
        Q_EMIT panelKey(e->key());
        return;
    }
    const int key = e->key();
    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab ||
        key == Qt::Key_Escape) {
        bool handled = false;
        Q_EMIT paletteKey(key, &handled);
        if (handled) return;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
        Q_EMIT submit();
        return;
    }
    // history on the first / last line, like a shell
    if (key == Qt::Key_Up && textCursor().blockNumber() == 0) {
        bool handled = false;
        Q_EMIT historyKey(-1, &handled);
        if (handled) return;
    }
    if (key == Qt::Key_Down && textCursor().blockNumber() == document()->blockCount() - 1) {
        bool handled = false;
        Q_EMIT historyKey(1, &handled);
        if (handled) return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

// ---- composer ----

Composer::Composer(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("rk_composer"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(4);

    queueBox_ = new QVBoxLayout;
    queueBox_->setSpacing(2);
    outer->addLayout(queueBox_);

    box_ = new QFrame(this);
    box_->setObjectName(QStringLiteral("rk_composer_box"));
    box_->setAttribute(Qt::WA_StyledBackground);
    auto* boxLayout = new QVBoxLayout(box_);
    boxLayout->setContentsMargins(6, 4, 6, 4);
    boxLayout->setSpacing(2);
    edit_ = new ComposerEdit(box_);
    boxLayout->addWidget(edit_);

    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    model_ = new QToolButton(box_);
    model_->setObjectName(QStringLiteral("rk_model_pill"));
    model_->setCursor(Qt::PointingHandCursor);
    model_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    model_->setToolTip(tr("Model and thinking (Ctrl+M)"));
    row->addWidget(model_);
    mode_ = new QToolButton(box_);
    mode_->hide();
    row->addWidget(mode_);
    row->addStretch(1);
    hint_ = new QLabel(box_);
    hint_->setObjectName(QStringLiteral("rk_hint"));
    row->addWidget(hint_);
    send_ = new QToolButton(box_);
    send_->setObjectName(QStringLiteral("rk_send"));
    send_->setCursor(Qt::PointingHandCursor);
    send_->setFixedSize(26, 26);
    row->addWidget(send_);
    boxLayout->addLayout(row);
    outer->addWidget(box_);

    connect(edit_, &ComposerEdit::submit, this, &Composer::send);
    connect(edit_, &QPlainTextEdit::textChanged, this, [this] {
        updateSlash();
        syncButton();
    });
    // the "/" list takes its keys first (one handler: a second would move it twice); Esc stops a running turn
    connect(edit_, &ComposerEdit::paletteKey, this, [this](int key, bool* handled) {
        if (slash_ && slash_->isVisible()) *handled = slash_->handleKey(key);
        if (!*handled && key == Qt::Key_Escape && running_) {
            *handled = true;
            Q_EMIT stopRequested();
        }
    });
    connect(edit_, &ComposerEdit::panelKey, this, &Composer::panelKey);
    connect(edit_, &ComposerEdit::historyKey, this, [this](int delta, bool* handled) {
        if (history_.isEmpty()) return;
        if (delta < 0 && historyPos_ < 0 && !edit_->toPlainText().isEmpty()) return;  // never lose a draft
        int pos = historyPos_ < 0 ? int(history_.size()) : historyPos_;
        pos += delta;
        if (pos < 0 || pos > history_.size()) return;
        *handled = true;
        historyPos_ = pos == history_.size() ? -1 : pos;
        edit_->setPlainText(historyPos_ < 0 ? QString() : history_.at(pos));
        edit_->moveCursor(QTextCursor::End);
    });
    connect(send_, &QToolButton::clicked, this, [this] {
        if (running_ && edit_->toPlainText().trimmed().isEmpty()) Q_EMIT stopRequested();
        else send();
    });
    connect(model_, &QToolButton::clicked, this, &Composer::modelPickerRequested);
    connect(mode_, &QToolButton::clicked, this, &Composer::stopRequested);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &Composer::restyle);
    restyle();
    syncButton();
}

void Composer::setPopupArea(QWidget* area) {
    slash_ = new PalettePopup(area, false);
    slash_->list()->setMaxRows(9);
    connect(slash_->list(), &PaletteList::activated, this, &Composer::acceptCommand);
}

void Composer::setCommands(const QList<CommandInfo>& commands) {
    commands_ = commands;
    if (!slash_) return;
    QList<PaletteEntry> entries;
    for (const CommandInfo& c : commands) {
        PaletteEntry e;
        e.key = c.name;
        e.section = groupTitle(c.group);
        e.glyph = c.icon;
        e.title = QStringLiteral("/") + c.name;
        e.detail = c.description;
        e.hint = c.argHint;
        e.mono = true;
        entries << e;
    }
    slash_->list()->setEntries(entries);
}

void Composer::updateSlash() {
    if (!slash_) return;
    const QString text = edit_->toPlainText();
    // the palette follows the first word while it is a command being typed
    const bool typing = text.startsWith(QLatin1Char('/')) && !text.contains(QLatin1Char(' ')) && !text.contains(QLatin1Char('\n'));
    if (!typing || !enabled_) {
        slash_->dismiss();
        return;
    }
    slash_->list()->setQuery(text.mid(1));
    if (slash_->list()->visibleCount() == 0) {
        slash_->dismiss();
        return;
    }
    // re-placed on every keystroke: the list's height follows the matches
    slash_->popupAt(this, true);
    edit_->setFocus();
}

void Composer::acceptCommand(const QString& key) {
    const auto it = std::find_if(commands_.cbegin(), commands_.cend(), [&](const CommandInfo& c) { return c.name == key; });
    slash_->dismiss();
    // a required argument: the user types it; otherwise the command runs now
    if (it != commands_.cend() && it->argHint.startsWith(QLatin1Char('<'))) {
        edit_->setPlainText(QStringLiteral("/%1 ").arg(key));
        edit_->moveCursor(QTextCursor::End);
        edit_->setFocus();
        return;
    }
    edit_->clear();
    Q_EMIT submitted(QStringLiteral("/") + key);
}

bool Composer::slashOpen() const { return slash_ && slash_->isVisible(); }

void Composer::send() {
    if (!enabled_) return;
    const QString text = edit_->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (history_.isEmpty() || history_.last() != text) history_ << text;
    while (history_.size() > kHistory) history_.removeFirst();
    historyPos_ = -1;
    edit_->clear();
    if (slash_) slash_->dismiss();
    Q_EMIT submitted(text);
}

void Composer::setRunning(bool running) {
    if (running_ == running) return;
    running_ = running;
    syncButton();
}

void Composer::setInputEnabled(bool enabled) {
    enabled_ = enabled;
    edit_->setReadOnly(!enabled);
    syncButton();
}

void Composer::setPlaceholder(const QString& text) {
    if (edit_->placeholderText() == text) return;
    edit_->setPlaceholderText(text);
    edit_->viewport()->update();  // QPlainTextEdit does not repaint a changed placeholder while it has focus
}

void Composer::setModelLabel(const QString& label, const QString& tooltip) {
    model_->setText(QStringLiteral("\u2699 ") + label + QStringLiteral("  \u25BE"));
    model_->setToolTip(tooltip);
}

void Composer::setMode(const QString& mode) {
    mode_->setVisible(!mode.isEmpty());
    if (!mode.isEmpty()) mode_->setText(mode + QStringLiteral("  \u00d7"));
    mode_->setToolTip(tr("Leave %1 mode (stops the run)").arg(mode));
}

void Composer::setQueue(const QStringList& queued) {
    while (QLayoutItem* item = queueBox_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const Theme& t = theme();
    for (int i = 0; i < queued.size(); ++i) {
        auto* row = new QFrame(this);
        row->setObjectName(QStringLiteral("rk_queued"));
        row->setAttribute(Qt::WA_StyledBackground);
        row->setToolTip(tr("The agent reads this at its next step. A long tool or a running subagent delays it: "
                           "Send now stops the current step and sends it at once."));
        auto* l = new QHBoxLayout(row);
        l->setContentsMargins(8, 2, 4, 2);
        auto* tag = new QLabel(i == 0 ? QStringLiteral("QUEUED %1").arg(queued.size()) : QString(), row);
        tag->setObjectName(QStringLiteral("rk_queued_tag"));
        tag->setMinimumWidth(QFontMetrics(t.uiSmall).horizontalAdvance(QStringLiteral("QUEUED 99")));
        tag->setFont(t.uiSmall);
        l->addWidget(tag);
        auto* text = new QLabel(row);
        text->setText(QFontMetrics(t.ui).elidedText(queued[i].simplified(), Qt::ElideRight, 600));
        l->addWidget(text, 1);
        auto* now = new QToolButton(row);
        now->setText(tr("Send now"));
        now->setToolTip(tr("Stop what the agent is doing and send this message now"));
        now->setCursor(Qt::PointingHandCursor);
        connect(now, &QToolButton::clicked, this, [this, i] { Q_EMIT sendQueuedNow(i); });
        l->addWidget(now);
        auto* x = new QToolButton(row);
        x->setText(QStringLiteral("\u00d7"));
        x->setToolTip(tr("Remove from the queue"));
        x->setCursor(Qt::PointingHandCursor);
        connect(x, &QToolButton::clicked, this, [this, i] { Q_EMIT removeQueued(i); });
        l->addWidget(x);
        queueBox_->addWidget(row);
    }
}

QString Composer::text() const { return edit_->toPlainText().trimmed(); }

void Composer::setText(const QString& text) {
    // read-only while a card waits for its buttons: text put there could not be edited or sent
    if (!enabled_) return;
    edit_->setPlainText(text);
    edit_->moveCursor(QTextCursor::End);
    edit_->setFocus();
}

void Composer::focusInput() { edit_->setFocus(); }

QWidget* Composer::modelButton() const { return model_; }

void Composer::syncButton() {
    const bool stop = running_ && edit_->toPlainText().trimmed().isEmpty();
    send_->setText(stop ? QStringLiteral("\u25A0") : QStringLiteral("\u2191"));
    send_->setToolTip(stop ? tr("Stop (Esc)") : running_ ? tr("Queue this message") : tr("Send (Enter)"));
    send_->setProperty("rkStop", stop);
    send_->setEnabled(stop || (enabled_ && !edit_->toPlainText().trimmed().isEmpty()));
    send_->style()->unpolish(send_);
    send_->style()->polish(send_);
    hint_->setText(!enabled_ ? QString()  // the placeholder says why
                   : running_ ? tr("typing queues a message")
                              : tr("\u21B5 send \u00b7 \u21E7\u21B5 newline \u00b7 / commands"));
}

void Composer::restyle() {
    const Theme& t = theme();
    auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
    setStyleSheet(
        QStringLiteral(
            "#rk_composer { background: %1; border-top: 1px solid %2; }"
            "#rk_composer_box { background: %3; border: 1px solid %4; border-radius: 8px; }"
            "#rk_composer_box[rkFocus=\"true\"] { border-color: %5; }"
            "#rk_input { background: transparent; border: none; color: %6; selection-background-color: %7; }"
            "#rk_model_pill { border: 1px solid %4; border-radius: 10px; padding: 1px 8px; color: %6; background: %1; }"
            "#rk_model_pill:hover { background: %8; }"
            "#rk_mode_chip { border: 1px solid %9; border-radius: 10px; padding: 1px 8px; color: %5; background: %10; }"
            "#rk_hint { color: %11; }"
            "#rk_send { border: none; border-radius: 13px; background: %5; color: %12; font-weight: bold; }"
            "#rk_send:disabled { background: %8; color: %11; }"
            "#rk_send[rkStop=\"true\"] { background: %13; color: white; }"
            "#rk_queued { background: %3; border: 1px dashed %4; border-radius: 6px; }"
            "#rk_queued_tag { color: %5; font-weight: bold; }")
            .arg(c(t.bg), c(t.line), c(t.bg), c(t.line2), c(t.accent), c(t.fg), c(t.selection), c(t.bg3), c(t.userLine))
            .arg(c(t.accentSoft), c(t.fg3), c(t.accentFg), c(t.err)));
    hint_->setFont(t.uiSmall);
    model_->setFont(t.uiSmall);
    mode_->setFont(t.uiSmall);
}

}  // namespace rk::ui
