#include "common/ui/settings/McpTab.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "common/config/Paths.h"
#include "common/ui/settings/ServiceRpc.h"
#include "common/util/Files.h"

namespace rk {

namespace {

// ../ohmyc/src/mcp/mcpcfg.c MCP_CONFIG_SCHEMA_URL (omc's writer puts it first in every file it writes)
constexpr char kSchemaUrl[] =
    "https://raw.githubusercontent.com/can1357/oh-my-pi/main/packages/coding-agent/src/config/mcp-schema.json";

QString typeOf(const QJsonObject& s) {
    const QString t = s.value(QStringLiteral("type")).toString();
    return t.isEmpty() ? QStringLiteral("stdio") : t;
}

QString summary(const QJsonObject& s) {
    if (typeOf(s) != QLatin1String("stdio")) return s.value(QStringLiteral("url")).toString();
    QStringList parts{s.value(QStringLiteral("command")).toString()};
    for (const QJsonValue& a : s.value(QStringLiteral("args")).toArray()) parts << a.toString();
    return parts.join(QLatin1Char(' '));
}

// mcp_validate_server_name
QString validateName(const QString& name) {
    if (name.isEmpty()) return QObject::tr("Server name cannot be empty");
    if (name.size() > 100) return QObject::tr("Server name is too long (max 100 characters)");
    const QString bad =
        QObject::tr("Server name can only contain letters, numbers, dash, underscore, dot, colon, and single spaces");
    // ^[a-zA-Z0-9_.:-]+(?: [a-zA-Z0-9_.:-]+)*$
    bool prevSpace = true;
    for (const QChar c : name) {
        const bool word = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                          (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_') || c == QLatin1Char('.') ||
                          c == QLatin1Char(':') || c == QLatin1Char('-');
        if (word) prevSpace = false;
        else if (c == QLatin1Char(' ') && !prevSpace) prevSpace = true;
        else return bad;
    }
    return prevSpace ? bad : QString();
}

QStringList lines(const QString& text) {
    QStringList out;
    for (const QString& l : text.split(QLatin1Char('\n')))
        if (!l.trimmed().isEmpty()) out << l.trimmed();
    return out;
}

// "KEY<sep>VALUE" lines -> object; *bad gets the first malformed line
QJsonObject pairs(const QString& text, QChar sep, QString* bad) {
    QJsonObject o;
    for (const QString& l : lines(text)) {
        const qsizetype i = l.indexOf(sep);
        if (i <= 0) {
            if (bad && bad->isEmpty()) *bad = l;
            continue;
        }
        o.insert(l.left(i).trimmed(), l.mid(i + 1).trimmed());
    }
    return o;
}

QString pairsText(const QJsonObject& o, const QString& sep) {
    QStringList out;
    for (auto it = o.begin(); it != o.end(); ++it) out << it.key() + sep + it.value().toString();
    return out.join(QLatin1Char('\n'));
}

// Add / edit one server. Starts from the existing object so keys it doesn't show (cwd, timeout, auth ...) stay.
class ServerDialog : public QDialog {
public:
    ServerDialog(const QString& name, const QJsonObject& server, const QStringList& taken, QWidget* parent)
        : QDialog(parent), server_(server), taken_(taken) {
        setWindowTitle(name.isEmpty() ? tr("Add MCP Server") : tr("Edit MCP Server"));
        setMinimumWidth(480);
        auto* lay = new QVBoxLayout(this);
        form_ = new QFormLayout;
        name_ = new QLineEdit(name);
        name_->setPlaceholderText(QStringLiteral("e.g. my-server"));
        form_->addRow(tr("Name:"), name_);
        type_ = new QComboBox;
        type_->addItems({QStringLiteral("stdio"), QStringLiteral("http"), QStringLiteral("sse")});
        type_->setCurrentText(typeOf(server));
        form_->addRow(tr("Type:"), type_);

        command_ = new QLineEdit(server.value(QStringLiteral("command")).toString());
        command_->setPlaceholderText(QStringLiteral("npx"));
        form_->addRow(tr("Command:"), command_);
        QStringList args;
        for (const QJsonValue& a : server.value(QStringLiteral("args")).toArray()) args << a.toString();
        args_ = new QPlainTextEdit(args.join(QLatin1Char('\n')));
        args_->setPlaceholderText(tr("One argument per line"));
        args_->setMaximumHeight(70);
        form_->addRow(tr("Arguments:"), args_);
        env_ = new QPlainTextEdit(pairsText(server.value(QStringLiteral("env")).toObject(), QStringLiteral("=")));
        env_->setPlaceholderText(tr("KEY=value, one per line"));
        env_->setMaximumHeight(60);
        form_->addRow(tr("Environment:"), env_);

        url_ = new QLineEdit(server.value(QStringLiteral("url")).toString());
        url_->setPlaceholderText(QStringLiteral("https://example.com/mcp"));
        form_->addRow(tr("URL:"), url_);
        headers_ = new QPlainTextEdit(pairsText(server.value(QStringLiteral("headers")).toObject(), QStringLiteral(": ")));
        headers_->setPlaceholderText(tr("Header: value, one per line"));
        headers_->setMaximumHeight(60);
        form_->addRow(tr("Headers:"), headers_);
        lay->addLayout(form_);

        error_ = new QLabel;
        error_->setStyleSheet(QString::fromLatin1(settings::kErrStyle));
        error_->setWordWrap(true);
        error_->hide();
        lay->addWidget(error_);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { validateAndAccept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(type_, &QComboBox::currentTextChanged, this, [this] { showRows(); });
        showRows();
    }

    QString name() const { return name_->text().trimmed(); }
    QJsonObject server() const { return server_; }

private:
    void showRows() {
        const bool stdio = type_->currentText() == QLatin1String("stdio");
        for (QWidget* w : {static_cast<QWidget*>(command_), static_cast<QWidget*>(args_), static_cast<QWidget*>(env_)})
            form_->setRowVisible(w, stdio);
        for (QWidget* w : {static_cast<QWidget*>(url_), static_cast<QWidget*>(headers_)}) form_->setRowVisible(w, !stdio);
    }

    void fail(const QString& e) {
        error_->setText(e);
        error_->show();
    }

    // mcp_validate_server_name / mcp_validate_server_config wording
    void validateAndAccept() {
        const QString n = name();
        if (const QString e = validateName(n); !e.isEmpty()) return fail(e);
        if (taken_.contains(n)) return fail(tr("Server \"%1\" already exists").arg(n));
        const QString type = type_->currentText();
        QJsonObject s = server_;
        QString bad;
        if (type == QLatin1String("stdio")) {
            const QString cmd = command_->text().trimmed();
            if (cmd.isEmpty()) return fail(tr("Server \"%1\": stdio server requires \"command\" field").arg(n));
            for (const char* k : {"url", "headers"}) s.remove(QLatin1String(k));
            if (s.contains(QStringLiteral("type"))) s.insert(QStringLiteral("type"), type);
            s.insert(QStringLiteral("command"), cmd);
            const QStringList args = lines(args_->toPlainText());
            if (args.isEmpty()) s.remove(QStringLiteral("args"));
            else s.insert(QStringLiteral("args"), QJsonArray::fromStringList(args));
            const QJsonObject env = pairs(env_->toPlainText(), QLatin1Char('='), &bad);
            if (!bad.isEmpty()) return fail(tr("Not KEY=value: %1").arg(bad));
            if (env.isEmpty()) s.remove(QStringLiteral("env"));
            else s.insert(QStringLiteral("env"), env);
        } else {
            const QString url = url_->text().trimmed();
            if (url.isEmpty()) return fail(tr("Server \"%1\": %2 server requires \"url\" field").arg(n, type));
            for (const char* k : {"command", "args", "env", "cwd"}) s.remove(QLatin1String(k));
            s.insert(QStringLiteral("type"), type);
            s.insert(QStringLiteral("url"), url);
            const QJsonObject headers = pairs(headers_->toPlainText(), QLatin1Char(':'), &bad);
            if (!bad.isEmpty()) return fail(tr("Not Header: value: %1").arg(bad));
            if (headers.isEmpty()) s.remove(QStringLiteral("headers"));
            else s.insert(QStringLiteral("headers"), headers);
        }
        server_ = s;
        accept();
    }

    QJsonObject server_;
    QStringList taken_;
    QFormLayout* form_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* type_ = nullptr;
    QLineEdit* command_ = nullptr;
    QPlainTextEdit* args_ = nullptr;
    QPlainTextEdit* env_ = nullptr;
    QLineEdit* url_ = nullptr;
    QPlainTextEdit* headers_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace

McpTab::McpTab(QWidget* parent) : QWidget(parent), path_(paths::omcAgentDir() + QStringLiteral("/mcp.json")) {
    auto* outer = new QVBoxLayout(this);
    auto* note = new QLabel(tr("Servers start with new chats."));
    note->setStyleSheet(QString::fromLatin1(settings::kHintStyle));
    outer->addWidget(note);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);

    auto* group = new QGroupBox(tr("Rikugan MCP Servers"));
    auto* gl = new QVBoxLayout(group);
    error_ = new QLabel;
    error_->setStyleSheet(QString::fromLatin1(settings::kErrStyle));
    error_->setWordWrap(true);
    error_->hide();
    gl->addWidget(error_);
    empty_ = new QLabel(tr("No MCP servers configured"));
    gl->addWidget(empty_);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("mcpServers"));
    gl->addWidget(list_);

    auto* row = new QHBoxLayout;
    add_ = new QPushButton(tr("Add"));
    edit_ = new QPushButton(tr("Edit"));
    remove_ = new QPushButton(tr("Remove"));
    row->addWidget(add_);
    row->addWidget(edit_);
    row->addWidget(remove_);
    row->addStretch();
    gl->addLayout(row);
    auto* where = new QLabel(path_);
    where->setStyleSheet(QString::fromLatin1(settings::kHintStyle));
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gl->addWidget(where);
    layout->addWidget(group);
    layout->addStretch();
    scroll->setWidget(container);
    outer->addWidget(scroll);

    connect(add_, &QPushButton::clicked, this, [this] { editServer(QString()); });
    connect(edit_, &QPushButton::clicked, this, [this] {
        if (const QListWidgetItem* it = list_->currentItem()) editServer(it->data(Qt::UserRole).toString());
    });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) { editServer(it->data(Qt::UserRole).toString()); });
    connect(remove_, &QPushButton::clicked, this, &McpTab::removeServer);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const QString name = it->data(Qt::UserRole).toString();
        QJsonObject s = servers_.value(name).toObject();
        if (it->checkState() == Qt::Checked) {
            if (s.contains(QStringLiteral("enabled"))) s.insert(QStringLiteral("enabled"), true);
            disabledList_.removeAll(name);
        } else {
            s.insert(QStringLiteral("enabled"), false);
        }
        servers_.insert(name, s);
        dirty_ = true;
    });
    reload();
}

void McpTab::reload() {
    bool ok = false;
    file_ = readJsonObject(path_, &ok);
    readable_ = ok || !QFileInfo::exists(path_);
    if (!readable_) {
        error_->setText(tr("%1 is not valid JSON; fix it by hand. Nothing here will overwrite it.").arg(path_));
        error_->show();
    }
    servers_ = file_.value(QStringLiteral("mcpServers")).toObject();
    disabledList_.clear();
    for (const QJsonValue& v : file_.value(QStringLiteral("disabledServers")).toArray())
        if (v.isString()) disabledList_ << v.toString();
    add_->setEnabled(readable_);
    edit_->setEnabled(readable_);
    remove_->setEnabled(readable_);
    list_->setEnabled(readable_);
    fillList();
}

void McpTab::fillList() {
    const QSignalBlocker block(list_);
    const QString selected = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    list_->clear();
    QStringList names = servers_.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString& name : names) {
        const QJsonObject s = servers_.value(name).toObject();
        auto* it = new QListWidgetItem(QStringLiteral("%1  —  %2").arg(name, summary(s)), list_);
        it->setData(Qt::UserRole, name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        const bool on = s.value(QStringLiteral("enabled")).toBool(true) && !disabledList_.contains(name);
        it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        if (name == selected) list_->setCurrentItem(it);
    }
    empty_->setVisible(names.isEmpty());
    list_->setVisible(!names.isEmpty());
}

void McpTab::editServer(const QString& name) {
    QStringList taken = servers_.keys();
    taken.removeAll(name);
    ServerDialog dlg(name, servers_.value(name).toObject(), taken, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString newName = dlg.name();
    if (!name.isEmpty() && newName != name) {
        servers_.remove(name);
        if (disabledList_.removeAll(name)) disabledList_ << newName;
    }
    servers_.insert(newName, dlg.server());
    dirty_ = true;
    fillList();
}

void McpTab::removeServer() {
    const QListWidgetItem* it = list_->currentItem();
    if (!it) return;
    const QString name = it->data(Qt::UserRole).toString();
    if (QMessageBox::question(this, tr("Remove MCP Server"), tr("Remove \"%1\"?").arg(name)) != QMessageBox::Yes) return;
    servers_.remove(name);
    disabledList_.removeAll(name);
    dirty_ = true;
    fillList();
}

QString McpTab::save() {
    if (!dirty_ || !readable_) return {};
    QJsonObject out = file_;
    out.insert(QStringLiteral("mcpServers"), servers_);
    QStringList disabled = disabledList_;
    disabled.removeDuplicates();
    disabled.sort();
    if (disabled.isEmpty()) out.remove(QStringLiteral("disabledServers"));
    else out.insert(QStringLiteral("disabledServers"), QJsonArray::fromStringList(disabled));
    if (!out.contains(QStringLiteral("$schema"))) out.insert(QStringLiteral("$schema"), QLatin1String(kSchemaUrl));
    QString err;
    if (!writeFileAtomic(path_, QJsonDocument(out).toJson(QJsonDocument::Indented), 0600, &err))
        return tr("Could not write %1: %2").arg(path_, err);
    file_ = out;
    dirty_ = false;
    return {};
}

}  // namespace rk
