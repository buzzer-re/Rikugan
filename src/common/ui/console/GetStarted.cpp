#include "common/ui/console/GetStarted.h"

#include <algorithm>

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "common/config/RikuganConfig.h"
#include "common/omc/AgentHome.h"
#include "common/omc/AuthSync.h"
#include "common/omc/ExternalLogins.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/Theme.h"
#include "common/ui/WelcomeView.h"
#include "common/ui/settings/ExternalLoginsConsent.h"
#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

namespace rk::ui {

namespace {

// Providers that take a pasted key, and where the key comes from.
struct KeyProvider {
    const char* id;
    const char* name;
    const char* keyUrl;
};
constexpr KeyProvider kKeyProviders[] = {
    {"anthropic", "Anthropic (Claude)", "https://console.anthropic.com/settings/keys"},
    {"openai", "OpenAI", "https://platform.openai.com/api-keys"},
    {"google", "Google (Gemini)", "https://aistudio.google.com/apikey"},
    {"openrouter", "OpenRouter", "https://openrouter.ai/keys"},
    {"xai", "xAI (Grok)", "https://console.x.ai"},
    {"groq", "Groq", "https://console.groq.com/keys"},
    {"mistral", "Mistral", "https://console.mistral.ai/api-keys"},
    {"deepseek", "DeepSeek", "https://platform.deepseek.com/api_keys"},
};

// Local servers: the connection's id, its URL and API.
struct LocalKind {
    const char* label;
    const char* id;
    const char* url;
    const char* api;
};
constexpr LocalKind kLocalKinds[] = {
    {"Ollama", "local-ollama", "http://localhost:11434", "ollama-chat"},
    {"LM Studio", "local-lmstudio", "http://localhost:1234/v1", "openai-completions"},
    {"Another OpenAI-compatible server", "local-server", "", "openai-completions"},
};

// The omc agent's own logins, when it is installed here and has any.
bool omcLoginsFound() {
    bool ok = false;
    const QJsonObject auth = readJsonObject(AgentHome::standaloneAgentDir() + QStringLiteral("/auth.json"), &ok);
    return ok && !auth.isEmpty();
}

QString contextLabel(qint64 n) {
    if (n <= 0) return {};
    if (n >= 1000000) return QStringLiteral("%1M context").arg(double(n) / 1000000.0, 0, 'g', 2);
    return QStringLiteral("%1k context").arg(n / 1000);
}

SuggestionRow* addRow(QVBoxLayout* into, QWidget* parent, const QString& title, const QString& detail, const QString& key) {
    auto* row = new SuggestionRow(Suggestion{QString(), title, key, detail}, parent);
    into->addWidget(row);
    return row;
}

void clearRows(QVBoxLayout* rows) {
    while (QLayoutItem* item = rows->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}

}  // namespace

GetStarted::GetStarted(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_setup"));
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_);
    buildChoose();
    buildSignIn();
    buildApiKey();
    buildLocal();
    buildPickModel();
    buildRestart();
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, &GetStarted::restyle);
    restyle();
}

// A page: a title, a line of text and the body under them, centred like the welcome.
QWidget* GetStarted::page(const QString& title, const QString& text, QVBoxLayout** body) {
    auto* w = new QWidget(pages_);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    v->addStretch(2);
    auto* logo = new QLabel(QStringLiteral("\u25C6"), w);
    logo->setObjectName(QStringLiteral("rk_setup_logo"));
    logo->setAlignment(Qt::AlignCenter);
    v->addWidget(logo);
    auto* t = new QLabel(title, w);
    t->setObjectName(QStringLiteral("rk_setup_title"));
    t->setAlignment(Qt::AlignCenter);
    t->setWordWrap(true);
    v->addWidget(t);
    auto* s = new QLabel(text, w);
    s->setObjectName(QStringLiteral("rk_setup_text"));
    s->setAlignment(Qt::AlignCenter);
    s->setWordWrap(true);
    v->addWidget(s);
    v->addSpacing(10);
    *body = new QVBoxLayout;
    (*body)->setSpacing(6);
    v->addLayout(*body);
    v->addStretch(3);
    pages_->addWidget(w);
    return w;
}

void GetStarted::buildChoose() {
    QVBoxLayout* body = nullptr;
    page(tr("Connect a model"),
         tr("Rikugan works with the AI model you pick. Choose how to connect it. You can change this later in "
            "Settings."),
         &body);
    chooseRows_ = new QVBoxLayout;
    chooseRows_->setSpacing(6);
    body->addLayout(chooseRows_);
    auto* later = new QPushButton(tr("Not now"), this);
    later->setFlat(true);
    later->setCursor(Qt::PointingHandCursor);
    body->addWidget(later, 0, Qt::AlignCenter);
    connect(later, &QPushButton::clicked, this, &GetStarted::dismissed);
}

void GetStarted::buildSignIn() {
    QVBoxLayout* body = nullptr;
    page(tr("Sign in with a subscription"),
         tr("Use a plan you already pay for. Your browser opens to sign in, then you come back here."), &body);
    signInRows_ = new QVBoxLayout;
    signInRows_->setSpacing(6);
    body->addLayout(signInRows_);
    signInNote_ = new QLabel(this);
    signInNote_->setObjectName(QStringLiteral("rk_setup_text"));
    signInNote_->setAlignment(Qt::AlignCenter);
    signInNote_->setWordWrap(true);
    body->addWidget(signInNote_);
    auto* back = new QPushButton(tr("\u2190 Back"), this);
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    body->addWidget(back, 0, Qt::AlignCenter);
    connect(back, &QPushButton::clicked, this, [this] { show(Choose); });
}

void GetStarted::buildApiKey() {
    QVBoxLayout* body = nullptr;
    page(tr("Paste an API key"), tr("Pick the provider, paste its key and press Connect. The key stays on this machine."),
         &body);
    keyProvider_ = new QComboBox(this);
    for (const KeyProvider& k : kKeyProviders) keyProvider_->addItem(QString::fromLatin1(k.name), QString::fromLatin1(k.id));
    keyProvider_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    keyProvider_->setMinimumContentsLength(8);
    body->addWidget(keyProvider_);
    keyLink_ = new QLabel(this);
    keyLink_->setWordWrap(true);
    keyLink_->setObjectName(QStringLiteral("rk_setup_text"));
    keyLink_->setOpenExternalLinks(true);
    keyLink_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    body->addWidget(keyLink_);
    key_ = new QLineEdit(this);
    key_->setEchoMode(QLineEdit::Password);
    key_->setPlaceholderText(tr("API key"));
    body->addWidget(key_);
    auto* row = new QHBoxLayout;
    auto* back = new QPushButton(tr("\u2190 Back"), this);
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    row->addWidget(back);
    row->addStretch(1);
    auto* save = new QPushButton(tr("Connect"), this);
    save->setProperty("rkPrimary", true);
    save->setCursor(Qt::PointingHandCursor);
    row->addWidget(save);
    body->addLayout(row);
    keyStatus_ = new QLabel(this);
    keyStatus_->setObjectName(QStringLiteral("rk_setup_text"));
    keyStatus_->setWordWrap(true);
    body->addWidget(keyStatus_);
    auto link = [this] {
        const int i = keyProvider_->currentIndex();
        if (i < 0) return;
        keyLink_->setText(tr("No key yet? <a href=\"%1\">Create one at %2</a>")
                              .arg(QString::fromLatin1(kKeyProviders[i].keyUrl), QUrl(QString::fromLatin1(kKeyProviders[i].keyUrl)).host()));
    };
    connect(keyProvider_, &QComboBox::currentIndexChanged, this, link);
    link();
    connect(back, &QPushButton::clicked, this, [this] { show(Choose); });
    connect(save, &QPushButton::clicked, this, &GetStarted::saveKey);
    connect(key_, &QLineEdit::returnPressed, this, &GetStarted::saveKey);
}

void GetStarted::buildLocal() {
    QVBoxLayout* body = nullptr;
    page(tr("Use a local model"),
         tr("Point Rikugan at a model server running on this machine or your network. Nothing leaves it."), &body);
    localKind_ = new QComboBox(this);
    for (const LocalKind& k : kLocalKinds) localKind_->addItem(QString::fromLatin1(k.label));
    localKind_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    localKind_->setMinimumContentsLength(8);
    body->addWidget(localKind_);
    localUrl_ = new QLineEdit(this);
    localUrl_->setPlaceholderText(QStringLiteral("http://localhost:8000/v1"));
    body->addWidget(localUrl_);
    localModels_ = new QLineEdit(this);
    localModels_->setPlaceholderText(tr("Model name, e.g. qwen2.5-coder:14b (several: separate with commas)"));
    body->addWidget(localModels_);
    auto* row = new QHBoxLayout;
    auto* back = new QPushButton(tr("\u2190 Back"), this);
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    row->addWidget(back);
    row->addStretch(1);
    auto* save = new QPushButton(tr("Save"), this);
    save->setProperty("rkPrimary", true);
    save->setCursor(Qt::PointingHandCursor);
    row->addWidget(save);
    body->addLayout(row);
    localStatus_ = new QLabel(this);
    localStatus_->setObjectName(QStringLiteral("rk_setup_text"));
    localStatus_->setWordWrap(true);
    body->addWidget(localStatus_);
    auto preset = [this] { localUrl_->setText(QString::fromLatin1(kLocalKinds[std::max(0, localKind_->currentIndex())].url)); };
    connect(localKind_, &QComboBox::currentIndexChanged, this, preset);
    preset();
    connect(back, &QPushButton::clicked, this, [this] { show(Choose); });
    connect(save, &QPushButton::clicked, this, &GetStarted::saveLocal);
}

void GetStarted::buildPickModel() {
    QVBoxLayout* body = nullptr;
    page(tr("Pick a model"), tr("New chats start with the model you pick. Ctrl+M switches it any time."),
         &body);
    modelNote_ = new QLabel(this);
    modelNote_->setObjectName(QStringLiteral("rk_setup_text"));
    modelNote_->setAlignment(Qt::AlignCenter);
    modelNote_->setWordWrap(true);
    body->addWidget(modelNote_);
    modelRows_ = new QVBoxLayout;
    modelRows_->setSpacing(6);
    body->addLayout(modelRows_);
    auto* back = new QPushButton(tr("\u2190 Back"), this);
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    body->addWidget(back, 0, Qt::AlignCenter);
    connect(back, &QPushButton::clicked, this, [this] { show(Choose); });
}

void GetStarted::buildRestart() {
    QVBoxLayout* body = nullptr;
    page(tr("Almost there"),
         tr("The local model is saved. Restart the host once so Rikugan can reach it, then start chatting."), &body);
    auto* ok = new QPushButton(tr("OK"), this);
    ok->setProperty("rkPrimary", true);
    ok->setCursor(Qt::PointingHandCursor);
    body->addWidget(ok, 0, Qt::AlignCenter);
    connect(ok, &QPushButton::clicked, this, &GetStarted::dismissed);
}

void GetStarted::start() {
    waiting_ = false;
    clearRows(chooseRows_);
    connect(addRow(chooseRows_, this, tr("Sign in with a subscription"),
                   tr("Claude Pro or Max, ChatGPT Plus or Pro, GitHub Copilot, Gemini\u2026"), QStringLiteral("signin")),
            &SuggestionRow::activated, this, [this] { show(SignIn); });
    connect(addRow(chooseRows_, this, tr("Paste an API key"), tr("Anthropic, OpenAI, Google, OpenRouter and others"),
                   QStringLiteral("key")),
            &SuggestionRow::activated, this, [this] { show(ApiKey); });
    connect(addRow(chooseRows_, this, tr("Use a local model"), tr("Ollama, LM Studio or any OpenAI-compatible server"),
                   QStringLiteral("local")),
            &SuggestionRow::activated, this, [this] { show(Local); });
    if (!RikuganConfig::instance().useExternalLogins)
        if (const QList<ExternalLogin> found = ExternalLogins::find(); !found.isEmpty())
            connect(addRow(chooseRows_, this, tr("Use my %1 login").arg(externalToolNames(found)),
                           tr("The subscription you are signed in to on this computer"), QStringLiteral("external")),
                    &SuggestionRow::activated, this, &GetStarted::useExternalLogins);
    if (!RikuganConfig::instance().useOmcLogins && omcLoginsFound())
        connect(addRow(chooseRows_, this, tr("Use my omc logins"),
                       tr("Reuse the sign-ins of the omc agent on this computer"),
                       QStringLiteral("omc")),
                &SuggestionRow::activated, this, &GetStarted::useOmcLogins);
    show(Choose);
}

void GetStarted::show(Page p) {
    pages_->setCurrentIndex(int(p));
    if (p == SignIn) fillSignIn();
    if (p == ApiKey) {
        keyStatus_->clear();
        key_->setFocus();
    }
    if (p == Local) localStatus_->clear();
    if (p == PickModel) fillModels();
}

void GetStarted::fillSignIn() {
    clearRows(signInRows_);
    signInNote_->setText(tr("Loading\u2026"));
    QPointer<GetStarted> self(this);
    fetchLoginProviders(this, [self](const QList<LoginProviderInfo>& providers, const QString& error) {
        if (!self) return;
        self->logins_ = providers;
        clearRows(self->signInRows_);
        self->signInNote_->setText(error.isEmpty() ? QString() : tr("Could not list the sign-in options: %1").arg(error));
        for (const LoginProviderInfo& p : providers) {
            const QString detail = p.authenticated ? tr("Signed in") : QString();
            connect(addRow(self->signInRows_, self, p.name.isEmpty() ? p.id : p.name, detail, p.id), &SuggestionRow::activated,
                    self, [self](const QString& id) {
                        if (self) Q_EMIT self->loginRequested(id);
                    });
        }
    });
}

void GetStarted::loggedIn(const QString& providerId) { waitForModels(providerId); }

// The model step for `provider` ("*": any) once the model list includes it.
void GetStarted::waitForModels(const QString& provider) {
    waitingFor_ = provider;
    waiting_ = true;
    show(PickModel);
    Q_EMIT refreshModels();
}

void GetStarted::setModels(const QList<ModelInfo>& models) {
    models_ = models;
    waiting_ = false;  // the list asked for came back
    if (pages_->currentIndex() == PickModel) fillModels();
}

void GetStarted::fillModels() {
    clearRows(modelRows_);
    QList<ModelInfo> shown;
    for (const ModelInfo& m : std::as_const(models_))
        if (waitingFor_.isEmpty() || waitingFor_ == QLatin1String("*") || m.provider == waitingFor_) shown << m;
    if (shown.isEmpty()) {
        modelNote_->setText(waiting_ ? tr("Loading the models\u2026")
                                     : tr("No models are available from it yet. Check the key or sign-in and try again."));
        return;
    }
    waiting_ = false;
    modelNote_->clear();
    const int limit = 12;  // the rest: Ctrl+M
    for (qsizetype i = 0; i < shown.size() && i < limit; ++i) {
        const ModelInfo& m = shown[i];
        QStringList detail{m.id};
        if (const QString c = contextLabel(m.contextWindow); !c.isEmpty()) detail << c;
        connect(addRow(modelRows_, this, m.name.isEmpty() ? m.id : m.name, detail.join(QStringLiteral(" \u00b7 ")), m.key()),
                &SuggestionRow::activated, this, &GetStarted::modelChosen);
    }
}

void GetStarted::saveKey() {
    const QString provider = keyProvider_->currentData().toString();
    const QString err = storeApiKey(provider, key_->text());
    if (!err.isEmpty()) {
        keyStatus_->setText(err);
        return;
    }
    key_->clear();
    waitForModels(provider);  // storeApiKey made omc re-read the keys; the reload runs before the model list
}

void GetStarted::saveLocal() {
    const LocalKind& kind = kLocalKinds[std::max(0, localKind_->currentIndex())];
    const QUrl url(localUrl_->text().trimmed());
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        localStatus_->setText(tr("Enter the server's address, such as http://localhost:11434."));
        return;
    }
    QStringList models;
    for (const QString& m : localModels_->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (!m.trimmed().isEmpty()) models << m.trimmed();
    if (models.isEmpty()) {
        localStatus_->setText(tr("Enter the name of at least one model the server runs."));
        return;
    }
    RikuganConfig& cfg = RikuganConfig::instance();
    const QString id = QString::fromLatin1(kind.id);
    cfg.customEndpoints.erase(std::remove_if(cfg.customEndpoints.begin(), cfg.customEndpoints.end(),
                                             [&](const CustomEndpoint& e) { return e.id == id; }),
                              cfg.customEndpoints.end());
    cfg.customEndpoints << CustomEndpoint{id, url.toString(), QString::fromLatin1(kind.api), QString(), models};
    cfg.model = id + QLatin1Char('/') + models.first();
    if (!cfg.save()) {
        localStatus_->setText(tr("Could not save the settings."));
        return;
    }
    AgentHome::writeModelsYaml();
    show(Restart);
}

void GetStarted::useExternalLogins() {
    const QList<ExternalLogin> found = ExternalLogins::find();
    if (found.isEmpty() || !confirmExternalLogins(this, found)) return;
    RikuganConfig& cfg = RikuganConfig::instance();
    cfg.useExternalLogins = true;
    cfg.save();
    QPointer<GetStarted> self(this);
    ServiceSession::get()->post([self](OmcSession&) {
        ExternalLogins::syncNow();
        QMetaObject::invokeMethod(
            uiAnchor(),
            [self] {
                if (self) self->waitForModels(QStringLiteral("*"));
            },
            Qt::QueuedConnection);
    });
}

void GetStarted::useOmcLogins() {
    RikuganConfig& cfg = RikuganConfig::instance();
    cfg.useOmcLogins = true;
    cfg.save();
    QPointer<GetStarted> self(this);
    // the copy reloads omc's keys on the service worker; the model list asked for after it sees them
    ServiceSession::get()->post([self](OmcSession&) {
        AuthSync::syncNow();
        QMetaObject::invokeMethod(
            uiAnchor(),
            [self] {
                if (self) self->waitForModels(QStringLiteral("*"));
            },
            Qt::QueuedConnection);
    });
}

void GetStarted::restyle() {
    const Theme& t = theme();
    QFont big = t.ui;
    if (big.pointSizeF() > 0) big.setPointSizeF(big.pointSizeF() + 3);
    big.setWeight(QFont::Bold);
    for (QLabel* l : findChildren<QLabel*>(QStringLiteral("rk_setup_title"))) l->setFont(big);
    setStyleSheet(QStringLiteral("#rk_setup { background: %1; }"
                                 "#rk_setup QLabel { color: %2; background: transparent; }"
                                 "#rk_setup #rk_setup_logo { color: %3; font-size: 22pt; }"
                                 "#rk_setup #rk_setup_text, #rk_setup #rk_suggestion_detail { color: %4; }"
                                 "#rk_setup #rk_setup_text a { color: %3; }"
                                 "#rk_suggestion { background: %1; border: 1px solid %5; border-radius: 6px; }"
                                 "#rk_suggestion:hover { background: %6; border-color: %7; }"
                                 "#rk_setup QLineEdit, #rk_setup QComboBox { background: %6; color: %2; border: 1px solid %5;"
                                 " border-radius: 5px; padding: 4px 6px; }")
                      .arg(t.bg.name(), t.fg.name(), t.accent.name(), t.fg3.name(), t.line2.name(), t.bg2.name(), t.userLine.name()));
}

}  // namespace rk::ui
