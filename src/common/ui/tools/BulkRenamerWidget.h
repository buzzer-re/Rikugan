#pragma once
// The Renamer tab: the function table
// (paged from list_functions), filters, search, quick/deep, batch size, jobs, start/stop/pause/undo all, progress.
// UI thread only; host calls and the engine run on background threads.
#include <memory>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QProgressBar)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QRadioButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTableWidgetItem)

namespace rk {

struct DbContext;
class BulkRenamerEngine;
class EventQueue;

class BulkRenamerWidget : public QWidget {
    Q_OBJECT
public:
    struct FunctionRow {
        quint64 address = 0;
        QString name;
        bool isImport = false;
        qint64 instructionCount = 0;
    };

    explicit BulkRenamerWidget(QWidget* parent = nullptr);
    ~BulkRenamerWidget() override;

    // Cancels a running engine and reloads the table for `db` (when shown).
    void setDatabase(std::shared_ptr<DbContext> db);
    // Send to bulk renamer: check these functions (fetched when not loaded yet); one address also filters to it.
    void selectAddresses(const QList<quint64>& addresses);

    // list_functions / search_functions page text -> rows (+ the "Functions a–b of N:" total when present).
    static QVector<FunctionRow> parseFunctionPage(const QString& raw, QString* totalHint = nullptr);
    // get_function_info text -> one row.
    static bool parseFunctionInfo(const QString& raw, FunctionRow* out);
    // The table's default-check heuristic (sub_, fn_, loc_, j_, nullsub_, unknown_, FUN_ prefixes).
    static bool isAutoNamed(const QString& name);

protected:
    bool event(QEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    struct Items {
        FunctionRow entry;
        QTableWidgetItem* check = nullptr;
        QTableWidgetItem* newName = nullptr;
        QTableWidgetItem* status = nullptr;
    };

    void ensureLoaded();
    void beginFunctionLoad();
    void fetchPage();
    void onPage(int generation, qint64 offset, const QString& raw);
    void appendFunctionPage(QVector<FunctionRow> functions, bool hasMore);
    void requestSearch(const QString& query);
    void lookupAddresses(const QList<quint64>& addresses);
    void setSearchLoading(bool loading);
    void updateLoadStatus();
    void requestLoadMore();
    void onFilterChanged();
    bool shouldRequestRemoteSearch(const QString& text, int visibleMatches) const;
    void updateSelectionCount();
    void onHeaderCheck(bool checked);
    void onCellClicked(int row, int column);
    void onCellDoubleClicked(int row, int column);
    void onStart();
    void onStop();
    void onPauseToggle();
    void onUndo();
    void updateJob(quint64 address, const QString& newName, const QString& status, const QString& error);
    void setProgress(int current, int total);
    void drainEvents();
    void setChecked(QTableWidgetItem* item, bool checked);
    quint64 addressForRow(int row) const;
    int batchValue() const;
    int concurrentValue() const;

    std::shared_ptr<DbContext> db_;
    std::shared_ptr<BulkRenamerEngine> engine_;
    std::shared_ptr<EventQueue> events_;

    QLineEdit* filterEdit_ = nullptr;
    QComboBox* filterCombo_ = nullptr;
    QLabel* selectionLabel_ = nullptr;
    QLabel* loadStatusLabel_ = nullptr;
    QPushButton* loadMoreBtn_ = nullptr;
    QTableWidget* table_ = nullptr;
    QCheckBox* headerCheck_ = nullptr;
    QRadioButton* quickRadio_ = nullptr;
    QRadioButton* deepRadio_ = nullptr;
    QLineEdit* batchInput_ = nullptr;
    QLineEdit* concurrentInput_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* stopBtn_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* undoBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* progressLabel_ = nullptr;

    QHash<quint64, Items> rows_;  // address -> row items (owned by the table)
    QSet<quint64> pendingChecks_;  // sent to the renamer before their row was loaded
    bool loading_ = false;         // suppresses the filter while rows are inserted
    bool bulkCheck_ = false;       // suppresses per-item count updates
    bool paused_ = false;
    bool loaded_ = false;
    int generation_ = 0;  // bumps on each database change; stale host replies are dropped
    qint64 loadOffset_ = 0;
    static constexpr int kPageSize = 250;
    QString totalHint_;  // "" none, a number, or text such as "unknown"
    bool hasMore_ = false;
    bool loadingMore_ = false;
    bool searching_ = false;
    QString lastRemoteSearch_;
};

}  // namespace rk
