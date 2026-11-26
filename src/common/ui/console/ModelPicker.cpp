#include "common/ui/console/ModelPicker.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

#include "common/omc/ServiceSession.h"
#include "common/omc/UserErrors.h"
#include "common/ui/Theme.h"
#include "common/util/ModuleLifetime.h"

namespace rk::ui {

namespace {

QString contextLabel(qint64 n) {
    if (n <= 0) return {};
    if (n >= 1000000) return QStringLiteral("%1M").arg(double(n) / 1e6, 0, 'g', 3);
    return QStringLiteral("%1k").arg(n / 1000);
}

}  // namespace

ModelPicker::ModelPicker(QWidget* area) : PalettePopup(area, true, tr("Search models\u2026")) {
    list()->setMaxRows(12);
    // thinking level, one segmented row
    thinkingRow_ = new QWidget(this);
    auto* row = new QHBoxLayout(thinkingRow_);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(0);
    auto* label = new QLabel(tr("THINKING"), thinkingRow_);
    label->setFont(theme().uiSmall);
    label->setProperty("rkMuted", true);
    row->addWidget(label);
    row->addSpacing(8);
    thinkingButtons_ = new QHBoxLayout;
    thinkingButtons_->setSpacing(0);
    row->addLayout(thinkingButtons_);
    row->addStretch(1);
    thinkingModel_ = new QLabel(thinkingRow_);
    thinkingModel_->setFont(theme().uiSmall);
    thinkingModel_->setProperty("rkMuted", true);
    row->addWidget(thinkingModel_);
    thinkingGroup_ = new QButtonGroup(this);
    thinkingGroup_->setExclusive(true);
    setHeaderWidget(thinkingRow_);

    auto* footer = new QWidget(this);
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(6, 2, 8, 2);
    auto* connectBtn = new QPushButton(tr("+  Connect a provider\u2026"), footer);
    connectBtn->setCursor(Qt::PointingHandCursor);
    connectBtn->setFlat(true);
    fl->addWidget(connectBtn);
    fl->addStretch(1);
    auto* keys = new QLabel(tr("\u2191\u2193 \u21B5 \u00b7 Alt \u2190/\u2192 thinking"), footer);
    keys->setFont(theme().uiSmall);
    keys->setProperty("rkMuted", true);
    fl->addWidget(keys);
    setFooterWidget(footer);

    connect(connectBtn, &QPushButton::clicked, this, [this] {
        dismiss();
        Q_EMIT connectRequested({});
    });
    connect(list(), &PaletteList::activated, this, [this](const QString& key) {
        dismiss();
        Q_EMIT modelChosen(key);
    });
    connect(list(), &PaletteList::sectionActionActivated, this, [this](const QString& section) {
        const QString id = sectionProvider_.value(section);
        dismiss();
        Q_EMIT connectRequested(id);
    });
    connect(thinkingGroup_, &QButtonGroup::idClicked, this, [this](int i) {
        if (i < 0 || i >= levels_.size()) return;
        thinking_ = levels_.at(i);
        Q_EMIT thinkingChosen(thinking_);
    });
    const QString muted = theme().fg3.name();
    setStyleSheet(styleSheet() + QStringLiteral("QLabel[rkMuted=\"true\"] { color: %1; }").arg(muted));
}

void ModelPicker::open(QWidget* anchor, const QString& current, const QString& thinking, const QStringList& levels) {
    current_ = current;
    thinking_ = thinking;
    setLevels(levels, thinking);
    thinkingModel_->setText(displayName(current));
    rebuild();
    popupAt(anchor, true);
    refresh();
}

void ModelPicker::setLevels(const QStringList& levels, const QString& current) {
    if (levels != levels_) {
        levels_ = levels;
        for (QAbstractButton* b : thinkingGroup_->buttons()) {
            thinkingGroup_->removeButton(b);
            b->hide();
            b->deleteLater();
        }
        const Theme& t = theme();
        for (int i = 0; i < levels.size(); ++i) {
            auto* b = new QToolButton(thinkingRow_);
            b->setText(levels[i]);
            b->setCheckable(true);
            b->setCursor(Qt::PointingHandCursor);
            b->setFont(t.uiSmall);
            b->setStyleSheet(QStringLiteral("QToolButton { border: 1px solid %1; border-radius: 0; padding: 1px 7px; color: %2;"
                                            " background: %3; } QToolButton:checked { background: %4; color: %5; border-color: %4; }")
                                 .arg(t.line2.name(), t.fg2.name(), t.bg.name(), t.accent.name(), t.accentFg.name()));
            thinkingGroup_->addButton(b, i);
            thinkingButtons_->addWidget(b);
        }
    }
    thinkingRow_->setVisible(!levels.isEmpty());
    const int i = int(levels.indexOf(current));
    if (i >= 0 && thinkingGroup_->button(i)) thinkingGroup_->button(i)->setChecked(true);
}

void ModelPicker::stepThinking(int delta) {
    if (levels_.isEmpty()) return;
    int i = int(levels_.indexOf(thinking_));
    i = std::clamp(i < 0 ? 0 : i + delta, 0, int(levels_.size()) - 1);
    if (levels_.at(i) == thinking_) return;
    thinking_ = levels_.at(i);
    if (QAbstractButton* b = thinkingGroup_->button(i)) b->setChecked(true);
    Q_EMIT thinkingChosen(thinking_);
}

void ModelPicker::keyPressEvent(QKeyEvent* e) {
    if (e->modifiers() & Qt::AltModifier && (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)) {
        stepThinking(e->key() == Qt::Key_Left ? -1 : 1);
        return;
    }
    PalettePopup::keyPressEvent(e);
}

QString ModelPicker::displayName(const QString& key) const {
    for (const ModelInfo& m : models_)
        if (m.key() == key || (!key.contains(QLatin1Char('/')) && m.id == key)) return m.name.isEmpty() ? m.id : m.name;
    return key.section(QLatin1Char('/'), -1);
}

void ModelPicker::refresh() {
    QPointer<ModelPicker> self(this);
    ServiceSession::get()->post([self](OmcSession& s) {
        const QJsonObject r = s.request(QJsonObject{{"type", "get_available_models"}});
        QMetaObject::invokeMethod(
            uiAnchor(),
            [self, r] {
                if (!self) return;
                QList<ModelInfo> models;
                QString error;
                if (!r.value(QLatin1String("success")).toBool()) {
                    error = userFacingError(r.value(QLatin1String("error")).toString());
                    if (error.isEmpty()) error = tr("request failed");
                } else {
                    for (const QJsonValue& v : r.value(QLatin1String("data")).toObject().value(QLatin1String("models")).toArray()) {
                        const QJsonObject m = v.toObject();
                        ModelInfo info{m.value(QLatin1String("provider")).toString(), m.value(QLatin1String("id")).toString(),
                                       m.value(QLatin1String("name")).toString(),
                                       qint64(m.value(QLatin1String("contextWindow")).toDouble()),
                                       m.value(QLatin1String("reasoning")).toBool()};
                        if (info.id.isEmpty()) continue;
                        if (std::none_of(models.cbegin(), models.cend(), [&](const ModelInfo& x) { return x.key() == info.key(); }))
                            models << info;
                    }
                }
                const bool changed = models != self->models_ || error != self->error_ || !self->loaded_;
                self->models_ = models;
                self->error_ = error;
                self->loaded_ = true;
                if (changed) {
                    self->thinkingModel_->setText(self->displayName(self->current_));
                    self->rebuild();
                    Q_EMIT self->modelsChanged();
                }
                Q_EMIT self->refreshed();
            },
            Qt::QueuedConnection);
    });
    fetchLoginProviders(this, [this](const QList<LoginProviderInfo>& providers, const QString&) {
        const bool changed = providers.size() != providers_.size() ||
                             !std::equal(providers.cbegin(), providers.cend(), providers_.cbegin(), [](const auto& a, const auto& b) {
                                 return a.id == b.id && a.authenticated == b.authenticated && a.name == b.name;
                             });
        providers_ = providers;
        if (changed) rebuild();
    });
}

void ModelPicker::rebuild() {
    QList<PaletteEntry> entries;
    sectionProvider_.clear();
    QHash<QString, QString> names;
    for (const LoginProviderInfo& p : std::as_const(providers_)) names.insert(p.id, p.name);
    if (!error_.isEmpty()) {
        PaletteEntry e;
        e.key = QStringLiteral("!error");
        e.title = tr("Could not load models: %1").arg(error_);
        e.enabled = false;
        entries << e;
    } else if (!loaded_) {
        PaletteEntry e;
        e.key = QStringLiteral("!loading");
        e.title = tr("Loading models\u2026");
        e.enabled = false;
        entries << e;
    }
    // connected providers in the order their models came, then the ones to connect
    QStringList order;
    for (const ModelInfo& m : std::as_const(models_))
        if (!order.contains(m.provider)) order << m.provider;
    for (const QString& provider : std::as_const(order)) {
        const QString section = names.value(provider, provider);
        sectionProvider_.insert(section, provider);
        bool first = true;
        for (const ModelInfo& m : std::as_const(models_)) {
            if (m.provider != provider) continue;
            PaletteEntry e;
            e.key = m.key();
            e.section = section;
            e.title = m.name.isEmpty() ? m.id : m.name;
            e.detail = m.id;
            e.search = m.id + QLatin1Char(' ') + provider;
            e.hint = contextLabel(m.contextWindow);
            e.checked = m.key() == current_ || (!current_.contains(QLatin1Char('/')) && m.id == current_);
            if (first) e.sectionNote = QStringLiteral("\u25CF ready");
            first = false;
            entries << e;
        }
    }
    for (const LoginProviderInfo& p : std::as_const(providers_)) {
        if (p.authenticated || order.contains(p.id)) continue;
        PaletteEntry e;  // heading only: no models until connected
        e.key = QStringLiteral("!connect:") + p.id;
        e.section = p.name.isEmpty() ? p.id : p.name;
        e.search = p.name + QLatin1Char(' ') + p.id;
        e.sectionNote = QStringLiteral("\u25CB not connected");
        e.sectionAction = tr("Connect");
        e.enabled = false;
        sectionProvider_.insert(e.section, p.id);
        entries << e;
    }
    if (loaded_ && models_.isEmpty() && error_.isEmpty()) {
        PaletteEntry e;
        e.key = QStringLiteral("!none");
        e.title = tr("No models yet: connect a provider below");
        e.enabled = false;
        entries << e;
    }
    list()->setEntries(entries);
}

}  // namespace rk::ui
