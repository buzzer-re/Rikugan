#include "common/ui/settings/ProfilesTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "common/config/Profile.h"
#include "common/ui/settings/ServiceRpc.h"

namespace rk {

namespace {

bool isBuiltin(const QString& name) { return name == QLatin1String("default") || name == QLatin1String("private"); }

QStringList textToLines(const QString& text) {
    QStringList out;
    for (const QString& l : text.split(QLatin1Char('\n')))
        if (!l.trimmed().isEmpty()) out << l.trimmed();
    return out;
}

QString ruleLabel(const QJsonObject& rule) {
    const QString name = rule.value(QStringLiteral("name")).toString(QStringLiteral("?"));
    const QJsonValue re = rule.value(QStringLiteral("is_regex"));
    const bool regex = re.isBool() ? re.toBool() : (re.isDouble() ? re.toDouble() != 0 : !re.toString().isEmpty());
    return QStringLiteral("%1 — %2: %3")
        .arg(name, regex ? QStringLiteral("regex") : QStringLiteral("exact"), rule.value(QStringLiteral("pattern")).toString());
}

}  // namespace

ProfilesTab::ProfilesTab(const RikuganConfig& cfg, QWidget* parent) : QWidget(parent), custom_(cfg.customProfiles) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);
    outer->addLayout(buildProfileSelector());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(buildDescriptionSection());
    hideMetadata_ = new QCheckBox(tr("Hide binary metadata"));
    layout->addWidget(hideMetadata_);
    layout->addWidget(buildIocFiltersSection());
    layout->addWidget(buildCustomRulesSection());
    layout->addWidget(buildDeniedToolsSection());
    layout->addWidget(buildAdvancedSection());
    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    populateCombo(cfg.activeProfile);
    loadProfile(combo_->currentText());
}

// ---------------------------------------------------------------- UI construction

QHBoxLayout* ProfilesTab::buildProfileSelector() {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Active Profile:")));
    combo_ = new QComboBox;
    combo_->setObjectName(QStringLiteral("profileCombo"));
    connect(combo_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        saveCurrentToWorkingCopy();
        loadProfile(name);
    });
    row->addWidget(combo_, 1);
    auto* add = new QPushButton(tr("+ New"));
    connect(add, &QPushButton::clicked, this, &ProfilesTab::newProfile);
    row->addWidget(add);
    auto* clone = new QPushButton(tr("Clone"));
    connect(clone, &QPushButton::clicked, this, &ProfilesTab::cloneProfile);
    row->addWidget(clone);
    deleteBtn_ = new QPushButton(tr("Delete"));
    connect(deleteBtn_, &QPushButton::clicked, this, &ProfilesTab::deleteProfile);
    row->addWidget(deleteBtn_);
    return row;
}

QGroupBox* ProfilesTab::buildDescriptionSection() {
    auto* group = new QGroupBox(tr("Description"));
    auto* lay = new QVBoxLayout(group);
    desc_ = new QPlainTextEdit;
    desc_->setMaximumHeight(60);
    desc_->setPlaceholderText(tr("Describe this profile's purpose (shown to the AI agent in the system prompt)"));
    desc_->setToolTip(
        tr("This description is shown to the AI agent in the system prompt.\n"
           "A clear description helps the agent understand and follow your\n"
           "constraints (e.g. 'Private analysis — never leak sample hashes\n"
           "or reference external threat intelligence')."));
    lay->addWidget(desc_);
    return group;
}

QGroupBox* ProfilesTab::buildIocFiltersSection() {
    auto* group = new QGroupBox(tr("IOC Redaction Filters"));
    auto* outer = new QVBoxLayout(group);
    outer->setSpacing(4);

    auto* btns = new QHBoxLayout;
    btns->setSpacing(6);
    iocSelectAll_ = new QPushButton(tr("Select All"));
    connect(iocSelectAll_, &QPushButton::clicked, this, [this] {
        for (QCheckBox* cb : std::as_const(ioc_))
            if (cb->isEnabled()) cb->setChecked(true);
    });
    btns->addWidget(iocSelectAll_);
    iocDeselectAll_ = new QPushButton(tr("Deselect All"));
    connect(iocDeselectAll_, &QPushButton::clicked, this, [this] {
        for (QCheckBox* cb : std::as_const(ioc_))
            if (cb->isEnabled()) cb->setChecked(false);
    });
    btns->addWidget(iocDeselectAll_);
    btns->addStretch();
    outer->addLayout(btns);

    // two columns
    const auto& cats = iocFilterCategories();
    const qsizetype mid = (cats.size() + 1) / 2;
    auto* grid = new QHBoxLayout;
    grid->setSpacing(16);
    for (int c = 0; c < 2; ++c) {
        auto* col = new QVBoxLayout;
        col->setSpacing(3);
        for (qsizetype i = c ? mid : 0; i < (c ? cats.size() : mid); ++i) {
            auto* cb = new QCheckBox(cats[i].second);
            ioc_.insert(cats[i].first, cb);
            col->addWidget(cb);
        }
        col->addStretch();
        grid->addLayout(col);
    }
    outer->addLayout(grid);
    return group;
}

QGroupBox* ProfilesTab::buildCustomRulesSection() {
    auto* group = new QGroupBox(tr("Custom Filter Rules"));
    auto* lay = new QVBoxLayout(group);
    lay->setSpacing(4);

    auto* row1 = new QHBoxLayout;
    row1->setSpacing(6);
    ruleName_ = new QLineEdit;
    ruleName_->setPlaceholderText(tr("Name"));
    ruleName_->setFixedWidth(120);
    row1->addWidget(ruleName_);
    rulePattern_ = new QLineEdit;
    rulePattern_->setPlaceholderText(tr("Pattern (regex or exact string)"));
    row1->addWidget(rulePattern_, 1);
    lay->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    row2->setSpacing(6);
    ruleType_ = new QComboBox;
    ruleType_->addItems({QStringLiteral("Regex"), QStringLiteral("Exact")});
    ruleType_->setFixedWidth(80);
    row2->addWidget(ruleType_);
    ruleReplacement_ = new QLineEdit;
    ruleReplacement_->setPlaceholderText(tr("Replacement (default: [CUSTOM_REDACTED])"));
    row2->addWidget(ruleReplacement_, 1);
    addRule_ = new QPushButton(tr("+ Add"));
    addRule_->setMinimumWidth(70);
    connect(addRule_, &QPushButton::clicked, this, &ProfilesTab::addRule);
    row2->addWidget(addRule_);
    lay->addLayout(row2);

    auto* listRow = new QHBoxLayout;
    listRow->setSpacing(6);
    rules_ = new QListWidget;
    rules_->setMaximumHeight(72);
    listRow->addWidget(rules_, 1);
    removeRule_ = new QPushButton(tr("Remove"));
    removeRule_->setMinimumWidth(70);
    connect(removeRule_, &QPushButton::clicked, this, &ProfilesTab::removeRule);
    listRow->addWidget(removeRule_);
    lay->addLayout(listRow);
    return group;
}

QGroupBox* ProfilesTab::buildDeniedToolsSection() {
    // tool registries belong to open databases; the dialog edits the global profile list by name
    auto* group = new QGroupBox(tr("Denied Tools"));
    auto* lay = new QVBoxLayout(group);
    deniedTools_ = new QPlainTextEdit;
    deniedTools_->setMaximumHeight(60);
    deniedTools_->setPlaceholderText(tr("One tool name per line (e.g. list_functions, read_bytes)"));
    lay->addWidget(deniedTools_);
    return group;
}

QGroupBox* ProfilesTab::buildAdvancedSection() {
    auto* group = new QGroupBox(tr("Advanced"));
    auto* form = new QFormLayout(group);
    form->setSpacing(6);
    deniedFuncs_ = new QPlainTextEdit;
    deniedFuncs_->setMaximumHeight(48);
    deniedFuncs_->setPlaceholderText(tr("One function name per line (binary-specific)"));
    form->addRow(tr("Denied Functions:"), deniedFuncs_);
    customFilters_ = new QPlainTextEdit;
    customFilters_->setMaximumHeight(48);
    customFilters_->setPlaceholderText(tr("Custom prompt instructions (one per line)"));
    form->addRow(tr("Prompt Filters:"), customFilters_);
    return group;
}

// ---------------------------------------------------------------- profiles

void ProfilesTab::populateCombo(const QString& select) {
    const QSignalBlocker block(combo_);
    combo_->clear();
    for (const AnalysisProfile& p : listProfiles(custom_)) combo_->addItem(p.name);
    const int idx = combo_->findText(select);
    combo_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void ProfilesTab::loadProfile(const QString& name) {
    displayed_ = name;
    const AnalysisProfile p = getProfile(name, custom_);
    const bool builtin = isBuiltin(name);

    desc_->setPlainText(p.description);
    hideMetadata_->setChecked(p.hideBinaryMetadata);
    deniedTools_->setPlainText(p.deniedTools.join(QLatin1Char('\n')));
    deniedFuncs_->setPlainText(p.deniedFunctions.join(QLatin1Char('\n')));
    customFilters_->setPlainText(p.customFilters.join(QLatin1Char('\n')));
    for (auto it = ioc_.begin(); it != ioc_.end(); ++it) {
        it.value()->setChecked(p.iocFilters.value(it.key(), false));
        it.value()->setEnabled(!builtin);
    }
    rules_->clear();
    for (const QJsonValue& r : p.customFilterRules) rules_->addItem(ruleLabel(r.toObject()));

    desc_->setReadOnly(builtin);
    hideMetadata_->setEnabled(!builtin);
    deniedTools_->setReadOnly(builtin);
    deniedFuncs_->setReadOnly(builtin);
    customFilters_->setReadOnly(builtin);
    deleteBtn_->setEnabled(!builtin);
    for (QWidget* w : std::initializer_list<QWidget*>{addRule_, removeRule_, ruleName_, rulePattern_, ruleType_,
                                                      ruleReplacement_, iocSelectAll_, iocDeselectAll_})
        w->setEnabled(!builtin);
}

void ProfilesTab::saveCurrentToWorkingCopy() {
    const QString name = displayed_;
    if (name.isEmpty() || isBuiltin(name)) return;
    AnalysisProfile p;
    p.name = name;
    p.description = desc_->toPlainText().trimmed();
    p.hideBinaryMetadata = hideMetadata_->isChecked();
    for (auto it = ioc_.begin(); it != ioc_.end(); ++it) p.iocFilters.insert(it.key(), it.value()->isChecked());
    p.customFilterRules = getProfile(name, custom_).customFilterRules;
    p.deniedTools = textToLines(deniedTools_->toPlainText());
    p.deniedFunctions = textToLines(deniedFuncs_->toPlainText());
    p.customFilters = textToLines(customFilters_->toPlainText());
    // keep what this tab doesn't edit (singular_analysis, unknown keys)
    QJsonObject o = custom_.value(name).toObject();
    const QJsonObject edited = p.toJson();
    for (auto it = edited.begin(); it != edited.end(); ++it)
        if (it.key() != QLatin1String("singular_analysis")) o.insert(it.key(), it.value());
    custom_.insert(name, o);
}

// ---------------------------------------------------------------- custom filter rules

void ProfilesTab::addRule() {
    const QString ruleName = ruleName_->text().trimmed();
    const QString pattern = rulePattern_->text().trimmed();
    if (ruleName.isEmpty() || pattern.isEmpty()) return;
    const QString name = combo_->currentText();
    if (name.isEmpty() || isBuiltin(name)) return;
    const bool regex = ruleType_->currentText() == QLatin1String("Regex");
    QString replacement = ruleReplacement_->text().trimmed();
    if (replacement.isEmpty()) replacement = QStringLiteral("[CUSTOM_REDACTED]");
    const QJsonObject rule{{"name", ruleName}, {"pattern", pattern}, {"is_regex", regex}, {"replacement", replacement}};

    QJsonObject data = custom_.value(name).toObject();
    QJsonArray rules = data.value(QStringLiteral("custom_filter_rules")).toArray();
    rules.append(rule);
    data.insert(QStringLiteral("custom_filter_rules"), rules);
    custom_.insert(name, data);
    rules_->addItem(ruleLabel(rule));
    ruleName_->clear();
    rulePattern_->clear();
    ruleReplacement_->clear();
}

void ProfilesTab::removeRule() {
    const int row = rules_->currentRow();
    if (row < 0) return;
    const QString name = combo_->currentText();
    if (name.isEmpty() || isBuiltin(name)) return;
    QJsonObject data = custom_.value(name).toObject();
    QJsonArray rules = data.value(QStringLiteral("custom_filter_rules")).toArray();
    if (row < rules.size()) {
        rules.removeAt(row);
        data.insert(QStringLiteral("custom_filter_rules"), rules);
        custom_.insert(name, data);
    }
    delete rules_->takeItem(row);
}

// ---------------------------------------------------------------- new / clone / delete

void ProfilesTab::newProfile() {
    QString name, desc;
    if (!promptNewProfile(tr("New Profile"), &name, &desc)) return;
    if (isBuiltin(name) || custom_.contains(name)) {
        QMessageBox::warning(this, tr("Error"), tr("Profile '%1' already exists.").arg(name));
        return;
    }
    AnalysisProfile p;
    p.name = name;
    p.description = desc;
    custom_.insert(name, p.toJson());
    const QString shown = combo_->currentText();
    populateCombo(shown);
    combo_->setCurrentIndex(combo_->findText(name));
}

void ProfilesTab::cloneProfile() {
    const QString current = combo_->currentText();
    if (current.isEmpty()) return;
    QString name, desc;
    if (!promptNewProfile(tr("Clone Profile"), &name, &desc)) return;
    if (isBuiltin(name) || custom_.contains(name)) {
        QMessageBox::warning(this, tr("Error"), tr("Profile '%1' already exists.").arg(name));
        return;
    }
    saveCurrentToWorkingCopy();
    AnalysisProfile p = getProfile(current, custom_);
    p.name = name;
    if (!desc.isEmpty()) p.description = desc;
    p.builtin = false;
    p.singularAnalysis = false;  // clones start with singular analysis off
    custom_.insert(name, p.toJson());
    populateCombo(current);
    combo_->setCurrentIndex(combo_->findText(name));
}

void ProfilesTab::deleteProfile() {
    const QString name = combo_->currentText();
    if (name.isEmpty() || isBuiltin(name)) return;
    if (QMessageBox::question(this, tr("Delete Profile"), tr("Delete profile '%1'?").arg(name),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    custom_.remove(name);
    displayed_.clear();
    populateCombo(QStringLiteral("default"));
    loadProfile(combo_->currentText());
}

bool ProfilesTab::promptNewProfile(const QString& title, QString* name, QString* description) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(400);
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText(tr("e.g. my-custom-profile"));
    form->addRow(tr("Name:"), nameEdit);
    auto* descEdit = new QPlainTextEdit;
    descEdit->setMaximumHeight(60);
    descEdit->setPlaceholderText(tr("Describe the profile's purpose (shown to the AI agent)"));
    form->addRow(tr("Description:"), descEdit);
    lay->addLayout(form);
    auto* error = new QLabel;
    error->setStyleSheet(QString::fromLatin1(settings::kErrStyle));
    error->hide();
    lay->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        const QString n = nameEdit->text().trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
        if (n.isEmpty()) {
            error->setText(tr("Name is required"));
            error->show();
            return;
        }
        if (descEdit->toPlainText().trimmed().isEmpty()) {
            error->setText(tr("Description is required — it helps the AI agent follow your constraints"));
            error->show();
            return;
        }
        nameEdit->setText(n);
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return false;
    *name = nameEdit->text().trimmed();
    *description = descEdit->toPlainText().trimmed();
    return !name->isEmpty();
}

// ---------------------------------------------------------------- apply

void ProfilesTab::apply(RikuganConfig& c) {
    saveCurrentToWorkingCopy();
    c.activeProfile = combo_->currentText().isEmpty() ? QStringLiteral("default") : combo_->currentText();
    c.customProfiles = custom_;
}

}  // namespace rk
