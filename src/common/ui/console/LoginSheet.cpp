#include "common/ui/console/LoginSheet.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "common/omc/AuthSync.h"
#include "common/ui/Theme.h"
#include "common/ui/console/Palette.h"

namespace rk::ui {

namespace {

enum Page { ListPage = 0, FlowPage = 1, ResultPage = 2 };

QPushButton* button(const QString& text, QWidget* parent, bool primary = false) {
    auto* b = new QPushButton(text, parent);
    b->setCursor(Qt::PointingHandCursor);
    if (primary) b->setProperty("rkPrimary", true);
    return b;
}

QLabel* label(QWidget* parent, bool muted = false, bool wrap = true) {
    auto* l = new QLabel(parent);
    l->setWordWrap(wrap);
    l->setProperty("rkMuted", muted);
    return l;
}

}  // namespace

LoginSheet::LoginSheet(QWidget* parent, bool overlay) : QWidget(parent), overlay_(overlay) {
    setObjectName(QStringLiteral("rk_login"));
    setAttribute(Qt::WA_StyledBackground);
    flow_ = new LoginFlow(this);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* head = new QWidget(this);
    head->setObjectName(QStringLiteral("rk_login_head"));
    head->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(10, 5, 6, 5);
    heading_ = new QLabel(tr("Connect a provider"), head);
    heading_->setObjectName(QStringLiteral("rk_login_title"));
    heading_->setWordWrap(true);  // a provider's long name in Binary Ninja's narrow sidebar
    h->addWidget(heading_, 1);
    auto* closeBtn = button(tr("Close"), head);
    h->addWidget(closeBtn);
    v->addWidget(head);

    pages_ = new QStackedWidget(this);
    v->addWidget(pages_, 1);

    // ---- list ----
    auto* list = new QWidget(pages_);
    auto* lv = new QVBoxLayout(list);
    lv->setContentsMargins(16, 12, 16, 12);
    lv->setSpacing(8);
    auto* intro = label(list, true);
    intro->setText(tr("Sign in once; every chat uses the account. Logins and keys stay in Rikugan's own settings."));
    lv->addWidget(intro);
    search_ = new QLineEdit(list);
    search_->setPlaceholderText(tr("Search providers\u2026"));
    search_->installEventFilter(this);
    lv->addWidget(search_);
    list_ = new PaletteList(list);
    list_->setMaxRows(14);
    lv->addWidget(list_, 1);
    listNote_ = label(list, true);
    lv->addWidget(listNote_);
    pages_->addWidget(list);

    // ---- flow ----
    auto* flow = new QWidget(pages_);
    auto* fv = new QVBoxLayout(flow);
    fv->setContentsMargins(16, 14, 16, 14);
    fv->setSpacing(10);
    flowTitle_ = label(flow);
    flowTitle_->setObjectName(QStringLiteral("rk_login_flowtitle"));
    fv->addWidget(flowTitle_);
    urlBox_ = new QWidget(flow);
    auto* ub = new QVBoxLayout(urlBox_);
    ub->setContentsMargins(0, 0, 0, 0);
    ub->setSpacing(6);
    instructions_ = label(urlBox_);
    instructions_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ub->addWidget(instructions_);
    auto* urlRow = new QHBoxLayout;
    url_ = new QLineEdit(urlBox_);
    url_->setReadOnly(true);
    urlRow->addWidget(url_, 1);
    auto* open = button(tr("Open browser"), urlBox_, true);
    auto* copy = button(tr("Copy link"), urlBox_);
    urlRow->addWidget(open);
    urlRow->addWidget(copy);
    ub->addLayout(urlRow);
    urlBox_->hide();
    fv->addWidget(urlBox_);
    inputBox_ = new QWidget(flow);
    auto* ib = new QVBoxLayout(inputBox_);
    ib->setContentsMargins(0, 0, 0, 0);
    inputTitle_ = label(inputBox_);
    ib->addWidget(inputTitle_);
    auto* inputRow = new QHBoxLayout;
    input_ = new QLineEdit(inputBox_);
    inputRow->addWidget(input_, 1);
    auto* submit = button(tr("Continue"), inputBox_, true);
    inputRow->addWidget(submit);
    ib->addLayout(inputRow);
    inputBox_->hide();
    fv->addWidget(inputBox_);
    // the browser runs on another machine (IDA in a VM) and cannot reach the callback on this one
    pasteBox_ = new QWidget(flow);
    auto* pb = new QVBoxLayout(pasteBox_);
    pb->setContentsMargins(0, 6, 0, 0);
    pb->setSpacing(6);
    auto* pasteTitle = label(pasteBox_, true);
    pasteTitle->setText(tr("Browser on another machine? When it can't come back here, copy the address it ended on "
                           "(or the code the page shows) and paste it:"));
    pb->addWidget(pasteTitle);
    auto* pasteRow = new QHBoxLayout;
    paste_ = new QLineEdit(pasteBox_);
    paste_->setPlaceholderText(tr("http://localhost:\u2026/callback?code=\u2026  or the code"));
    pasteRow->addWidget(paste_, 1);
    auto* pasteBtn = button(tr("Continue"), pasteBox_);
    pasteRow->addWidget(pasteBtn);
    pb->addLayout(pasteRow);
    pasteBox_->hide();
    fv->addWidget(pasteBox_);
    progress_ = label(flow, true);
    fv->addWidget(progress_);
    fv->addStretch(1);
    auto* flowButtons = new QHBoxLayout;
    flowButtons->addStretch(1);
    auto* cancel = button(tr("Cancel"), flow);
    flowButtons->addWidget(cancel);
    fv->addLayout(flowButtons);
    pages_->addWidget(flow);

    // ---- result ----
    auto* result = new QWidget(pages_);
    auto* rv = new QVBoxLayout(result);
    rv->setContentsMargins(16, 14, 16, 14);
    rv->setSpacing(10);
    resultTitle_ = label(result);
    resultTitle_->setObjectName(QStringLiteral("rk_login_flowtitle"));
    rv->addWidget(resultTitle_);
    resultText_ = label(result);
    resultText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(resultText_);
    keyBox_ = new QWidget(result);
    auto* kb = new QVBoxLayout(keyBox_);
    kb->setContentsMargins(0, 0, 0, 0);
    auto* keyLabel = label(keyBox_, true);
    keyLabel->setText(tr("Use an API key instead:"));
    kb->addWidget(keyLabel);
    auto* keyRow = new QHBoxLayout;
    key_ = new QLineEdit(keyBox_);
    key_->setEchoMode(QLineEdit::Password);
    key_->setPlaceholderText(tr("API key"));
    keyRow->addWidget(key_, 1);
    auto* saveKeyBtn = button(tr("Save key"), keyBox_, true);
    keyRow->addWidget(saveKeyBtn);
    kb->addLayout(keyRow);
    keyStatus_ = label(keyBox_, true);
    kb->addWidget(keyStatus_);
    rv->addWidget(keyBox_);
    rv->addStretch(1);
    auto* resultButtons = new QHBoxLayout;
    resultButtons->addStretch(1);
    auto* back = button(tr("All providers"), result);
    retry_ = button(tr("Try again"), result);
    chooseModel_ = button(tr("Choose a model"), result, true);
    resultButtons->addWidget(back);
    resultButtons->addWidget(retry_);
    resultButtons->addWidget(chooseModel_);
    rv->addLayout(resultButtons);
    pages_->addWidget(result);

    connect(closeBtn, &QPushButton::clicked, this, &LoginSheet::dismiss);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& q) { list_->setQuery(q.trimmed()); });
    connect(list_, &PaletteList::activated, this, &LoginSheet::startLogin);
    connect(open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(openedUrl_)); });
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(url_->text()); });
    auto answer = [this] {
        if (input_->text().isEmpty()) return;
        flow_->answerInput(input_->text());
        input_->clear();
        inputBox_->hide();
    };
    connect(input_, &QLineEdit::returnPressed, this, answer);
    connect(submit, &QPushButton::clicked, this, answer);
    auto paste = [this] {
        if (paste_->text().trimmed().isEmpty()) return;
        onProgress(tr("Handing the code to the login\u2026"));
        flow_->submitCode(paste_->text());
        paste_->clear();
    };
    connect(paste_, &QLineEdit::returnPressed, this, paste);
    connect(pasteBtn, &QPushButton::clicked, this, paste);
    connect(flow_, &LoginFlow::pastedCodeFailed, this, &LoginSheet::onProgress);
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (flow_->running()) flow_->cancel();
        else showList();
    });
    connect(back, &QPushButton::clicked, this, &LoginSheet::showList);
    connect(retry_, &QPushButton::clicked, this, [this] { startLogin(provider_); });
    connect(chooseModel_, &QPushButton::clicked, this, [this] {
        const QString id = provider_;
        dismiss();
        Q_EMIT connected(id);
    });
    connect(key_, &QLineEdit::returnPressed, this, &LoginSheet::saveKey);
    connect(saveKeyBtn, &QPushButton::clicked, this, &LoginSheet::saveKey);
    connect(flow_, &LoginFlow::openUrl, this, &LoginSheet::onOpenUrl);
    connect(flow_, &LoginFlow::progress, this, &LoginSheet::onProgress);
    connect(flow_, &LoginFlow::inputRequested, this, &LoginSheet::onInput);
    connect(flow_, &LoginFlow::finished, this, &LoginSheet::onFinished);
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &LoginSheet::restyle);
    restyle();
    if (overlay_ && parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
        hide();
    }
}

LoginSheet::~LoginSheet() {
    // a login still running ends here; its finished() must not reach this half-destroyed sheet (it would list the
    // providers again on the service session while the plugin shuts down)
    disconnect(flow_, nullptr, this, nullptr);
    if (flow_->running()) flow_->cancel();
}

bool LoginSheet::busy() const { return flow_->running(); }

QString LoginSheet::nameOf(const QString& id) const {
    for (const LoginProviderInfo& p : providers_)
        if (p.id == id && !p.name.isEmpty()) return p.name;
    return id;
}

void LoginSheet::showList() {
    heading_->setText(tr("Connect a provider"));
    pages_->setCurrentIndex(ListPage);
    search_->clear();
    if (overlay_) {
        if (parentWidget()) setGeometry(parentWidget()->rect());
        show();
        raise();
    }
    search_->setFocus();
    loadProviders();
}

void LoginSheet::loadProviders() {
    if (providers_.isEmpty()) listNote_->setText(tr("Loading providers\u2026"));
    fetchLoginProviders(this, [this](const QList<LoginProviderInfo>& providers, const QString& error) {
        providers_ = providers;
        listNote_->setText(error.isEmpty() ? QString() : tr("Could not list providers: %1").arg(error));
        QList<PaletteEntry> entries;
        for (const LoginProviderInfo& p : providers) {
            PaletteEntry e;
            e.key = p.id;
            e.glyph = p.authenticated ? QStringLiteral("\u2713") : QStringLiteral("\u203A");
            e.title = p.name.isEmpty() ? p.id : p.name;
            e.search = p.id;
            e.detail = p.id;
            if (p.authenticated) e.hint = p.source.isEmpty() ? tr("\u2713 logged in") : tr("\u2713 logged in (%1)").arg(p.source);
            entries << e;
        }
        list_->setEntries(entries);
        list_->setQuery(search_->text().trimmed());
        if (pages_->currentIndex() == FlowPage) heading_->setText(tr("Log in to %1").arg(nameOf(provider_)));
    });
}

void LoginSheet::startLogin(const QString& providerId) {
    if (providerId.isEmpty() || flow_->running()) return;
    provider_ = providerId;
    if (overlay_) {
        if (parentWidget()) setGeometry(parentWidget()->rect());
        show();
        raise();
    }
    heading_->setText(tr("Log in to %1").arg(nameOf(providerId)));
    flowTitle_->setText(tr("Starting the login\u2026"));
    urlBox_->hide();
    inputBox_->hide();
    pasteBox_->hide();
    paste_->clear();
    progress_->clear();
    openedUrl_.clear();
    pages_->setCurrentIndex(FlowPage);
    if (providers_.isEmpty()) loadProviders();  // opened from the setup: the providers' names come with the list
    flow_->start(providerId);
}

void LoginSheet::onOpenUrl(const QString& url, const QString& launchUrl, const QString& instructions) {
    flowTitle_->setText(tr("Finish signing in in your browser."));
    url_->setText(url);
    url_->setCursorPosition(0);
    // a device code reads better large
    static const QRegularExpression code(QStringLiteral("\\b([A-Z0-9]{4,}-[A-Z0-9]{4,})\\b"));
    const auto m = code.match(instructions);
    if (m.hasMatch()) {
        instructions_->setTextFormat(Qt::RichText);
        instructions_->setText(QStringLiteral("%1<br><span style=\"font-size:20pt; font-family:'%2'; font-weight:600\">%3</span>")
                                   .arg(instructions.toHtmlEscaped(), theme().mono.family(), m.captured(1)));
    } else {
        instructions_->setTextFormat(Qt::PlainText);
        instructions_->setText(instructions.isEmpty() ? tr("Your browser opens the sign-in page. If it didn't, open the link.")
                                                      : instructions);
    }
    urlBox_->show();
    pasteBox_->setVisible(flow_->acceptsPastedCode());
    const QString target = launchUrl.isEmpty() ? url : launchUrl;
    if (openedUrl_ != target) {
        openedUrl_ = target;
        QDesktopServices::openUrl(QUrl(target));
    }
    onProgress(tr("Waiting for the sign-in to finish\u2026"));
}

void LoginSheet::onProgress(const QString& message) {
    if (message.trimmed().isEmpty()) return;
    QStringList lines = progress_->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    lines << QStringLiteral("\u00b7 ") + message.trimmed();
    while (lines.size() > 6) lines.removeFirst();
    progress_->setText(lines.join(QLatin1Char('\n')));
}

void LoginSheet::onInput(const QString& title, const QString& placeholder) {
    inputTitle_->setText(title);
    input_->setPlaceholderText(placeholder);
    input_->clear();
    inputBox_->show();
    input_->setFocus();
}

void LoginSheet::onFinished(bool ok, const QString& error) {
    if (ok) {
        heading_->setText(tr("Connected"));
        resultTitle_->setText(QStringLiteral("\u2713  ") + tr("Logged in to %1.").arg(nameOf(provider_)));
        resultText_->setText(tr("Its models are in the model picker now."));
        keyBox_->hide();
        retry_->hide();
        chooseModel_->show();
        pages_->setCurrentIndex(ResultPage);
        chooseModel_->setFocus();
        loadProviders();
        return;
    }
    if (error == QLatin1String("cancelled")) {
        showList();
        return;
    }
    showFailure(error);
}

void LoginSheet::showFailure(const QString& error) {
    heading_->setText(tr("Log in to %1").arg(nameOf(provider_)));
    resultTitle_->setText(QStringLiteral("\u2715  ") + tr("The login did not finish."));
    resultText_->setText(error);
    keyBox_->show();
    keyStatus_->clear();
    key_->clear();
    retry_->setVisible(error != LoginFlow::kTerminalOnly);
    chooseModel_->hide();
    pages_->setCurrentIndex(ResultPage);
    key_->setFocus();
}

void LoginSheet::saveKey() {
    const QString err = storeApiKey(provider_, key_->text());
    if (!err.isEmpty()) {
        keyStatus_->setText(err);
        return;
    }
    key_->clear();
    onFinished(true, {});
    resultTitle_->setText(QStringLiteral("\u2713  ") + tr("Saved the API key for %1.").arg(nameOf(provider_)));
}

void LoginSheet::dismiss() {
    if (flow_->running()) flow_->cancel();
    if (overlay_) hide();
    Q_EMIT closed();
}

bool LoginSheet::eventFilter(QObject* o, QEvent* e) {
    if (o == parentWidget() && e->type() == QEvent::Resize && overlay_) setGeometry(parentWidget()->rect());
    if (o == search_ && e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Down) {
            list_->moveSelection(1);
            return true;
        }
        if (key == Qt::Key_Up) {
            list_->moveSelection(-1);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) return list_->activateCurrent();
    }
    return QWidget::eventFilter(o, e);
}

void LoginSheet::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        if (pages_->currentIndex() == FlowPage && flow_->running()) flow_->cancel();
        else dismiss();
        return;
    }
    QWidget::keyPressEvent(e);
}

void LoginSheet::restyle() {
    const Theme& t = theme();
    setStyleSheet(QStringLiteral("#rk_login { background: %1; }"
                                 "#rk_login_head { background: %2; border-bottom: 1px solid %3; }"
                                 "#rk_login_title { font-weight: bold; color: %4; }"
                                 "#rk_login_flowtitle { font-weight: bold; color: %4; }"
                                 "#rk_login QLabel { color: %4; background: transparent; }"
                                 "#rk_login QLabel[rkMuted=\"true\"] { color: %5; }")
                      .arg(t.bg.name(), t.bg2.name(), t.line.name(), t.fg.name(), t.fg3.name()));
}

}  // namespace rk::ui
