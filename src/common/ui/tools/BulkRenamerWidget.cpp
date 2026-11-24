#include "common/ui/tools/BulkRenamerWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include "common/agent/BulkRenamer.h"
#include "common/agent/DbContext.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/EventQueue.h"
#include "common/tools/ToolDispatcher.h"
#include "common/ui/Theme.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

enum Column { ColCheck = 0, ColAddr, ColName, ColLength, ColNewName, ColStatus };

QColor statusColor(const QString& status) {
    const ui::Theme& t = ui::theme();
    if (status == QLatin1String("analyzing")) return t.warn;
    if (status == QLatin1String("renamed")) return t.ok;
    if (status == QLatin1String("reverted")) return t.sym;
    if (status == QLatin1String("skipped")) return t.workspace;
    if (status == QLatin1String("error")) return t.err;
    return status == QLatin1String("queued") ? t.fg3 : t.fg;
}

// Sorts by a number instead of the text.
class NumericItem : public QTableWidgetItem {
public:
    NumericItem(const QString& text, qint64 key) : QTableWidgetItem(text), key_(key) {}
    bool operator<(const QTableWidgetItem& other) const override {
        if (auto* o = dynamic_cast<const NumericItem*>(&other)) return key_ < o->key_;
        return QTableWidgetItem::operator<(other);
    }

private:
    qint64 key_;
};

// Runs `work` on a background thread and `done(result)` back on the UI thread while `guard` lives.
template <class R, class Work, class Done>
void runInBackground(QObject* guard, Work work, Done done) {
    QPointer<QObject> p(guard);
    spawnThread([p, work = std::move(work), done = std::move(done)]() mutable {
        R r = work();
        QMetaObject::invokeMethod(
            uiAnchor(), [p, r = std::move(r), done = std::move(done)]() mutable {
                if (p) done(std::move(r));
            },
            Qt::QueuedConnection);
    });
}

QString eventJson(const RenameEvent& e) {
    const QJsonObject o{{"type", int(e.type)},          {"address", QString::number(e.address)},
                        {"currentName", e.currentName}, {"newName", e.newName},
                        {"error", e.error},             {"completed", e.completed},
                        {"total", e.total}};
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

Qt::ItemFlags readOnlyFlags() { return Qt::ItemIsEnabled | Qt::ItemIsSelectable; }

}  // namespace

// ---- parsing (list_functions pages, function info results) ----

QVector<BulkRenamerWidget::FunctionRow> BulkRenamerWidget::parseFunctionPage(const QString& raw, QString* totalHint) {
    static const QRegularExpression header(QStringLiteral("^Functions\\s+\\d+[\\-\u2013]\\d+\\s+of\\s+([^:]+):"));
    static const QRegularExpression row(QStringLiteral("^\\s*0x([0-9a-fA-F]+)\\s+(.+)"));
    QVector<FunctionRow> out;
    for (const QString& line : raw.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch h = header.match(line);
        if (h.hasMatch()) {
            if (totalHint) *totalHint = h.captured(1).trimmed();
            continue;
        }
        const QRegularExpressionMatch m = row.match(line);
        if (!m.hasMatch()) continue;
        bool ok = false;
        FunctionRow r;
        r.address = m.captured(1).toULongLong(&ok, 16);
        r.name = m.captured(2).trimmed();
        if (ok) out.push_back(r);
    }
    return out;
}

bool BulkRenamerWidget::parseFunctionInfo(const QString& raw, FunctionRow* out) {
    static const QRegularExpression name(QStringLiteral("^Name:\\s+(.+)$"), QRegularExpression::MultilineOption);
    static const QRegularExpression address(QStringLiteral("^Address:\\s+0x([0-9a-fA-F]+)"), QRegularExpression::MultilineOption);
    static const QRegularExpression instrs(QStringLiteral("^Instructions:\\s+(\\d+)"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch n = name.match(raw), a = address.match(raw);
    if (!n.hasMatch() || !a.hasMatch()) return false;
    bool ok = false;
    out->address = a.captured(1).toULongLong(&ok, 16);
    if (!ok) return false;
    out->name = n.captured(1).trimmed();
    out->isImport = false;
    const QRegularExpressionMatch i = instrs.match(raw);
    out->instructionCount = i.hasMatch() ? i.captured(1).toLongLong() : 0;
    return true;
}

bool BulkRenamerWidget::isAutoNamed(const QString& name) {
    for (const char* p : {"sub_", "fn_", "loc_", "j_", "nullsub_", "unknown_", "FUN_"})
        if (name.startsWith(QLatin1String(p))) return true;
    return false;
}

// ---- construction ----

BulkRenamerWidget::BulkRenamerWidget(QWidget* parent) : QWidget(parent), events_(std::make_shared<EventQueue>(this)) {
    setObjectName(QStringLiteral("bulk_renamer_widget"));
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(4, 4, 4, 4);
    main->setSpacing(4);

    // Top bar: filter + selection controls
    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    filterEdit_ = new QLineEdit;
    filterEdit_->setPlaceholderText(QStringLiteral("Filter by name or address..."));
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { onFilterChanged(); });
    top->addWidget(filterEdit_, 1);

    filterCombo_ = new QComboBox;
    filterCombo_->addItems({QStringLiteral("All Functions"), QStringLiteral("Auto-named Only"), QStringLiteral("User-renamed"),
                            QStringLiteral("Imports")});
    connect(filterCombo_, &QComboBox::currentIndexChanged, this, [this] { onFilterChanged(); });
    top->addWidget(filterCombo_);

    selectionLabel_ = new QLabel(QStringLiteral("0 / 0 selected"));
    selectionLabel_->setProperty("rkMuted", true);
    top->addWidget(selectionLabel_);

    loadStatusLabel_ = new QLabel;
    loadStatusLabel_->setProperty("rkMuted", true);
    top->addWidget(loadStatusLabel_);

    loadMoreBtn_ = new QPushButton(QStringLiteral("Load More"));
    loadMoreBtn_->setVisible(false);
    connect(loadMoreBtn_, &QPushButton::clicked, this, [this] { requestLoadMore(); });
    top->addWidget(loadMoreBtn_);
    main->addLayout(top);

    // Table
    table_ = new QTableWidget;
    table_->setObjectName(QStringLiteral("renamer_table"));
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({QString(), QStringLiteral("Address"), QStringLiteral("Current Name"),
                                       QStringLiteral("Length"), QStringLiteral("New Name"), QStringLiteral("Status")});
    table_->setAlternatingRowColors(true);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->setSortingEnabled(true);

    QHeaderView* header = table_->horizontalHeader();
    header->setSectionResizeMode(ColCheck, QHeaderView::Fixed);
    header->setSectionResizeMode(ColAddr, QHeaderView::Interactive);
    header->setSectionResizeMode(ColName, QHeaderView::Interactive);
    header->setSectionResizeMode(ColLength, QHeaderView::Interactive);
    header->setSectionResizeMode(ColNewName, QHeaderView::Stretch);
    header->setSectionResizeMode(ColStatus, QHeaderView::Fixed);
    table_->setColumnWidth(ColCheck, 30);
    table_->setColumnWidth(ColAddr, 110);
    table_->setColumnWidth(ColName, 180);
    table_->setColumnWidth(ColLength, 60);
    table_->setColumnWidth(ColStatus, 80);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(ColAddr, Qt::AscendingOrder);

    // select all / none: a checkbox over the first header section
    headerCheck_ = new QCheckBox(header);
    headerCheck_->setGeometry(8, 3, 16, 16);
    connect(headerCheck_, &QCheckBox::toggled, this, [this](bool on) { onHeaderCheck(on); });
    connect(header, &QHeaderView::sectionResized, this,
            [this] { headerCheck_->setGeometry((table_->columnWidth(ColCheck) - 16) / 2, 3, 16, 16); });

    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!bulkCheck_ && !loading_ && item->column() == ColCheck) updateSelectionCount();
    });
    connect(table_, &QTableWidget::cellClicked, this, [this](int r, int c) { onCellClicked(r, c); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int c) { onCellDoubleClicked(r, c); });
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (!hasMore_ || loadingMore_) return;
        if (value >= std::max(0, table_->verticalScrollBar()->maximum() - 3)) requestLoadMore();
    });
    main->addWidget(table_);

    // Analysis controls
    const RikuganConfig& cfg = RikuganConfig::instance();
    auto* analysis = new QHBoxLayout;
    analysis->setSpacing(6);
    auto* modeLabel = new QLabel(QStringLiteral("Mode:"));
    analysis->addWidget(modeLabel);
    quickRadio_ = new QRadioButton(QStringLiteral("Quick"));
    quickRadio_->setChecked(true);
    analysis->addWidget(quickRadio_);
    deepRadio_ = new QRadioButton(QStringLiteral("Deep"));
    connect(deepRadio_, &QRadioButton::toggled, this, [this] { updateSelectionCount(); });
    analysis->addWidget(deepRadio_);
    analysis->addSpacing(12);

    const QString batchTip = QStringLiteral("Quick: functions per LLM prompt. Deep: ignored (1 agent per function).");
    auto* batchLabel = new QLabel(QStringLiteral("Batch:"));
    batchLabel->setToolTip(batchTip);
    analysis->addWidget(batchLabel);
    batchInput_ = new QLineEdit(QString::number(cfg.bulkRenamerBatchSize));
    batchInput_->setValidator(new QIntValidator(1, 999999, batchInput_));
    batchInput_->setFixedWidth(50);
    batchInput_->setAlignment(Qt::AlignCenter);
    batchInput_->setToolTip(batchTip);
    connect(batchInput_, &QLineEdit::textChanged, this, [this] { updateSelectionCount(); });
    analysis->addWidget(batchInput_);

    const QString jobsTip = QStringLiteral("Max parallel agents/requests running at the same time");
    auto* concurrentLabel = new QLabel(QStringLiteral("Jobs:"));
    concurrentLabel->setToolTip(jobsTip);
    analysis->addWidget(concurrentLabel);
    concurrentInput_ = new QLineEdit(QString::number(cfg.bulkRenamerMaxConcurrent));
    concurrentInput_->setValidator(new QIntValidator(1, 999999, concurrentInput_));
    concurrentInput_->setFixedWidth(50);
    concurrentInput_->setAlignment(Qt::AlignCenter);
    concurrentInput_->setToolTip(jobsTip);
    analysis->addWidget(concurrentInput_);
    analysis->addStretch();
    main->addLayout(analysis);

    // Action bar
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    startBtn_ = new QPushButton(QStringLiteral("Start"));
    startBtn_->setProperty("rkPrimary", true);
    connect(startBtn_, &QPushButton::clicked, this, [this] { onStart(); });
    actions->addWidget(startBtn_);
    stopBtn_ = new QPushButton(QStringLiteral("Stop"));
    stopBtn_->setProperty("rkDanger", true);
    stopBtn_->setEnabled(false);
    connect(stopBtn_, &QPushButton::clicked, this, [this] { onStop(); });
    actions->addWidget(stopBtn_);
    pauseBtn_ = new QPushButton(QStringLiteral("Pause"));
    pauseBtn_->setEnabled(false);
    connect(pauseBtn_, &QPushButton::clicked, this, [this] { onPauseToggle(); });
    actions->addWidget(pauseBtn_);
    undoBtn_ = new QPushButton(QStringLiteral("Undo All"));
    undoBtn_->setEnabled(false);
    connect(undoBtn_, &QPushButton::clicked, this, [this] { onUndo(); });
    actions->addWidget(undoBtn_);
    progress_ = new QProgressBar;
    progress_->setFixedHeight(18);
    progress_->setValue(0);
    actions->addWidget(progress_, 1);
    progressLabel_ = new QLabel(QStringLiteral("0 / 0"));
    progressLabel_->setProperty("rkMuted", true);
    actions->addWidget(progressLabel_);
    main->addLayout(actions);
}

BulkRenamerWidget::~BulkRenamerWidget() {
    events_->detach();
    if (engine_) engine_->cancel();
}

void BulkRenamerWidget::setDatabase(std::shared_ptr<DbContext> db) {
    if (db == db_) return;
    if (engine_) engine_->cancel();
    engine_.reset();
    db_ = std::move(db);
    ++generation_;
    loaded_ = false;
    pendingChecks_.clear();
    paused_ = false;
    startBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
    pauseBtn_->setEnabled(false);
    pauseBtn_->setText(QStringLiteral("Pause"));
    undoBtn_->setEnabled(false);
    progress_->setMaximum(1);
    progress_->setValue(0);
    progressLabel_->setText(QStringLiteral("0 / 0"));
    beginFunctionLoad();
    loadingMore_ = false;
    updateLoadStatus();
    if (isVisible()) ensureLoaded();
}

void BulkRenamerWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    ensureLoaded();
}

// ---- function loading ----

void BulkRenamerWidget::ensureLoaded() {
    if (loaded_ || !db_) return;
    if (!db_->registry.find(QStringLiteral("list_functions"))) {
        log(QStringLiteral("Rikugan: list_functions tool not available — renamer table will be empty"));
        return;
    }
    loaded_ = true;
    loadOffset_ = 0;
    totalHint_.clear();
    beginFunctionLoad();
    fetchPage();
}

void BulkRenamerWidget::beginFunctionLoad() {
    loading_ = true;
    loadingMore_ = true;
    searching_ = false;
    lastRemoteSearch_.clear();
    hasMore_ = false;
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    rows_.clear();
    loading_ = false;
    updateLoadStatus();
    updateSelectionCount();
}

void BulkRenamerWidget::fetchPage() {
    if (!db_) return;
    loadingMore_ = true;
    updateLoadStatus();
    const std::shared_ptr<DbContext> db = db_;
    const int gen = generation_;
    const qint64 offset = loadOffset_;
    runInBackground<QString>(
        this,
        [db, offset] {
            try {
                return ToolDispatcher::runDirect(*db->host, db->registry, nullptr, QStringLiteral("list_functions"),
                                                 QJsonObject{{"offset", offset}, {"limit", kPageSize}});
            } catch (const std::exception& e) {
                log(QStringLiteral("Rikugan: list_functions failed at offset %1: %2").arg(offset).arg(QString::fromUtf8(e.what())));
                return QString();
            }
        },
        [this, gen, offset](QString raw) { onPage(gen, offset, raw); });
}

void BulkRenamerWidget::onPage(int generation, qint64 offset, const QString& raw) {
    if (generation != generation_) return;
    QString total;
    QVector<FunctionRow> functions = parseFunctionPage(raw, &total);
    if (!total.isEmpty()) totalHint_ = total;
    for (qsizetype i = 0; i + 1 < functions.size(); ++i)
        functions[i].instructionCount = qint64(functions[i + 1].address - functions[i].address);
    const qint64 count = functions.size();
    const qint64 next = offset + count;
    bool totalIsInt = false;
    const qint64 totalNum = totalHint_.toLongLong(&totalIsInt);
    const bool more = count >= kPageSize && (!totalIsInt || next < totalNum);
    loadOffset_ = next;
    appendFunctionPage(std::move(functions), more);
    if (count) log(QStringLiteral("Rikugan: loaded bulk renamer function page %1-%2").arg(offset).arg(next));
    else if (offset == 0) log(QStringLiteral("Rikugan: no functions found for bulk renamer"));
}

void BulkRenamerWidget::appendFunctionPage(QVector<FunctionRow> functions, bool hasMore) {
    // drop rows already present
    QVector<FunctionRow> fresh;
    QSet<quint64> seen;
    for (const FunctionRow& f : functions) {
        if (rows_.contains(f.address) || seen.contains(f.address)) continue;
        seen.insert(f.address);
        fresh.push_back(f);
    }
    loading_ = true;
    loadingMore_ = true;
    table_->setSortingEnabled(false);
    const int start = table_->rowCount();
    table_->setRowCount(start + int(fresh.size()));
    for (int k = 0; k < int(fresh.size()); ++k) {
        const FunctionRow& e = fresh[k];
        const int row = start + k;
        Items it;
        it.entry = e;
        it.check = new QTableWidgetItem;
        const bool pending = pendingChecks_.remove(e.address);
        const bool checked = pending || (isAutoNamed(e.name) && !e.isImport);
        it.check->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        it.check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        table_->setItem(row, ColCheck, it.check);

        auto* addr = new NumericItem(QStringLiteral("0x") + QString::number(e.address, 16).toUpper(), qint64(e.address));
        addr->setFlags(readOnlyFlags());
        addr->setData(Qt::UserRole, QVariant::fromValue<quint64>(e.address));
        addr->setToolTip(QStringLiteral("0x") + QStringLiteral("%1").arg(e.address, 16, 16, QLatin1Char('0')).toUpper());
        table_->setItem(row, ColAddr, addr);

        auto* name = new QTableWidgetItem(e.name);
        name->setFlags(readOnlyFlags());
        table_->setItem(row, ColName, name);

        auto* length = new NumericItem(QString::number(e.instructionCount), e.instructionCount);
        length->setFlags(readOnlyFlags());
        length->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, ColLength, length);

        it.newName = new QTableWidgetItem;
        it.newName->setFlags(readOnlyFlags());
        table_->setItem(row, ColNewName, it.newName);

        it.status = new QTableWidgetItem;
        it.status->setFlags(readOnlyFlags());
        table_->setItem(row, ColStatus, it.status);

        rows_.insert(e.address, it);
    }
    table_->setSortingEnabled(true);
    loading_ = false;
    loadingMore_ = false;
    hasMore_ = hasMore;
    updateLoadStatus();
    onFilterChanged();
    updateSelectionCount();
}

void BulkRenamerWidget::requestLoadMore() {
    if (hasMore_ && !loadingMore_) fetchPage();
}

void BulkRenamerWidget::updateLoadStatus() {
    const qsizetype loaded = rows_.size();
    QString base;
    if (totalHint_.isEmpty())
        base = loaded ? QStringLiteral("%1 loaded").arg(loaded) : QStringLiteral("Loading functions...");
    else if (totalHint_ == QLatin1String("unknown"))
        base = QStringLiteral("%1 loaded").arg(loaded);
    else
        base = QStringLiteral("%1 / %2 loaded").arg(loaded).arg(totalHint_);
    if (!db_) base = loaded ? QStringLiteral("%1 loaded").arg(loaded) : QString();
    QString text = base;
    if (searching_) text = base + QStringLiteral(" - searching...");
    else if (loadingMore_ && db_) text = base + QStringLiteral("...");
    loadStatusLabel_->setText(text);
    loadMoreBtn_->setVisible(hasMore_ || (loadingMore_ && loaded_));
    loadMoreBtn_->setEnabled(hasMore_ && !loadingMore_);
}

// ---- search ----

void BulkRenamerWidget::requestSearch(const QString& query) {
    if (!db_) return;
    searching_ = true;
    lastRemoteSearch_ = query;
    updateLoadStatus();
    const std::shared_ptr<DbContext> db = db_;
    const int gen = generation_;
    const QString q = query.trimmed();
    runInBackground<QVector<FunctionRow>>(
        this,
        [db, q] {
            static const QRegularExpression hexQuery(QStringLiteral("^0x[0-9a-fA-F]+$"));
            QVector<FunctionRow> found;
            try {
                if (hexQuery.match(q).hasMatch()) {
                    if (db->registry.find(QStringLiteral("get_function_info"))) {
                        FunctionRow r;
                        if (parseFunctionInfo(ToolDispatcher::runDirect(*db->host, db->registry, nullptr,
                                                                        QStringLiteral("get_function_info"), QJsonObject{{"address", q}}),
                                              &r))
                            found.push_back(r);
                    }
                } else if (db->registry.find(QStringLiteral("search_functions"))) {
                    found = parseFunctionPage(ToolDispatcher::runDirect(*db->host, db->registry, nullptr, QStringLiteral("search_functions"),
                                                                        QJsonObject{{"query", q}, {"limit", 50}}));
                }
            } catch (const std::exception& e) {
                log(QStringLiteral("Rikugan: renamer function search failed for '%1': %2").arg(q, QString::fromUtf8(e.what())));
            }
            return found;
        },
        [this, gen](QVector<FunctionRow> found) {
            if (gen != generation_) return;
            if (!found.isEmpty()) appendFunctionPage(std::move(found), hasMore_);
            setSearchLoading(false);
        });
}

void BulkRenamerWidget::lookupAddresses(const QList<quint64>& addresses) {
    if (!db_ || !db_->registry.find(QStringLiteral("get_function_info"))) return;
    searching_ = true;
    updateLoadStatus();
    const std::shared_ptr<DbContext> db = db_;
    const int gen = generation_;
    runInBackground<QVector<FunctionRow>>(
        this,
        [db, addresses] {
            QVector<FunctionRow> found;
            for (quint64 a : addresses) {
                try {
                    FunctionRow r;
                    if (parseFunctionInfo(ToolDispatcher::runDirect(*db->host, db->registry, nullptr, QStringLiteral("get_function_info"),
                                                                    QJsonObject{{"address", hex(a)}}),
                                          &r))
                        found.push_back(r);
                } catch (const std::exception& e) {
                    log(QStringLiteral("Rikugan: renamer lookup failed for %1: %2").arg(hex(a), QString::fromUtf8(e.what())));
                }
            }
            return found;
        },
        [this, gen](QVector<FunctionRow> found) {
            if (gen != generation_) return;
            if (!found.isEmpty()) appendFunctionPage(std::move(found), hasMore_);
            setSearchLoading(false);
        });
}

void BulkRenamerWidget::setSearchLoading(bool loading) {
    const bool was = searching_;
    searching_ = loading;
    updateLoadStatus();
    if (was && !loading && !loading_) onFilterChanged();
}

void BulkRenamerWidget::selectAddresses(const QList<quint64>& addresses) {
    ensureLoaded();
    QList<quint64> missing;
    if (addresses.size() == 1) {
        const QString text = hex(addresses.first());
        lastRemoteSearch_ = text;  // the lookup below covers it
        filterEdit_->setText(text);
    }
    bulkCheck_ = true;
    for (quint64 a : addresses) {
        const auto it = rows_.constFind(a);
        if (it != rows_.constEnd()) {
            it->check->setCheckState(Qt::Checked);
        } else {
            pendingChecks_.insert(a);
            missing << a;
        }
    }
    bulkCheck_ = false;
    updateSelectionCount();
    if (!missing.isEmpty()) lookupAddresses(missing);
}

// ---- filtering and selection ----

quint64 BulkRenamerWidget::addressForRow(int row) const {
    const QTableWidgetItem* item = table_->item(row, ColAddr);
    return item ? item->data(Qt::UserRole).value<quint64>() : 0;
}

void BulkRenamerWidget::onFilterChanged() {
    if (loading_) return;
    const QString text = filterEdit_->text().trimmed().toLower();
    const int mode = filterCombo_->currentIndex();
    int visible = 0;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto it = rows_.constFind(addressForRow(row));
        if (it == rows_.constEnd()) continue;
        const FunctionRow& e = it->entry;
        const QString hexLower = hex(e.address);
        bool textMatch = text.isEmpty() || e.name.toLower().contains(text) || hexLower.contains(text);
        if (!textMatch) {
            QString hexUpper = QStringLiteral("0x") + QString::number(e.address, 16).toUpper();
            textMatch = hexUpper.contains(text);
        }
        bool comboMatch = true;
        if (mode == 1) comboMatch = isAutoNamed(e.name);
        else if (mode == 2) comboMatch = !isAutoNamed(e.name) && !e.isImport;
        else if (mode == 3) comboMatch = e.isImport;
        const bool show = textMatch && comboMatch;
        if (show) ++visible;
        table_->setRowHidden(row, !show);
    }
    if (shouldRequestRemoteSearch(text, visible)) requestSearch(text);
}

bool BulkRenamerWidget::shouldRequestRemoteSearch(const QString& text, int visibleMatches) const {
    if (text.isEmpty() || visibleMatches > 0) return false;
    if (!hasMore_ || searching_) return false;
    if (text == lastRemoteSearch_) return false;
    return text.size() >= 2 || text.startsWith(QLatin1String("0x"));
}

void BulkRenamerWidget::setChecked(QTableWidgetItem* item, bool checked) {
    if (item) item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

void BulkRenamerWidget::onHeaderCheck(bool checked) {
    bulkCheck_ = true;
    for (int row = 0; row < table_->rowCount(); ++row)
        if (!table_->isRowHidden(row)) setChecked(table_->item(row, ColCheck), checked);
    bulkCheck_ = false;
    updateSelectionCount();
}

void BulkRenamerWidget::onCellClicked(int row, int column) {
    if (column != ColCheck) return;
    QSet<int> selected;
    for (const QModelIndex& idx : table_->selectionModel()->selectedRows()) selected.insert(idx.row());
    if (selected.size() <= 1 || !selected.contains(row)) return;
    const QTableWidgetItem* clicked = table_->item(row, ColCheck);
    if (!clicked) return;
    const bool state = clicked->checkState() == Qt::Checked;
    bulkCheck_ = true;
    for (int r : selected) setChecked(table_->item(r, ColCheck), state);
    bulkCheck_ = false;
    updateSelectionCount();
}

void BulkRenamerWidget::onCellDoubleClicked(int row, int column) {
    if (column != ColAddr && column != ColName && column != ColNewName) return;
    const quint64 addr = addressForRow(row);
    if (db_ && rows_.contains(addr)) db_->host->navigate(addr);
}

int BulkRenamerWidget::batchValue() const {
    bool ok = false;
    const int v = batchInput_->text().toInt(&ok);
    return ok ? std::max(1, v) : 10;
}

int BulkRenamerWidget::concurrentValue() const {
    bool ok = false;
    const int v = concurrentInput_->text().toInt(&ok);
    return ok ? std::max(1, v) : 3;
}

void BulkRenamerWidget::updateSelectionCount() {
    const int total = table_->rowCount();
    int selected = 0;
    for (int row = 0; row < total; ++row) {
        const QTableWidgetItem* item = table_->item(row, ColCheck);
        if (item && item->checkState() == Qt::Checked) ++selected;
    }
    if (deepRadio_->isChecked() && selected > 0) {
        selectionLabel_->setText(QStringLiteral("%1 / %2 selected \u2022 %1 subagents").arg(selected).arg(total));
    } else if (selected > 0) {
        const int batch = batchValue();
        const int batches = (selected + batch - 1) / batch;
        selectionLabel_->setText(QStringLiteral("%1 / %2 selected \u2022 %3 batch(es)").arg(selected).arg(total).arg(batches));
    } else {
        selectionLabel_->setText(QStringLiteral("%1 / %2 selected").arg(selected).arg(total));
    }
}

// ---- running ----

void BulkRenamerWidget::onStart() {
    if (!db_) return;
    std::vector<RenameJob> jobs;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QTableWidgetItem* check = table_->item(row, ColCheck);
        if (!check || check->checkState() != Qt::Checked) continue;
        const auto it = rows_.constFind(addressForRow(row));
        if (it == rows_.constEnd()) continue;
        RenameJob j;
        j.address = it->entry.address;
        j.currentName = it->entry.name;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) return;
    const bool deep = deepRadio_->isChecked();

    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
    pauseBtn_->setEnabled(true);
    pauseBtn_->setText(QStringLiteral("Pause"));
    paused_ = false;
    for (const RenameJob& j : jobs) updateJob(j.address, QString(), QStringLiteral("queued"), QString());
    setProgress(0, int(jobs.size()));

    std::weak_ptr<EventQueue> wq = events_;
    engine_ = BulkRenamerEngine::create(db_, BulkRenamerEngine::Options{batchValue(), concurrentValue(), {}}, [wq](const RenameEvent& e) {
        if (auto q = wq.lock()) q->push(eventJson(e).toStdString());
    });
    engine_->enqueue(jobs);
    engine_->start(deep);
}

void BulkRenamerWidget::onStop() {
    stopBtn_->setEnabled(false);
    pauseBtn_->setEnabled(false);
    pauseBtn_->setText(QStringLiteral("Pause"));
    startBtn_->setEnabled(true);
    paused_ = false;
    if (engine_) engine_->cancel();
}

void BulkRenamerWidget::onPauseToggle() {
    paused_ = !paused_;
    pauseBtn_->setText(paused_ ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    if (!engine_) return;
    if (engine_->isPaused()) engine_->resume();
    else engine_->pause();
}

void BulkRenamerWidget::onUndo() {
    if (engine_) engine_->undoAll();
}

void BulkRenamerWidget::updateJob(quint64 address, const QString& newName, const QString& status, const QString& error) {
    const auto it = rows_.constFind(address);
    if (it == rows_.constEnd()) return;
    const QSignalBlocker block(table_);
    it->newName->setText(newName);
    it->status->setText(error.isEmpty() ? status : error);
    it->status->setForeground(statusColor(status));
}

void BulkRenamerWidget::setProgress(int current, int total) {
    if (total > 0) {
        progress_->setMaximum(total);
        progress_->setValue(current);
    } else {
        progress_->setMaximum(1);
        progress_->setValue(0);
    }
    progressLabel_->setText(QStringLiteral("%1 / %2").arg(current).arg(total));
    undoBtn_->setEnabled(current > 0);
    if (current >= total && total > 0) {
        startBtn_->setEnabled(true);
        stopBtn_->setEnabled(false);
        pauseBtn_->setEnabled(false);
        pauseBtn_->setText(QStringLiteral("Pause"));
        paused_ = false;
    }
}

bool BulkRenamerWidget::event(QEvent* e) {
    if (e->type() == EventQueue::DrainEvent) {
        drainEvents();
        return true;
    }
    return QWidget::event(e);
}

void BulkRenamerWidget::drainEvents() {
    for (const std::string& raw : events_->drain()) {
        const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(raw)).object();
        const auto type = RenameEventType(o.value(QStringLiteral("type")).toInt());
        const quint64 address = o.value(QStringLiteral("address")).toString().toULongLong();
        const QString newName = o.value(QStringLiteral("newName")).toString();
        switch (type) {
        case RenameEventType::JobStarted: updateJob(address, newName, QStringLiteral("analyzing"), QString()); break;
        case RenameEventType::JobCompleted:
            // an undo reports the reverted rename with no new name
            updateJob(address, newName, newName.isEmpty() ? QStringLiteral("reverted") : QStringLiteral("renamed"), QString());
            break;
        case RenameEventType::JobError:
            updateJob(address, newName, QStringLiteral("error"), o.value(QStringLiteral("error")).toString());
            break;
        case RenameEventType::BatchProgress:
        case RenameEventType::AllDone:
            setProgress(o.value(QStringLiteral("completed")).toInt(), o.value(QStringLiteral("total")).toInt());
            break;
        }
    }
}

}  // namespace rk
