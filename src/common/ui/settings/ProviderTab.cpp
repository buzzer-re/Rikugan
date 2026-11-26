#include "common/ui/settings/ProviderTab.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "common/config/ConnectionTest.h"
#include "common/config/Paths.h"
#include "common/omc/AgentHome.h"
#include "common/omc/AuthSync.h"
#include "common/omc/ExternalLogins.h"
#include "common/omc/ServiceSession.h"
#include "common/ui/Theme.h"
#include "common/ui/console/LoginSheet.h"
#include "common/ui/settings/ExternalLoginsConsent.h"
#include "common/ui/settings/ServiceRpc.h"
#include "common/util/Files.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

const QStringList& connectionApis() {
    static const QStringList apis{QStringLiteral("openai-completions"), QStringLiteral("openai-responses"),
                                  QStringLiteral("anthropic-messages"), QStringLiteral("ollama-chat")};
    return apis;
}

// omc's effort order (ai_common.c effort_names)
const QStringList& effortNames() {
    static const QStringList e{QStringLiteral("off"),    QStringLiteral("minimal"), QStringLiteral("low"),
                               QStringLiteral("medium"), QStringLiteral("high"),    QStringLiteral("xhigh"),
                               QStringLiteral("max")};
    return e;
}

QString keyOf(const QJsonObject& m) {
    return m.value(QStringLiteral("provider")).toString() + QLatin1Char('/') + m.value(QStringLiteral("id")).toString();
}

QString authPath() { return paths::omcAgentDir() + QStringLiteral("/auth.json"); }

// A provider's state in the combo: a filled green dot when it can be used, an empty ring when not.
QIcon statusDot(bool ready) {
    const qreal dpr = 2.0;
    QPixmap pm(QSize(12, 12) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = ready ? ui::theme().ok : ui::theme().fg3;
    p.setPen(ready ? Qt::NoPen : QPen(c, 1.2));
    p.setBrush(ready ? QBrush(c) : QBrush(Qt::NoBrush));
    p.drawEllipse(QRectF(2, 2, 8, 8));
    return QIcon(pm);
}

QPushButton* squareButton(const QString& text, const QString& tip, const char* name) {
    auto* b = new QPushButton(text);
    b->setObjectName(QLatin1String(name));
    b->setFixedWidth(32);
    b->setToolTip(tip);
    return b;
}

}  // namespace

ProviderTab::ProviderTab(const RikuganConfig& cfg, QWidget* parent)
    : QWidget(parent),
      wantedModel_(cfg.model),
      wantedThinking_(cfg.thinking),
      wantedProvider_(cfg.model.section(QLatin1Char('/'), 0, 0)),
      customs_(cfg.customEndpoints) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* group = new QGroupBox(tr("LLM Provider"));
    form_ = new QFormLayout(group);

    auto* providerRow = new QHBoxLayout;
    provider_ = new QComboBox;
    provider_->setObjectName(QStringLiteral("providerCombo"));
    provider_->setMinimumWidth(300);
    providerRow->addWidget(provider_, 1);
    addConn_ = squareButton(QStringLiteral("+"), tr("Add a custom connection (OpenAI-compatible, Anthropic or Ollama)"),
                            "addConnectionButton");
    removeConn_ = squareButton(QStringLiteral("\u2212"), tr("Remove this custom connection"), "removeConnectionButton");
    providerRow->addWidget(addConn_);
    providerRow->addWidget(removeConn_);
    form_->addRow(tr("Provider:"), providerRow);

    auto* accountRow = new QHBoxLayout;
    account_ = new QLabel;
    account_->setObjectName(QStringLiteral("accountStatus"));
    accountRow->addWidget(account_, 1);
    loginBtn_ = new QPushButton(tr("Log in"));
    loginBtn_->setObjectName(QStringLiteral("loginButton"));
    logoutBtn_ = new QPushButton(tr("Log out"));
    logoutBtn_->setObjectName(QStringLiteral("logoutButton"));
    accountRow->addWidget(loginBtn_);
    accountRow->addWidget(logoutBtn_);
    form_->addRow(tr("Account:"), accountRow);

    auto* keyRow = new QHBoxLayout;
    key_ = new QLineEdit;
    key_->setObjectName(QStringLiteral("apiKeyEdit"));
    key_->setEchoMode(QLineEdit::Password);
    keyRow->addWidget(key_, 1);
    saveKey_ = new QPushButton(tr("Save key"));
    saveKey_->setObjectName(QStringLiteral("saveApiKeyButton"));
    keyRow->addWidget(saveKey_);
    form_->addRow(tr("API Key:"), keyRow);

    apiBase_ = new QLineEdit;
    apiBase_->setObjectName(QStringLiteral("apiBaseEdit"));
    apiBase_->setPlaceholderText(QStringLiteral("https://api.example.com/v1"));
    form_->addRow(tr("API Base:"), apiBase_);
    apiType_ = new QComboBox;
    apiType_->setObjectName(QStringLiteral("apiTypeCombo"));
    apiType_->addItems(connectionApis());
    form_->addRow(tr("API type:"), apiType_);
    connModels_ = new QLineEdit;
    connModels_->setObjectName(QStringLiteral("connectionModelsEdit"));
    connModels_->setPlaceholderText(tr("Model ids the endpoint serves, comma separated"));
    form_->addRow(tr("Models:"), connModels_);
    auto tokenSpin = [](const char* name, int max, const QString& unset, const QString& tip) {
        auto* s = new QSpinBox;
        s->setObjectName(QLatin1String(name));
        s->setRange(0, max);
        s->setSingleStep(1024);
        s->setSpecialValueText(unset);  // shown at 0
        s->setSuffix(tr(" tokens"));
        s->setToolTip(tip);
        return s;
    };
    connContext_ = tokenSpin("connectionContextSpin", 100'000'000, tr("Default (128000)"),
                             tr("The context window of this endpoint's models."));
    form_->addRow(tr("Context window:"), connContext_);
    connMaxTokens_ = tokenSpin("connectionMaxTokensSpin", 10'000'000, tr("Default"),
                               tr("The most tokens a reply may have. Default: 32000, or the context window when smaller."));
    form_->addRow(tr("Max output:"), connMaxTokens_);
    // a private server with a self-signed certificate: trust that certificate (verification stays on)
    connCaRow_ = new QWidget;
    auto* caRow = new QHBoxLayout(connCaRow_);
    caRow->setContentsMargins(0, 0, 0, 0);
    connCa_ = new QLineEdit;
    connCa_->setObjectName(QStringLiteral("connectionCaEdit"));
    connCa_->setPlaceholderText(tr("None: the system's trusted authorities"));
    connCa_->setToolTip(tr("A PEM file to trust for this server's HTTPS: its self-signed certificate, or the CA that "
                           "signed it. The connection is still verified."));
    caRow->addWidget(connCa_, 1);
    auto* caBrowse = new QPushButton(tr("Browse\u2026"));
    caRow->addWidget(caBrowse);
    form_->addRow(tr("Certificate:"), connCaRow_);
    connTestRow_ = new QWidget;
    auto* testRow = new QVBoxLayout(connTestRow_);
    testRow->setContentsMargins(0, 0, 0, 0);
    auto* testBtn = new QPushButton(tr("Test connection"));
    testBtn->setObjectName(QStringLiteral("testConnectionButton"));
    testRow->addWidget(testBtn, 0, Qt::AlignLeft);
    connTestResult_ = new QLabel;
    connTestResult_->setObjectName(QStringLiteral("testConnectionResult"));
    connTestResult_->setWordWrap(true);
    connTestResult_->setTextFormat(Qt::PlainText);
    connTestResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    testRow->addWidget(connTestResult_);
    form_->addRow(QString(), connTestRow_);
    connect(caBrowse, &QPushButton::clicked, this, [this] {
        const int i = customIndex(currentProvider());
        if (i < 0) return;
        const QString f = QFileDialog::getOpenFileName(this, tr("Certificate to trust"), QFileInfo(connCa_->text()).path(),
                                                       tr("Certificates (*.pem *.crt *.cer);;All files (*)"));
        if (f.isEmpty()) return;
        connCa_->setText(f);
        customs_[i].caFile = f;
    });
    connect(testBtn, &QPushButton::clicked, this, [this, testBtn] {
        const int i = customIndex(currentProvider());
        if (i < 0) return;
        const CustomEndpoint e = customs_[i];
        testBtn->setEnabled(false);
        setStatus(connTestResult_, tr("Asking %1\u2026").arg(connectionTestUrl(e)), settings::kHintStyle);
        QPointer<ProviderTab> self(this);
        QPointer<QPushButton> btn(testBtn);
        spawnThread([self, btn, e] {
            const ConnectionTestResult r = testConnection(e);
            QMetaObject::invokeMethod(
                uiAnchor(), [self, btn, r] {
                    if (btn) btn->setEnabled(true);
                    if (self) self->setStatus(self->connTestResult_, r.message, r.ok ? settings::kOkStyle : settings::kErrStyle);
                },
                Qt::QueuedConnection);
        });
    });

    auto* modelRow = new QHBoxLayout;
    model_ = new QComboBox;
    model_->setObjectName(QStringLiteral("modelCombo"));
    model_->setEditable(true);
    model_->setInsertPolicy(QComboBox::NoInsert);
    model_->lineEdit()->setPlaceholderText(tr("Automatic: the first model you have access to"));
    modelRow->addWidget(model_, 1);
    auto* refresh = new QPushButton(tr("Refresh"));
    refresh->setObjectName(QStringLiteral("refreshModelsButton"));
    modelRow->addWidget(refresh);
    form_->addRow(tr("Model:"), modelRow);

    thinking_ = new QComboBox;
    thinking_->setObjectName(QStringLiteral("thinkingCombo"));
    form_->addRow(tr("Thinking:"), thinking_);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("providerStatus"));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    status_->setOpenExternalLinks(true);
    status_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    form_->addRow(QString(), status_);
    // omc reads models.yml once per process (catalog_init); only its own TUI re-reads it
    restartHint_ = new QLabel(tr("New or changed custom connections take effect after restarting the host."));
    restartHint_->setWordWrap(true);
    restartHint_->setStyleSheet(QString::fromLatin1(settings::kHintStyle));
    form_->addRow(QString(), restartHint_);
    layout->addWidget(group);

    useOmcLogins_ = new QCheckBox(tr("Also use the logins of the omc command-line agent (~/.omc/agent)"));
    useOmcLogins_->setObjectName(QStringLiteral("useOmcLogins"));
    useOmcLogins_->setChecked(cfg.useOmcLogins);
    useOmcLogins_->setToolTip(
        tr("If you also use omc outside Rikugan, the providers you logged into there are copied here, and the newer\n"
           "of two OAuth logins is kept on both sides. Keys you add in Rikugan stay in Rikugan."));
    layout->addWidget(useOmcLogins_);
    // turning it on copies those logins now, so their models show without reopening Settings
    connect(useOmcLogins_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) return;
        setStatus(status_, tr("Looking for omc logins\u2026"), settings::kHintStyle);
        settings::serviceJob(
            this, [](OmcSession&) { return QJsonObject{{"changed", AuthSync::syncNow()}}; },
            [this](const QJsonObject& r) {
                setStatus(status_, r.value(QStringLiteral("changed")).toBool() ? tr("Copied your omc logins.") : tr("No new omc logins found."),
                          settings::kHintStyle);
                readAuthFile();
                refreshProviders();
                refreshModels();
            });
    });
    useExternal_ = new QCheckBox(tr("Also use the Claude Code and Codex logins on this computer"));
    useExternal_->setObjectName(QStringLiteral("useExternalLogins"));
    useExternal_->setChecked(cfg.useExternalLogins);
    useExternal_->setToolTip(
        tr("Shares the subscription you are signed in to in Claude Code (Anthropic) and the Codex CLI (ChatGPT) with\n"
           "Rikugan; refreshed tokens are written back so both keep working. Using a subscription outside its own\n"
           "tool may be against the provider's terms."));
    layout->addWidget(useExternal_);
    connect(useExternal_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) return;
        const QList<ExternalLogin> found = ExternalLogins::find();
        if (found.isEmpty()) {
            setStatus(status_, tr("No Claude Code or Codex login found on this computer."), settings::kHintStyle);
            return;
        }
        if (!ui::confirmExternalLogins(this, found)) {
            QSignalBlocker block(useExternal_);
            useExternal_->setChecked(false);
            return;
        }
        setStatus(status_, tr("Using your %1 login\u2026").arg(ui::externalToolNames(found)), settings::kHintStyle);
        settings::serviceJob(
            this, [](OmcSession&) { return QJsonObject{{"changed", ExternalLogins::syncNow()}}; },
            [this, found](const QJsonObject& r) {
                setStatus(status_, r.value(QStringLiteral("changed")).toBool()
                                       ? tr("Using your %1 login.").arg(ui::externalToolNames(found))
                                       : tr("Rikugan already has a login for that provider: it stays as it is."),
                          settings::kHintStyle);
                readAuthFile();
                refreshProviders();
                refreshModels();
            });
    });
    discover_ = new QCheckBox(tr("Discover local model servers (Ollama, LM Studio, llama.cpp) — restart required"));
    discover_->setObjectName(QStringLiteral("discoverLocalModels"));
    discover_->setChecked(cfg.discoverLocalModels);
    discover_->setToolTip(tr("Looks for model servers on this machine when the host starts."));
    layout->addWidget(discover_);
    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);

    connect(provider_, &QComboBox::activated, this, [this] {
        const QString p = currentProvider();
        if (wantedModel_.section(QLatin1Char('/'), 0, 0) != p) {
            const QStringList ids = modelIdsFor(p);
            wantedModel_ = ids.isEmpty() ? QString() : p + QLatin1Char('/') + ids.first();
        }
        onProviderChanged();
    });
    connect(addConn_, &QPushButton::clicked, this, &ProviderTab::addConnection);
    connect(removeConn_, &QPushButton::clicked, this, &ProviderTab::removeConnection);
    connect(loginBtn_, &QPushButton::clicked, this, &ProviderTab::login);
    connect(logoutBtn_, &QPushButton::clicked, this, &ProviderTab::logout);
    connect(saveKey_, &QPushButton::clicked, this, &ProviderTab::saveApiKey);
    connect(key_, &QLineEdit::returnPressed, this, [this] {
        if (customIndex(currentProvider()) < 0) saveApiKey();
    });
    connect(key_, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (const int i = customIndex(currentProvider()); i >= 0) {
            customs_[i].apiKey = t.trimmed();
            updateAccountRow();
        }
    });
    connect(apiBase_, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (const int i = customIndex(currentProvider()); i >= 0) customs_[i].baseUrl = t.trimmed();
    });
    connect(apiType_, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        if (filling_) return;
        if (const int i = customIndex(currentProvider()); i >= 0) customs_[i].api = t;
    });
    connect(connModels_, &QLineEdit::textEdited, this, [this](const QString& t) {
        const int i = customIndex(currentProvider());
        if (i < 0) return;
        customs_[i].models.clear();
        for (const QString& m : t.split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!m.trimmed().isEmpty()) customs_[i].models << m.trimmed();
        fillModelCombo();
    });
    connect(connContext_, &QSpinBox::valueChanged, this, [this](int v) {
        if (filling_) return;
        if (const int i = customIndex(currentProvider()); i >= 0) customs_[i].contextWindow = v;
    });
    connect(connMaxTokens_, &QSpinBox::valueChanged, this, [this](int v) {
        if (filling_) return;
        if (const int i = customIndex(currentProvider()); i >= 0) customs_[i].maxTokens = v;
    });
    connect(connCa_, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (const int i = customIndex(currentProvider()); i >= 0) customs_[i].caFile = t.trimmed();
    });
    connect(model_, &QComboBox::currentTextChanged, this, [this] {
        if (filling_) return;
        wantedModel_ = modelKey();
        fillThinkingCombo();
    });
    connect(thinking_, &QComboBox::activated, this, [this] { wantedThinking_ = thinking_->currentData().toString(); });
    connect(refresh, &QPushButton::clicked, this, [this] {
        readAuthFile();
        refreshProviders();
        refreshModels();
    });

    readAuthFile();
    fillProviderCombo();
    refreshProviders();
    refreshModels();
    refreshThinkingLevels();
}

void ProviderTab::setStatus(QLabel* l, const QString& text, const char* style) {
    l->setStyleSheet(QString::fromLatin1(style));
    l->setText(text);
}

// ---------------------------------------------------------------- data

void ProviderTab::readAuthFile() {
    bool ok = false;
    auth_ = readJsonObject(authPath(), &ok);
    if (!ok) auth_ = QJsonObject();
}

void ProviderTab::refreshModels() {
    settings::serviceRequest(this, QJsonObject{{"type", "get_available_models"}}, [this](const QJsonObject& r) {
        if (!settings::succeeded(r)) {
            setStatus(status_, tr("Could not list models: %1").arg(settings::errorOf(r).toHtmlEscaped()), settings::kErrStyle);
            return;
        }
        models_ = settings::dataOf(r).value(QStringLiteral("models")).toArray();
        fillProviderCombo();
    });
}

void ProviderTab::refreshProviders() {
    settings::serviceRequest(this, QJsonObject{{"type", "get_login_providers"}}, [this](const QJsonObject& r) {
        if (!settings::succeeded(r)) {
            setStatus(status_, tr("Could not list providers: %1").arg(settings::errorOf(r).toHtmlEscaped()),
                      settings::kErrStyle);
            return;
        }
        logins_.clear();
        for (const QJsonValue& v : settings::dataOf(r).value(QStringLiteral("providers")).toArray()) {
            const QJsonObject p = v.toObject();
            logins_ << LoginProvider{p.value(QStringLiteral("id")).toString(), p.value(QStringLiteral("name")).toString(),
                                     p.value(QStringLiteral("authenticated")).toBool()};
        }
        fillProviderCombo();
    });
}

void ProviderTab::refreshThinkingLevels() {
    settings::serviceRequest(this, QJsonObject{{"type", "get_available_thinking_levels"}}, [this](const QJsonObject& r) {
        if (!settings::succeeded(r)) return;
        serviceLevels_.clear();
        for (const QJsonValue& v : settings::dataOf(r).value(QStringLiteral("levels")).toArray())
            serviceLevels_ << v.toString();
        fillThinkingCombo();
    });
}

QString ProviderTab::currentProvider() const { return provider_->currentData().toString(); }

int ProviderTab::customIndex(const QString& id) const {
    for (int i = 0; i < customs_.size(); ++i)
        if (customs_[i].id == id) return i;
    return -1;
}

QStringList ProviderTab::modelIdsFor(const QString& provider) const {
    if (const int i = customIndex(provider); i >= 0) return customs_[i].models;
    QStringList ids;
    for (const QJsonValue& v : models_) {
        const QJsonObject m = v.toObject();
        if (m.value(QStringLiteral("provider")).toString() == provider) ids << m.value(QStringLiteral("id")).toString();
    }
    ids.sort(Qt::CaseInsensitive);
    ids.removeDuplicates();
    return ids;
}

QString ProviderTab::modelKey() const {
    const QString text = model_->currentText().trimmed();
    if (text.isEmpty()) return {};
    const int idx = model_->findText(text);
    if (idx >= 0) return model_->itemData(idx).toString();
    return currentProvider() + QLatin1Char('/') + text;  // typed by hand; ids may contain '/' (openrouter)
}

// ---------------------------------------------------------------- combos

void ProviderTab::fillProviderCombo() {
    QSet<QString> withModels;
    for (const QJsonValue& v : models_) withModels.insert(v.toObject().value(QStringLiteral("provider")).toString());

    struct Entry {
        QString id, label;
        bool ready;
    };
    QList<Entry> entries;
    QSet<QString> seen;
    auto add = [&](const QString& id, const QString& label, bool ready) {
        if (id.isEmpty() || seen.contains(id) || id == AgentHome::serviceProvider()) return;
        seen.insert(id);
        entries << Entry{id, label, ready};
    };
    for (const CustomEndpoint& e : customs_) add(e.id, tr("%1 (custom connection)").arg(e.id), true);
    for (const LoginProvider& l : logins_)
        add(l.id, l.name.isEmpty() ? l.id : l.name, l.authenticated || auth_.contains(l.id) || withModels.contains(l.id));
    for (const QString& p : withModels) add(p, p, true);  // a key from the environment, a discovered local server
    if (!wantedProvider_.isEmpty()) add(wantedProvider_, wantedProvider_, false);

    filling_ = true;
    provider_->clear();
    for (const Entry& e : entries) {
        provider_->addItem(statusDot(e.ready), e.label, e.id);
        provider_->setItemData(provider_->count() - 1, e.ready ? tr("%1: ready").arg(e.id) : tr("%1: not set up").arg(e.id),
                               Qt::ToolTipRole);
    }
    int idx = provider_->findData(wantedProvider_);
    if (idx < 0)
        for (int i = 0; i < entries.size() && idx < 0; ++i)
            if (entries[i].ready) idx = i;
    if (idx < 0) idx = provider_->findData(QStringLiteral("anthropic"));
    provider_->setCurrentIndex(std::max(idx, 0));
    filling_ = false;
    onProviderChanged();
}

void ProviderTab::onProviderChanged() {
    if (provider_->count() == 0) return;
    wantedProvider_ = currentProvider();
    updateConnectionRows();
    updateAccountRow();
    fillModelCombo();
    fillThinkingCombo();
}

void ProviderTab::fillModelCombo() {
    const QString p = currentProvider();
    const QStringList ids = modelIdsFor(p);
    QHash<QString, QString> names;
    for (const QJsonValue& v : models_) {
        const QJsonObject m = v.toObject();
        names.insert(keyOf(m), m.value(QStringLiteral("name")).toString());
    }
    filling_ = true;
    model_->clear();
    for (const QString& id : ids) {
        const QString key = p + QLatin1Char('/') + id;
        model_->addItem(id, key);
        if (!names.value(key).isEmpty() && names.value(key) != id) model_->setItemData(model_->count() - 1, names.value(key), Qt::ToolTipRole);
    }
    const QString prefix = p + QLatin1Char('/');
    if (wantedModel_.startsWith(prefix)) {
        const int idx = model_->findData(wantedModel_);
        if (idx >= 0) model_->setCurrentIndex(idx);
        else model_->setEditText(wantedModel_.mid(prefix.size()));  // configured but not available right now
    } else {
        model_->setCurrentIndex(-1);
        model_->setEditText(QString());  // automatic
    }
    filling_ = false;

    const bool custom = customIndex(p) >= 0;
    if (!ids.isEmpty())
        setStatus(status_, tr("%n model(s) available.", nullptr, int(ids.size())), settings::kHintStyle);
    else if (custom)
        setStatus(status_, tr("List the model ids this connection serves above."), settings::kHintStyle);
    else
        setStatus(status_, tr("No models listed for this provider yet: log in or save an API key, then press Refresh."),
                  settings::kHintStyle);
}

QStringList ProviderTab::levelsFor(const QString& model) const {
    if (model.isEmpty()) return serviceLevels_;
    for (const QJsonValue& v : models_) {
        const QJsonObject m = v.toObject();
        if (keyOf(m) != model) continue;
        // omc's model_supports_effort: no reasoning -> off only; thinking.efforts lists them; else up to high
        QStringList out{QStringLiteral("off")};
        if (!m.value(QStringLiteral("reasoning")).toBool()) return out;
        const QJsonValue efforts = m.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("efforts"));
        for (qsizetype i = 1; i < effortNames().size(); ++i) {
            const QString& e = effortNames()[i];
            if (efforts.isArray() ? efforts.toArray().contains(e) : e != QLatin1String("xhigh") && e != QLatin1String("max"))
                out << e;
        }
        return out;
    }
    return serviceLevels_;
}

void ProviderTab::fillThinkingCombo() {
    thinking_->clear();
    thinking_->addItem(tr("(model default)"), QString());
    for (const QString& l : levelsFor(wantedModel_)) thinking_->addItem(l, l);
    int idx = thinking_->findData(wantedThinking_);
    if (idx < 0) {
        thinking_->addItem(wantedThinking_, wantedThinking_);  // clamped to the model's nearest level when it runs
        idx = thinking_->count() - 1;
    }
    thinking_->setCurrentIndex(idx);
}

void ProviderTab::updateConnectionRows() {
    const int i = customIndex(currentProvider());
    const bool custom = i >= 0;
    form_->setRowVisible(apiBase_, custom);
    form_->setRowVisible(apiType_, custom);
    form_->setRowVisible(connModels_, custom);
    form_->setRowVisible(connContext_, custom);
    form_->setRowVisible(connMaxTokens_, custom);
    form_->setRowVisible(connCaRow_, custom);
    form_->setRowVisible(connTestRow_, custom);
    connTestResult_->clear();
    restartHint_->setVisible(custom);
    removeConn_->setEnabled(custom);
    saveKey_->setVisible(!custom);
    filling_ = true;
    if (custom) {
        const CustomEndpoint& e = customs_[i];
        apiBase_->setText(e.baseUrl);
        if (apiType_->findText(e.api) < 0 && !e.api.isEmpty()) apiType_->addItem(e.api);
        apiType_->setCurrentText(e.api.isEmpty() ? connectionApis().first() : e.api);
        connModels_->setText(e.models.join(QStringLiteral(", ")));
        connContext_->setValue(e.contextWindow);
        connMaxTokens_->setValue(e.maxTokens);
        connCa_->setText(e.caFile);
        key_->setText(e.apiKey);
        key_->setPlaceholderText(tr("API key for this endpoint (leave empty for local servers)"));
    } else {
        key_->clear();
        key_->setPlaceholderText(tr("sk-…  or use Log in"));
    }
    filling_ = false;
}

void ProviderTab::updateAccountRow() {
    const QString p = currentProvider();
    const int ci = customIndex(p);
    bool loginOffered = false, signedIn = false;
    for (const LoginProvider& l : logins_)
        if (l.id == p) {
            loginOffered = true;
            signedIn = l.authenticated;
        }
    QString text;
    if (ci >= 0) {
        text = customs_[ci].apiKey.isEmpty() ? tr("Custom connection, no API key") : tr("Custom connection, API key set");
    } else {
        const QString type = auth_.value(p).toObject().value(QStringLiteral("type")).toString();
        if (type == QLatin1String("oauth")) text = tr("Logged in");
        else if (type == QLatin1String("api_key")) text = tr("API key saved");
        else if (signedIn) text = tr("Logged in");
        else if (!modelIdsFor(p).isEmpty()) text = tr("Ready (key from the environment)");
        else text = tr("Not set up");
    }
    account_->setText(text);
    loginBtn_->setVisible(ci < 0 && loginOffered);
    loginBtn_->setEnabled(true);
    logoutBtn_->setVisible(ci < 0);
    logoutBtn_->setEnabled(signedIn || auth_.contains(p));
}

// ---------------------------------------------------------------- custom connections

void ProviderTab::addConnection() {
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("addConnectionDialog"));
    dlg->setWindowTitle(tr("Add Custom Connection"));
    auto* form = new QFormLayout(dlg);
    auto* name = new QLineEdit;
    name->setObjectName(QStringLiteral("connectionNameEdit"));
    name->setPlaceholderText(tr("e.g. minimax, deepseek, local-vllm"));
    form->addRow(tr("Connection Name:"), name);
    auto* url = new QLineEdit;
    url->setObjectName(QStringLiteral("connectionUrlEdit"));
    url->setPlaceholderText(QStringLiteral("https://api.example.com/v1"));
    form->addRow(tr("API Base URL:"), url);
    auto* api = new QComboBox;
    api->setObjectName(QStringLiteral("connectionApiCombo"));
    api->addItems(connectionApis());
    form->addRow(tr("API type:"), api);
    auto* error = new QLabel;
    error->setStyleSheet(QString::fromLatin1(settings::kErrStyle));
    error->setWordWrap(true);
    form->addRow(error);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(box);
    connect(box, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, dlg, [this, dlg, name, url, api, error] {
        const QString id = name->text().trimmed();
        const QUrl u(url->text().trimmed());
        if (id.isEmpty() || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char(' ')))
            return error->setText(tr("The name can't be empty or contain spaces or '/'."));
        if (customIndex(id) >= 0 || provider_->findData(id) >= 0 || id == AgentHome::serviceProvider())
            return error->setText(tr("\"%1\" is already a provider; choose another name.").arg(id));
        if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty())
            return error->setText(tr("Enter a base URL such as https://api.example.com/v1."));
        customs_ << CustomEndpoint{id, u.toString(), api->currentText(), QString(), {}};
        wantedProvider_ = id;
        wantedModel_.clear();
        fillProviderCombo();
        connModels_->setFocus();
        dlg->accept();
    });
    dlg->open();
}

void ProviderTab::removeConnection() {
    const QString p = currentProvider();
    const int i = customIndex(p);
    if (i < 0) return;
    if (QMessageBox::question(this, tr("Remove connection"), tr("Remove the custom connection \"%1\"?").arg(p)) !=
        QMessageBox::Yes)
        return;
    customs_.removeAt(i);
    if (wantedModel_.startsWith(p + QLatin1Char('/'))) wantedModel_.clear();
    wantedProvider_.clear();
    fillProviderCombo();
}

QString ProviderTab::validate() const {
    QSet<QString> ids;
    for (const CustomEndpoint& e : customs_) {
        if (e.id.isEmpty() || e.id.contains(QLatin1Char('/')) || e.id.contains(QLatin1Char(' ')))
            return tr("Custom connection \"%1\" needs a name without spaces or '/'.").arg(e.id);
        if (ids.contains(e.id)) return tr("Two custom connections are named \"%1\".").arg(e.id);
        ids.insert(e.id);
        for (const LoginProvider& l : logins_)
            if (l.id == e.id) return tr("\"%1\" is already a provider; rename the custom connection.").arg(e.id);
        if (e.id == AgentHome::serviceProvider()) return tr("\"%1\" is reserved; rename the custom connection.").arg(e.id);
        const QUrl u(e.baseUrl);
        if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty())
            return tr("Custom connection \"%1\" needs a base URL such as https://api.example.com/v1.").arg(e.id);
        if (e.models.isEmpty()) return tr("Custom connection \"%1\" lists no models.").arg(e.id);
    }
    return {};
}

void ProviderTab::apply(RikuganConfig& c) const {
    c.model = modelKey();
    c.thinking = thinking_->currentData().toString();
    c.useOmcLogins = useOmcLogins_->isChecked();
    c.useExternalLogins = useExternal_->isChecked();
    c.discoverLocalModels = discover_->isChecked();
    c.customEndpoints = customs_;
}

// ---------------------------------------------------------------- keys and logins

void ProviderTab::saveApiKey() {
    const QString provider = currentProvider();
    const QString key = key_->text().trimmed();
    if (provider.isEmpty() || key.isEmpty()) {
        setStatus(status_, tr("Enter the API key first."), settings::kErrStyle);
        return;
    }
    if (storedCredentialType(provider) == QLatin1String("oauth") &&
        QMessageBox::question(this, tr("Save API key"),
                              tr("You are logged in to %1. Replace the login with this API key?").arg(provider)) !=
            QMessageBox::Yes)
        return;
    const QString err = storeApiKey(provider, key);
    if (!err.isEmpty()) {
        setStatus(status_, err.toHtmlEscaped(), settings::kErrStyle);
        return;
    }
    key_->clear();
    readAuthFile();
    setStatus(status_, tr("Saved the API key for %1. Loading its models…").arg(provider.toHtmlEscaped()), settings::kOkStyle);
    updateAccountRow();
    refreshModels();  // queued behind the credential reload on the service worker
    refreshProviders();
}

// The same login sheet as the panel's "Connect a provider", in a dialog.
void ProviderTab::login() {
    const QString pid = currentProvider();
    if (pid.isEmpty()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Log in"));
    dlg.setObjectName(QStringLiteral("rk_root"));
    dlg.setStyleSheet(ui::themeStylesheet());
    dlg.resize(560, 420);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* sheet = new ui::LoginSheet(&dlg, false);
    layout->addWidget(sheet);
    connect(sheet, &ui::LoginSheet::closed, &dlg, &QDialog::accept);
    connect(sheet, &ui::LoginSheet::connected, &dlg, &QDialog::accept);
    sheet->startLogin(pid);
    dlg.exec();
    readAuthFile();
    updateAccountRow();
    refreshProviders();
    refreshModels();
}

void ProviderTab::logout() {
    const QString pid = currentProvider();
    if (pid.isEmpty()) return;
    if (QMessageBox::question(this, tr("Log out"), tr("Remove the stored login or key for %1?").arg(pid)) != QMessageBox::Yes)
        return;
    settings::serviceJob(
        this,
        [pid](OmcSession& s) {
            const QJsonObject acc = s.request(QJsonObject{{"type", "get_logout_accounts"}, {"providerId", pid}});
            if (!settings::succeeded(acc)) return acc;
            const QJsonArray rows = settings::dataOf(acc).value(QStringLiteral("accounts")).toArray();
            if (rows.isEmpty()) return QJsonObject{{"success", false}, {"error", "nothing is stored for this provider"}};
            return s.request(QJsonObject{{"type", "logout"},
                                         {"providerId", pid},
                                         {"credentialId", rows.first().toObject().value(QStringLiteral("credentialId"))}});
        },
        [this, pid](const QJsonObject& r) {
            if (settings::succeeded(r))
                setStatus(status_, tr("Logged out of %1.").arg(pid.toHtmlEscaped()), settings::kOkStyle);
            else
                setStatus(status_, tr("Logout from %1 failed: %2").arg(pid.toHtmlEscaped(), settings::errorOf(r).toHtmlEscaped()),
                          settings::kErrStyle);
            readAuthFile();
            refreshProviders();
            refreshModels();
        });
}

}  // namespace rk
