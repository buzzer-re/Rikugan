#include "common/ui/console/AgentsPanel.h"

#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatSession.h"
#include "common/ui/Theme.h"
#include "common/util/ModuleLifetime.h"

namespace rk::ui {

namespace {

constexpr int kNarrowWidth = 560;
constexpr int kRefreshMs = 100;

bool running(SubagentStatus s) { return s == SubagentStatus::Running || s == SubagentStatus::Pending; }

QString elapsedText(const SubagentInfo& i) {
    const qint64 end = i.completedAt ? i.completedAt : QDateTime::currentMSecsSinceEpoch();
    const qint64 ms = std::max<qint64>(0, end - i.createdAt);
    if (ms < 60000) return QString::number(double(ms) / 1000.0, 'f', 1) + QLatin1Char('s');
    return QStringLiteral("%1:%2").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
}

QString tokens(qint64 n) {
    if (n < 1000) return QString::number(n);
    return QString::number(double(n) / 1000.0, 'f', 1) + QLatin1Char('k');
}

QColor statusColor(SubagentStatus s) {
    const Theme& t = theme();
    switch (s) {
    case SubagentStatus::Pending:
    case SubagentStatus::Running: return t.agent;
    case SubagentStatus::Completed: return t.ok;
    case SubagentStatus::Failed: return t.err;
    case SubagentStatus::Cancelled: return t.fg3;
    }
    return t.fg2;
}

QString statusGlyph(SubagentStatus s) {
    switch (s) {
    case SubagentStatus::Pending:
    case SubagentStatus::Running: return QStringLiteral("\u25D0");
    case SubagentStatus::Completed: return QStringLiteral("\u2713");
    case SubagentStatus::Failed: return QStringLiteral("\u2715");
    case SubagentStatus::Cancelled: return QStringLiteral("\u2013");
    }
    return {};
}

// the agent a dotted id ("KeyDerivation.SerialSrc") or parentId names
QString parentOf(const SubagentInfo& i, const QHash<QString, SubagentInfo>& all) {
    if (!i.parentId.isEmpty() && all.contains(i.parentId)) return i.parentId;
    const qsizetype dot = i.id.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && all.contains(i.id.left(dot))) return i.id.left(dot);
    return {};
}

}  // namespace

// ---- SpawnAgentDialog ----

SpawnAgentDialog::SpawnAgentDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("New agent"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    type_ = new QComboBox;
    type_->addItem(tr("Custom task"), QStringLiteral("custom"));
    type_->addItem(tr("Network reconstructor"), QStringLiteral("network_recon"));
    type_->addItem(tr("Report writer"), QStringLiteral("report_writer"));
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { onPresetChanged(); });
    form->addRow(tr("Agent type"), type_);
    task_ = new QPlainTextEdit;
    task_->setPlaceholderText(tr("What should the agent do?"));
    task_->setMinimumHeight(90);
    connect(task_, &QPlainTextEdit::textChanged, this, [this] { launch_->setEnabled(!task().isEmpty()); });
    form->addRow(tr("Task"), task_);
    layout->addLayout(form);

    auto* perkBox = new QGroupBox(tr("Perks"));
    auto* perkLayout = new QVBoxLayout(perkBox);
    const std::pair<const char*, const char*> perks[] = {
        {"deep_decompilation", "Deep decompilation (chase xrefs 3+ levels)"},
        {"string_harvesting", "String harvesting (dump all referenced strings)"},
        {"import_mapping", "Import mapping (map all API calls)"},
        {"memory_layout", "Memory layout (analyze stack frames, globals)"},
        {"hypothesis_mode", "Hypothesis mode (generate and test theories)"},
    };
    for (const auto& [key, label] : perks) {
        auto* cb = new QCheckBox(QString::fromUtf8(label));
        perkLayout->addWidget(cb);
        perks_.append({QString::fromLatin1(key), cb});
    }
    layout->addWidget(perkBox);
    auto* turnsRow = new QFormLayout;
    maxTurns_ = new QSpinBox;
    maxTurns_->setRange(1, 200);
    maxTurns_->setValue(20);
    turnsRow->addRow(tr("Max turns"), maxTurns_);
    layout->addLayout(turnsRow);
    auto* buttons = new QDialogButtonBox;
    launch_ = buttons->addButton(tr("Launch"), QDialogButtonBox::AcceptRole);
    launch_->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SpawnAgentDialog::onPresetChanged() {
    // presets bring their own instructions and turn budget (network_recon 30, report_writer 5); perks are custom-only
    const QString t = agentType();
    const bool custom = t == QLatin1String("custom");
    for (auto& [key, cb] : perks_) {
        cb->setEnabled(custom);
        if (t == QLatin1String("network_recon"))
            cb->setChecked(key == QLatin1String("import_mapping") || key == QLatin1String("string_harvesting") ||
                           key == QLatin1String("deep_decompilation"));
        else if (!custom)
            cb->setChecked(false);
    }
    maxTurns_->setValue(t == QLatin1String("network_recon") ? 30 : t == QLatin1String("report_writer") ? 5 : 20);
}

QString SpawnAgentDialog::agentType() const { return type_->currentData().toString(); }
QString SpawnAgentDialog::task() const { return task_->toPlainText().trimmed(); }
int SpawnAgentDialog::maxTurns() const { return maxTurns_->value(); }

QStringList SpawnAgentDialog::perks() const {
    QStringList out;
    for (const auto& [key, cb] : perks_)
        if (cb->isChecked()) out << key;
    return out;
}

// ---- AgentsPanel ----

AgentsPanel::AgentsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("rk_agents"));
    setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* head = new QWidget(this);
    head->setObjectName(QStringLiteral("rk_agents_head"));
    head->setAttribute(Qt::WA_StyledBackground);
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(10, 3, 6, 3);
    h->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("\u2442  ") + tr("Agents"), head);
    title->setObjectName(QStringLiteral("rk_agents_title"));
    h->addWidget(title);
    counts_ = new QLabel(head);
    counts_->setProperty("rkMuted", true);
    h->addWidget(counts_);
    h->addStretch(1);
    new_ = new QPushButton(tr("+ New agent"), head);
    new_->setToolTip(tr("Start an agent with a preset, perks and a turn budget"));
    h->addWidget(new_);
    auto* clean = new QPushButton(tr("Clear finished"), head);
    h->addWidget(clean);
    auto* close = new QToolButton(head);
    close->setText(QStringLiteral("\u00d7"));
    close->setToolTip(tr("Close (Ctrl+J)"));
    h->addWidget(close);
    v->addWidget(head);

    auto* body = new QWidget(this);
    auto* b = new QHBoxLayout(body);
    b->setContentsMargins(0, 0, 0, 0);
    b->setSpacing(0);
    tree_ = new QTreeWidget(body);
    tree_->setObjectName(QStringLiteral("rk_agents_tree"));
    tree_->setColumnCount(2);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setIndentation(14);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    b->addWidget(tree_, 2);

    detail_ = new QWidget(body);
    detail_->setObjectName(QStringLiteral("rk_agents_detail"));
    auto* d = new QVBoxLayout(detail_);
    d->setContentsMargins(10, 6, 10, 6);
    d->setSpacing(4);
    auto* top = new QHBoxLayout;
    back_ = new QToolButton(detail_);
    back_->setText(QStringLiteral("\u2039"));
    back_->setToolTip(tr("All agents"));
    top->addWidget(back_);
    name_ = new QLabel(detail_);
    name_->setObjectName(QStringLiteral("rk_agent_name"));
    top->addWidget(name_);
    status_ = new QLabel(detail_);
    status_->setObjectName(QStringLiteral("rk_agent_status"));
    top->addWidget(status_);
    top->addStretch(1);
    elapsed_ = new QLabel(detail_);
    elapsed_->setProperty("rkMuted", true);
    top->addWidget(elapsed_);
    openChat_ = new QPushButton(tr("Open chat"), detail_);
    openChat_->setToolTip(tr("See this agent's whole conversation like a chat (double click does the same)"));
    top->addWidget(openChat_);
    stop_ = new QPushButton(tr("Stop"), detail_);
    stop_->setProperty("rkDanger", true);
    top->addWidget(stop_);
    d->addLayout(top);
    task_ = new QLabel(detail_);
    task_->setWordWrap(true);
    task_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    d->addWidget(task_);
    logHead_ = new QLabel(detail_);
    logHead_->setObjectName(QStringLiteral("rk_agent_loghead"));
    d->addWidget(logHead_);
    log_ = new QTextBrowser(detail_);
    log_->setObjectName(QStringLiteral("rk_agent_log"));
    log_->setFrameShape(QFrame::NoFrame);
    log_->setOpenLinks(false);
    d->addWidget(log_, 1);
    // a live agent rewrites its log: not while the user selects in it (the new text lands when the selection goes)
    connect(log_, &QTextBrowser::selectionChanged, this, [this] {
        if (!log_->textCursor().hasSelection() && !logPending_.isEmpty()) setLogHtml(std::exchange(logPending_, QString()));
    });
    auto* steerRow = new QHBoxLayout;
    steer_ = new QLineEdit(detail_);
    steerRow->addWidget(steer_, 1);
    steerBtn_ = new QPushButton(tr("Steer"), detail_);
    steerBtn_->setProperty("rkPrimary", true);
    steerRow->addWidget(steerBtn_);
    toChat_ = new QPushButton(tr("Send summary to chat"), detail_);
    steerRow->addWidget(toChat_);
    copy_ = new QPushButton(tr("Copy output"), detail_);
    copy_->setToolTip(tr("Copy the agent's whole answer"));
    steerRow->addWidget(copy_);
    d->addLayout(steerRow);
    b->addWidget(detail_, 3);
    v->addWidget(body, 1);

    connect(close, &QToolButton::clicked, this, &AgentsPanel::closeRequested);
    connect(new_, &QPushButton::clicked, this, &AgentsPanel::newAgent);
    connect(clean, &QPushButton::clicked, this, [] { SubagentRegistry::instance().removeFinished(); });
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) { showAgent(item->data(0, Qt::UserRole).toString()); });
    connect(back_, &QToolButton::clicked, this, [this] {
        current_.clear();
        tree_->clearSelection();
        applyNarrow();
        refreshDetail();
    });
    connect(openChat_, &QPushButton::clicked, this, [this] {
        if (!current_.isEmpty()) Q_EMIT openChatRequested(current_);
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item) { Q_EMIT openChatRequested(item->data(0, Qt::UserRole).toString()); });
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (!current_.isEmpty()) SubagentRegistry::instance().cancel(current_);
    });
    auto steer = [this] {
        const QString text = steer_->text().trimmed();
        if (text.isEmpty() || current_.isEmpty()) return;
        if (SubagentRegistry::instance().steer(current_, text)) steer_->clear();
        else steer_->setPlaceholderText(tr("%1 is not running").arg(infos_.value(current_).name));
    };
    connect(steer_, &QLineEdit::returnPressed, this, steer);
    connect(steerBtn_, &QPushButton::clicked, this, steer);
    connect(toChat_, &QPushButton::clicked, this, [this] {
        const auto it = infos_.constFind(current_);
        if (it == infos_.constEnd() || it->summary.isEmpty()) return;
        const qint64 secs = std::max<qint64>(0, ((it->completedAt ? it->completedAt : it->createdAt) - it->createdAt) / 1000);
        Q_EMIT summaryForChat(QStringLiteral("[Subagent \u201c%1\u201d completed (%2 turns, %3s)]\n\n%4")
                                  .arg(it->name)
                                  .arg(it->turnCount)
                                  .arg(secs)
                                  .arg(it->summary));
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        const auto it = infos_.constFind(current_);
        if (it == infos_.constEnd()) return;
        QApplication::clipboard()->setText(!it->summary.isEmpty() ? it->summary : it->recentOutput.join(QStringLiteral("\n")));
        copy_->setText(tr("Copied"));
        QTimer::singleShot(1200, copy_, [b = QPointer<QPushButton>(copy_)] {
            if (b) b->setText(tr("Copy output"));
        });
    });

    clock_.setInterval(1000);
    connect(&clock_, &QTimer::timeout, this, [this] {
        refreshDetail();
        for (auto it = infos_.cbegin(); it != infos_.cend(); ++it)
            if (QTreeWidgetItem* item = items_.value(it.key()); item && running(it->status)) item->setToolTip(0, elapsedText(*it));
    });
    QPointer<AgentsPanel> self(this);
    observer_ = SubagentRegistry::instance().addObserver([self] {
        QMetaObject::invokeMethod(
            uiAnchor(),
            [self] {
                if (!self || self->dirty_) return;
                self->dirty_ = true;
                QTimer::singleShot(kRefreshMs, self.data(), [self] {
                    if (!self) return;
                    self->dirty_ = false;
                    if (self->isVisible()) self->refresh();
                });
            },
            Qt::QueuedConnection);
    });
    connect(&ThemeNotifier::instance(), &ThemeNotifier::changed, this, [this] {
        restyle();
        refresh();
    });
    restyle();
    refresh();
}

AgentsPanel::~AgentsPanel() { SubagentRegistry::instance().removeObserver(observer_); }

void AgentsPanel::setChat(ChatSession* chat) {
    chat_ = chat;
    new_->setEnabled(chat != nullptr);
    current_.clear();
    refresh();
}

QList<SubagentInfo> AgentsPanel::agents() const {
    QList<SubagentInfo> out;
    const QString chatId = chat_ ? chat_->chatId() : QString();
    for (const SubagentInfo& i : SubagentRegistry::instance().all())
        if (!chatId.isEmpty() && i.chatId == chatId) out << i;
    return out;
}

void AgentsPanel::showAgent(const QString& id) {
    current_ = id;
    if (QTreeWidgetItem* item = items_.value(id)) tree_->setCurrentItem(item);
    applyNarrow();
    refreshDetail();
}

void AgentsPanel::refresh() {
    const QList<SubagentInfo> list = agents();
    QHash<QString, SubagentInfo> all;
    for (const SubagentInfo& i : list) all.insert(i.id, i);
    // drop rows of agents that went away (Clear finished); a row's children are its own: detach them first, the
    // survivors find their place below
    QList<QTreeWidgetItem*> gone;
    for (auto it = items_.begin(); it != items_.end();) {
        if (all.contains(it.key())) {
            ++it;
            continue;
        }
        gone << it.value();
        it = items_.erase(it);
    }
    for (QTreeWidgetItem* item : std::as_const(gone))
        for (QTreeWidgetItem* child : item->takeChildren()) tree_->addTopLevelItem(child);
    qDeleteAll(gone);
    int run = 0, finished = 0;
    bool anyRunning = false;
    for (const SubagentInfo& i : list) {
        running(i.status) ? ++run : ++finished;
        anyRunning |= running(i.status);
        QTreeWidgetItem* item = items_.value(i.id);
        const QString parent = parentOf(i, all);
        QTreeWidgetItem* parentItem = parent.isEmpty() ? nullptr : items_.value(parent);
        if (item && item->parent() != parentItem && parentItem) {
            // the parent row appeared after the child
            (item->parent() ? item->parent() : tree_->invisibleRootItem())->removeChild(item);
            parentItem->addChild(item);
        }
        if (!item) {
            item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
            item->setData(0, Qt::UserRole, i.id);
            items_.insert(i.id, item);
            if (parentItem) parentItem->setExpanded(true);
        }
        const QString name = i.name.isEmpty() ? i.id : i.name;
        item->setText(0, statusGlyph(i.status) + QStringLiteral("  ") + name);
        item->setForeground(0, running(i.status) ? theme().fg : theme().fg2);
        item->setText(1, QStringLiteral("%1 \u00b7 %2").arg(i.toolCount).arg(tokens(i.tokens)));
        item->setForeground(1, theme().fg3);
        item->setToolTip(0, i.task);
    }
    infos_ = all;
    counts_->setText(QStringLiteral("%1 running \u00b7 %2 finished").arg(run).arg(finished));
    if (!current_.isEmpty() && !all.contains(current_)) current_.clear();
    if (current_.isEmpty() && !narrow_ && !list.isEmpty()) current_ = list.last().id;
    if (QTreeWidgetItem* item = items_.value(current_)) tree_->setCurrentItem(item);
    if (anyRunning && isVisible() && !clock_.isActive()) clock_.start();
    else if (!anyRunning) clock_.stop();
    applyNarrow();
    refreshDetail();
}

void AgentsPanel::refreshDetail() {
    const Theme& t = theme();
    const auto it = infos_.constFind(current_);
    const bool has = it != infos_.constEnd();
    for (QWidget* w : {static_cast<QWidget*>(name_), static_cast<QWidget*>(status_), static_cast<QWidget*>(elapsed_),
                       static_cast<QWidget*>(stop_), static_cast<QWidget*>(task_), static_cast<QWidget*>(logHead_),
                       static_cast<QWidget*>(steer_), static_cast<QWidget*>(steerBtn_)})
        w->setVisible(has);
    if (!has) {
        toChat_->hide();
        log_->setHtml(QStringLiteral("<p style=\"color:%1\">%2</p>")
                          .arg(t.fg3.name(), infos_.isEmpty() ? tr("No agents in this chat yet. The model starts them with "
                                                                    "spawn_subagent; \u201c+ New agent\u201d starts one yourself.")
                                                               : tr("Select an agent.")));
        return;
    }
    const SubagentInfo& i = *it;
    const bool live = running(i.status);
    name_->setText(i.name.isEmpty() ? i.id : i.name);
    status_->setText(subagentStatusName(i.status).toUpper());
    status_->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:3px; padding:0 4px;")
                               .arg(statusColor(i.status).name(), blend(statusColor(i.status), t.bg, 0.85).name()));
    elapsed_->setText(elapsedText(i));
    stop_->setVisible(live);
    task_->setText(i.task.left(600));
    logHead_->setText(QStringLiteral("TOOL LOG \u00b7 %1 TOOLS \u00b7 %2 TOKENS").arg(i.toolCount).arg(tokens(i.tokens).toUpper()));
    // a finished agent cannot be steered: its summary can go to the chat instead
    steer_->setVisible(live);
    steerBtn_->setVisible(live);
    steer_->setPlaceholderText(live ? tr("Steer %1\u2026").arg(name_->text()) : tr("%1 has finished").arg(name_->text()));
    toChat_->setVisible(!live && !i.summary.isEmpty() && chat_);
    // the log: one line per event, the output at the end
    QString html = QStringLiteral("<table cellspacing=0 cellpadding=1 style=\"font-family:'%1'; font-size:%2pt\">")
                       .arg(t.monoSmall.family())
                       .arg(t.monoSmall.pointSizeF() > 0 ? t.monoSmall.pointSizeF() : 9);
    auto row = [&](qint64 ms, const QString& text, const QColor& ink) {
        html += QStringLiteral("<tr><td style=\"color:%1; padding-right:8px\">%2s</td><td style=\"color:%3\">%4</td></tr>")
                    .arg(t.fg3.name())
                    .arg(double(std::max<qint64>(0, ms - i.createdAt)) / 1000.0, 0, 'f', 1)
                    .arg(ink.name(), text.toHtmlEscaped());
    };
    row(i.createdAt, QStringLiteral("spawned \u00b7 ") + i.task.section(QLatin1Char('\n'), 0, 0).left(160), t.fg2);
    for (const SubagentLogEntry& e : i.log) {
        if (e.kind == QLatin1String("tool"))
            row(e.ms, e.tool + QLatin1Char(' ') + e.summary + (e.running ? QStringLiteral(" \u2026") : QString()), e.isError ? t.err : t.fg);
        else row(e.ms, e.summary.section(QLatin1Char('\n'), 0, 1), e.kind == QLatin1String("error") ? t.err : t.fg2);
    }
    if (!live && i.completedAt) row(i.completedAt, subagentStatusName(i.status), statusColor(i.status));
    html += QStringLiteral("</table>");
    const QString output = !i.summary.isEmpty() ? i.summary : i.recentOutput.join(QStringLiteral("\n"));
    copy_->setEnabled(!output.isEmpty());
    if (!output.isEmpty())
        html += QStringLiteral("<p style=\"color:%1; font-size:small; margin-top:8px\">OUTPUT</p><p style=\"color:%2; white-space:pre-wrap\">%3</p>")
                    .arg(t.fg3.name(), t.fg.name(), output.toHtmlEscaped());
    if (log_->property("rkHtml").toString() == html) return;
    if (log_->textCursor().hasSelection() || (QApplication::mouseButtons() & Qt::LeftButton)) {
        logPending_ = html;
        return;
    }
    setLogHtml(html);
}

void AgentsPanel::setLogHtml(const QString& html) {
    const bool atEnd = log_->verticalScrollBar()->value() >= log_->verticalScrollBar()->maximum() - 4;
    log_->setHtml(html);
    log_->setProperty("rkHtml", html);
    if (atEnd) log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
}

void AgentsPanel::applyNarrow() {
    narrow_ = width() < kNarrowWidth;
    if (!narrow_) {
        tree_->show();
        detail_->show();
        back_->hide();
        return;
    }
    const bool detail = !current_.isEmpty();
    tree_->setVisible(!detail);
    detail_->setVisible(detail);
    back_->setVisible(detail);
}

void AgentsPanel::newAgent() {
    if (!chat_) return;
    auto* dlg = new SpawnAgentDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        if (!chat_ || dlg->task().isEmpty()) return;
        const QString type = dlg->agentType();
        SubagentSpec spec;
        spec.task = dlg->task();
        spec.agentType = type;
        spec.perks = dlg->perks();
        spec.maxTurns = dlg->maxTurns();
        spec.systemAddendum = subagentAddendum(type, spec.perks);
        if (type == QLatin1String("network_recon")) spec.name = QStringLiteral("Network Recon");
        else if (type == QLatin1String("report_writer")) spec.name = QStringLiteral("Report Writer");
        const bool reportWriter = type == QLatin1String("report_writer");
        spec.noTools = reportWriter;  // works only from the conversation
        std::shared_ptr<ChatContext> ctx = chat_->context();
        std::shared_ptr<OmcSession> session;
        if (reportWriter) {
            chat_->ensureOpen();
            session = ctx->session.lock();
        }
        spawnThread([ctx, spec, session]() mutable {
            if (session) {
                const QString transcript = subagentTranscript(fetchSessionMessages(session));
                if (!transcript.isEmpty()) spec.task += QStringLiteral("\n\nConversation transcript:\n\n") + transcript;
            }
            runSubagent(*ctx, spec, std::make_shared<CancelToken>());
        });
    });
    dlg->open();
}

void AgentsPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    refresh();
}

void AgentsPanel::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    clock_.stop();
}

void AgentsPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if ((width() < kNarrowWidth) != narrow_) applyNarrow();
}

void AgentsPanel::restyle() {
    const Theme& t = theme();
    auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
    setStyleSheet(QStringLiteral("#rk_agents { background: %1; border-top: 1px solid %2; }"
                                 "#rk_agents_head { background: %3; border-bottom: 1px solid %2; }"
                                 "#rk_agents_title { font-weight: bold; color: %4; }"
                                 "#rk_agents QLabel { color: %4; background: transparent; }"
                                 "#rk_agents QLabel[rkMuted=\"true\"] { color: %5; }"
                                 "#rk_agents_tree { background: %1; color: %4; border-right: 1px solid %2; }"
                                 "#rk_agents_tree::item { padding: 2px 0; }"
                                 "#rk_agents_tree::item:selected { background: %6; color: %4; }"
                                 "#rk_agent_name { font-weight: bold; }"
                                 "#rk_agents #rk_agent_loghead { color: %5; }"
                                 "#rk_agent_log { background: %1; }")
                      .arg(c(t.bg), c(t.line), c(t.bg2), c(t.fg), c(t.fg3), c(t.accentSoft2)));
    name_->setFont(t.mono);
    logHead_->setFont(t.uiSmall);
    counts_->setFont(t.uiSmall);
}

}  // namespace rk::ui
