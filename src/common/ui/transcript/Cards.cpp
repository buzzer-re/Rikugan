#include "common/ui/transcript/Cards.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "common/agent/ChatSession.h"
#include "common/ui/Syntax.h"
#include "common/ui/Theme.h"

namespace rk::ui {

namespace {

QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

QString css(const QColor& c) { return c.name(QColor::HexArgb); }

// ---- disk / shell permission -------------------------------------------------------------------------------

class PermissionCard : public TranscriptCard {
public:
    PermissionCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        setProperty("rkTone", QStringLiteral("warn"));
        const QString kind = str(item.payload, "kind");
        const QString tool = str(item.payload, "tool");
        QString question = kind == QLatin1String("bash")    ? tr("Run a shell command?")
                           : kind == QLatin1String("write") ? tr("Write a file?")
                           : kind == QLatin1String("edit")  ? tr("Edit a file?")
                                                            : tr("Read files on disk?");
        const QString agent = str(item.payload, "subagentId");
        if (!agent.isEmpty()) question += QStringLiteral("  ") + tr("(subagent %1)").arg(agent);
        addText(QStringLiteral("**%1**  `%2`").arg(question, tool), true);
        const QString details = str(item.payload, "details").trimmed();
        if (!details.isEmpty()) addCode(details, kind == QLatin1String("bash") ? kLanguageShell : kLanguagePlain, 8);
        addText(tr("\u201cAllow for this chat\u201d stops asking about %1 in this chat and its subagents.")
                    .arg(kind == QLatin1String("bash") ? tr("shell commands") : tr("%1 access").arg(kind)),
                false, true);
        const QString key = item.key;
        auto answer = [this, key](PermissionChoice c, const QString& label) {
            if (chat_) chat_->answerPermission(key, c);
            finish(label);
        };
        connect(addButton(tr("Allow once"), true), &QPushButton::clicked, this,
                [answer] { answer(PermissionChoice::AllowOnce, QStringLiteral("allowed_once")); });
        connect(addButton(tr("Allow for this chat")), &QPushButton::clicked, this,
                [answer] { answer(PermissionChoice::AllowChat, QStringLiteral("allowed_chat")); });
        connect(addButton(tr("Deny"), false, true), &QPushButton::clicked, this,
                [answer] { answer(PermissionChoice::Deny, QStringLiteral("denied")); });
    }
};

// ---- script / database change approval -------------------------------------------------------------------

class ApprovalCard : public TranscriptCard {
public:
    ApprovalCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        setProperty("rkTone", QStringLiteral("warn"));
        const bool script = str(item.payload, "kind") == QLatin1String("script");
        const QString tool = str(item.payload, "toolName");
        const QJsonObject args = item.payload.value(QLatin1String("arguments")).toObject();
        addText(script ? tr("**Run this script?**  `%1`").arg(tool) : tr("**Apply this database change?**  `%1`").arg(tool), true);
        const QString reason = str(item.payload, "reason");
        if (!reason.isEmpty()) addText(reason, false, true);
        if (script && args.value(QLatin1String("code")).isString())
            addCode(str(args, "code"), kLanguagePython, 16);
        else if (!args.isEmpty())
            addCode(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented)).trimmed(), kLanguageJson, 12);
        const int id = item.requestId;
        auto answer = [this, id](ApprovalChoice c, const QString& label) {
            if (chat_) chat_->answerApproval(id, c);
            finish(label);
        };
        connect(addButton(tr("Allow"), true), &QPushButton::clicked, this, [answer] { answer(ApprovalChoice::Allow, tr("Allowed")); });
        connect(addButton(script ? tr("Always allow scripts in this chat") : tr("Allow for this chat")), &QPushButton::clicked, this,
                [answer] { answer(ApprovalChoice::AlwaysAllow, tr("Allowed for this chat")); });
        connect(addButton(tr("Deny"), false, true), &QPushButton::clicked, this, [answer] { answer(ApprovalChoice::Deny, tr("Denied")); });
    }
};

// ---- question from the model -----------------------------------------------------------------------------

class QuestionCard : public TranscriptCard {
public:
    QuestionCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        addText(str(item.payload, "question"), true);
        const int id = item.requestId;
        auto answer = [this, id](const QString& text) {
            if (text.trimmed().isEmpty()) return;
            if (chat_) chat_->answerQuestion(id, UserAnswer{false, text});
            finish(text);
        };
        const QJsonArray options = item.payload.value(QLatin1String("options")).toArray();
        if (!options.isEmpty()) {
            for (const QJsonValue& v : options) {
                const QString o = v.toString();
                connect(addButton(o, false), &QPushButton::clicked, this, [answer, o] { answer(o); });
            }
            return;
        }
        auto* field = new QLineEdit(this);
        field->setPlaceholderText(tr("Your answer (or type it in the composer)"));
        body()->addWidget(field);
        first_ = field;
        connect(field, &QLineEdit::returnPressed, this, [answer, field] { answer(field->text()); });
        connect(addButton(tr("Answer"), true), &QPushButton::clicked, this, [answer, field] { answer(field->text()); });
        connect(addButton(tr("Skip")), &QPushButton::clicked, this, [this, id] {
            if (chat_) chat_->answerQuestion(id, UserAnswer{true, {}});
            finish(tr("skipped"));
        });
    }
    QWidget* first_ = nullptr;
};

// ---- omc dialogs (extension_ui_request) ------------------------------------------------------------------

class DialogCard : public TranscriptCard {
public:
    DialogCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        const QJsonObject& f = item.payload;
        const QString method = str(f, "method");
        const QString id = str(f, "id");
        addText(str(f, "title"), true);
        if (!str(f, "message").isEmpty()) addText(str(f, "message"), true, true);
        auto send = [this, id](const QJsonObject& answer, const QString& label) {
            if (chat_) chat_->answerUiRequest(id, answer);
            finish(label);
        };
        if (method == QLatin1String("select")) {
            for (const QJsonValue& v : f.value(QLatin1String("options")).toArray()) {
                const QString o = v.isString() ? v.toString() : str(v.toObject(), "label");
                const QString value = v.isString() ? v.toString() : str(v.toObject(), "value").isEmpty() ? o : str(v.toObject(), "value");
                connect(addButton(o), &QPushButton::clicked, this, [send, value, o] { send(QJsonObject{{"value", value}}, o); });
            }
        } else if (method == QLatin1String("confirm")) {
            connect(addButton(tr("Yes"), true), &QPushButton::clicked, this, [send] { send(QJsonObject{{"confirmed", true}}, tr("Yes")); });
            connect(addButton(tr("No")), &QPushButton::clicked, this, [send] { send(QJsonObject{{"confirmed", false}}, tr("No")); });
            return;
        } else if (method == QLatin1String("editor")) {
            auto* edit = new QPlainTextEdit(this);
            edit->setPlainText(str(f, "prefill"));
            edit->setFixedHeight(QFontMetrics(theme().mono).lineSpacing() * 8 + 12);
            edit->setFont(theme().mono);
            body()->addWidget(edit);
            connect(addButton(tr("Submit"), true), &QPushButton::clicked, this,
                    [send, edit] { send(QJsonObject{{"value", edit->toPlainText()}}, tr("submitted")); });
        } else {  // input
            auto* field = new QLineEdit(this);
            field->setPlaceholderText(str(f, "placeholder"));
            body()->addWidget(field);
            connect(field, &QLineEdit::returnPressed, this, [send, field] { send(QJsonObject{{"value", field->text()}}, field->text()); });
            connect(addButton(tr("Submit"), true), &QPushButton::clicked, this,
                    [send, field] { send(QJsonObject{{"value", field->text()}}, field->text()); });
        }
        connect(addButton(tr("Cancel")), &QPushButton::clicked, this, [send] { send(QJsonObject{{"cancelled", true}}, QStringLiteral("cancelled")); });
    }
};

// ---- mode decisions ------------------------------------------------------------------------------------

// The plan as a checklist: untick a step to drop it, ✎ to reword it, "Add step" for one more. Approve sends the
// plan as shown ("approve", or "approve:" + the JSON array of steps when it was changed).
class PlanCard : public TranscriptCard {
public:
    PlanCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        setProperty("rkTone", QStringLiteral("accent"));
        const QJsonObject p = item.payload.value(QLatin1String("payload")).toObject();
        for (const QJsonValue& v : p.value(QLatin1String("steps")).toArray()) original_ << (v.isString() ? v.toString() : str(v.toObject(), "text"));
        title_ = new QLabel(this);
        title_->setTextFormat(Qt::MarkdownText);
        body()->addWidget(title_);
        if (original_.isEmpty()) addText(str(p, "text"), true);
        stepsBox_ = new QVBoxLayout;
        stepsBox_->setSpacing(2);
        body()->addLayout(stepsBox_);
        for (const QString& s : std::as_const(original_)) addStep(s, false);
        auto* add = new QToolButton(this);
        add->setText(tr("+ Add step"));
        add->setCursor(Qt::PointingHandCursor);
        add->setAutoRaise(true);
        add->setProperty("rkMuted", true);
        body()->addWidget(add, 0, Qt::AlignLeft);
        connect(add, &QToolButton::clicked, this, [this] { addStep(QString(), true); });

        const int id = item.requestId;
        auto answer = [this, id](const QString& a, const QString& label) {
            if (chat_) chat_->answerQuestion(id, UserAnswer{false, a});
            finish(label);
        };
        approve_ = addButton(QString(), true);
        connect(approve_, &QPushButton::clicked, this, [this, answer] {
            const QStringList kept = chosen();
            if (kept.isEmpty()) return;
            if (kept == original_) answer(QStringLiteral("approve"), tr("Approved"));
            else answer(QStringLiteral("approve:") + QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(kept)).toJson(QJsonDocument::Compact)),
                        tr("Approved %n edited step(s)", nullptr, int(kept.size())));
        });
        auto* reject = addButton(tr("Reject\u2026"));
        // after Reject: feedback, regenerate or cancel
        feedback_ = new QWidget(this);
        auto* fl = new QHBoxLayout(feedback_);
        fl->setContentsMargins(0, 0, 0, 0);
        auto* field = new QLineEdit(feedback_);
        field->setPlaceholderText(tr("What should change?"));
        fl->addWidget(field, 1);
        auto* sendFb = new QPushButton(tr("Revise"), feedback_);
        sendFb->setProperty("rkPrimary", true);
        auto* regen = new QPushButton(tr("Regenerate"), feedback_);
        auto* cancel = new QPushButton(tr("Cancel plan"), feedback_);
        cancel->setProperty("rkDanger", true);
        fl->addWidget(sendFb);
        fl->addWidget(regen);
        fl->addWidget(cancel);
        feedback_->hide();
        body()->addWidget(feedback_);
        auto revise = [answer, field] {
            const QString fb = field->text().trimmed();
            if (fb.isEmpty()) return;
            answer(QStringLiteral("reject:") + fb, tr("Revise: %1").arg(fb));
        };
        connect(field, &QLineEdit::returnPressed, this, revise);
        connect(sendFb, &QPushButton::clicked, this, revise);
        connect(regen, &QPushButton::clicked, this, [answer] { answer(QStringLiteral("regenerate"), tr("Regenerate")); });
        connect(cancel, &QPushButton::clicked, this, [answer] { answer(QStringLiteral("cancel"), tr("Plan cancelled")); });
        connect(reject, &QPushButton::clicked, this, [this, field] {
            feedback_->show();
            field->setFocus();
            Q_EMIT heightChanged();
        });
        sync();
    }

private:
    struct Step {
        QCheckBox* on = nullptr;
        QLabel* text = nullptr;
        QLineEdit* edit = nullptr;
    };

    void addStep(const QString& text, bool editing) {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        Step s;
        s.on = new QCheckBox(row);
        s.on->setChecked(true);
        s.on->setToolTip(tr("Include this step"));
        h->addWidget(s.on, 0, Qt::AlignTop);
        s.text = new QLabel(row);
        s.text->setTextFormat(Qt::MarkdownText);
        s.text->setWordWrap(true);
        s.text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        s.text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        h->addWidget(s.text, 1);
        s.edit = new QLineEdit(text, row);
        s.edit->setPlaceholderText(tr("Describe the step"));
        s.edit->hide();
        h->addWidget(s.edit, 1);
        auto* pen = new QToolButton(row);
        pen->setText(QStringLiteral("\u270E"));
        pen->setToolTip(tr("Edit this step"));
        pen->setAutoRaise(true);
        pen->setCursor(Qt::PointingHandCursor);
        h->addWidget(pen, 0, Qt::AlignTop);
        stepsBox_->addWidget(row);
        const int i = int(steps_.size());
        steps_ << s;
        connect(s.on, &QCheckBox::toggled, this, [this] { sync(); });
        connect(pen, &QToolButton::clicked, this, [this, i] { setEditing(i, !steps_[i].edit->isVisible()); });
        connect(s.edit, &QLineEdit::editingFinished, this, [this, i] { setEditing(i, false); });
        setEditing(i, editing);
    }

    void setEditing(int i, bool on) {
        Step& s = steps_[i];
        s.edit->setVisible(on);
        s.text->setVisible(!on);
        if (on) {
            s.edit->setFocus();
            s.edit->selectAll();
        }
        sync();
    }

    QStringList chosen() const {
        QStringList out;
        for (const Step& s : steps_)
            if (s.on->isChecked() && !s.edit->text().trimmed().isEmpty()) out << s.edit->text().trimmed();
        return out;
    }

    // numbers follow the steps that are kept; dropped ones fade and strike through
    void sync() {
        if (!approve_) return;  // the steps are added before the buttons; the constructor syncs once at the end
        int n = 0;
        for (const Step& s : std::as_const(steps_)) {
            const QString text = s.edit->text().trimmed();
            const bool kept = s.on->isChecked() && !text.isEmpty();
            if (kept) ++n;
            const QString shown = text.isEmpty() ? tr("*(empty)*") : text;
            s.text->setText(kept ? QStringLiteral("%1. %2").arg(n).arg(shown) : QStringLiteral("~~%1~~").arg(shown));
            s.text->setProperty("rkMuted", !kept);
            s.text->style()->unpolish(s.text);
            s.text->style()->polish(s.text);
        }
        title_->setText(tr("**Plan** \u00b7 %n step(s)", nullptr, n));
        approve_->setText(n == int(steps_.size()) ? tr("Approve") : tr("Approve %n step(s)", nullptr, n));
        approve_->setEnabled(n > 0);
        Q_EMIT heightChanged();
    }

    QStringList original_;
    QList<Step> steps_;
    QLabel* title_ = nullptr;
    QVBoxLayout* stepsBox_ = nullptr;
    QPushButton* approve_ = nullptr;
    QWidget* feedback_ = nullptr;
};

class SaveCard : public TranscriptCard {
public:
    SaveCard(const TranscriptItem& item, ChatSession* chat, QWidget* parent) : TranscriptCard(chat, parent) {
        setProperty("rkTone", QStringLiteral("accent"));
        const QJsonArray patches = item.payload.value(QLatin1String("payload")).toObject().value(QLatin1String("patches")).toArray();
        addText(tr("**Save %n patch(es) to the database?**", nullptr, int(patches.size())), true);
        QStringList lines;
        for (const QJsonValue& v : patches) {
            const QJsonObject o = v.toObject();
            lines << QStringLiteral("%1  %2 \u2192 %3  %4").arg(str(o, "address"), str(o, "original_hex"), str(o, "new_hex"), str(o, "description"));
        }
        if (!lines.isEmpty()) addCode(lines.join(QLatin1Char('\n')), kLanguagePlain, 10);
        const int id = item.requestId;
        auto answer = [this, id](const QString& a, const QString& label) {
            if (chat_) chat_->answerQuestion(id, UserAnswer{false, a});
            finish(label);
        };
        connect(addButton(tr("Save all"), true), &QPushButton::clicked, this, [answer] { answer(QStringLiteral("save"), tr("Saved")); });
        connect(addButton(tr("Discard all"), false, true), &QPushButton::clicked, this,
                [answer] { answer(QStringLiteral("discard"), tr("Discarded")); });
    }
};

}  // namespace

// ---- base ---------------------------------------------------------------------------------------------------

TranscriptCard::TranscriptCard(ChatSession* chat, QWidget* parent) : QFrame(parent), chat_(chat) {
    setObjectName(QStringLiteral("rk_card"));
    setAttribute(Qt::WA_StyledBackground);
    body_ = new QVBoxLayout(this);
    body_->setContentsMargins(10, 8, 10, 8);
    body_->setSpacing(6);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &TranscriptCard::restyle);
    restyle();
}

TranscriptCard* TranscriptCard::create(const TranscriptItem& item, ChatSession* chat, QWidget* parent) {
    if (item.resolved) return nullptr;
    switch (item.kind) {
    case ItemKind::Permission: return new PermissionCard(item, chat, parent);
    case ItemKind::Approval: return new ApprovalCard(item, chat, parent);
    case ItemKind::Question: return new QuestionCard(item, chat, parent);
    case ItemKind::Dialog: return new DialogCard(item, chat, parent);
    case ItemKind::Decision: {
        const QString kind = str(item.payload, "kind");
        if (kind == QLatin1String("plan")) return new PlanCard(item, chat, parent);
        if (kind == QLatin1String("save")) return new SaveCard(item, chat, parent);
        // an unknown decision has no card: dismiss it rather than leave the worker waiting
        if (chat) chat->answerQuestion(item.requestId, UserAnswer{true, {}});
        return nullptr;
    }
    default: return nullptr;
    }
}

int TranscriptCard::heightFor(int width) const {
    return hasHeightForWidth() ? heightForWidth(width) : sizeHint().height();
}

void TranscriptCard::focusFirst() {
    if (auto* q = dynamic_cast<QuestionCard*>(this); q && q->first_) {
        q->first_->setFocus();
        return;
    }
    if (buttons_ && buttons_->count() && buttons_->itemAt(0)->widget()) buttons_->itemAt(0)->widget()->setFocus();
}

bool TranscriptCard::event(QEvent* e) {
    // a wrapped label's height follows the width: the row must follow the card
    if (e->type() == QEvent::LayoutRequest) Q_EMIT heightChanged();
    return QFrame::event(e);
}

QHBoxLayout* TranscriptCard::buttonRow() {
    if (!buttons_) {
        buttons_ = new QHBoxLayout;
        buttons_->setSpacing(6);
        buttons_->addStretch(1);
        body_->addLayout(buttons_);
    }
    return buttons_;
}

QPushButton* TranscriptCard::addButton(const QString& text, bool primary, bool danger) {
    QHBoxLayout* row = buttonRow();
    auto* b = new QPushButton(text, this);
    b->setCursor(Qt::PointingHandCursor);
    if (primary) b->setProperty("rkPrimary", true);
    if (danger) b->setProperty("rkDanger", true);
    if (!primary) b->setProperty("rkOutline", true);
    row->insertWidget(row->count() - 1, b);
    return b;
}

void TranscriptCard::addText(const QString& text, bool markdown, bool muted) {
    auto* l = new QLabel(this);
    l->setWordWrap(true);
    l->setTextFormat(markdown ? Qt::MarkdownText : Qt::PlainText);
    l->setText(text);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    l->setOpenExternalLinks(true);
    l->setProperty("rkMuted", muted);
    if (muted) l->setFont(theme().uiSmall);
    body_->addWidget(l);
}

void TranscriptCard::addCode(const QString& code, const QString& language, int maxLines) {
    auto* view = new QPlainTextEdit(this);
    view->setObjectName(QStringLiteral("rk_code"));
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(theme().monoSmall);
    view->setPlainText(code);
    new CodeHighlighter(view->document(), language);
    const int lines = std::min<int>(int(code.count(QLatin1Char('\n'))) + 1, maxLines);
    view->setFixedHeight(QFontMetrics(theme().monoSmall).lineSpacing() * lines + 20 +
                         (code.section(QLatin1Char('\n'), 0, maxLines - 1).size() > 200 ? view->horizontalScrollBar()->sizeHint().height() : 0));
    body_->addWidget(view);
}

void TranscriptCard::finish(const QString& resolution) {
    if (done_) return;
    done_ = true;
    setEnabled(false);
    Q_EMIT answered(resolution);
}

void TranscriptCard::restyle() {
    const Theme& t = theme();
    const QString tone = property("rkTone").toString();
    const QColor border = tone == QLatin1String("warn") ? blend(t.warn, t.bg, 0.45) : tone == QLatin1String("accent") ? t.userLine : t.line2;
    const QColor bg = tone == QLatin1String("warn") ? blend(t.warn, t.bg, 0.93) : t.bg2;
    setStyleSheet(QStringLiteral("#rk_card { background: %1; border: 1px solid %2; border-radius: %3px; }"
                                 "#rk_card QLabel { background: transparent; color: %4; }"
                                 "#rk_card QLabel[rkMuted=\"true\"] { color: %5; }"
                                 "#rk_card QPushButton[rkOutline=\"true\"] { border-color: %6; background: %7; }"
                                 "#rk_card #rk_code { background: %8; border: 1px solid %6; }")
                      .arg(css(bg), css(border), QString::number(t.radius), css(t.fg), css(t.fg3), css(t.line2), css(t.bg),
                           css(t.codeBg)));
}

}  // namespace rk::ui
