#pragma once
// The chat manager: a column beside the full-window panel that lists the chats of every database open in the host,
// each database a section with a "New chat" link, with what each chat is doing (running, waiting for an answer).
// Choosing one shows it in the panel, whichever database it belongs to. Follows ChatHub with one coalesced refresh
// per burst of changes, only while shown. UI thread only.
#include <QFrame>
#include <QHash>
#include <QTimer>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk::ui {

class PaletteList;

// "just now", "5m ago", "3h ago", "Nov 2" (ms since epoch; empty for 0).
QString relativeTime(qint64 ms);

class ChatManager : public QFrame {
    Q_OBJECT
public:
    explicit ChatManager(QWidget* parent = nullptr);

    // The chat the panel shows (checked in the list); empty chatId: none.
    void setCurrent(const QString& dbId, const QString& chatId);
    // Whether "Open a binary…" is offered (the host can open files).
    void setCanOpen(bool canOpen);
    void refresh();

Q_SIGNALS:
    void chatActivated(const QString& dbId, const QString& chatId);
    void newChatRequested(const QString& dbId);
    void openRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void scheduleRefresh();
    void restyle();
    QLabel* title_ = nullptr;
    QLineEdit* search_ = nullptr;
    PaletteList* list_ = nullptr;
    QPushButton* open_ = nullptr;
    QTimer refresh_;
    QString dbId_, chatId_;
    QHash<QString, QString> sectionDb_;  // section heading -> database id
};

}  // namespace rk::ui
