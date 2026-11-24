#pragma once
// The chat transcript: a virtualized QListView over TranscriptModel. Rows are painted by the delegate (timeline
// rail and nodes, tool rows with kind, chips, undo and status, subagent task cards, Markdown through cached
// QTextDocuments); pending cards are live widgets in their rows. Sticks to the bottom while the user has not
// scrolled up; a "new" pill brings them back. UI thread only.
#include <QElapsedTimer>
#include <QHash>
#include <QListView>
#include <QPointer>
#include <QTimer>

#include "common/ui/transcript/TranscriptModel.h"

QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTextBrowser)

namespace rk::ui {

class TranscriptCard;
class TranscriptDelegate;

class TranscriptView : public QListView {
    Q_OBJECT
public:
    explicit TranscriptView(QWidget* parent = nullptr);
    ~TranscriptView() override;

    void setTranscriptModel(TranscriptModel* model);
    TranscriptModel* transcriptModel() const { return model_; }
    // Shown instead of the list while the transcript is empty (the welcome screen).
    void setEmptyWidget(QWidget* widget);
    void scrollToEnd();
    bool following() const { return follow_; }

    // The live card of a row (layout and tests); nullptr when the row has none.
    TranscriptCard* cardFor(quint64 itemId) const;

Q_SIGNALS:
    void navigateRequested(quint64 address);
    // Undo the database change recorded at `mutationIndex` of the chat's log (and every later one).
    void undoRequested(int mutationIndex);
    void agentActivated(const QString& subagentId);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void rowsInserted(const QModelIndex& parent, int start, int end) override;
    void dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles = {}) override;
    void reset() override;

private:
    void syncCards(int from, int to);
    void syncGroups(int from, int to);
    void syncEmpty();
    void placePill();
    void onRangeChanged(int min, int max);
    void onScrolled(int value);
    void updateSpinner();
    void tickSpinner();
    void openSelection(const QModelIndex& index, QMouseEvent* press);
    void closeSelection();
    void activate(const QModelIndex& index, const QPoint& pos);
    void restyle();

    QPointer<TranscriptModel> model_;
    TranscriptDelegate* delegate_ = nullptr;
    QHash<quint64, QPointer<TranscriptCard>> cards_;
    QPointer<QWidget> empty_;
    QPushButton* pill_ = nullptr;
    QPointer<QTextBrowser> selection_;
    quint64 selectionId_ = 0;
    QTimer spinner_;  // runs only while the chat works
    QElapsedTimer spinClock_;
    bool follow_ = true;
    bool settingScroll_ = false;
    int unseen_ = 0;
};

}  // namespace rk::ui
